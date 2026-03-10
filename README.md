# Air Quality Mesh Monitor
**ESP-IDF only** 

## Features
| Requirement | Implementation |
|---|---|
| Distributed mesh | `esp_mesh` — self-organizing, self-healing |
| ETX routing | Per-link delivery ratio tracking + proactive OLSR hellos |
| Gossip flooding | TTL-limited gossip with dedup cache + jitter |
| Proactive routing | 10s hello broadcast, neighbor table, ETX-based parent selection |
| Light sleep | `esp_pm` auto light sleep + mesh PS duty cycle |
| HMAC authentication | mbedtls HMAC-SHA256 + challenge-response |
| Session expiry | 1-hour sessions, watchdog re-challenges, revocation |


## Build & Flash
```bash
# Set target
idf.py set-target esp32

# Build
idf.py build
If in VSCODE ctrl + shift p 

# Flash + monitor
idf.py -p COM5 -b 115200 flash monitor                   
```

## Key Provisioning
Each node needs a unique 32-byte HMAC key stored in NVS.
Provision once during manufacturing / first boot:

```c
// // Run this once, then comment out
// uint8_t my_key[32] = { /* 32 random bytes */ };
// auth_provision_key(my_key, 32);
// ```

Or write a provisioning script using `nvs_partition_gen.py` from ESP-IDF tools.

## Configuration
All tunable parameters are in `main/include/air_mesh.h`:

| Parameter | Default | Description |
|---|---|---|
| `MESH_CHANNEL` | 6 | WiFi channel for mesh |
| `MESH_MAX_LAYERS` | 6 | Max mesh depth |
| `HELLO_INTERVAL_MS` | 10000 | Proactive hello period |
| `NEIGHBOR_TIMEOUT_MS` | 35000 | Stale neighbor expiry |
| `GOSSIP_TTL_DEFAULT` | 6 | Gossip flood TTL |
| `SESSION_TTL_SEC` | 3600 | Auth session lifetime |
| `SENSOR_INTERVAL_MS` | 30000 | Sensor read period |
| `CPU_MAX_FREQ_MHZ` | 80 | Max CPU freq (PM) |
| `CPU_MIN_FREQ_MHZ` | 10 | Min CPU freq (light sleep) |

## Replacing Stub Sensors
In `sensor.c`, replace the stub functions:
```c
// PM2.5/PM10 — PMS5003 over UART
static float read_pm25(void) {
    pms5003_data_t d;
    pms5003_read(&d);
    return d.pm2_5_atm;
}

// Temperature/Humidity — SHT31 over I2C
static float read_temperature(void) {
    sht31_data_t d;
    sht31_read(&d);
    return d.temperature;
}
```

## Architecture Notes

### Why Proactive Routing?
Nodes are fixed (air monitoring stations). Proactive routing keeps routing
tables pre-computed so there's no discovery delay when sending readings.
Route changes are rare, so the hello overhead (one small packet / 10s) is minimal.

### Why Gossip + TTL?
For critical alerts (threshold breach, config update), gossip ensures
all nodes receive the message even if some are temporarily unreachable.
TTL=6 with 50ms jitter prevents broadcast storms on a ~50-node mesh.

### Why HMAC vs TLS?
- No PKI infrastructure needed
- Much lower RAM/CPU overhead on ESP32
- Keys provisioned at manufacturing time
- Challenge-response prevents replay attacks
- Session expiry limits window of a compromised key