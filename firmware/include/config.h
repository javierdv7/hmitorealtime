#pragma once
// ---------------------------------------------------------------------------
// Edge Rules — device configuration (no secrets here; see secrets.h)
// ---------------------------------------------------------------------------

#ifndef DEVICE_ID
#define DEVICE_ID "edge01"
#endif

// Input: BOOT button (active LOW, internal pull-up)
#ifndef BUTTON_PIN
#define BUTTON_PIN 9
#endif

// Output: alarm LED
//   SuperMini / most C3 boards: plain LED on GPIO8, active LOW
//   ESP32-C3-DevKitM-1 / DevKitC-02: WS2812 RGB on GPIO8 -> set LED_IS_WS2812 1
#ifndef LED_PIN
#define LED_PIN 8
#endif
#ifndef LED_IS_WS2812
#define LED_IS_WS2812 0
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif
#define WS2812_BRIGHTNESS 80   // 0..255, red channel when alarm on

// Timing
#define CONTROL_PERIOD_MS      10     // rule engine tick
#define BRAIN_TIMEOUT_MS       6000   // no heartbeat for this long -> brain offline
#define TELEMETRY_PERIOD_MS    1000
#define STATE_PERIOD_MS        5000   // periodic rules/state refresh
#define TEMP_PERIOD_MS         1000

// Simulated process value ("level", 0..100 %): rises while button held, drains when released
#define LEVEL_RISE_PER_S       20.0f
#define LEVEL_DRAIN_PER_S      10.0f

// Demo helper: when NVS is empty on boot, install this rule (build step 1).
// Set to 0 once the agent is pushing rules.
#ifndef DEMO_DEFAULT_RULE
#define DEMO_DEFAULT_RULE 1
#endif
#define DEMO_DEFAULT_RULE_JSON \
  "[{\"id\":\"demo\",\"input\":\"button\",\"op\":\"==\",\"value\":1,\"for_ms\":1000," \
  "\"action\":{\"type\":\"blink\",\"period_ms\":250},\"latch\":false,\"enabled\":true}]"
