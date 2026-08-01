#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>

// ── WiFi ──────────────────────────────────────
const char* ssid     = "TP-Link_CB74";
const char* password = "yk19920211";

// ── MQTT (HiveMQ Cloud) ───────────────────────
const char* mqtt_server = "7c11a17ad37643f7a3572dccfdfae248.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "YutoK";
const char* mqtt_pass   = "Yut0ichigeki";

// ── MQTTトピック ──────────────────────────────
// control : "ON" / "ON:秒数" / "OFF"
// status  : "ON" / "OFF" (retain)
// schedule: "HH:MM:秒数:有効|..." (retain)
const char* TOPIC_CONTROL  = "tomato/pump/control";
const char* TOPIC_STATUS   = "tomato/pump/status";
const char* TOPIC_SCHEDULE = "tomato/pump/schedule";

const int RELAY_PIN = 26;

WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);
Preferences      prefs;

// ── ポンプ状態 ───────────────────────────────
bool          pumpOn        = false;
bool          timerActive   = false;
unsigned long timerEndMs    = 0;

// ── スケジュール ─────────────────────────────
struct Schedule {
  uint8_t  hour;
  uint8_t  minute;
  uint16_t durationSec;
  bool     enabled;
};
#define MAX_SCHEDULES 10
Schedule schedules[MAX_SCHEDULES];
int numSchedules   = 0;
int lastCheckedMin = -1;

// ─────────────────────────────────────────────
// ポンプ制御
// ─────────────────────────────────────────────
void publishStatus() {
  mqttClient.publish(TOPIC_STATUS, pumpOn ? "ON" : "OFF", true);
}

void pumpTurnOn(int durationSec) {
  pumpOn = true;
  digitalWrite(RELAY_PIN, HIGH);
  if (durationSec > 0) {
    timerEndMs  = millis() + (unsigned long)durationSec * 1000UL;
    timerActive = true;
    Serial.printf("ポンプON（%d秒タイマー）\n", durationSec);
  } else {
    timerActive = false;
    timerEndMs  = 0;
    Serial.println("ポンプON（手動・タイマーなし）");
  }
  publishStatus();
}

void pumpTurnOff() {
  if (!pumpOn) return;
  pumpOn      = false;
  timerActive = false;
  timerEndMs  = 0;
  digitalWrite(RELAY_PIN, LOW);
  publishStatus();
  Serial.println("ポンプOFF");
}

// ─────────────────────────────────────────────
// スケジュール保存・読み込み（Preferences = 不揮発）
// ─────────────────────────────────────────────
void saveSchedules() {
  prefs.begin("sched", false);
  prefs.putInt("n", numSchedules);
  for (int i = 0; i < numSchedules; i++) {
    prefs.putUChar( ("h" + String(i)).c_str(), schedules[i].hour);
    prefs.putUChar( ("m" + String(i)).c_str(), schedules[i].minute);
    prefs.putUShort(("d" + String(i)).c_str(), schedules[i].durationSec);
    prefs.putBool(  ("e" + String(i)).c_str(), schedules[i].enabled);
  }
  prefs.end();
}

void loadSchedules() {
  prefs.begin("sched", true);
  numSchedules = prefs.getInt("n", 0);
  for (int i = 0; i < numSchedules; i++) {
    schedules[i].hour        = prefs.getUChar( ("h" + String(i)).c_str(), 0);
    schedules[i].minute      = prefs.getUChar( ("m" + String(i)).c_str(), 0);
    schedules[i].durationSec = prefs.getUShort(("d" + String(i)).c_str(), 0);
    schedules[i].enabled     = prefs.getBool(  ("e" + String(i)).c_str(), true);
  }
  prefs.end();
  Serial.printf("スケジュール読み込み: %d件\n", numSchedules);
}

// ─────────────────────────────────────────────
// スケジュール文字列パース
// フォーマット: "HH:MM:DURSEC:EN|HH:MM:DURSEC:EN|..."
// 例: "07:00:30:1|18:00:120:0"
// ─────────────────────────────────────────────
void parseScheduleStr(const String& str) {
  numSchedules = 0;
  if (str.length() == 0) { saveSchedules(); return; }

  int start = 0;
  while (start < (int)str.length() && numSchedules < MAX_SCHEDULES) {
    int end   = str.indexOf('|', start);
    if (end < 0) end = str.length();

    String entry = str.substring(start, end);
    int c1 = entry.indexOf(':');
    int c2 = entry.indexOf(':', c1 + 1);
    int c3 = entry.indexOf(':', c2 + 1);

    if (c1 > 0 && c2 > c1 && c3 > c2) {
      schedules[numSchedules].hour        = entry.substring(0,    c1).toInt();
      schedules[numSchedules].minute      = entry.substring(c1+1, c2).toInt();
      schedules[numSchedules].durationSec = entry.substring(c2+1, c3).toInt();
      schedules[numSchedules].enabled     = (entry.substring(c3+1).toInt() == 1);
      numSchedules++;
    }
    start = end + 1;
  }
  saveSchedules();
  Serial.printf("スケジュール更新: %d件\n", numSchedules);
}

// ─────────────────────────────────────────────
// MQTTコールバック
// ─────────────────────────────────────────────
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("受信 [%s]: %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_CONTROL) {
    if (msg == "OFF") {
      pumpTurnOff();
    } else if (msg == "ON") {
      if (!pumpOn) pumpTurnOn(0);
    } else if (msg.startsWith("ON:")) {
      int dur = msg.substring(3).toInt();
      pumpTurnOn(dur);
    }
  } else if (String(topic) == TOPIC_SCHEDULE) {
    parseScheduleStr(msg);
  }
}

// ─────────────────────────────────────────────
// WiFi接続 + NTP時刻同期
// ─────────────────────────────────────────────
void connectWiFi() {
  Serial.print("WiFi接続中...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println(" 完了");

  // 日本標準時（UTC+9）
  configTime(9 * 3600L, 0, "pool.ntp.org", "ntp.nict.jp");
  Serial.print("NTP同期中...");
  struct tm t;
  int tries = 0;
  while (!getLocalTime(&t) && tries++ < 40) {
    delay(500); Serial.print(".");
  }
  if (tries < 40)
    Serial.printf(" 完了 %04d/%02d/%02d %02d:%02d:%02d\n",
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
  else
    Serial.println(" タイムアウト（スケジュール機能が動作しない可能性あり）");
}

// ─────────────────────────────────────────────
// MQTT接続
// ─────────────────────────────────────────────
void connectMQTT() {
  espClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(512);

  while (!mqttClient.connected()) {
    Serial.print("MQTT接続中...");
    if (mqttClient.connect("ESP32-AutoWater", mqtt_user, mqtt_pass)) {
      Serial.println(" 完了");
      mqttClient.subscribe(TOPIC_CONTROL);
      mqttClient.subscribe(TOPIC_SCHEDULE);  // スケジュールをブローカーから受信
      publishStatus();
    } else {
      Serial.printf(" 失敗 rc=%d、3秒後に再試行\n", mqttClient.state());
      delay(3000);
    }
  }
}

// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  loadSchedules();  // Preferencesからスケジュールを復元
  connectWiFi();
  connectMQTT();
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  // ── タイマーOFF ──
  if (timerActive && pumpOn && millis() >= timerEndMs) {
    Serial.println("タイマー満了 → ポンプOFF");
    pumpTurnOff();
  }

  // ── スケジュールチェック（毎分、秒が0〜9の間のみ） ──
  struct tm t;
  if (getLocalTime(&t) && t.tm_sec < 10) {
    int nowMin = t.tm_hour * 60 + t.tm_min;
    if (nowMin != lastCheckedMin) {
      lastCheckedMin = nowMin;
      for (int i = 0; i < numSchedules; i++) {
        if (schedules[i].enabled         &&
            schedules[i].hour   == t.tm_hour &&
            schedules[i].minute == t.tm_min  &&
            !pumpOn) {
          Serial.printf("スケジュール起動: %02d:%02d %d秒\n",
                        t.tm_hour, t.tm_min, schedules[i].durationSec);
          pumpTurnOn(schedules[i].durationSec);
          break;
        }
      }
    }
  }

  delay(200);
}
