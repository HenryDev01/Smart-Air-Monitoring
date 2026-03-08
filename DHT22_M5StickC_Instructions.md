
# Quick Start: DHT22 on M5StickC with ESP-IDF
## Attribution

DHT22 driver logic and explanation adapted from  
[https://esp32tutorials.com/dht22-esp32-esp-idf/](https://esp32tutorials.com/dht22-esp32-esp-idf/)  
(licensed under CC0/public domain).
---


# Smart Air Monitoring — Project Documentation

## Project Overview

ESP32-based mesh air quality monitoring system using 5x M5StickC Plus devices.  
Sensors: DHT22 (temperature + humidity), MQ-2 (smoke).  
Networking: WiFi Mesh (ESP-MESH) + BLE Mesh (ESP-BLE-MESH) running simultaneously.

---

## Hardware

| Device | Quantity | Role |
|---|---|---|
| M5StickC Plus (ESP32) | 5 | Mesh nodes (sensor + routing) |
| nRF52840 USB Dongle | 1 | BLE mesh sniffer / debugger (Wireshark) |
| DHT22 sensor | per node | Temperature + Humidity on GPIO26 |
| MQ-2 sensor | per node | Smoke detection (ADC, TBD) |

### DHT22 Wiring
| DHT22 Pin | M5StickC Pin |
|---|---|
| VCC | 3.3V |
| DATA | G26 |
| GND | GND |

---

## Setup Instructions

### 1. Install ESP-IDF Extension
1. In VS Code, go to Extensions (`Ctrl+Shift+X`)
2. Search for **ESP-IDF** and install the official Espressif extension
3. Open `ESP-IDF: Installation Manager` from Command Palette (`Ctrl+Shift+P`)
4. Install all required tools and Python dependencies
5. Restart VS Code

### 2. Clone and Configure Secrets
```bash
git clone <repo>
cd Smart-Air-Monitoring
cp components/configuration/secrets.h.example components/configuration/secrets.h
# Edit secrets.h with your WiFi credentials
```

### 3. Build and Flash
```bash
# Build
idf.py build

# Flash and monitor (replace COM10 with your port)
idf.py -p COM10 -b 115200 flash monitor

# Exit monitor
Ctrl + ]

# Clean build
idf.py fullclean && idf.py build

# Open menuconfig
idf.py menuconfig
```

---

## Project Structure

```
SmartAirMonitoring/
├── main/
│   ├── main.c                      ✅ updated
│   └── CMakeLists.txt
├── components/
│   ├── configuration/
│   │   ├── air_mesh.h              ✅ all constants, packet structs, BLE mesh config
│   │   ├── secrets.h               ❌ gitignored — WiFi credentials (create from example)
│   │   └── secrets.h.example       ✅ committed — template for teammates
│   ├── mesh/
│   │   ├── CMakeLists.txt          ✅ updated
│   │   ├── initialization/
│   │   │   ├── mesh_init.c         ✅ WiFi mesh init + coexistence + BLE mesh trigger
│   │   │   └── mesh_init.h
│   │   ├── routing/
│   │   │   ├── mesh_routing.c      ✅ ETX routing, neighbor table, HELLO broadcast
│   │   │   └── mesh_routing.h
│   │   └── ble_mesh/               ✅ NEW
│   │       ├── ble_mesh_init.c     ✅ BLE mesh implementation
│   │       └── ble_mesh_init.h     ✅ BLE mesh public API
│   ├── sensor/
│   │   ├── sensor.c                ✅ DHT22 reads, WiFi + BLE mesh send, alerts
│   │   ├── sensor.h
│   │   ├── CMakeLists.txt          ✅ updated
│   │   └── dht22/
│   │       ├── DHT22.c             ✅ DHT22 driver (GPIO26)
│   │       ├── DHT22.h
│   │       └── CMakeLists.txt
│   ├── display/
│   │   ├── display.c               ✅ ST7789 SPI driver, heap_caps framebuffer
│   │   ├── display.h
│   │   └── CMakeLists.txt
│   └── wifi/
│       ├── wifi_service.c          ⚠️ redundant — kept for reference only
│       ├── wifi_service.h
│       └── CMakeLists.txt
├── partitions.csv                  ✅ custom partition table (3MB app)
├── sdkconfig.defaults              ✅ BT + coex + flash size config
├── .gitignore                      ✅ excludes secrets.h, build/, sdkconfig
└── CMakeLists.txt
```
## Setup secrets.h
1. Create components/configuration/secrets.h
2. Fill in your WiFi credentials 
#define MESH_ROUTER_SSID      "YOUR_WIFI_SSID"
#define MESH_ROUTER_PASSWORD  "YOUR_WIFI_PASSWORD"
#define MESH_PASSWORD         "YOUR_MESH_PASSWORD"
3. Run idf.py build
---

## Component Status

| Component | Status | Notes |
|---|---|---|
| WiFi Mesh (ESP-MESH) | ✅ Working | Self-elected root, auto channel, tested |
| ETX Routing | ✅ Working | Neighbor table, HELLO broadcast, parent switching |
| DHT22 Sensor | ✅ Working | GPIO26, stable readings confirmed on device |
| Display (ST7789) | ✅ Working | SPI, heap_caps framebuffer, shows readings |
| BLE Mesh stack | ⚠️ Partial | Code written, `ESP_ERR_INVALID_STATE` — timing fix needed |
| WiFi/BLE Coexistence | ✅ Enabled | `esp_coex_preference_set(ESP_COEX_PREFER_BALANCE)` |
| MQ-2 Smoke Sensor | ⚠️ Stub | Returns random values, real ADC not implemented |
| MQTT / Dashboard | ❌ Not started | Next milestone after BLE mesh fix |

---

## Architecture

### Network Layers
```
All 5 M5StickC nodes run BOTH simultaneously:

BLE Mesh layer (ESP-BLE-MESH)        WiFi Mesh layer (ESP-MESH)
─────────────────────────────        ──────────────────────────
Sensor data (local broadcast)        Sensor data → root → cloud
Emergency alerts / gossip            ETX routing / parent selection
Node discovery / HELLO packets       Root → WiFi router → internet
```

### Root Node Election
- Self-elected via ESP-MESH RSSI-based election
- Node with best RSSI to WiFi router becomes root automatically
- If root goes down, mesh re-elects a new root
- Enabled via `esp_mesh_set_self_organized(true, true)`

### BLE Mesh Roles
- Root node → **Provisioner** (assigns addresses to other nodes)
- All other nodes → **Node** (advertise for provisioning)

### nRF52840 Dongle Role
The dongle is **not** a mesh node. It is a **BLE sniffer** only:
1. Flash with nRF Sniffer firmware via `nrfutil`
2. Install Wireshark + nRF Sniffer plugin
3. Plug in — passively captures all BLE mesh traffic
4. No VSCode nRF Connect extension needed

### sensor.c Task Loop
```
Every 10 seconds:
├── Check mesh active
├── Read DHT22 (temp + humidity)
├── Build pkt_sensor_t
├── Send → root via WiFi mesh
├── Send → neighbors via BLE mesh
├── Temp > 35°C → BLE alert
├── Smoke > 70% → BLE alert
├── Log parent info + record ETX tx
└── Send HELLO over BLE mesh
```

---

## Packet Types (air_mesh.h)

| Type | Value | Description |
|---|---|---|
| PKT_SENSOR_DATA | 0x01 | Temperature, humidity, smoke, ETX |
| PKT_HELLO | 0x02 | Neighbor discovery, ETX broadcast |
| PKT_GOSSIP | 0x03 | Flood alerts, config updates |
| PKT_JOIN_REQUEST | 0x10 | New node authentication |
| PKT_JOIN_RESPONSE | 0x11 | Root accept/reject |
| PKT_CHALLENGE | 0x12 | Periodic re-auth challenge |
| PKT_CHALLENGE_RESP | 0x13 | HMAC-SHA256 response |
| PKT_SESSION_REVOKE | 0x14 | Force re-authentication |

---

## Key Configuration Constants (air_mesh.h)

| Constant | Value | Description |
|---|---|---|
| MESH_MAX_LAYERS | 6 | Max mesh depth |
| MESH_AP_MAX_CONN | 6 | Max children per node |
| HELLO_INTERVAL_MS | 10000 | 10s between HELLO broadcasts |
| NEIGHBOR_TIMEOUT_MS | 35000 | 3.5x hello = stale neighbor |
| SENSOR_INTERVAL_MS | 30000 | 30s between sensor reads |
| ETX_SWITCH_HYSTERESIS | 0.85 | Only switch parent if 15% better |
| GOSSIP_TTL_DEFAULT | 6 | Max gossip hops |
| SESSION_TTL_SEC | 3600 | 1 hour session timeout |
| BLE_MESH_GROUP_ADDR | 0xC000 | BLE mesh multicast group |
| AIR_MESH_TTL_DEFAULT | 7 | BLE mesh TTL |

---

## ESP-IDF / menuconfig Settings

### BLE Mesh memory optimisations (idf.py menuconfig)
```
Number of advertising buffers:        60 → 20
Replay protection list size:          10 → 6
Network message cache size:           10 → 6
Max outgoing message segments:        32 → 4
Max incoming Upper Transport PDU:    384 → 128
Disable BLE Mesh debug logs:         enabled
```
Component Config → Bluetooth
  → Host: Bluedroid - Dual-mode
  → Bluedroid Options
    → ESP BLE Mesh Support
      → Support for BLE Mesh Provisioner   [*]
      → Support for BLE Mesh Node          [*]
      → BLE Mesh GATT Proxy Support        [*]
      → BLE Mesh PB-GATT Support           [*]

Component Config → ESP COEX
  → Software controls WiFi/BT coexistence  [*]
---

## Partition Table (partitions.csv)
```
# Name,   Type, SubType, Offset,   Size,
nvs,      data, nvs,     0x9000,   0x5000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x300000,
storage,  data, spiffs,  0x310000, 0xF0000,
```

---

## Key Technical Decisions

**1. wifi_init_sta() removed**
`mesh_init()` handles all WiFi internally. `wifi_init_sta()` permanently commented out. `wifi` component kept for reference only.

**2. BLE mesh init triggered by mesh event**
`ble_mesh_init()` called inside `MESH_EVENT_PARENT_CONNECTED` — ensures `esp_mesh_is_root()` returns correct value before role assignment.

**3. Framebuffer uses heap_caps_malloc**
```c
// Correct embedded/IoT practice — DMA-capable heap
s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t),
                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
```

**4. Custom partition table**
Binary is 1.4MB — default 1MB partition too small. Custom table gives 3MB app partition.

---

## Troubleshooting

| Issue | Cause | Fix |
|---|---|---|
| DHT22 Sensor Timeout | Bad wiring on DATA pin | Check G26 connection |
| DHT22 CheckSum error | Unstable power or long wires | Use shorter wires, stable 3.3V |
| BLE mesh `ESP_ERR_INVALID_STATE` | BT stack not ready at init time | Timing fix pending — needs investigation |
| DRAM overflow | Large static framebuffer | Fixed — using `heap_caps_malloc` |
| App partition too small | Default partition only 1MB | Fixed — custom `partitions.csv` |

---

## Remaining Roadmap

| Week | Tasks | Status |
|---|---|---|
| Week 1 | Build passes, flash, DHT22 test, WiFi mesh test | ✅ Done |
| Week 1 | BLE mesh basic messaging working | ⚠️ Partial — timing fix needed |
| Week 2 | Fix BLE mesh timing, test multi-node (need other M5StickC devices) | ⏳ Pending |
| Week 2 | Wire + implement MQ2 ADC reading | ⏳ Pending |
| Week 3 | MQTT integration | ⏳ Pending |
| Week 4 | Full system test, demo prep, documentation | ⏳ Pending |

---

## Notes
- `DHT22_TEST_MODE 1` in `sensor.c` disables mesh and tests sensor only — set to `0` for full mesh mode
- WiFi mesh channel set to `0` (auto-detect router channel)
- BLE and WiFi share the same 2.4GHz radio — slight performance degradation expected when coexistence is active
- All nodes run identical firmware — root election is automatic
- Binary size: ~1.4MB | Flash: 4MB | DRAM: fixed via heap allocation