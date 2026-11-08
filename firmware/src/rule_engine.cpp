#include "rule_engine.h"

#include <ArduinoJson.h>
#include <string.h>

extern void logf(const char* fmt, ...);

static const char* IN_NAMES[] = {"button", "temp", "level"};
static const char* OP_NAMES[] = {"==", "!=", ">", "<", ">=", "<="};
static const char* ACT_NAMES[] = {"none", "off", "blink", "on"};

const char* inputName(InputId i) { return IN_NAMES[i]; }
const char* opName(OpId o) { return OP_NAMES[o]; }
const char* actName(ActId a) { return ACT_NAMES[a]; }

void describeRule(const Rule& r, char* buf, size_t len) {
  char act[32];
  if (r.action == ACT_BLINK)
    snprintf(act, sizeof(act), "BLINK %ums", (unsigned)r.periodMs);
  else
    snprintf(act, sizeof(act), "%s", r.action == ACT_ON ? "ON" : "OFF");
  snprintf(buf, len, "%s: WHEN %s %s %g FOR %ums -> %s%s%s", r.id, inputName(r.input), opName(r.op),
           (double)r.value, (unsigned)r.forMs, act, r.latch ? " [LATCH]" : "",
           r.enabled ? "" : " (disabled)");
}

template <size_t N>
static int lookup(const char* s, const char* const (&names)[N]) {
  if (!s) return -1;
  for (size_t i = 0; i < N; i++)
    if (strcmp(s, names[i]) == 0) return (int)i;
  return -1;
}

static bool validId(const char* s) {
  if (!s) return false;
  size_t n = strlen(s);
  if (n == 0 || n > 15) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

#define FAIL(...)                          \
  do {                                     \
    snprintf(err, errLen, __VA_ARGS__);    \
    return -1;                             \
  } while (0)

int parseRules(const char* json, size_t len, Rule* out, char* err, size_t errLen) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json, len);
  if (e) FAIL("invalid JSON: %s", e.c_str());

  JsonArrayConst arr;
  if (doc.is<JsonArrayConst>())
    arr = doc.as<JsonArrayConst>();
  else if (doc["rules"].is<JsonArrayConst>())
    arr = doc["rules"].as<JsonArrayConst>();
  else
    FAIL("expected a JSON array of rules");

  if (arr.size() > MAX_RULES) FAIL("too many rules: %u (max %d)", (unsigned)arr.size(), MAX_RULES);

  int n = 0;
  for (JsonVariantConst v : arr) {
    if (!v.is<JsonObjectConst>()) FAIL("rule[%d]: not an object", n);
    JsonObjectConst o = v.as<JsonObjectConst>();
    Rule r;
    memset(&r, 0, sizeof(r));

    const char* id = o["id"].as<const char*>();
    if (!validId(id)) FAIL("rule[%d]: bad id", n);
    strncpy(r.id, id, sizeof(r.id) - 1);
    for (int k = 0; k < n; k++)
      if (strcmp(out[k].id, r.id) == 0) FAIL("rule %s: duplicate id", r.id);

    const char* label = o["label"] | "";
    strncpy(r.label, label, sizeof(r.label) - 1);

    int in = lookup(o["input"].as<const char*>(), IN_NAMES);
    if (in < 0) FAIL("rule %s: bad input", r.id);
    r.input = (InputId)in;

    int op = lookup(o["op"].as<const char*>(), OP_NAMES);
    if (op < 0) FAIL("rule %s: bad op", r.id);
    r.op = (OpId)op;

    if (!o["value"].is<float>()) FAIL("rule %s: value must be a number", r.id);
    r.value = o["value"].as<float>();
    if (r.value < -1000 || r.value > 1000) FAIL("rule %s: value out of range", r.id);

    if (!o["for_ms"].isNull()) {
      if (!o["for_ms"].is<long>() || o["for_ms"].as<long>() < 0 || o["for_ms"].as<long>() > 3600000)
        FAIL("rule %s: for_ms must be an integer 0..3600000", r.id);
      r.forMs = o["for_ms"].as<uint32_t>();
    }

    JsonObjectConst a = o["action"].as<JsonObjectConst>();
    if (a.isNull()) FAIL("rule %s: missing action", r.id);
    const char* t = a["type"].as<const char*>();
    if (!t) FAIL("rule %s: missing action.type", r.id);
    if (strcmp(t, "on") == 0)
      r.action = ACT_ON;
    else if (strcmp(t, "off") == 0)
      r.action = ACT_OFF;
    else if (strcmp(t, "blink") == 0)
      r.action = ACT_BLINK;
    else
      FAIL("rule %s: bad action.type", r.id);
    if (!a["period_ms"].isNull()) {
      if (!a["period_ms"].is<long>() || a["period_ms"].as<long>() < 0 || a["period_ms"].as<long>() > 10000)
        FAIL("rule %s: period_ms must be an integer 0..10000", r.id);
      r.periodMs = a["period_ms"].as<uint32_t>();
    }
    if (r.action == ACT_BLINK && r.periodMs < 50) FAIL("rule %s: blink needs period_ms >= 50", r.id);

    if (!o["latch"].isNull() && !o["latch"].is<bool>()) FAIL("rule %s: latch must be boolean", r.id);
    r.latch = o["latch"] | false;
    if (!o["enabled"].isNull() && !o["enabled"].is<bool>()) FAIL("rule %s: enabled must be boolean", r.id);
    r.enabled = o["enabled"] | true;

    out[n++] = r;
  }
  return n;
}

void RuleEngine::install(const Rule* rules, int n, uint32_t now) {
  for (int i = 0; i < n_; i++)
    if (rules_[i].firing) logf("RULE CLEARED %s (rule set replaced)", rules_[i].id);
  n_ = n;
  char buf[128];
  for (int i = 0; i < n; i++) {
    rules_[i] = rules[i];
    rules_[i].condHeld = false;
    rules_[i].condSince = now;
    rules_[i].firing = false;
    rules_[i].latched = false;
    describeRule(rules_[i], buf, sizeof(buf));
    logf("RULE LOADED  %s", buf);
  }
  if (n == 0) logf("RULES: none active");
}

static bool compare(float x, OpId op, float v) {
  switch (op) {
    case OP_EQ: return fabsf(x - v) < 1e-4f;
    case OP_NE: return fabsf(x - v) >= 1e-4f;
    case OP_GT: return x > v;
    case OP_LT: return x < v;
    case OP_GE: return x >= v;
    case OP_LE: return x <= v;
  }
  return false;
}

bool RuleEngine::tick(const Inputs& in, uint32_t now) {
  bool changed = false;
  for (int i = 0; i < n_; i++) {
    Rule& r = rules_[i];
    if (!r.enabled) continue;
    float x = r.input == IN_BUTTON ? (float)in.button : r.input == IN_TEMP ? in.temp : in.level;
    bool cond = compare(x, r.op, r.value);
    if (cond && !r.condHeld) {
      r.condHeld = true;
      r.condSince = now;
    } else if (!cond) {
      r.condHeld = false;
    }
    bool active = r.condHeld && (now - r.condSince >= r.forMs);
    if (active && r.latch) r.latched = true;
    bool firing = active || r.latched;
    if (firing != r.firing) {
      r.firing = firing;
      changed = true;
      if (firing)
        logf("RULE FIRED   %s (%s=%g) -> %s%s", r.id, inputName(r.input), (double)x, actName(r.action),
             r.latch ? " [LATCHED until cleared]" : "");
      else
        logf("RULE CLEARED %s (%s=%g)", r.id, inputName(r.input), (double)x);
    }
  }
  return changed;
}

Output RuleEngine::output() const {
  Output o{ACT_NONE, 0};
  for (int i = 0; i < n_; i++) {
    const Rule& r = rules_[i];
    if (!r.enabled || !r.firing) continue;
    if (r.action > o.action) {
      o.action = r.action;
      o.periodMs = r.periodMs;
    } else if (r.action == ACT_BLINK && o.action == ACT_BLINK && r.periodMs < o.periodMs) {
      o.periodMs = r.periodMs;  // fastest blink wins
    }
  }
  return o;
}
