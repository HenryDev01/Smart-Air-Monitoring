# AirMesh Dashboard

Real-time air quality monitoring dashboard for the AirMesh dual-protocol IoT mesh network (CSC2106).

## Technology

**Stack:**
- **Backend**: Python 3.10+ with FastAPI + uvicorn (async server)
- **MQTT Bridge**: `paho-mqtt` subscribes to broker → pushes over WebSocket to browser
- **Frontend**: Single-file HTML/CSS/JS (Chart.js for graphs, vanilla JS for topology)
- **Simulation**: Built-in async data generator when no hardware is connected

## Folder Structure

```
dashboard/              ← completely isolated from ESP-IDF firmware
├── README.md
├── requirements.txt
├── main.py             ← FastAPI app, WebSocket hub, MQTT bridge
├── simulator.py        ← fake sensor data generator (no hardware mode)
├── mqtt_client.py      ← REAL hardware data receiver (swap in when hardware ready)
├── models.py           ← shared data models (Pydantic)
└── static/
    └── index.html      ← full dashboard UI (HTML + CSS + JS, single file)
```

This folder sits at the repo root and has **zero interaction** with the ESP-IDF CMake build system. The `.gitignore` already excludes Python caches.

## Data Displayed (from equipment list)

| Sensor | Data | Unit | Source |
|---|---|---|---|
| Sensirion SCD40/41 | CO₂ concentration | ppm | I2C on M5Stack |
| Sensirion SCD40/41 | Temperature | °C | I2C on M5Stack |
| Sensirion SCD40/41 | Relative Humidity | % RH | I2C on M5Stack |
| MQ-2 Gas Sensor | Smoke / Gas level | raw ADC / % | ADC on M5Stack |
| Mesh routing | ETX to root | dimensionless | Computed in firmware |
| Mesh routing | Hop count | integer | esp_mesh_get_layer() |
| Mesh routing | Node role | ROOT / MESH / BLE-RELAY | Firmware |
| BLE link | BLE RSSI | dBm | NRF52840 → M5Stack |
| System | Packet delivery ratio | % | Sequence number tracking |
| System | Last seen timestamp | ISO8601 | Server-side |

## Alert Thresholds

| Condition | Threshold | Action |
|---|---|---|
| CO₂ High | > 1000 ppm | Yellow warning |
| CO₂ Critical | > 2000 ppm | Red alert |
| Temperature High | > 35 °C | Yellow warning |
| Gas Detected | > 60% | Red alert |
| Node Offline | No data > 60 s | Grey node in topology |

## Running the Dashboard

### 1. Install dependencies

```bash
cd dashboard
py -m venv .venv 
.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

### 2. Run in SIMULATION mode (no hardware)

```bash
python main.py --simulate
```

Open browser at: http://localhost:8000

### 3. Run with REAL hardware (MQTT broker)

Ensure Mosquitto is running on the same LAN as the root ESP32 node.
The root node publishes to: `airmesh/nodes/<MAC>/sensors`

```bash
python main.py --mqtt-host 192.168.1.x --mqtt-port 1883
```

Replace `192.168.1.x` with your Mosquitto broker IP.

### 4. Run with both (hybrid — real nodes + simulated fill-in)

```bash
python main.py --mqtt-host 192.168.1.x --simulate-missing
```

## MQTT Topic Schema (for firmware team)

Root ESP32 should publish JSON to these topics:

```
airmesh/nodes/<MAC>/sensors   →  {"co2": 854, "temp": 27.3, "humidity": 61.2, "gas": 23.1, "etx": 1.42, "hops": 2, "role": "MESH", "seq": 1042}
airmesh/nodes/<MAC>/status    →  {"online": true, "rssi": -67, "role": "ROOT"}
airmesh/topology              →  {"edges": [["AA:BB:CC:DD:EE:FF", "11:22:33:44:55:66"]]}
```

## Connecting Real Hardware — What to Change

See `mqtt_client.py` — the `# HARDWARE:` comments mark every section that needs updating when real nodes are available:
1. Set broker IP/port
2. Confirm MQTT topic names match firmware
3. Adjust JSON field names if firmware uses different keys
4. Remove simulator import in `main.py`
