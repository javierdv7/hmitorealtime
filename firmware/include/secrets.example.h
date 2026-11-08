#pragma once
// Copy to secrets.h and fill in. secrets.h is git-ignored.
// Leave WIFI_SSID empty ("") to run fully offline (rule engine + NVS only).
#define WIFI_SSID   "your-ssid"
#define WIFI_PASS   "your-password"
#define MQTT_HOST   "192.168.1.27"   // LAB (CM4) — use the IP, avoids DNS/mDNS stalls
#define MQTT_PORT   1883
