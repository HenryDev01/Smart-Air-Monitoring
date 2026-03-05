"""
main.py — AirMesh Dashboard Server

FastAPI application with:
- WebSocket hub broadcasting real-time sensor data to all connected browsers
- In-memory state store (last reading per node, history, alerts, topology)
- REST endpoints for initial page load data
- Simulation mode (--simulate) or MQTT mode (--mqtt-host)

Usage:
    python main.py --simulate                          # no hardware
    python main.py --mqtt-host 192.168.1.100           # real hardware
    python main.py --mqtt-host 192.168.1.100 --port 8080
"""

import asyncio
import argparse
import json
import logging
from collections import deque
from datetime import datetime, timezone
from typing import Optional

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from models import (
    SensorReading, NodeStatus, TopologyUpdate, AlertEvent,
    check_alerts
)

# ── Logging ──────────────────────────────────────────────────────────────────

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(levelname)s %(message)s")
log = logging.getLogger("main")

# ── App ───────────────────────────────────────────────────────────────────────

app = FastAPI(title="AirMesh Dashboard", version="1.0.0")
app.mount("/static", StaticFiles(directory="static"), name="static")

# ── In-memory state store ─────────────────────────────────────────────────────

MAX_HISTORY = 60   # keep last 60 readings per node (≈ 30 minutes at 30s intervals)

class StateStore:
    def __init__(self):
        self.latest: dict[str, SensorReading]       = {}   # node_id → latest reading
        self.history: dict[str, deque]               = {}   # node_id → deque[SensorReading]
        self.statuses: dict[str, NodeStatus]         = {}   # node_id → NodeStatus
        self.topology: TopologyUpdate                = TopologyUpdate()
        self.alerts: deque[AlertEvent]               = deque(maxlen=100)
        self.pdr_tracker: dict[str, dict]            = {}   # for PDR calculation

    def ingest_reading(self, r: SensorReading) -> list[AlertEvent]:
        """Store reading, compute PDR, generate alerts. Returns new alerts."""
        self.latest[r.node_id] = r

        if r.node_id not in self.history:
            self.history[r.node_id] = deque(maxlen=MAX_HISTORY)
        self.history[r.node_id].append(r)

        # PDR tracking via sequence numbers
        tracker = self.pdr_tracker.setdefault(r.node_id, {"last_seq": -1, "total": 0, "missed": 0})
        if tracker["last_seq"] >= 0 and r.seq > tracker["last_seq"] + 1:
            tracker["missed"] += r.seq - tracker["last_seq"] - 1
        tracker["last_seq"] = r.seq
        tracker["total"] += 1

        pdr = 100.0
        if tracker["total"] > 0:
            pdr = round(100 * (1 - tracker["missed"] / max(1, tracker["total"] + tracker["missed"])), 1)

        # Update or create status
        status = self.statuses.get(r.node_id, NodeStatus(node_id=r.node_id))
        status.last_seen = r.timestamp
        status.pdr = pdr
        status.total_packets = tracker["total"]
        status.missed_packets = tracker["missed"]
        if r.role:
            status.role = r.role
        status.online = True
        self.statuses[r.node_id] = status

        # Alerts
        new_alerts = check_alerts(r)
        for a in new_alerts:
            self.alerts.appendleft(a)
        return new_alerts

    def get_snapshot(self) -> dict:
        """Full state snapshot for a newly connected WebSocket client."""
        now = datetime.utcnow()
        # Mark nodes offline if not seen in 90 seconds
        for nid, status in self.statuses.items():
            delta = (now - status.last_seen.replace(tzinfo=None)).total_seconds()
            status.online = delta < 90

        return {
            "type": "snapshot",
            "nodes": {
                nid: {
                    "latest": r.model_dump(mode="json") if r else None,
                    "history": [h.model_dump(mode="json") for h in self.history.get(nid, [])],
                    "status": self.statuses.get(nid, NodeStatus(node_id=nid)).model_dump(mode="json"),
                }
                for nid, r in self.latest.items()
            },
            "topology": self.topology.model_dump(mode="json"),
            "alerts": [a.model_dump(mode="json") for a in self.alerts],
        }


store = StateStore()

# ── WebSocket Hub ─────────────────────────────────────────────────────────────

class WSHub:
    def __init__(self):
        self._clients: set[WebSocket] = set()

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self._clients.add(ws)
        # Send full snapshot immediately on connect
        await self._send_one(ws, store.get_snapshot())
        log.info(f"WS client connected. Total: {len(self._clients)}")

    def disconnect(self, ws: WebSocket):
        self._clients.discard(ws)
        log.info(f"WS client disconnected. Total: {len(self._clients)}")

    async def broadcast(self, msg: dict):
        if not self._clients:
            return
        data = json.dumps(msg, default=str)
        dead = set()
        for ws in list(self._clients):
            try:
                await ws.send_text(data)
            except Exception:
                dead.add(ws)
        self._clients -= dead

    async def _send_one(self, ws: WebSocket, msg: dict):
        try:
            await ws.send_text(json.dumps(msg, default=str))
        except Exception:
            pass


hub = WSHub()

# ── Data ingestion callback (called by simulator OR mqtt_client) ──────────────

async def on_reading(reading: SensorReading):
    """Ingest a sensor reading and broadcast update to all WS clients."""
    new_alerts = store.ingest_reading(reading)
    msg = {
        "type": "reading",
        "node_id": reading.node_id,
        "data": reading.model_dump(mode="json"),
        "status": store.statuses[reading.node_id].model_dump(mode="json"),
    }
    await hub.broadcast(msg)
    for alert in new_alerts:
        await hub.broadcast({"type": "alert", "data": alert.model_dump(mode="json")})


async def on_status(status: NodeStatus):
    store.statuses[status.node_id] = status
    await hub.broadcast({"type": "status", "node_id": status.node_id, "data": status.model_dump(mode="json")})


async def on_topology(topo: TopologyUpdate):
    store.topology = topo
    await hub.broadcast({"type": "topology", "data": topo.model_dump(mode="json")})


# ── Routes ────────────────────────────────────────────────────────────────────

@app.get("/")
async def index():
    return FileResponse("static/index.html")


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await hub.connect(ws)
    try:
        while True:
            await ws.receive_text()   # keep alive; client can send pings
    except WebSocketDisconnect:
        hub.disconnect(ws)


@app.get("/api/snapshot")
async def snapshot():
    """REST fallback for clients that can't use WebSocket."""
    return store.get_snapshot()


@app.get("/api/nodes")
async def nodes():
    return {nid: r.model_dump(mode="json") for nid, r in store.latest.items()}


@app.get("/api/history/{node_id}")
async def history(node_id: str):
    h = store.history.get(node_id, [])
    return [r.model_dump(mode="json") for r in h]


# ── Startup / background tasks ────────────────────────────────────────────────

_mqtt_bridge = None


@app.on_event("startup")
async def startup():
    global _mqtt_bridge
    loop = asyncio.get_running_loop()

    if _args.simulate:
        log.info("▶ SIMULATION MODE — generating fake sensor data")
        from simulator import run_simulator, get_simulated_topology, get_simulated_statuses
        # Seed topology and statuses
        store.topology = get_simulated_topology()
        for s in get_simulated_statuses():
            store.statuses[s.node_id] = s
        asyncio.create_task(run_simulator(on_reading, interval_s=5.0))

    elif _args.mqtt_host:
        # HARDWARE: Real MQTT mode
        log.info(f"▶ MQTT MODE — connecting to broker at {_args.mqtt_host}:{_args.mqtt_port}")
        from mqtt_client import MQTTBridge
        _mqtt_bridge = MQTTBridge(
            on_reading=on_reading,
            on_status=on_status,
            on_topology=on_topology,
            broker_host=_args.mqtt_host,
            broker_port=_args.mqtt_port,
        )
        await _mqtt_bridge.start(loop)

    else:
        log.warning("No data source configured. Pass --simulate or --mqtt-host <IP>")


@app.on_event("shutdown")
async def shutdown():
    if _mqtt_bridge:
        await _mqtt_bridge.stop()


# ── CLI entry point ────────────────────────────────────────────────────────────

import types as _types
import sys as _sys

def _parse_args():
    parser = argparse.ArgumentParser(description="AirMesh Dashboard Server", add_help=False)
    parser.add_argument("--simulate",  action="store_true")
    parser.add_argument("--mqtt-host", type=str,  default=None)
    parser.add_argument("--mqtt-port", type=int,  default=1883)
    parser.add_argument("--port",      type=int,  default=8000)
    parser.add_argument("--host",      type=str,  default="0.0.0.0")
    known, _ = parser.parse_known_args()
    return known

_args = _parse_args()

if __name__ == "__main__":
    import argparse as _ap
    parser = _ap.ArgumentParser(description="AirMesh Dashboard Server")
    parser.add_argument("--simulate",    action="store_true",       help="Generate fake sensor data (no hardware)")
    parser.add_argument("--mqtt-host",   type=str,  default=None,   help="MQTT broker IP for real hardware")
    parser.add_argument("--mqtt-port",   type=int,  default=1883,   help="MQTT broker port (default 1883)")
    parser.add_argument("--port",        type=int,  default=8000,   help="Dashboard HTTP port (default 8000)")
    parser.add_argument("--host",        type=str,  default="0.0.0.0", help="Bind host")
    _args = parser.parse_args()

    uvicorn.run(
        "main:app",
        host=_args.host,
        port=_args.port,
        reload=False,
        log_level="info",
    )
