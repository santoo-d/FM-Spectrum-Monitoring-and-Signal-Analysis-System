/*
  FM Spectrum Monitoring System
  Board   : ESP32
  Radio   : TEA5767 FM module (I2C, address 0x60)   <- assumed
  Display : 16x2 I2C LCD (address 0x27)
  Input   : Potentiometer for manual tuning
  App     : Blynk IoT dashboard

  Features:
  - Tune 87.5 - 108.0 MHz from the app (+/-), the tune button or the potentiometer
  - Live RSSI, SNR (estimated), signal quality, stereo / pilot status
  - Auto scan: sweeps the FM band, counts stations and tunes to the strongest
  - LCD shows frequency, mono/stereo, RSSI level and quality

  Note: the TEA5767 reports a 4-bit signal level (0-15) and a stereo flag.
  RSSI (dBm), SNR (dB) and quality (%) are ESTIMATED from that level.

  Blynk virtual pins:
    V0  Current frequency (MHz)       V6  Pilot LED
    V1  Frequency control (87.5-108)  V7  Scan control switch
    V2  RSSI (dBm)                    V8  Scan status (text)
    V3  SNR (dB)                      V9  Station number
    V4  Signal quality (%)            V10 Uptime (text)
    V5  Stereo LED                    V11 Tune command button
*/

#include "secrets.h"            // copy secrets_example.h -> secrets.h

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- Pins & addresses (change to match your wiring) ----------
const int SDA_PIN = 21;
const int SCL_PIN = 22;
const int POT_PIN = 34;          // potentiometer wiper (analog)
const uint8_t TEA_ADDR = 0x60;
const uint8_t LCD_ADDR = 0x27;

// ---------- FM band (stored in tenths of MHz: 875 = 87.5 MHz) ----------
const int FM_MIN = 875;
const int FM_MAX = 1080;
const int MAX_STATIONS = 30;
const int PEAK_MIN_LEVEL = 7;    // minimum level (0-15) to count as a station

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
BlynkTimer timer;

// ---------- State ----------
int freq10 = 1015;               // current frequency x10
int potLast = -1;
uint8_t level = 0;               // 0-15
bool stereo = false;
bool scanning = false;
int stationCount = 0;
int stations[MAX_STATIONS];
int stationLevel[MAX_STATIONS];
int appFreq10 = 1015;            // value requested from the app

// ---------- TEA5767 ----------
void tuneTo(int f10) {
  freq10 = constrain(f10, FM_MIN, FM_MAX);
  unsigned long hz  = (unsigned long)freq10 * 100000UL;
  unsigned long pll = 4UL * (hz + 225000UL) / 32768UL;
  Wire.beginTransmission(TEA_ADDR);
  Wire.write((pll >> 8) & 0x3F);
  Wire.write(pll & 0xFF);
  Wire.write(0xB0);              // high-side injection, stereo on
  Wire.write(0x10);              // 32.768 kHz crystal
  Wire.write(0x00);
  Wire.endTransmission();
}

bool readRadio() {
  if (Wire.requestFrom((int)TEA_ADDR, 5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();
  stereo = b[2] & 0x80;
  level  = b[3] >> 4;
  return true;
}

// ---------- Estimated values from the 0-15 level ----------
int estRssiDbm()  { return map(level, 0, 15, -120, -60); }
int estSnrDb()    { return map(level, 0, 15, 0, 40); }
int qualityPct()  { return map(level, 0, 15, 0, 100); }

// ---------- LCD ----------
void updateLcd() {
  lcd.setCursor(0, 0);
  char line1[17];
  snprintf(line1, sizeof(line1), "FM:%d.%dMHz %s ", freq10 / 10, freq10 % 10, stereo ? "ST" : "MO");
  lcd.print(line1);
  lcd.setCursor(0, 1);
  char line2[17];
  snprintf(line2, sizeof(line2), "RSSI:%d Q:%d%%   ", level, qualityPct());
  lcd.print(line2);
}

// ---------- Blynk inputs ----------
BLYNK_WRITE(V1)  { appFreq10 = (int)(param.asFloat() * 10.0 + 0.5); if (!scanning) tuneTo(appFreq10); }
BLYNK_WRITE(V11) { if (param.asInt() && !scanning) tuneTo(appFreq10); }
BLYNK_WRITE(V7)  { scanning = param.asInt(); }

// ---------- Upload to dashboard ----------
void sendData() {
  readRadio();
  Blynk.virtualWrite(V0, freq10 / 10.0);
  Blynk.virtualWrite(V2, estRssiDbm());
  Blynk.virtualWrite(V3, estSnrDb());
  Blynk.virtualWrite(V4, qualityPct());
  Blynk.virtualWrite(V5, stereo ? 255 : 0);
  Blynk.virtualWrite(V6, stereo ? 255 : 0);   // TEA5767 has no separate pilot flag
  Blynk.virtualWrite(V9, stationCount);

  unsigned long s = millis() / 1000;
  char up[16];
  snprintf(up, sizeof(up), "%lum %02lus", s / 60, s % 60);
  Blynk.virtualWrite(V10, up);

  updateLcd();
}

// ---------- Band scan ----------
void runScan() {
  static int levels[FM_MAX - FM_MIN + 1];
  stationCount = 0;
  int total = FM_MAX - FM_MIN + 1;

  for (int i = 0; i < total; i++) {
    tuneTo(FM_MIN + i);
    for (int k = 0; k < 5; k++) { Blynk.run(); delay(10); }   // let the PLL settle
    readRadio();
    levels[i] = level;

    if (i % 10 == 0) {
      char msg[20];
      snprintf(msg, sizeof(msg), "Scanning %d%%", i * 100 / total);
      Blynk.virtualWrite(V8, msg);
      freq10 = FM_MIN + i;
      updateLcd();
    }
    if (!scanning) { Blynk.virtualWrite(V8, "Stopped"); return; }
  }

  // Find local peaks that are strong enough
  int lastPeak = -10;
  for (int i = 1; i < total - 1 && stationCount < MAX_STATIONS; i++) {
    if (levels[i] >= PEAK_MIN_LEVEL && levels[i] >= levels[i - 1] && levels[i] > levels[i + 1] && i - lastPeak >= 2) {
      stations[stationCount] = FM_MIN + i;
      stationLevel[stationCount] = levels[i];
      stationCount++;
      lastPeak = i;
    }
  }

  // Tune to the strongest station
  int best = 0;
  for (int i = 1; i < stationCount; i++) if (stationLevel[i] > stationLevel[best]) best = i;
  if (stationCount > 0) {
    tuneTo(stations[best]);
    appFreq10 = freq10;
    Blynk.virtualWrite(V1, freq10 / 10.0);
  }

  scanning = false;
  Blynk.virtualWrite(V7, 0);
  Blynk.virtualWrite(V9, stationCount);
  Blynk.virtualWrite(V8, "Done");
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.print("FM Monitor");
  tuneTo(freq10);

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS);
  Blynk.virtualWrite(V8, "Idle");
  timer.setInterval(1000L, sendData);
}

void loop() {
  Blynk.run();
  timer.run();

  if (scanning) {
    runScan();
    return;
  }

  // Potentiometer tuning (only when the knob is actually turned)
  int pot = analogRead(POT_PIN);
  if (potLast < 0 || abs(pot - potLast) > 60) {
    if (potLast >= 0) {
      tuneTo(map(pot, 0, 4095, FM_MIN, FM_MAX));
      Blynk.virtualWrite(V1, freq10 / 10.0);
    }
    potLast = pot;
  }
}
