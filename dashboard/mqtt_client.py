"""
mqtt_client.py — Real hardware MQTT data receiver.

This module is used when real M5Stack nodes are publishing sensor data
over MQTT. It is NOT used in simulation mode.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
HARDWARE SETUP CHECKLIST (fill in when equipment is available)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. Install Mosquitto on the same machine as this dashboard:
       sudo apt install mosquitto mosquitto-clients     # Linux
       brew install mosquitto                           # macOS

2. Start Mosquitto:
       mosquitto -v

3. On the root ESP32 node, ensure main.c calls an MQTT publish task.
   The firmware team must add an MQTT client (esp-mqtt component).
   Topics to publish:
       airmesh/nodes/<MAC>/sensors  →  JSON payload (see PAYLOAD FORMAT below)
       airmesh/nodes/<MAC>/status   →  JSON status
       airmesh/topology             →  JSON edge list

4. Set MQTT_BROKER_HOST below to the IP of the machine running Mosquitto.
   This IP must be reachable from the ESP32 root node's WiFi interface.

5. Run dashboard with:
       python main.py --mqtt-host <broker-ip>

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
PAYLOAD FORMAT (firmware team: publish this JSON structure)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Topic:   airmesh/nodes/AA:BB:CC:DD:EE:FF/sensors
Payload: {
  "seq":         1042,        // pkt_hdr_t.seq (monotonic)
  "co2":         854.0,       // SCD40 CO2 in ppm
  "temp":        27.3,        // SCD40 temperature in °C
  "humidity":    61.2,        // SCD40 relative humidity in %
  "gas":         23.1,        // MQ-2 calibrated 0-100%
  "etx":         1.42,        // routing_get_etx_to_root()
  "hops":        2,           // esp_mesh_get_layer()
  "role":        "MESH",      // "ROOT" | "MESH" | "BLE_RELAY"
  "via_ble":     false,       // true if relayed from NRF52840
  "ble_rssi":    null         // BLE RSSI if via_ble is true
}

Topic:   airmesh/nodes/AA:BB:CC:DD:EE:FF/status
Payload: {
  "online":  true,
  "rssi":    -67,             // WiFi RSSI to parent AP
  "role":    "MESH"
}

Topic:   airmesh/topology
Payload: {
  "edges": [
    {"parent": "AA:BB:CC:DD:EE:01", "child": "AA:BB:CC:DD:EE:02"},
    {"parent": "AA:BB:CC:DD:EE:02", "child": "AA:BB:CC:DD:EE:03"}
  ]
}
"""

import asyncio
import json
import logging
from datetime import datetime
from typing import Callable, Awaitable

import paho.mqtt.client as mqtt_lib

from models import SensorReading, NodeStatus, TopologyUpdate, TopologyEdge

log = logging.getLogger("mqtt_client")

# HARDWARE: Set these to match your lab network configuration
MQTT_BROKER_HOST = "127.0.0.1"   # HARDWARE: change to broker IP (e.g. "192.168.1.100")
MQTT_BROKER_PORT = 1883           # HARDWARE: change if non-default port
MQTT_KEEPALIVE   = 60
MQTT_CLIENT_ID   = "airmesh_dashboard"

# HARDWARE: Update these if the firmware team uses different topic names
TOPIC_SENSORS    = "airmesh/nodes/+/sensors"
TOPIC_STATUS     = "airmesh/nodes/+/status"
TOPIC_TOPOLOGY   = "airmesh/topology"


class MQTTBridge:
    """
    Bridges MQTT messages from the mesh root node to the FastAPI WebSocket hub.
    """

    def __init__(
        self,
        on_reading: Callable[[SensorReading], Awaitable[None]],
        on_status:  Callable[[NodeStatus],    Awaitable[None]],
        on_topology: Callable[[TopologyUpdate], Awaitable[None]],
        broker_host: str = MQTT_BROKER_HOST,
        broker_port: int = MQTT_BROKER_PORT,
    ):
        self._on_reading   = on_reading
        self._on_status    = on_status
        self._on_topology  = on_topology
        self._broker_host  = broker_host
        self._broker_port  = broker_port
        self._loop         = None
        self._client       = mqtt_lib.Client(client_id=MQTT_CLIENT_ID, protocol=mqtt_lib.MQTTv311)
        self._client.on_connect    = self._on_connect
        self._client.on_message    = self._on_message
        self._client.on_disconnect = self._on_disconnect

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            log.info(f"MQTT connected to {self._broker_host}:{self._broker_port}")
            # HARDWARE: Subscribe to all relevant topics
            client.subscribe(TOPIC_SENSORS)
            client.subscribe(TOPIC_STATUS)
            client.subscribe(TOPIC_TOPOLOGY)
        else:
            log.error(f"MQTT connection failed, rc={rc}")

    def _on_disconnect(self, client, userdata, rc):
        log.warning(f"MQTT disconnected (rc={rc}), will retry...")

    def _on_message(self, client, userdata, msg):
        """Called by paho in its own thread — schedule coroutines on the event loop."""
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
            topic   = msg.topic

            if "/sensors" in topic:
                # Extract MAC from topic: airmesh/nodes/<MAC>/sensors
                mac = topic.split("/")[2]
                reading = self._parse_sensor(mac, payload)
                if reading and self._loop:
                    asyncio.run_coroutine_threadsafe(self._on_reading(reading), self._loop)

            elif "/status" in topic:
                mac = topic.split("/")[2]
                status = self._parse_status(mac, payload)
                if status and self._loop:
                    asyncio.run_coroutine_threadsafe(self._on_status(status), self._loop)

            elif topic == TOPIC_TOPOLOGY:
                topo = self._parse_topology(payload)
                if topo and self._loop:
                    asyncio.run_coroutine_threadsafe(self._on_topology(topo), self._loop)

        except Exception as e:
            log.warning(f"Failed to parse MQTT message on {msg.topic}: {e}")

    def _parse_sensor(self, mac: str, p: dict) -> SensorReading | None:
        """
        HARDWARE: Adjust field names here if firmware publishes different keys.
        Current expected keys: seq, co2, temp, humidity, gas, etx, hops, role, via_ble, ble_rssi
        """
        try:
            return SensorReading(
                node_id=mac,
                timestamp=datetime.utcnow(),
                seq=p.get("seq", 0),
                co2=p.get("co2"),
                temperature=p.get("temp"),        # HARDWARE: firmware uses "temp" not "temperature"
                humidity=p.get("humidity"),
                gas=p.get("gas"),
                etx=p.get("etx"),
                hops=p.get("hops"),
                role=p.get("role", "MESH"),
                via_ble=p.get("via_ble", False),
                ble_rssi=p.get("ble_rssi"),
            )
        except Exception as e:
            log.warning(f"Sensor parse error for {mac}: {e}")
            return None

    def _parse_status(self, mac: str, p: dict) -> NodeStatus | None:
        try:
            return NodeStatus(
                node_id=mac,
                online=p.get("online", True),
                role=p.get("role", "MESH"),
                rssi=p.get("rssi"),
                last_seen=datetime.utcnow(),
            )
        except Exception as e:
            log.warning(f"Status parse error for {mac}: {e}")
            return None

    def _parse_topology(self, p: dict) -> TopologyUpdate | None:
        try:
            edges = [TopologyEdge(parent=e["parent"], child=e["child"]) for e in p.get("edges", [])]
            return TopologyUpdate(edges=edges)
        except Exception as e:
            log.warning(f"Topology parse error: {e}")
            return None

    async def start(self, loop: asyncio.AbstractEventLoop):
        """Connect to MQTT broker and start background network loop."""
        self._loop = loop
        # HARDWARE: Add authentication here if broker requires credentials:
        # self._client.username_pw_set("user", "password")
        # HARDWARE: Add TLS here if broker uses TLS:
        # self._client.tls_set(ca_certs="ca.crt")
        self._client.connect_async(self._broker_host, self._broker_port, MQTT_KEEPALIVE)
        self._client.loop_start()
        log.info(f"MQTT bridge started → {self._broker_host}:{self._broker_port}")

    async def stop(self):
        self._client.loop_stop()
        self._client.disconnect()
        log.info("MQTT bridge stopped")
