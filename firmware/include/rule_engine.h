#pragma once
#include <Arduino.h>

#define MAX_RULES 8

enum InputId : uint8_t { IN_BUTTON = 0, IN_TEMP, IN_LEVEL };
enum OpId : uint8_t { OP_EQ = 0, OP_NE, OP_GT, OP_LT, OP_GE, OP_LE };
// Ordered by priority: when several rules fire, the highest wins.
enum ActId : uint8_t { ACT_NONE = 0, ACT_OFF, ACT_BLINK, ACT_ON };

struct Rule {
  // definition
  char id[16];
  char label[72];  // optional human text (the sentence it was compiled from)
  InputId input;
  OpId op;
  float value;
  uint32_t forMs;
  ActId action;
  uint32_t periodMs;
  bool latch;
  bool enabled;
  // runtime
  bool condHeld;
  uint32_t condSince;
  bool firing;
  bool latched;
};

struct Inputs {
  int button;    // 1 pressed, 0 released
  float temp;    // deg C
  float level;   // 0..100 %
};

struct Output {
  ActId action;      // ACT_NONE / ACT_OFF -> LED off
  uint32_t periodMs; // for blink
};

const char* inputName(InputId i);
const char* opName(OpId o);
const char* actName(ActId a);

// Human-readable one-liner for serial logs.
void describeRule(const Rule& r, char* buf, size_t len);

// Parse + validate a JSON rules array (or {"rules":[...]}).
// Returns number of rules (0..MAX_RULES) or -1 with a message in err.
int parseRules(const char* json, size_t len, Rule* out, char* err, size_t errLen);

class RuleEngine {
 public:
  void install(const Rule* rules, int n, uint32_t now);  // replace all, reset runtime
  // Evaluate every rule. Logs FIRED / CLEARED transitions. Returns true if any firing state changed.
  bool tick(const Inputs& in, uint32_t now);
  Output output() const;
  int count() const { return n_; }
  const Rule& rule(int i) const { return rules_[i]; }

 private:
  Rule rules_[MAX_RULES];
  int n_ = 0;
};
