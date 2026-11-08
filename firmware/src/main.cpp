// Edge Rules — ESP32-C3 firmware
//
// Two FreeRTOS tasks:
//   control task (prio 5, every 10 ms): debounce inputs, run rule engine, drive alarm LED,
//                                       track brain heartbeat. Never touches the network.
//   loop()       (prio 1, Arduino):     WiFi + MQTT with non-blocking backoff, rules ingestion,
//                                       NVS persistence, telemetry. May block on TCP — that's fine,
//                                       the control task preempts it.
// Rules are loaded from NVS before any network is touched, so the device protects the
// process from the first millisecond after boot, with or without the brain.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdarg.h>

#include "config.h"
#include "rule_engine.h"
#include "web_page.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h missing -> running OFFLINE (copy secrets.example.h to secrets.h)"
#define WIFI_SSID ""
#define WIFI_PASS ""
#define MQTT_HOST ""
#define MQTT_PORT 1883
#endif

// ----------------------------------------------------------------------------- topics
static const char* T_RULES_SET = "edge/" DEVICE_ID "/rules/set";
static const char* T_RULES_STATE = "edge/" DEVICE_ID "/rules/state";
static const char* T_TELEMETRY = "edge/" DEVICE_ID "/telemetry";
static const char* T_STATUS = "edge/" DEVICE_ID "/status";
static const char* T_HEARTBEAT = "brain/heartbeat";
static const char* T_CMD = "edge/" DEVICE_ID "/cmd";
static const char* T_LOG = "edge/" DEVICE_ID "/log";

// ----------------------------------------------------------------------------- logging
// Every log line goes to USB serial AND into a queue that the network task publishes on
// edge/<id>/log. Lines logged while offline stay queued (newest dropped when full) and are
// delivered when the broker comes back. Never blocks the caller.
struct LogLine {
  char s[176];
};
static QueueHandle_t gLogQ = nullptr;
// Ring of recent lines for the device's own web page.
#define LOG_RING 120
static LogLine gRing[LOG_RING];
static uint32_t gRingSeq = 0;  // total lines ever logged
static portMUX_TYPE gRingMux = portMUX_INITIALIZER_UNLOCKED;

void logf(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  uint32_t ms = millis();
  LogLine l;
  snprintf(l.s, sizeof(l.s), "[%6lu.%03lu] %s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), buf);
  if (Serial.availableForWrite() > (int)strlen(l.s) + 4) Serial.println(l.s);
  if (gLogQ) {
    if (xQueueSend(gLogQ, &l, 0) != pdTRUE) {
      LogLine drop;
      xQueueReceive(gLogQ, &drop, 0);
      xQueueSend(gLogQ, &l, 0);
    }
  }
  portENTER_CRITICAL(&gRingMux);
  gRing[gRingSeq % LOG_RING] = l;
  gRingSeq++;
  portEXIT_CRITICAL(&gRingMux);
}

// ----------------------------------------------------------------------------- shared state
// Everything crossing tasks goes through `gLock`. The control task only ever *tries* the
// lock (timeout 0) so it can never be blocked by the network side.
static SemaphoreHandle_t gLock;

struct RuleView {
  char id[16];
  bool enabled, firing, latched;
};
struct Snapshot {
  int n;
  RuleView r[MAX_RULES];
  int button;
  float level;
  bool led;
  bool ledOut;       // instantaneous output (blink phase included)
  uint8_t act;       // ActId winning
  uint32_t periodMs; // blink period
  int brain;  // -1 unknown, 0 offline, 1 online
  uint32_t maxExecUs;
  uint32_t maxJitterUs;
  uint32_t changeSeq;  // bumps whenever any rule's firing state changes
};
static Snapshot gSnap;

static Rule gPending[MAX_RULES];  // network -> control
static int gPendingN = 0;
static bool gPendingReady = false;

static volatile uint32_t gLastHeartbeatMs = 0;
static volatile float gTemp = NAN;

// Network-side copy of the installed definitions (for rules/state).
static Rule gDefs[MAX_RULES];
static int gDefsN = 0;
static char gLastError[96] = "";
static const char* gRulesSource = "none";

static RuleEngine engine;
static Preferences prefs;

// ----------------------------------------------------------------------------- LED
// LED type: 0 = GPIO active-low, 1 = GPIO active-high, 2 = WS2812 (RGB). Runtime-configurable
// over MQTT (edge/<id>/cmd) and persisted in NVS, so a board with a different LED needs no reflash.
enum { LED_GPIO_LOW = 0, LED_GPIO_HIGH = 1, LED_WS2812 = 2 };
static const char* LED_TYPE_NAMES[] = {"gpio_low", "gpio_high", "ws2812"};
static int ledPin = LED_PIN;
static int ledType = LED_IS_WS2812 ? LED_WS2812 : (LED_ACTIVE_LOW ? LED_GPIO_LOW : LED_GPIO_HIGH);
static volatile int pendLedPin = -1, pendLedType = -1;  // network -> control
static volatile uint32_t ledTestUntil = 0;              // force LED on (identification)
static bool ledState = false;

static void ledRaw(bool on) {
  if (ledType == LED_WS2812)
    neopixelWrite(ledPin, on ? WS2812_BRIGHTNESS : 0, 0, 0);
  else
    digitalWrite(ledPin, (on ^ (ledType == LED_GPIO_LOW)) ? HIGH : LOW);
}
static void ledSetup() {
  if (ledType != LED_WS2812) pinMode(ledPin, OUTPUT);
  ledRaw(false);
  ledState = false;
}
static void ledWrite(bool on) {
  if (on == ledState) return;
  ledState = on;
  ledRaw(on);
}
static void ledApplyPending() {  // control task only
  if (pendLedPin < 0) return;
  ledRaw(false);
  if (ledType != LED_WS2812) pinMode(ledPin, INPUT);
  ledPin = pendLedPin;
  ledType = pendLedType;
  pendLedPin = -1;
  ledSetup();
  logf("LED CONFIG  GPIO%d %s", ledPin, LED_TYPE_NAMES[ledType]);
}

// ----------------------------------------------------------------------------- control task
static void controlTask(void*) {
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t lastUs = micros();
  uint32_t lastTickMs = millis();
  int rawPrev = 0, stableCount = 0, button = 0;
  float level = 0;
  int brain = -1;  // unknown until first heartbeat or timeout
  uint32_t maxExec = 0, maxJitter = 0, changeSeq = 0;
  uint32_t windowStart = millis();
  uint32_t pubMaxExec = 0, pubMaxJitter = 0;

  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    uint32_t t0 = micros();
    uint32_t jitter = (uint32_t)abs((int32_t)(t0 - lastUs) - CONTROL_PERIOD_MS * 1000);
    lastUs = t0;
    uint32_t now = millis();
    float dt = (now - lastTickMs) / 1000.0f;
    lastTickMs = now;

    // 1. pick up a new rule set from the network task (never block)
    if (gPendingReady && xSemaphoreTake(gLock, 0) == pdTRUE) {
      engine.install(gPending, gPendingN, now);
      gPendingReady = false;
      changeSeq++;
      xSemaphoreGive(gLock);
    }

    // 2. inputs: debounced button (3 stable samples = 30 ms), simulated level
    int raw = digitalRead(BUTTON_PIN) == LOW ? 1 : 0;
    stableCount = (raw == rawPrev) ? stableCount + 1 : 0;
    rawPrev = raw;
    if (stableCount >= 3) button = raw;
    level += (button ? LEVEL_RISE_PER_S : -LEVEL_DRAIN_PER_S) * dt;
    level = constrain(level, 0.0f, 100.0f);
    Inputs in{button, gTemp, level};

    // 3. rules
    if (engine.tick(in, now)) changeSeq++;

    // 4. output
    Output o = engine.output();
    bool on = false;
    if (o.action == ACT_ON)
      on = true;
    else if (o.action == ACT_BLINK)
      on = (now % o.periodMs) < (o.periodMs / 2);
    ledApplyPending();
    if ((int32_t)(ledTestUntil - now) > 0) on = ((now / 250) % 2) == 0;  // identification blink
    ledWrite(on);

    // 5. brain heartbeat watchdog
    uint32_t hb = gLastHeartbeatMs;
    bool alive = hb != 0 && (now - hb) < BRAIN_TIMEOUT_MS;
    if (alive && brain != 1) {
      brain = 1;
      logf("BRAIN ONLINE — accepting rule updates");
    } else if (!alive && brain != 0 && (brain == 1 || now > BRAIN_TIMEOUT_MS)) {
      brain = 0;
      logf("BRAIN OFFLINE — running autonomously with %d rules", engine.count());
    }

    // 6. health + snapshot for the network task
    uint32_t exec = micros() - t0;
    if (exec > maxExec) maxExec = exec;
    if (jitter > maxJitter) maxJitter = jitter;
    if (now - windowStart >= 1000) {
      pubMaxExec = maxExec;
      pubMaxJitter = maxJitter;
      maxExec = maxJitter = 0;
      windowStart = now;
    }
    if (xSemaphoreTake(gLock, 0) == pdTRUE) {
      gSnap.n = engine.count();
      for (int i = 0; i < gSnap.n; i++) {
        const Rule& r = engine.rule(i);
        memcpy(gSnap.r[i].id, r.id, sizeof(r.id));
        gSnap.r[i].enabled = r.enabled;
        gSnap.r[i].firing = r.firing;
        gSnap.r[i].latched = r.latched;
      }
      gSnap.button = button;
      gSnap.level = level;
      gSnap.led = o.action == ACT_ON || o.action == ACT_BLINK;
      gSnap.ledOut = on;
      gSnap.act = o.action;
      gSnap.periodMs = o.periodMs;
      gSnap.brain = brain;
      gSnap.maxExecUs = pubMaxExec;
      gSnap.maxJitterUs = pubMaxJitter;
      gSnap.changeSeq = changeSeq;
      xSemaphoreGive(gLock);
    }
  }
}

// ----------------------------------------------------------------------------- rules in
// Validate, then hand to the control task and persist. Returns false (and sets gLastError).
static bool acceptRules(const char* json, size_t len, const char* source, bool persist) {
  static Rule parsed[MAX_RULES];
  char err[96];
  int n = parseRules(json, len, parsed, err, sizeof(err));
  if (n < 0) {
    snprintf(gLastError, sizeof(gLastError), "%s", err);
    logf("RULES REJECTED (%s): %s — keeping %d current rules", source, err, gDefsN);
    return false;
  }
  gLastError[0] = 0;
  xSemaphoreTake(gLock, portMAX_DELAY);
  memcpy(gPending, parsed, sizeof(Rule) * n);
  gPendingN = n;
  gPendingReady = true;
  xSemaphoreGive(gLock);
  xSemaphoreTake(gLock, portMAX_DELAY);
  memcpy(gDefs, parsed, sizeof(Rule) * n);
  gDefsN = n;
  gRulesSource = source;
  xSemaphoreGive(gLock);
  logf("RULES ACCEPTED from %s: %d rule(s)", source, n);
  if (persist) {
    prefs.putString("rules", json);  // json is NUL-terminated by callers
    logf("RULES SAVED to NVS (%u bytes)", (unsigned)len);
  }
  return true;
}

// ----------------------------------------------------------------------------- MQTT
static WiFiClient net;
static PubSubClient mqtt(net);
static const bool netEnabled = strlen(WIFI_SSID) > 0;
static bool forceStatePublish = false;
static volatile bool gMqttUp = false;

static Snapshot takeSnapshot() {
  Snapshot s;
  xSemaphoreTake(gLock, portMAX_DELAY);
  s = gSnap;
  xSemaphoreGive(gLock);
  return s;
}

static void publishState() {
  if (!mqtt.connected()) return;
  Snapshot s = takeSnapshot();
  JsonDocument doc;
  doc["device"] = DEVICE_ID;
  doc["source"] = gRulesSource;
  doc["count"] = gDefsN;
  doc["max"] = MAX_RULES;
  if (gLastError[0]) doc["error"] = gLastError;
  JsonArray arr = doc["rules"].to<JsonArray>();
  for (int i = 0; i < gDefsN; i++) {
    const Rule& d = gDefs[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = d.id;
    o["input"] = inputName(d.input);
    o["op"] = opName(d.op);
    o["value"] = d.value;
    o["for_ms"] = d.forMs;
    JsonObject a = o["action"].to<JsonObject>();
    a["type"] = actName(d.action);
    a["period_ms"] = d.periodMs;
    o["latch"] = d.latch;
    o["enabled"] = d.enabled;
    if (d.label[0]) o["label"] = d.label;
    bool firing = false, latched = false;
    for (int k = 0; k < s.n; k++)
      if (strcmp(s.r[k].id, d.id) == 0) {
        firing = s.r[k].firing;
        latched = s.r[k].latched;
      }
    o["firing"] = firing;
    o["latched"] = latched;
  }
  JsonArray f = doc["firing"].to<JsonArray>();
  for (int k = 0; k < s.n; k++)
    if (s.r[k].firing) f.add(s.r[k].id);
  char out[3200];
  size_t len = serializeJson(doc, out, sizeof(out));
  mqtt.publish(T_RULES_STATE, (const uint8_t*)out, len, true);
}

static void publishTelemetry() {
  if (!mqtt.connected()) return;
  Snapshot s = takeSnapshot();
  JsonDocument doc;
  doc["button"] = s.button;
  float t = gTemp;
  if (isnan(t))
    doc["temp"] = nullptr;
  else
    doc["temp"] = roundf(t * 10) / 10;
  doc["level"] = roundf(s.level * 10) / 10;
  doc["led"] = s.led;
  char lc[24];
  snprintf(lc, sizeof(lc), "%s@%d", LED_TYPE_NAMES[ledType], ledPin);
  doc["led_cfg"] = lc;
  doc["uptime_ms"] = millis();
  doc["brain_online"] = s.brain == 1;
  doc["rules"] = s.n;
  JsonArray f = doc["firing"].to<JsonArray>();
  for (int k = 0; k < s.n; k++)
    if (s.r[k].firing) f.add(s.r[k].id);
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["loop_max_us"] = s.maxExecUs;
  doc["jitter_max_us"] = s.maxJitterUs;
  char out[400];
  size_t len = serializeJson(doc, out, sizeof(out));
  mqtt.publish(T_TELEMETRY, (const uint8_t*)out, len, false);
}

static void handleCmd(const byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* cmd = doc["cmd"] | "";
  if (strcmp(cmd, "led") == 0) {
    int pin = doc["pin"] | ledPin;
    const char* t = doc["type"] | LED_TYPE_NAMES[ledType];
    int type = -1;
    for (int i = 0; i < 3; i++)
      if (strcmp(t, LED_TYPE_NAMES[i]) == 0) type = i;
    // never touch the button, flash (11-17) or USB (18-19) pins
    if (type < 0 || pin < 0 || pin > 21 || pin == BUTTON_PIN || (pin >= 11 && pin <= 19)) {
      logf("LED CONFIG rejected (pin=%d type=%s)", pin, t);
      return;
    }
    pendLedType = type;
    pendLedPin = pin;
    if (doc["save"] | false) {
      prefs.putInt("led_pin", pin);
      prefs.putInt("led_type", type);
      logf("LED CONFIG saved to NVS");
    }
    uint32_t ms = doc["test_ms"] | 3000;
    ledTestUntil = millis() + ms;
  } else if (strcmp(cmd, "led_test") == 0) {
    ledTestUntil = millis() + (uint32_t)(doc["ms"] | 3000);
    logf("LED TEST blinking for %lums", (unsigned long)(doc["ms"] | 3000));
  } else if (strcmp(cmd, "reset") == 0) {
    prefs.remove("rules");
    acceptRules("[]", 2, "mqtt", false);
    xQueueReset(gLogQ);
    portENTER_CRITICAL(&gRingMux);
    gRingSeq = 0;
    portEXIT_CRITICAL(&gRingMux);
    forceStatePublish = true;
  }
}

static void onMessage(char* topic, byte* payload, unsigned int len) {
  if (strcmp(topic, T_CMD) == 0) {
    handleCmd(payload, len);
    return;
  }
  if (strcmp(topic, T_HEARTBEAT) == 0) {
    uint32_t m = millis();
    gLastHeartbeatMs = m ? m : 1;
    return;
  }
  if (strcmp(topic, T_RULES_SET) == 0) {
    if (len == 0) return;  // retained message being deleted — not a rule set
    static char buf[3073];
    if (len >= sizeof(buf)) {
      snprintf(gLastError, sizeof(gLastError), "payload too large (%u bytes)", len);
      logf("RULES REJECTED: %s", gLastError);
      forceStatePublish = true;
      return;
    }
    memcpy(buf, payload, len);
    buf[len] = 0;
    // Retained message re-delivered on every reconnect: ignore if identical to what we run.
    if (prefs.getString("rules", "") == buf && strcmp(gRulesSource, "none") != 0) {
      logf("RULES unchanged — keeping %d rule(s)", gDefsN);
      gRulesSource = "mqtt";
      gLastError[0] = 0;
    } else {
      acceptRules(buf, len, "mqtt", true);
    }
    forceStatePublish = true;
  }
}

// Non-blocking reconnect state machine with exponential backoff.
static uint32_t wifiNextTry = 0, wifiBackoff = 2000;
static uint32_t mqttNextTry = 0, mqttBackoff = 1000;
static bool wifiWasUp = false;

static void networkStep(uint32_t now) {
  if (!netEnabled) return;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wifiWasUp) {
    wifiWasUp = up;
    if (up) {
      logf("WIFI UP   ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      wifiBackoff = 2000;
      mqttNextTry = now;
    } else {
      logf("WIFI DOWN — rules keep running locally");
      wifiNextTry = now + wifiBackoff;
    }
  }
  if (!up) {
    if ((int32_t)(now - wifiNextTry) >= 0) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);  // returns immediately
      wifiNextTry = now + wifiBackoff;
      wifiBackoff = min<uint32_t>(wifiBackoff * 2, 30000);
    }
    return;
  }

  if (!mqtt.connected()) {
    if ((int32_t)(now - mqttNextTry) < 0) return;
    char cid[32];
    snprintf(cid, sizeof(cid), "%s-%06lx", DEVICE_ID, (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
    // May block up to the socket timeout — only this low-priority task waits.
    if (mqtt.connect(cid, nullptr, nullptr, T_STATUS, 1, true, "offline")) {
      logf("MQTT CONNECTED to %s:%d", MQTT_HOST, MQTT_PORT);
      mqttBackoff = 1000;
      mqtt.publish(T_STATUS, "online", true);
      mqtt.subscribe(T_RULES_SET, 1);
      mqtt.subscribe(T_HEARTBEAT, 0);
      mqtt.subscribe(T_CMD, 0);
      forceStatePublish = true;
    } else {
      logf("MQTT connect failed (rc=%d), retry in %lus", mqtt.state(), (unsigned long)(mqttBackoff / 1000));
      mqttNextTry = now + mqttBackoff;
      mqttBackoff = min<uint32_t>(mqttBackoff * 2, 2000);
    }
    return;
  }
  mqtt.loop();
}

// ----------------------------------------------------------------------------- serial console
static void serialConsole() {
  if (!Serial.available()) return;
  int c = Serial.read();
  if (c == 'r') {
    char buf[128];
    logf("--- %d rule(s), source=%s ---", gDefsN, gRulesSource);
    for (int i = 0; i < gDefsN; i++) {
      describeRule(gDefs[i], buf, sizeof(buf));
      logf("  %s", buf);
    }
  } else if (c == 'X') {
    prefs.remove("rules");
    acceptRules("[]", 2, "serial", false);
    logf("NVS rules erased");
  } else if (c == 'h' || c == '?') {
    logf("console: r=list rules, X=erase all rules + NVS");
  }
}

// ----------------------------------------------------------------------------- local web page
// The MCU serves its own page: what it sees (inputs), what it decides (rules, output) and its
// log. Runs in its own task (prio 2) so it stays responsive while loop() is stuck in an MQTT
// reconnect, and it does not depend on the CM4 at all.
static WebServer web(80);

static void handleState() {
  Snapshot s = takeSnapshot();
  JsonDocument doc;
  doc["device"] = DEVICE_ID;
  doc["uptime_ms"] = millis();
  doc["button"] = s.button;
  doc["level"] = roundf(s.level * 10) / 10;
  float t = gTemp;
  if (!isnan(t)) doc["temp"] = roundf(t * 10) / 10;
  doc["alarm"] = s.led;
  doc["out"] = s.ledOut;
  doc["act"] = actName((ActId)s.act);
  doc["period_ms"] = s.periodMs;
  doc["brain"] = s.brain;
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["mqtt"] = (bool)gMqttUp;
  doc["rssi"] = WiFi.RSSI();
  doc["loop_max_us"] = s.maxExecUs;
  doc["jitter_max_us"] = s.maxJitterUs;
  doc["heap"] = ESP.getFreeHeap();
  JsonArray arr = doc["rules"].to<JsonArray>();
  xSemaphoreTake(gLock, portMAX_DELAY);
  doc["source"] = gRulesSource;
  if (gLastError[0]) doc["error"] = gLastError;
  for (int i = 0; i < gDefsN; i++) {
    const Rule& d = gDefs[i];
    JsonObject o = arr.add<JsonObject>();
    char buf[128];
    describeRule(d, buf, sizeof(buf));
    o["id"] = d.id;
    o["desc"] = strchr(buf, ':') ? strchr(buf, ':') + 2 : buf;
    if (d.label[0]) o["label"] = d.label;
    for (int k = 0; k < s.n; k++)
      if (strcmp(s.r[k].id, d.id) == 0) {
        o["firing"] = s.r[k].firing;
        o["latched"] = s.r[k].latched;
      }
  }
  xSemaphoreGive(gLock);
  // log lines newer than ?since=
  uint32_t since = web.hasArg("since") ? strtoul(web.arg("since").c_str(), nullptr, 10) : 0;
  JsonArray lg = doc["log"].to<JsonArray>();
  static LogLine copy[LOG_RING];
  uint32_t seq, first;
  portENTER_CRITICAL(&gRingMux);
  seq = gRingSeq;
  first = seq > LOG_RING ? seq - LOG_RING : 0;
  if (since > first && since <= seq) first = since;
  for (uint32_t i = first; i < seq; i++) copy[i - first] = gRing[i % LOG_RING];
  portEXIT_CRITICAL(&gRingMux);
  for (uint32_t i = first; i < seq; i++) lg.add(copy[i - first].s);
  doc["log_seq"] = seq;
  String out;
  serializeJson(doc, out);
  web.sendHeader("cache-control", "no-store");
  web.send(200, "application/json", out);
}

static void webTask(void*) {
  while (WiFi.status() != WL_CONNECTED) delay(200);
  web.on("/", []() { web.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  web.on("/state", handleState);
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
  if (MDNS.begin(DEVICE_ID)) MDNS.addService("http", "tcp", 80);
  logf("WEB  http://%s/  (http://%s.local/)", WiFi.localIP().toString().c_str(), DEVICE_ID);
  for (;;) {
    web.handleClient();
    delay(2);
  }
}

// ----------------------------------------------------------------------------- setup / loop
void setup() {
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(4096);  // absorb log bursts...
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);      // ...and never stall the control loop if no host is listening
#else
  Serial.begin(115200);
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  gLogQ = xQueueCreate(120, sizeof(LogLine));

  // LED config from NVS (falls back to config.h)
  prefs.begin("edge", false);
  ledPin = prefs.getInt("led_pin", ledPin);
  ledType = prefs.getInt("led_type", ledType);
  ledSetup();

  gLock = xSemaphoreCreateMutex();
  memset(&gSnap, 0, sizeof(gSnap));
  gSnap.brain = -1;

  // 1. Rules from NVS FIRST — before any network.
  String saved = prefs.getString("rules", "");
  bool loaded = false;
  if (saved.length()) {
    logf("NVS: found %u bytes of rules", saved.length());
    loaded = acceptRules(saved.c_str(), saved.length(), "nvs", false);
  }
#if DEMO_DEFAULT_RULE
  if (!loaded && !saved.length()) {
    logf("NVS empty — installing built-in demo rule");
    acceptRules(DEMO_DEFAULT_RULE_JSON, strlen(DEMO_DEFAULT_RULE_JSON), "builtin", false);
  }
#endif
  // Install synchronously so protection starts before the control task even runs.
  engine.install(gPending, gPendingN, millis());
  gPendingReady = false;

  // 2. Control loop.
  xTaskCreate(controlTask, "control", 6144, nullptr, 5, nullptr);
  uint32_t protectingSince = millis();

  // USB-CDC needs a moment to re-enumerate after reset; the control loop is already
  // running, so waiting here costs nothing. Then replay the boot summary for the log.
  while (millis() < 2500) delay(10);
  logf("==================================================");
  logf("EDGE RULES  device=%s  led=GPIO%d %s  button=GPIO%d", DEVICE_ID, ledPin, LED_TYPE_NAMES[ledType],
       BUTTON_PIN);
  logf("==================================================");
  logf("BOOT: %d rule(s) from %s installed at t=%lums — BEFORE any network", gDefsN, gRulesSource,
       (unsigned long)protectingSince);
  char buf[128];
  for (int i = 0; i < gDefsN; i++) {
    describeRule(gDefs[i], buf, sizeof(buf));
    logf("RULE LOADED  %s", buf);
  }
  if (gLastError[0]) logf("NVS rules invalid: %s", gLastError);
  logf("CONTROL LOOP running every %d ms — device is protecting the process", CONTROL_PERIOD_MS);

  // 3. Network (non-blocking).
  if (netEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiNextTry = millis() + 10000;
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);
    mqtt.setBufferSize(3400);
    mqtt.setKeepAlive(5);
    mqtt.setSocketTimeout(2);
    logf("WIFI connecting to '%s' (broker %s:%d)", WIFI_SSID, MQTT_HOST, MQTT_PORT);
    xTaskCreate(webTask, "web", 8192, nullptr, 2, nullptr);
  } else {
    logf("NETWORK DISABLED (no secrets.h) — standalone mode");
  }
}

void loop() {
  static uint32_t lastTelemetry = 0, lastState = 0, lastTemp = 0, lastSeq = 0;
  uint32_t now = millis();

  if (now - lastTemp >= TEMP_PERIOD_MS) {
    lastTemp = now;
    gTemp = temperatureRead();
  }

  networkStep(now);
  serialConsole();

  gMqttUp = mqtt.connected();
  if (gMqttUp) {
    uint32_t hb = gLastHeartbeatMs;
    if (hb != 0 && (now - hb) < BRAIN_TIMEOUT_MS) {
      LogLine l;
      for (int i = 0; i < 24 && xQueuePeek(gLogQ, &l, 0) == pdTRUE; i++) {
        if (!mqtt.publish(T_LOG, l.s)) break;
        xQueueReceive(gLogQ, &l, 0);
      }
    }
    uint32_t seq = takeSnapshot().changeSeq;
    if (forceStatePublish || seq != lastSeq || now - lastState >= STATE_PERIOD_MS) {
      forceStatePublish = false;
      lastSeq = seq;
      lastState = now;
      publishState();
    }
    if (now - lastTelemetry >= TELEMETRY_PERIOD_MS) {
      lastTelemetry = now;
      publishTelemetry();
    }
  }
  delay(5);
}
