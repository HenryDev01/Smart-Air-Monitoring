"""
simulator.py — Fake sensor data generator.
Runs when --simulate flag is passed to main.py.
Produces realistic readings for all 5 M5Stack nodes + NRF52840 BLE node.

HARDWARE NOTE: When real equipment is available, this module is NOT imported.
See mqtt_client.py for the real data path.
"""
import asyncio
import random
import math
from datetime import datetime
from models import SensorReading, NodeStatus, TopologyEdge, TopologyUpdate

# ── Simulated node definitions ───────────────────────────────────────────────

SIMULATED_NODES = [
    {"id": "AA:BB:CC:DD:EE:01", "role": "ROOT",      "parent": None,               "floor": "Root Gateway"},
    {"id": "AA:BB:CC:DD:EE:02", "role": "MESH",      "parent": "AA:BB:CC:DD:EE:01","floor": "Lab Room A"},
    {"id": "AA:BB:CC:DD:EE:03", "role": "MESH",      "parent": "AA:BB:CC:DD:EE:02","floor": "Corridor"},
    {"id": "AA:BB:CC:DD:EE:04", "role": "MESH",      "parent": "AA:BB:CC:DD:EE:02","floor": "Lab Room B"},
    {"id": "AA:BB:CC:DD:EE:05", "role": "BLE_RELAY", "parent": "AA:BB:CC:DD:EE:03","floor": "Storage Room (BLE)"},
]

# Per-node state for realistic drift simulation
_node_state = {
    n["id"]: {
        "co2_base":    420 + i * 80,
        "temp_base":   24.0 + i * 0.5,
        "hum_base":    55.0 + i * 2,
        "gas_base":    5.0 + i * 3,
        "co2_drift":   0.0,
        "seq":         0,
        "etx":         1.0 + i * 0.3,
        "hops":        i,
    }
    for i, n in enumerate(SIMULATED_NODES)
}


def _gen_reading(node: dict, t: float) -> SensorReading:
    """Generate one realistic reading for a node at time t."""
    nid = node["id"]
    s = _node_state[nid]
    s["seq"] += 1

    # Simulate slow CO2 drift (occupancy cycle over ~10 min)
    s["co2_drift"] += random.gauss(0, 2)
    s["co2_drift"] = max(-200, min(600, s["co2_drift"]))

    # Occasional gas spike (1% chance per reading)
    gas_spike = random.gauss(40, 5) if random.random() < 0.01 else 0.0

    co2      = s["co2_base"] + s["co2_drift"] + math.sin(t / 300) * 50 + random.gauss(0, 8)
    temp     = s["temp_base"] + math.sin(t / 600) * 1.5 + random.gauss(0, 0.2)
    humidity = s["hum_base"]  + math.sin(t / 400) * 5   + random.gauss(0, 0.5)
    gas      = s["gas_base"]  + gas_spike               + random.gauss(0, 1.5)

    # ETX varies slightly
    s["etx"] = max(1.0, s["etx"] + random.gauss(0, 0.05))

    return SensorReading(
        node_id=nid,
        timestamp=datetime.utcnow(),
        seq=s["seq"],
        co2=round(max(400, co2), 1),
        temperature=round(temp, 2),
        humidity=round(max(0, min(100, humidity)), 1),
        gas=round(max(0, min(100, gas)), 1),
        etx=round(s["etx"], 3),
        hops=s["hops"],
        role=node["role"],
        via_ble=(node["role"] == "BLE_RELAY"),
        ble_rssi=random.randint(-85, -55) if node["role"] == "BLE_RELAY" else None,
    )


def get_simulated_topology() -> TopologyUpdate:
    """Return the static simulated mesh topology."""
    edges = [
        TopologyEdge(parent=n["parent"], child=n["id"])
        for n in SIMULATED_NODES if n["parent"]
    ]
    return TopologyUpdate(edges=edges)


def get_simulated_statuses() -> list[NodeStatus]:
    return [
        NodeStatus(
            node_id=n["id"],
            online=True,
            role=n["role"],
            rssi=random.randint(-75, -40),
            pdr=round(random.uniform(92, 100), 1),
        )
        for n in SIMULATED_NODES
    ]


async def run_simulator(on_reading, interval_s: float = 5.0):
    """
    Coroutine that generates sensor readings at `interval_s` intervals.
    Calls `on_reading(SensorReading)` for each generated reading.

    HARDWARE NOTE: This entire coroutine is replaced by mqtt_client.py
    when real hardware is connected. The `on_reading` callback signature
    is identical — swapping is seamless.
    """
    t0 = 0.0
    while True:
        t0 += interval_s
        for node in SIMULATED_NODES:
            reading = _gen_reading(node, t0)
            await on_reading(reading)
            await asyncio.sleep(0.1)   # stagger node emissions slightly
        await asyncio.sleep(max(0, interval_s - len(SIMULATED_NODES) * 0.1))
