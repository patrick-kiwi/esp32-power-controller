# ESP32 Hot Water Cylinder Controller

An ESP32-based companion controller for solar power diverters that monitors hot water cylinder heating time and applies overnight top-ups when solar heating was insufficient.

## Overview

When paired with a solar power diverter (like the [Arduino Nano Power Diverter](link-to-nano-repo)), this controller solves a common problem: on cloudy days, the diverter may not activate enough to fully heat your hot water cylinder.

This controller:
- Monitors how long the cylinder element was powered each day
- Calculates seasonal heating requirements (more in winter, less in summer)
- Automatically tops up overnight if there's a shortfall
- Provides remote monitoring and control via Blynk

## Features

- **Accurate on-time tracking** — Interrupt-driven pulse timing handles the diverter's rapid 50Hz switching
- **Seasonal adjustment** — Cosine curve automatically varies required heating between summer and winter values
- **Overnight top-up** — Configurable alarm time for off-peak electricity rates
- **Reboot protection** — Skips top-up after power cuts to avoid wasting electricity on already-hot water
- **Remote control via Blynk** — Monitor status, adjust settings, and trigger manual boost from your phone
- **Non-blocking operation** — Blynk stays responsive even during multi-hour boost cycles

## How It Works

### Daily Cycle

```
                    ┌─────────────────────────────────────┐
                    │         WAITING_FOR_ALARM          │
                    │   (counting diverter on-time)      │
                    └───────────────┬─────────────────────┘
                                    │ Alarm time reached
                                    ▼
                    ┌─────────────────────────────────────┐
                    │     Calculate seasonal requirement  │
                    │     Compare with actual on-time     │
                    └───────────────┬─────────────────────┘
                                    │
                    ┌───────────────┴───────────────┐
                    ▼                               ▼
            Shortfall > 0                    No shortfall
                    │                               │
                    ▼                               │
        ┌───────────────────┐                       │
        │ BOOST_IN_PROGRESS │                       │
        │  (heating on)     │                       │
        └─────────┬─────────┘                       │
                  │                                 │
                  └──────────────┬──────────────────┘
                                 ▼
                    ┌─────────────────────────────────────┐
                    │          CYCLE_COMPLETE            │
                    │    (reset counters, wait for       │
                    │         new day)                   │
                    └─────────────────────────────────────┘
```

### Seasonal Calculation

Required heating hours follow a cosine curve peaking at winter solstice (June 21 in NZ):

```
Hours
  │
  │    Winter ─────╮                              ╭─────
  │                 ╲                            ╱
  │                  ╲                          ╱
  │                   ╲                        ╱
  │    Summer ─────────╲──────────────────────╱───────
  │
  └────────────────────────────────────────────────────
       Jun    Sep    Dec    Mar    Jun    Sep
```

### Reboot Protection

If the ESP32 loses power during the day (e.g., brief power cut on a sunny afternoon), it can't know how much heating actually occurred. Without protection, it would see zero on-time and apply a full top-up — wasting electricity on an already-hot cylinder.

The solution: a `hasCompletedDailyCycle` flag that defaults to `false` on boot. The first night after a reboot, top-up is skipped. Once one complete daily cycle finishes, the flag becomes `true` and normal operation resumes.

## Hardware

### Requirements

- ESP32 development board
- Connection to power diverter status signal (shows when load is active)
- Output signal to trigger diverter boost mode

### Pin Assignments

| Pin | Direction | Function |
|-----|-----------|----------|
| GPIO32 | Input | Diverter status (HIGH = load on) |
| GPIO33 | Output | Boost signal to diverter (HIGH = force on) |

### Wiring Notes

- The input signal should be 3.3V logic level (use a voltage divider if coming from 5V Arduino)
- The output has a 1.5kΩ pull-down resistor to ensure clean LOW state

## Software Setup

### Dependencies

Install via Arduino Library Manager:
- [Blynk](https://github.com/blynkkk/blynk-library)

### Configuration

1. Copy `config_template.h` to `config.h`
2. Fill in your credentials:

```cpp
#define BLYNK_TEMPLATE_ID   "your_template_id"
#define BLYNK_TEMPLATE_NAME "ESP32 Power Diverter"
#define BLYNK_AUTH          "your_blynk_auth_token"

#define WIFI_SSID           "your_wifi_ssid"
#define WIFI_PASSWORD       "your_wifi_password"
```

3. Adjust timezone in the main sketch if not in New Zealand:
```cpp
const char* TIMEZONE = "NZST-12NZDT-13,M9.4.0/02:00:00,M4.1.0/03:00:00";
```
   Find your timezone string at: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

4. Adjust default heating requirements if needed:
```cpp
float requiredHoursSummer = 4.0;
float requiredHoursWinter = 5.5;
```

### Blynk Virtual Pins

| Pin | Type | Function |
|-----|------|----------|
| V1 | Input (int) | Alarm hour (0-23) |
| V2 | Input (int) | Alarm minute (0-59) |
| V3 | Input (float) | Summer heating hours required |
| V4 | Input (float) | Winter heating hours required |
| V5 | Output (string) | Uptime display |
| V6 | Input (int) | Manual boost toggle (0/1) |
| V7 | Output (float) | Cumulative on-time today (hours) |
| V8 | Output (string) | Last cycle completion time |
| V10 | Output (int) | Pulse count (switching cycles) |
| V11 | Output (float) | Shortfall (hours) |

## Installation

1. Install the Blynk library
2. Create a new device in the [Blynk Console](https://blynk.cloud)
3. Copy `config_template.h` to `config.h` and add your credentials
4. Upload to ESP32
5. Set up Blynk dashboard widgets for the virtual pins above

## Serial Monitor Output

```
========================================
ESP32 Hot Water Cylinder Controller
========================================

Connecting to WiFi and Blynk...
NTP time obtained
  Timezone set: NZST-12NZDT-13,M9.4.0/02:00:00,M4.1.0/03:00:00
Wednesday, July 16 2025 14:32:15 NZST
Free heap: 285632 bytes

NOTE: Reboot protection active - first daily cycle must complete
      before overnight top-ups will be applied.

Ready.

Day 197, 14:32 | On-time: 0.00h | State: Waiting
Day 197, 14:33 | On-time: 0.12h | State: Waiting
...
```

## Related Projects

- [Arduino Nano Power Diverter](https://github.com/patrick-kiwi/nano-power-diverter) — The solar power diverter that this controller monitors
- [Instructables Guide](https://www.instructables.com/Solar-Power-Diverter/) — Build instructions and hardware details.

## Acknowledgements

- [Blynk](https://blynk.io/) for the IoT platform
- Robin Emley and the OpenEnergyMonitor community for power diverter concepts

## License

MIT License — feel free to use, modify, and share.
