/*
  GIAM SAT NHIET DO & DO AM - ESP32 + BLYNK + MQTT (web tach rieng, khong can chung WiFi)
  Chan: DHT11=GPIO4, Buzzer=GPIO5, Relay=GPIO18, LCD I2C: SDA=21, SCL=22
*/
#define BLYNK_PRINT Serial
#include <Arduino.h>
#include "secrets.h"          // phải nằm TRƯỚC BlynkSimpleEsp32.h (chứa BLYNK_TEMPLATE_ID...)
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Preferences.h>

const char* ssid     = CDP301;
const char* password = duancuoimon;

// ---- MQTT ----
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
const String PREFIX   = MQTT_PREFIX;

#define CHO_PHEP_BLYNK_DOI_NGUONG false
#define DHTPIN 4
#define DHTTYPE DHT11
#define BUZZER_PIN 5
#define RELAY_PIN 18
#define LCD_I2C_ADDRESS 0x27   // neu LCD khong hien chu, thu 0x3F

float nhietDoNguongCao = 35.0, doAmNguongCao = 80.0;
float nhietDoHienTai = NAN, doAmHienTai = NAN;
bool dangBaoDong = false, daTatChuong = false;
int soLanLoiLienTiep = 0;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);
BlynkTimer timer;
Preferences luuTru;
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
unsigned long lanThuKetNoiCuoi = 0;

void batBaoDong()  { digitalWrite(BUZZER_PIN, HIGH); digitalWrite(RELAY_PIN, HIGH); }
void tatBaoDong()  { digitalWrite(BUZZER_PIN, LOW);  digitalWrite(RELAY_PIN, LOW); }

// ---------- Gui trang thai len MQTT (retained de web vao la thay ngay) ----------
void guiTrangThai() {
  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"humidity\":%.1f,\"tempThreshold\":%.1f,\"humidThreshold\":%.1f,\"alarm\":%s,\"muted\":%s}",
    isnan(nhietDoHienTai) ? 0 : nhietDoHienTai, isnan(doAmHienTai) ? 0 : doAmHienTai,
    nhietDoNguongCao, doAmNguongCao, dangBaoDong ? "true" : "false", daTatChuong ? "true" : "false");
  mqtt.publish((PREFIX + "/status").c_str(), buf, true);
}

// ---------- Dat nguong ----------
void datNguongNhiet(float v, const char* nguon) {
  v = constrain(v, 0, 60);
  if (v == nhietDoNguongCao) return;
  Serial.printf("[DOI NGUONG NHIET] %.1f -> %.1f (nguon: %s)\n", nhietDoNguongCao, v, nguon);
  nhietDoNguongCao = v;
  luuTru.putFloat("tNguong", v);
  Blynk.virtualWrite(V2, v);
}
void datNguongDoAm(float v, const char* nguon) {
  v = constrain(v, 0, 100);
  if (v == doAmNguongCao) return;
  Serial.printf("[DOI NGUONG DO AM] %.1f -> %.1f (nguon: %s)\n", doAmNguongCao, v, nguon);
  doAmNguongCao = v;
  luuTru.putFloat("hNguong", v);
  Blynk.virtualWrite(V3, v);
}

// ---------- Nhan lenh tu web qua MQTT ----------
void onMqtt(char* topic, byte* payload, unsigned int len) {
  String t = String(topic), v;
  for (unsigned int i = 0; i < len; i++) v += (char)payload[i];
  if      (t.endsWith("/cmd/temp"))   datNguongNhiet(v.toFloat(), "web");
  else if (t.endsWith("/cmd/humid"))  datNguongDoAm(v.toFloat(), "web");
  else if (t.endsWith("/cmd/mute"))   { daTatChuong = true; tatBaoDong(); }
  else if (t.endsWith("/cmd/unmute")) { daTatChuong = false; if (dangBaoDong) batBaoDong(); }
  guiTrangThai();
}

// Ket noi lai khong chan (khong lam treo Blynk)
void duyTriMqtt() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - lanThuKetNoiCuoi < 5000) return;
  lanThuKetNoiCuoi = millis();
  String id = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String online = PREFIX + "/online";
  // LWT: mat dien/mat mang thi broker tu bao "0"
  if (mqtt.connect(id.c_str(), NULL, NULL, online.c_str(), 0, true, "0")) {
    mqtt.publish(online.c_str(), "1", true);
    mqtt.subscribe((PREFIX + "/cmd/#").c_str());
    guiTrangThai();
    Serial.println("MQTT da ket noi");
  }
}

// ---------- Blynk ----------
BLYNK_WRITE(V2) {
  if (CHO_PHEP_BLYNK_DOI_NGUONG) datNguongNhiet(param.asFloat(), "Blynk");
  else Blynk.virtualWrite(V2, nhietDoNguongCao);
}
BLYNK_WRITE(V3) {
  if (CHO_PHEP_BLYNK_DOI_NGUONG) datNguongDoAm(param.asFloat(), "Blynk");
  else Blynk.virtualWrite(V3, doAmNguongCao);
}
BLYNK_CONNECTED() {
  Blynk.virtualWrite(V2, nhietDoNguongCao);
  Blynk.virtualWrite(V3, doAmNguongCao);
}

// ---------- LCD ----------
void hienThiLCD(float nhietDo, float doAm, bool baoDong) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Nhiet do: "); lcd.print(nhietDo, 1); lcd.print("C");
  lcd.setCursor(0, 1);
  if (baoDong) lcd.print("!! CANH BAO !!");
  else { lcd.print("Do am: "); lcd.print(doAm, 1); lcd.print("%"); }
}

// ---------- Doc cam bien (2 giay/lan) ----------
void docCamBienVaCapNhat() {
  float doAm = dht.readHumidity();
  float nhietDo = dht.readTemperature();
  if (isnan(doAm) || isnan(nhietDo)) {
    soLanLoiLienTiep++;
    lcd.clear(); lcd.setCursor(0, 0); lcd.print("Loi cam bien!");
    lcd.setCursor(0, 1); lcd.print("Kiem tra day noi");
    if (soLanLoiLienTiep >= 3) tatBaoDong();
    return;
  }
  soLanLoiLienTiep = 0;
  nhietDoHienTai = nhietDo;
  doAmHienTai = doAm;
  Serial.printf("Nhiet do: %.1f C | Do am: %.1f %% | Nguong: %.1f C, %.1f %%\n",
                nhietDo, doAm, nhietDoNguongCao, doAmNguongCao);

  dangBaoDong = (nhietDo > nhietDoNguongCao) || (doAm > doAmNguongCao);
  if (!dangBaoDong) daTatChuong = false;
  if (dangBaoDong && !daTatChuong) batBaoDong(); else tatBaoDong();

  hienThiLCD(nhietDo, doAm, dangBaoDong);
  Blynk.virtualWrite(V0, nhietDo);
  Blynk.virtualWrite(V1, doAm);
  Blynk.virtualWrite(V4, dangBaoDong ? 255 : 0);
  guiTrangThai();
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  luuTru.begin("giamsat", false);
  nhietDoNguongCao = luuTru.getFloat("tNguong", 35.0);
  doAmNguongCao    = luuTru.getFloat("hNguong", 80.0);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  tatBaoDong();
  lcd.init(); lcd.backlight();
  lcd.print("He thong giam sat");
  lcd.setCursor(0, 1); lcd.print("Dang ket noi...");

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, password);
  mqtt.setBufferSize(512);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);

  lcd.clear(); lcd.setCursor(0, 0); lcd.print("Blynk + MQTT OK");
  delay(2000);
  lcd.clear();
  timer.setInterval(2000L, docCamBienVaCapNhat);
}

void loop() {
  Blynk.run();
  timer.run();
  duyTriMqtt();
}
