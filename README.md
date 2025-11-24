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

# Build

```bash
./up.sh
```
```bash
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

# Payloads

`ble/scanner/data/46:23:00:00:02:42/debug`
```json
{"mac":"46:23:00:00:02:42","name":"BR","data":"0303A5270308425208FF101D0901F07FFC","voltage":2.9,"temperature_c":9.0,"pressure_psi":35.11}
```
`ble/scanner/data/46:45:00:00:19:22/debug`
```json
{"mac":"46:45:00:00:19:22","name":"BR","data":"0303A5270308425208FF401D0701E24BF2","voltage":2.9,"temperature_c":7.0,"pressure_psi":33.71}
```
`ble/scanner/data/46:23:00:00:02:42/debug`
```json
{"mac":"46:23:00:00:02:42","name":"BR","data":"0303A5270308425208FF101D0801E67FC0","voltage":2.9,"temperature_c":8.0,"pressure_psi":34.11}
```
```json
`ble/scanner/data/46:45:00:00:19:22/debug`
{"mac":"46:45:00:00:19:22","name":"BR","data":"0303A5270308425208FF401D0601DD0779","voltage":2.9,"temperature_c":6.0,"pressure_psi":33.21}
```
