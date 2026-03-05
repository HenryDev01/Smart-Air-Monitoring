"""
models.py — Shared data models for AirMesh dashboard.
These mirror the pkt_sensor_t and pkt_hello_t structs in the ESP-IDF firmware.
"""
from pydantic import BaseModel, Field
from typing import Optional, Literal
from datetime import datetime


class SensorReading(BaseModel):
    """
    Mirrors pkt_sensor_t in components/configuration/air_mesh.h
    All fields are optional so partial updates are accepted gracefully.
    """
    node_id: str                          # MAC address string e.g. "AA:BB:CC:DD:EE:FF"
    timestamp: datetime = Field(default_factory=datetime.utcnow)
    seq: int = 0                          # monotonic sequence number from firmware

    # --- SCD40 / SCD41 (I2C) ---
    co2: Optional[float] = None           # ppm   (400–5000 typical range)
    temperature: Optional[float] = None   # °C
    humidity: Optional[float] = None      # % RH

    # --- MQ-2 Gas Sensor (ADC) ---
    gas: Optional[float] = None           # 0–100 (calibrated %) or raw ADC 0–4095

    # --- Mesh routing metrics (from firmware routing module) ---
    etx: Optional[float] = None           # ETX to root (1.0 = perfect, higher = worse)
    hops: Optional[int] = None            # hop count = esp_mesh_get_layer()
    role: Optional[Literal["ROOT", "MESH", "BLE_RELAY"]] = "MESH"

    # --- BLE link info (only set if this reading came via NRF52840 → M5Stack bridge) ---
    ble_rssi: Optional[int] = None        # dBm, None if WiFi-native node
    via_ble: bool = False                 # True = originated from NRF52840 peripheral


class NodeStatus(BaseModel):
    """Live status of a mesh node."""
    node_id: str
    online: bool = True
    role: Literal["ROOT", "MESH", "BLE_RELAY"] = "MESH"
    rssi: Optional[int] = None            # WiFi RSSI to parent
    last_seen: datetime = Field(default_factory=datetime.utcnow)
    pdr: float = 100.0                    # packet delivery ratio %
    total_packets: int = 0
    missed_packets: int = 0


class TopologyEdge(BaseModel):
    """A parent→child edge in the mesh topology."""
    parent: str   # MAC
    child: str    # MAC


class TopologyUpdate(BaseModel):
    edges: list[TopologyEdge] = []


class AlertEvent(BaseModel):
    """Generated server-side when a threshold is crossed."""
    node_id: str
    timestamp: datetime = Field(default_factory=datetime.utcnow)
    level: Literal["WARNING", "CRITICAL"]
    metric: str          # e.g. "co2", "gas", "temperature"
    value: float
    threshold: float
    message: str


# ── Alert thresholds ────────────────────────────────────────────────────────

ALERT_THRESHOLDS = {
    "co2": [
        {"level": "WARNING",  "value": 1000, "message": "CO₂ elevated — consider ventilation"},
        {"level": "CRITICAL", "value": 2000, "message": "CO₂ critically high — ventilate immediately"},
    ],
    "temperature": [
        {"level": "WARNING",  "value": 35,   "message": "High temperature detected"},
        {"level": "CRITICAL", "value": 40,   "message": "Critical temperature"},
    ],
    "gas": [
        {"level": "WARNING",  "value": 40,   "message": "Gas / smoke detected"},
        {"level": "CRITICAL", "value": 70,   "message": "Dangerous gas level — evacuate"},
    ],
    "humidity": [
        {"level": "WARNING",  "value": 80,   "message": "High humidity"},
    ],
}


def check_alerts(reading: SensorReading) -> list[AlertEvent]:
    """Generate alert events for a sensor reading that crosses thresholds."""
    alerts = []
    checks = {
        "co2": reading.co2,
        "temperature": reading.temperature,
        "gas": reading.gas,
        "humidity": reading.humidity,
    }
    for metric, value in checks.items():
        if value is None:
            continue
        for rule in ALERT_THRESHOLDS.get(metric, []):
            if value >= rule["value"]:
                alerts.append(AlertEvent(
                    node_id=reading.node_id,
                    level=rule["level"],
                    metric=metric,
                    value=value,
                    threshold=rule["value"],
                    message=rule["message"],
                ))
                break  # only highest matching threshold per metric
    return alerts
