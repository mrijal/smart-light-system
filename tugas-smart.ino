#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

volatile bool btnPressed = false;
volatile bool btnReleased = false;

unsigned long btnPressTime = 0;

#define LONG_PRESS_MS 3000

// ================= PIN =================
#define LED_RED     25
#define LED_GREEN   26
#define BTN         14

// ================= WIFI =================
const char* ssid = "Infinix NOTE 50 Pro";
const char* password = "mrh00man";

// ================= MQTT =================
const char* mqttServer = "9570c9cffa3c47269f3f99fb6c4d1084.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* mqtt_username = "mrhooman";
const char* mqtt_password = "Password123";

WiFiClientSecure espClient;

PubSubClient mqtt(espClient);

// ================= GLOBAL =================
WebServer server(80);
Preferences prefs;


// ================= SYSTEM =================
enum SystemMode {
  MODE_NORMAL,
  MODE_WARNING,
  MODE_EMERGENCY,
  MODE_CRITICAL
};

SystemMode currentMode = MODE_NORMAL;

int emergencyCount = 0;
unsigned long emergencyStart = 0;
unsigned long emergencyDuration = 0;
volatile bool btnHeld = false;
volatile bool longPressTriggered = false;


// ================= PWM =================
#define PWM_FREQ 5000
#define PWM_RES  8

// ================= DECISION TREE =================
struct TreeNode {
  int feature;
  float threshold;
  int left;
  int right;
  bool leaf;
};

TreeNode tree[] = {
  {0, 3, 1, 4, false},       // emergencyCount < 3 ?
  {1, 10000, 3, 2, false},   // duration < 10s ?
  {-1, -1, -1, -1, true},    // node 2 = EMERGENCY
  {-1, -1, -1, -1, true},    // node 3 = WARNING
  {-1, -1, -1, -1, true}     // node 4 = CRITICAL
};

// ================= DECISION =================
SystemMode evaluateTree() {
  // BELUM ADA EMERGENCY SAMA SEKALI
  if (emergencyCount == 0) {
    return MODE_NORMAL;
  }

  int node = 0;
  while (!tree[node].leaf) {
    float val = (tree[node].feature == 0)
      ? emergencyCount
      : emergencyDuration;

    node = (val < tree[node].threshold)
      ? tree[node].left
      : tree[node].right;
  }

  if (node == 2) return MODE_EMERGENCY;
  if (node == 3) return MODE_WARNING;
  if (node == 4) return MODE_CRITICAL;
  return MODE_NORMAL;
}

// ================= ISR =================
void IRAM_ATTR buttonISR() {
  if (digitalRead(BTN) == LOW) {
    btnPressed = true;
  } else {
    btnReleased = true;
  }
}



// ================= MQTT CALLBACK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  deserializeJson(doc, payload, length);

  Serial.print("[MQTT IN] ");
  serializeJson(doc, Serial);
  Serial.println();

  if (doc["cmd"] == "emergency") {
    emergencyCount++;
    emergencyStart = millis();

    prefs.putInt("emg_cnt", emergencyCount);
  }


  if (doc["cmd"] == "reset") {
    emergencyCount = 0;
    emergencyDuration = 0;
    emergencyStart = 0;
    currentMode = MODE_NORMAL;

    prefs.putInt("emg_cnt", 0);
    prefs.putULong("emg_dur", 0);
    prefs.putInt("mode", MODE_NORMAL);
  }

}

// ================= MQTT =================
void connectMQTT() {
  espClient.setInsecure(); // skip certificate validation
  while (!mqtt.connected()) {
    if (mqtt.connect("ESP32_SMART_NODE", mqtt_username, mqtt_password)) {
      mqtt.subscribe("smart/control");
      Serial.println("[MQTT] Connected");
    } else {
      delay(2000);
    }
  }
}

const char* modeToString(SystemMode mode) {
  switch (mode) {
    case MODE_NORMAL:    return "NORMAL";
    case MODE_WARNING:   return "WARNING";
    case MODE_EMERGENCY: return "EMERGENCY";
    case MODE_CRITICAL:  return "CRITICAL";
    default:             return "UNKNOWN";
  }
}

// ================= HTML =================
String htmlPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Smart ESP32</title>
<style>
body{background:#111;color:#fff;font-family:Arial;text-align:center}
.card{background:#222;padding:20px;margin:20px auto;width:300px;border-radius:10px}
button{padding:10px;width:100%;margin-top:10px}
</style>
</head>
<body>
<h2>Smart ESP32 Dashboard by M Rijal</h2>

<div class="card">
<p>Mode: <b id="mode">---</b></p>
<p>Emergency Count: <b id="count">0</b></p>
<p>Duration: <b id="dur">0</b> ms</p>
</div>

<div class="card">
<button onclick="send('emergency')">Trigger Emergency</button>
<button onclick="send('reset')">Reset</button>
</div>

<script>
function load(){
 fetch('/status').then(r=>r.json()).then(d=>{
  mode.innerText=d.mode;
  count.innerText=d.count;
  dur.innerText=d.duration;
 });
}
function send(cmd){
 fetch('/cmd?c='+cmd);
}
setInterval(load,1000);
load();
</script>
</body>
</html>
)rawliteral";
}

// ================= WEB =================
void setupWeb() {
  server.on("/", []() {
    server.send(200, "text/html", htmlPage());
  });

  server.on("/status", []() {
    StaticJsonDocument<256> doc;
    doc["mode"] = modeToString(currentMode);
    doc["count"] = emergencyCount;
    doc["duration"] = emergencyDuration;

    String res;
    serializeJson(doc, res);
    server.send(200, "application/json", res);
  });

  server.on("/cmd", []() {
    String c = server.arg("c");

    if (c == "emergency") {
      triggerEmergency();
      Serial.println("[WEB] Emergency");
    }

    if (c == "reset") {
      resetSystem();
      Serial.println("[WEB] Reset");
    }

    server.send(200, "text/plain", "OK");
  });


  server.begin();
}

void triggerEmergency() {
  emergencyCount++;
  emergencyStart = millis();
}

void resetSystem() {
  emergencyCount = 0;
  emergencyDuration = 0;
  emergencyStart = 0;
  currentMode = MODE_NORMAL;
}

// ================= TASK: BUTTON =================
void buttonTask(void*) {
  while (1) {

    // ===== PRESS =====
    if (btnPressed) {
      btnPressed = false;
      btnHeld = true;
      longPressTriggered = false;
      btnPressTime = millis();
    }

    // ===== HOLD CHECK =====
    if (btnHeld && !longPressTriggered) {
      if (millis() - btnPressTime >= LONG_PRESS_MS) {

        // ===== RESET SAAT MASIH DITEKAN =====
        emergencyCount = 0;
        emergencyDuration = 0;
        emergencyStart = 0;
        currentMode = MODE_NORMAL;

        prefs.putInt("emg_cnt", 0);
        prefs.putULong("emg_dur", 0);
        prefs.putInt("mode", MODE_NORMAL);

        longPressTriggered = true;

        Serial.println("[BTN] Long press (hold) → RESET");
      }
    }

    // ===== RELEASE =====
    if (btnReleased) {
      btnReleased = false;
      btnHeld = false;

      static unsigned long lastAction = 0;
      if (millis() - lastAction < 200) continue;
      lastAction = millis();

      // kalau long press sudah terjadi → JANGAN trigger emergency
      if (longPressTriggered) {
        Serial.println("[BTN] Released after long press");
        continue;
      }

      // ===== SHORT PRESS =====
      emergencyCount++;
      emergencyStart = millis();

      prefs.putInt("emg_cnt", emergencyCount);

      Serial.println("[BTN] Short press → EMERGENCY");
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}



// ================= TASK: AI =================
void aiTask(void*) {
  while (1) {
    if (emergencyStart > 0)
      emergencyDuration = millis() - emergencyStart;
    else
      emergencyDuration = 0;

    SystemMode prev = currentMode;
    currentMode = evaluateTree();

    if (prev != currentMode) {
      Serial.print("[MODE] Changed to ");
      Serial.println(currentMode);

      prefs.putInt("mode", currentMode);
    }

    StaticJsonDocument<256> doc;
    doc["mode"] = modeToString(currentMode);
    doc["count"] = emergencyCount;
    doc["duration"] = emergencyDuration;

    String msg;
    serializeJson(doc, msg);
    mqtt.publish("smart/status", msg.c_str());

    Serial.print("[MQTT OUT] ");
    Serial.println(msg);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ================= TASK: OUTPUT =================
void outputTask(void*) {
  while (1) {
    if (currentMode == MODE_NORMAL) {
      ledcWrite(LED_GREEN, 80);
      ledcWrite(LED_RED, 0);
    }
    if (currentMode == MODE_WARNING) {
      ledcWrite(LED_GREEN, millis() % 1000 < 500 ? 255 : 0);
      ledcWrite(LED_RED, 0);
    }
    if (currentMode == MODE_EMERGENCY) {
      ledcWrite(LED_GREEN, 0);
      ledcWrite(LED_RED, millis() % 1000 < 500 ? 255 : 0);
    }
    if (currentMode == MODE_CRITICAL) {
      ledcWrite(LED_GREEN, 255);
      ledcWrite(LED_RED, 255);
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  prefs.begin("smart", false);

  // restore last data
  emergencyCount    = prefs.getInt("emg_cnt", 0);
  emergencyDuration = prefs.getULong("emg_dur", 0);
  currentMode       = (SystemMode)prefs.getInt("mode", MODE_NORMAL);

  Serial.println("[PREFS] State restored");


  pinMode(BTN, INPUT_PULLUP);
  attachInterrupt(BTN, buttonISR, CHANGE);

  ledcAttach(LED_RED, PWM_FREQ, PWM_RES);
  ledcAttach(LED_GREEN, PWM_FREQ, PWM_RES);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  Serial.print("[IP] ");
  Serial.println(WiFi.localIP());

  mqtt.setServer(mqttServer, mqttPort);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  setupWeb();

  xTaskCreatePinnedToCore(buttonTask, "BTN", 2048, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(aiTask, "AI", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(outputTask, "OUT", 2048, NULL, 1, NULL, 1);
}

// ================= LOOP =================
void loop() {
  server.handleClient();
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
}
