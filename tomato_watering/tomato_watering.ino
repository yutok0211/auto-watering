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
const char* TOPIC_CONTROL  = "tomato/pump/control";
const char* TOPIC_STATUS   = "tomato/pump/status";
const char* TOPIC_SCHEDULE = "tomato/pump/schedule";
const char* TOPIC_EVENTLOG = "tomato/pump/eventlog";  // 全イベント履歴（retain）

const int RELAY_PIN = 26;

WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);
Preferences      prefs;

// ── ポンプ状態 ───────────────────────────────
bool          pumpOn      = false;
bool          timerActive = false;
unsigned long timerEndMs  = 0;

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

// ── イベントログ（直近8件）────────────────────
// フォーマット: "ON:schedule:1722553200|OFF:timer:1722553230|..."
#define MAX_LOG 8
struct LogEntry { bool isOn; char reason[12]; long ts; };
LogEntry evtLog[MAX_LOG];
int      evtLogSize = 0;

void appendEventLog(bool isOn, const char* reason, long ts) {
  if (evtLogSize < MAX_LOG) {
    evtLog[evtLogSize] = { isOn, {0}, ts };
    strncpy(evtLog[evtLogSize].reason, reason, 11);
    evtLogSize++;
  } else {
    for (int i = 0; i < MAX_LOG - 1; i++) evtLog[i] = evtLog[i + 1];
    evtLog[MAX_LOG - 1] = { isOn, {0}, ts };
    strncpy(evtLog[MAX_LOG - 1].reason, reason, 11);
  }
}

void publishEventLog() {
  char buf[400] = "";
  for (int i = 0; i < evtLogSize; i++) {
    char entry[40];
    snprintf(entry, sizeof(entry), "%s%s:%s:%ld",
      i > 0 ? "|" : "",
      evtLog[i].isOn ? "ON" : "OFF",
      evtLog[i].reason,
      evtLog[i].ts);
    strncat(buf, entry, sizeof(buf) - strlen(buf) - 1);
  }
  mqttClient.publish(TOPIC_EVENTLOG, buf, true);
}

// ─────────────────────────────────────────────
// ポンプ制御
// ─────────────────────────────────────────────
void publishStatus(const char* reason = "manual") {
  struct tm t;
  long epoch = 0;
  if (getLocalTime(&t)) epoch = (long)mktime(&t);

  appendEventLog(pumpOn, reason, epoch);
  publishEventLog();

  char buf[80];
  snprintf(buf, sizeof(buf), "%s:%s:%ld", pumpOn ? "ON" : "OFF", reason, epoch);
  mqttClient.publish(TOPIC_STATUS, buf, true);
}

void pumpTurnOn(int durationSec, const char* reason = "manual") {
  pumpOn = true;
  digitalWrite(RELAY_PIN, HIGH);
  if (durationSec > 0) {
    timerEndMs  = millis() + (unsigned long)durationSec * 1000UL;
    timerActive = true;
    Serial.printf("ポンプON（%d秒タイマー reason=%s）\n", durationSec, reason);
  } else {
    timerActive = false;
    timerEndMs  = 0;
    Serial.printf("ポンプON（手動 reason=%s）\n", reason);
  }
  publishStatus(reason);
}

void pumpTurnOff(const char* reason = "manual") {
  if (!pumpOn) return;
  pumpOn      = false;
  timerActive = false;
  timerEndMs  = 0;
  digitalWrite(RELAY_PIN, LOW);
  publishStatus(reason);
  Serial.printf("ポンプOFF reason=%s\n", reason);
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
// スケジュール文字列パース "HH:MM:DURSEC:EN|..."
// ─────────────────────────────────────────────
void parseScheduleStr(const String& str) {
  numSchedules = 0;
  if (str.length() == 0) { saveSchedules(); return; }

  int start = 0;
  while (start < (int)str.length() && numSchedules < MAX_SCHEDULES) {
    int end = str.indexOf('|', start);
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
  mqttClient.setBufferSize(600);

  while (!mqttClient.connected()) {
    Serial.print("MQTT接続中...");
    if (mqttClient.connect("ESP32-AutoWater", mqtt_user, mqtt_pass)) {
      Serial.println(" 完了");
      mqttClient.subscribe(TOPIC_CONTROL);
      mqttClient.subscribe(TOPIC_SCHEDULE);
      publishStatus();  // 現在状態を送信（eventlogも更新）
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

  loadSchedules();
  connectWiFi();
  connectMQTT();
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  // ── タイマーOFF ──
  if (timerActive && pumpOn && millis() >= timerEndMs) {
    Serial.println("タイマー満了 → ポンプOFF");
    pumpTurnOff("timer");
  }

  // ── スケジュールチェック（毎分、秒が0〜9の間のみ） ──
  struct tm t;
  if (getLocalTime(&t) && t.tm_sec < 10) {
    int nowMin = t.tm_hour * 60 + t.tm_min;
    if (nowMin != lastCheckedMin) {
      lastCheckedMin = nowMin;
      for (int i = 0; i < numSchedules; i++) {
        if (schedules[i].enabled          &&
            schedules[i].hour   == t.tm_hour &&
            schedules[i].minute == t.tm_min  &&
            !pumpOn) {
          Serial.printf("スケジュール起動: %02d:%02d %d秒\n",
                        t.tm_hour, t.tm_min, schedules[i].durationSec);
          pumpTurnOn(schedules[i].durationSec, "schedule");
          break;
        }
      }
    }
  }

  delay(200);
}
