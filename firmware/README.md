# Waveshare ESP32-S3-Relay-6CH firmware

Firmware for the Waveshare ESP32-S3-Relay-6CH board, built on **ESP-IDF v6.1**.

- Web UI and JSON API for the 6 relays, with per-relay names
- Wi-Fi setup through a temporary access point; reconnects on its own after router outages
- Configurable **hostname**, used for DHCP, **mDNS** (`http://<hostname>.local/`) and the MQTT client ID
- **NTP** time sync with a configurable server and timezone
- **MQTT**: JSON and per-relay topics, retained state, last-will availability, optional TLS, and **Home Assistant discovery**
- **OTA updates** from the web UI or `curl`, with automatic rollback if the new image doesn't come up
- Power-on behaviour: restore last state, all off or all on
- Optional admin password (HTTP Basic auth)
- Factory reset from the web UI, or by holding **BOOT** for 5 seconds
- Crash dumps saved to flash (`idf.py coredump-info`)

## Building

Install ESP-IDF v6.1 ([guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/)), then:

```bash
. ~/esp/esp-idf-v6.1/export.sh
idf.py set-target esp32s3     # first time only; creates sdkconfig from sdkconfig.defaults
idf.py build
```

The IDF component manager downloads `cjson`, `mqtt` and `mdns` on the first build. All three were part of ESP-IDF before v6.0. Their versions are pinned in `dependencies.lock`.

Board options (relay GPIOs, polarity, default hostname, setup AP name and password, NTP and timezone defaults, reset button) are under **Relay Box** in `idf.py menuconfig`.

## Flashing

The first flash has to go over USB, because the partition table changed from v1.x:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Saved Wi-Fi and MQTT settings from v1.x are kept: the NVS partition is unchanged, and the old MQTT settings are migrated automatically.

After that, update over the network with the web UI (**System → Firmware update**) or with:

```bash
curl --data-binary @build/waveshare-relay-firmware.bin http://waveshare-relay-xxxxxx.local/api/ota
```

Add `-u admin:<password>` if you set an admin password. A new image has to run for 30 s with Wi-Fi connected (or 5 minutes without Wi-Fi) before it is confirmed. If it crashes or resets before then, the bootloader goes back to the previous image.

## First-time setup

1. With no saved network, the board starts an access point called `WaveshareRelay-XXXX` (password `waveshare`).
2. Join it and open `http://192.168.4.1/`.
3. Go to **Network**, pick your network and click **Connect**.
4. The device then shows up at `http://waveshare-relay-xxxxxx.local/`, where `xxxxxx` is the end of its MAC address. The setup access point turns itself off about two minutes after the device connects.

If the device loses Wi-Fi for 30 seconds, the setup access point comes back while it keeps trying to reconnect.

## HTTP API

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/status` | relays, Wi-Fi, MQTT, time, system |
| GET | `/api/config` | settings (passwords are not returned) |
| POST | `/api/config` | any subset of the config fields, applied immediately |
| POST | `/api/relays` | `{"0": true, "3": "toggle"}` |
| GET | `/api/wifi/scan` | nearby networks |
| POST | `/api/wifi` | `{"ssid": "...", "password": "..."}` |
| POST | `/api/ota` | raw `.bin` as the request body |
| POST | `/api/reboot` | |
| POST | `/api/factory-reset` | erases all settings |

`GET /status` and `POST /relay` from v1.x still work.

Example:

```bash
curl -X POST -d '{"hostname":"garage-relays","timezone":"CET-1CEST,M3.5.0,M10.5.0/3"}' http://waveshare-relay-xxxxxx.local/api/config
```

## MQTT

Relays are numbered 0-5. `<root>` defaults to `waveshare/relay`.

| Topic | Direction | Payload |
|---|---|---|
| `<root>/set` | → device | `{"0": true, "1": false, "2": "toggle"}` |
| `<root>/relay/<n>/set` | → device | `ON`, `OFF` or `TOGGLE` |
| `<root>/status` | ← retained | `{"0": true, ...}` |
| `<root>/relay/<n>/state` | ← retained | `ON` / `OFF` |
| `<root>/availability` | ← retained | `online` / `offline` (last will) |
| `<root>/info` | ← retained | hostname, IP, firmware version |

The broker field takes a plain host (with the port field), or a full URI such as `mqtts://broker.example.com:8883`. TLS connections are checked against the built-in CA bundle. With Home Assistant discovery on, each relay appears as a switch under one device.

`examples/mqtt_test.py` is a small interactive test client.

## Layout

```
main/
  app_main.c      boot sequence, OTA confirmation, reset button
  settings.c      NVS-backed settings and v1.x migration
  relays.c        GPIO control, power-on state
  wifi_mgr.c      station + setup AP, reconnect with backoff, scanning
  net_services.c  mDNS, SNTP, timezone
  mqtt_link.c     MQTT client, topics, Home Assistant discovery
  ota.c           OTA upload, image checks, rollback confirmation
  web_server.c    HTTP server, JSON API, auth
  Kconfig.projbuild
web/index.html    web UI (embedded in the firmware image)
partitions.csv    nvs, otadata, 2 × 1.875 MB app slots, coredump (4 MB flash)
```

## Relay polarity

Relays default to **active-low** (GPIO low = ON), as in v1.x, and start ON when no state has been saved yet. If your board's relays come out inverted, turn off `Relay Box → Relays → Relays are active-low` in menuconfig.
