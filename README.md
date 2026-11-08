# Edge Rules

An AI agent on a Raspberry Pi CM4 compiles plain-language rules into small JSON rules. It pushes them over MQTT to a $3 ESP32-C3, and the ESP32-C3 enforces them locally in real time. The CM4 also serves the HMI. When you unplug the CM4, the HMI goes dark, but the MCU keeps running its rules and still triggers the alarm.

```
 you ── plain English ──► agent (CM4) ──► LLM ──► JSON rule ──► schema check
                              │                                     │
                              │  edge/edge01/rules/set (retained)   ▼
                              └──────────── Mosquitto (CM4) ◄───────┘
                                                 │  ▲ telemetry / rules/state / LWT
                       brain/heartbeat (2 s) ──► │  │
                                                 ▼  │
                                ESP32-C3: NVS ─► rule engine (10 ms task) ─► alarm LED
                                          BOOT button · chip temp · simulated tank level
 HMI (CM4 :8090) ◄── SSE ── hmi_server.py ◄── MQTT
```

## Repo layout

| Path | What |
|---|---|
| `schema/rule.schema.json` | Rule format, the single source of truth. The agent validates against it and the LLM gets it in its prompt. |
| `firmware/` | PlatformIO project for the ESP32-C3 (Arduino, PubSubClient, ArduinoJson) |
| `agent/edge_agent.py` | Optional CLI agent: LLM compile, validate, push, heartbeat, status |
| `hmi/` | Web HMI (stdlib HTTP + SSE + paho-mqtt). It loads nothing from the internet. |
| `tools/monitor.py` | Serial monitor that auto-reconnects (for the power-cycle shot) |
| `deploy/` | Mosquitto LAN config, systemd unit for the HMI, `deploy_lab.sh` |

## Design notes (why it survives the CM4 dying)

- **Two FreeRTOS tasks on the C3.** The rule engine runs in a priority-5 task every 10 ms (`vTaskDelayUntil`). WiFi and MQTT run in Arduino `loop()` at priority 1. A blocking TCP connect to a dead broker therefore stalls only the network task, never the control loop. The control task only ever *tries* the shared mutex (timeout 0).
- **NVS first.** At boot the device loads its rules from NVS and installs them before it starts WiFi. It protects the process from the first millisecond after boot, with no network.
- **Validated twice.** The agent validates against the JSON Schema and retries the LLM once with the error. The device validates again and rejects the whole set if any rule is bad. It keeps its current rules and reports the error on `rules/state`.
- **Heartbeat watchdog.** If no `brain/heartbeat` arrives for 6 s, the device sets `brain_online=false` and logs `BRAIN OFFLINE — running autonomously with N rules`.
- **Health telemetry.** `loop_max_us` (worst control-tick execution time) and `jitter_max_us` (worst tick-period deviation) are published every second, so the HMI shows that real-time control is unaffected.

### Rule format

```json
{"id":"r1","input":"button","op":"==","value":1,"for_ms":3000,
 "action":{"type":"on","period_ms":0},"latch":true,"enabled":true}
```

| Field | Meaning |
|---|---|
| `input` | `button` (1 = pressed, 0 = released), `temp` (chip °C), or `level` |
| `level` | Simulated tank level, 0–100 %, computed on the device. It rises 20 %/s while the button is held and drains 10 %/s when the button is released. This is the "simulated sensor" for the video, and it keeps working without the CM4. |
| `for_ms` | How long the condition must hold continuously before the rule fires |
| `latch` | When true, the action stays on until the rule is cleared or the rule set is replaced |
| Output priority | When several rules fire at once: `on` > `blink` (the fastest period wins) > `off` |

The device holds at most 8 rules.

### MQTT topics (`DEVICE_ID` defaults to `edge01`)

| Topic | Direction | Notes |
|---|---|---|
| `edge/edge01/rules/set` | CM4 → device | Retained. The full array, which replaces all rules. |
| `edge/edge01/rules/state` | device → CM4 | Retained. Rule definitions, plus `firing`/`latched`, `source` (`nvs`/`mqtt`/`builtin`) and `error`. |
| `edge/edge01/telemetry` | device → CM4 | Every 1 s: `button, temp, level, led, uptime_ms, brain_online, firing, rssi, heap, loop_max_us, jitter_max_us` |
| `edge/edge01/status` | device LWT | `online` / `offline`, retained |
| `brain/heartbeat` | CM4 → device | Every 2 s |

---

## Setup

### 0. CM4 ("lab", 192.168.1.27): broker, HMI, and agent

Mosquitto listens on `0.0.0.0:1883` with anonymous access. That is fine for a LAN demo, but don't expose it outside the LAN. Code lives on the external disk at `/mnt/external/edge-rules`.

```bash
./deploy/deploy_lab.sh          # from your Mac: rsync + venv + mosquitto + edge-hmi service
```

- HMI: <http://192.168.1.27:8090>
- Agent: `ssh -t lab 'cd /mnt/external/edge-rules && .venv/bin/python agent/edge_agent.py'`

**Configuring the LLM.** In the HMI, click **⚙ LLM** in the header. Choose the provider (Anthropic or Ollama), then enter the API key and model. **Save & test** does a round-trip to the LLM.

**Controlling rules from the HMI.** Use the **Control rules** box to describe a rule in plain language and select **Compile & send**. The HMI compiles and validates it, publishes the retained rule set, and shows device acknowledgement and firing state. Use **Remove** beside a rule or **Clear all** to replace the device's rule set. The CLI agent is optional; the HMI sends its own heartbeat while connected to the broker.

- The settings are stored on the CM4 in `config/llm.json` with mode 600, and that file is git-ignored.
- The key is never sent back to the browser.
- The agent re-reads the file on every compile, so changes apply without a restart.
- Environment variables (`ANTHROPIC_API_KEY`, `LLM_PROVIDER`, `ANTHROPIC_MODEL`, `OLLAMA_HOST`, `OLLAMA_MODEL`) are used only as fallbacks.
- The HMI has no login. Anyone on the LAN can change the key. POSTs must be JSON and from the same origin.

### 1. Firmware (from the Mac, board on USB-C)

```bash
uv tool install platformio --with intelhex      # once (intelhex is needed by the pinned esptool 4.8.1)
cd firmware
cp include/secrets.example.h include/secrets.h   # set WIFI_SSID / WIFI_PASS; MQTT_HOST=192.168.1.27
pio run -t upload
python3 ../tools/monitor.py                      # serial monitor that reconnects after reset/power-cycle (pip install pyserial)
```

- esptool is pinned to 4.8.1 in `platformio.ini`, and `upload_speed` is set to 115200. The bundled esptool 4.5 fails on macOS with native USB-JTAG at the baud-rate change, with the error `No serial data received`.

- If you see no serial output, switch environments. `c3_usbcdc` (the default) is for boards whose USB-C goes straight to the C3, such as the SuperMini. `c3_uart` is for boards with a CH340 or CP2102 USB-UART chip, such as the DevKitM-1. Use `pio run -e c3_uart -t upload`.
- If upload fails, hold BOOT, tap RESET, release BOOT, and retry.
- LED settings live in `include/config.h`:
  - SuperMini: plain LED on GPIO8, active LOW. This is the default.
  - DevKitM-1: WS2812 on GPIO8. Set `LED_IS_WS2812 1`.
- Serial console: `r` lists rules, `X` erases all rules from NVS, `h` shows help.

---

## Build order and what you should see

### Step 1: Rule engine plus a hardcoded rule (no network needed)

Flash **without** `include/secrets.h`. The device runs offline, and the built-in demo rule loads because NVS is empty. Rules are active at about 90 ms. The boot summary is replayed at 2.5 s so that the USB monitor has time to reattach.

```
[     0.093] RULE LOADED  demo: WHEN button == 1 FOR 1000ms -> BLINK 250ms
[     2.503] EDGE RULES  device=edge01  led=GPIO8  button=GPIO9
[     2.504] BOOT: 1 rule(s) from builtin installed at t=94ms — BEFORE any network
[     2.504] RULE LOADED  demo: WHEN button == 1 FOR 1000ms -> BLINK 250ms
[     2.504] CONTROL LOOP running every 10 ms — device is protecting the process
[     2.504] NETWORK DISABLED (no secrets.h) — standalone mode
[     6.003] BRAIN OFFLINE — running autonomously with 1 rules
```

Hold BOOT for more than 1 s. The LED blinks and the log shows `RULE FIRED   demo (button=1) -> blink`. Release it, and the log shows `RULE CLEARED demo (button=0)`.

### Step 2: NVS plus MQTT ingestion

Create `secrets.h` and reflash. You should see `WIFI UP ip=…` and then `MQTT CONNECTED to 192.168.1.27:1883`. From any machine:

```bash
mosquitto_sub -h 192.168.1.27 -t 'edge/#' -v      # telemetry every 1s, status online
mosquitto_pub -h 192.168.1.27 -r -t edge/edge01/rules/set \
  -m '[{"id":"t1","input":"button","op":"==","value":1,"for_ms":0,"action":{"type":"on","period_ms":0},"latch":false,"enabled":true}]'
```

The serial log shows `RULES ACCEPTED from mqtt`, `RULES SAVED to NVS`, and `RULE LOADED t1`. Pressing BOOT turns the LED on. Press RESET: the log shows `NVS: found … bytes` and `t1` loads before WiFi.

To check rejection, push garbage such as `-m '[{"id":"x"}]'`. The log shows `RULES REJECTED … keeping 1 current rules`, and `rules/state` carries `"error"`.

### Step 3: Create a rule from the HMI (or CLI)

In the HMI's **Control rules** box, enter a rule such as:

```
if the button is held for 3 seconds, turn on the alarm and latch it
```

Select **Compile & send** and check that the rule appears in the list and the device reports it. To use the optional CLI instead, start the agent on the CM4. Without an LLM, push a hand-written rule:

```
/raw {"id":"r1","input":"button","op":"==","value":1,"for_ms":0,"action":{"type":"blink","period_ms":100},"latch":false,"enabled":true}
```

The CLI shows `✔ device acknowledged — enforcing 1 rule(s) locally`. In the HMI, check the rule's status and events. Configure the LLM in **⚙ LLM** to submit plain-language rules from the HMI.

### Step 4: Heartbeat, telemetry, and status

The agent publishes a heartbeat. The device logs `BRAIN ONLINE`. `/status` shows `brain_online: True` and the device's own view of its rules. `/watch` shows a live status line. Quit the agent and about 6 s later the device logs `BRAIN OFFLINE — running autonomously with N rules`.

### Step 5: HMI

Open <http://192.168.1.27:8090>. The HMI fits the PLC test view in one screen: rule input, rules stored on the device, a 60-second tank-level graph, alarm history, and PLC logs.

---

## Demo script (video)

Setup: the ESP32-C3 is powered from the Mac or a USB charger, with a serial monitor visible. On screen: the agent terminal (ssh to lab), the HMI in a browser, and the board.

1. **HMI → rule → alarm**

   ```
   HMI Control rules › if the button is held for 3 seconds, turn on the alarm and latch it
   ```

   The rule compiles, is pushed, and is acknowledged. Hold BOOT for 3 s: the LED comes on and stays on, and the HMI lamp turns red with the rule shown as LATCHED.

2. **Live update**

   ```
   HMI Control rules › Clear all
   HMI Control rules › blink the alarm every 200ms while the button is pressed
   ```

   Press BOOT and the LED blinks. Release it and the blinking stops.

3. **Simulated sensor**

   ```
   you › if the tank level goes above 80% for 1 second, turn the alarm on and keep it on
   ```

   Hold BOOT. The level climbs on the HMI gauge past the 80 % marker, and the alarm fires and latches.

4. **Unplug the CM4**

   1. Select **Clear all**, then send the tank rule again (step 3) so it starts un-latched.
   2. Pull the CM4 power.
   3. The HMI shows **HMI LINK LOST**.
   4. About 6 s later, the serial log shows `BRAIN OFFLINE — running autonomously with 1 rules`.
   5. Hold BOOT. The level rises, the log shows `RULE FIRED`, and the LED alarm comes on. The CM4 is not involved.

5. **Power-cycle the ESP32-C3 with the CM4 still off**

   1. Unplug and replug the board. The log shows `NVS: found …`, then `RULE LOADED …`, then `CONTROL LOOP running`.
   2. `WIFI UP` appears, followed by repeated `MQTT connect failed … retry`, while control continues.
   3. Hold BOOT. The alarm fires again.

6. (Optional) Power the CM4 back on. The device logs `MQTT CONNECTED` and `BRAIN ONLINE`, and the HMI recovers.

## Troubleshooting

| Problem | Fix |
|---|---|
| The LED is inverted | Set `LED_ACTIVE_LOW` in `config.h`. |
| The LED does nothing on a DevKitM-1 | Set `LED_IS_WS2812 1`. |
| `MQTT connect failed rc=-2` | The broker can't be reached. Check `MQTT_HOST` and `ss -ltn \| grep 1883` on the CM4. |
| The agent shows "no ack from device" | The device is offline. The rule set is retained and loads when the device reconnects. |
| A stale rule set is on the broker | `mosquitto_pub -h 192.168.1.27 -r -n -t edge/edge01/rules/set` |
