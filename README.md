# TPMS BLE to MQTT

Passive scan BLE advertisements from TPMS sensors ("BR"), parses voltage/temperature/pressure, and publishes advertisements as JSON to MQTT.

# Hardware

ESP32-C3 SuperMini

# Toolchain

ESP-IDF 5.x

# Configuration

Create `main/consts.h`

```c
#pragma once
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-wifi-pass"
#define MQTT_URI "mqtt://broker-hostname-or-ip:1883"
```

## Build

```bash
./up.sh
```
```bash
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
