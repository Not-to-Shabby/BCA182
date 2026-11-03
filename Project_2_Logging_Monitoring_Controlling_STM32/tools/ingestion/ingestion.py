#!/usr/bin/env python3
"""BenchWatch ingestion service.

Subscribes to the BenchWatch MQTT topic tree, validates JSON payloads, and
writes validated measurements to InfluxDB with ingestion-time UTC timestamps
(the assignment's "reliable timestamps" requirement - the device timestamp
"ts" is stored as a field for diagnostics, never used as the point time).

Measurements:
  bench_telemetry  fields: temperature_c, humidity_pct, light, proximity,
                           ax..gz, motion_mag, motion_event, buzzer, seq,
                           device_ts_ms  | tags: device_id
  bench_status     fields: online, buzzer, wifi, mqtt, uptime_s
                           | tags: device_id, led_mode
  bench_ack        fields: id, accepted, actual  | tags: device_id, reason

Malformed payloads are counted and logged, never written.
"""

import json
import logging
import os
import sys
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS

# --- Configuration (env, docker-compose provides defaults) -------------------
MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_TOPIC_FILTER = os.environ.get("MQTT_TOPIC_FILTER", "bca182/#")
INFLUX_URL = os.environ.get("INFLUX_URL", "http://localhost:8086")
INFLUX_TOKEN = os.environ.get("INFLUX_TOKEN", "")
INFLUX_ORG = os.environ.get("INFLUX_ORG", "bca182")
INFLUX_BUCKET = os.environ.get("INFLUX_BUCKET", "benchwatch")

LOG = logging.getLogger("benchwatch-ingestion")

stats = {"telemetry": 0, "status": 0, "ack": 0, "invalid": 0}

write_api = None


def now_utc() -> datetime:
    """Reliable ingestion timestamp: server UTC clock, not the device's."""
    return datetime.now(timezone.utc)


def fnum(v):
    """Accept int/float, reject bool/None/str."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    return float(v)


def handle_telemetry(topic: str, msg: dict) -> bool:
    device_id = msg.get("device_id")
    if not isinstance(device_id, str):
        return False
    fields = {}
    for key in ("temperature_c", "humidity_pct", "motion_mag", "ax", "ay", "az",
                "gx", "gy", "gz"):
        val = fnum(msg.get(key))
        if val is not None:
            fields[key] = val
    for key in ("light", "proximity"):
        val = msg.get(key)
        if isinstance(val, int) and not isinstance(val, bool):
            fields[key] = int(val)
    for key in ("motion_event", "buzzer"):
        if isinstance(msg.get(key), bool):
            fields[key] = msg[key]
    for key in ("seq",):
        val = msg.get(key)
        if isinstance(val, int) and not isinstance(val, bool):
            fields[key] = int(val)
    if "device_ts_ms" not in fields and isinstance(msg.get("ts"), int):
        fields["device_ts_ms"] = msg["ts"]

    if not fields:
        return False
    point = Point("bench_telemetry").tag("device_id", device_id)
    for k, v in fields.items():
        point = point.field(k, v)
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point,
                    write_precision=WritePrecision.NS)
    return True


def handle_status(topic: str, msg: dict) -> bool:
    device_id = msg.get("device_id")
    if not isinstance(device_id, str):
        return False
    point = Point("bench_status").tag("device_id", device_id)
    if isinstance(msg.get("led_mode"), str):
        point = point.tag("led_mode", msg["led_mode"])
    wrote = False
    for key in ("online", "buzzer", "wifi", "mqtt"):
        if isinstance(msg.get(key), bool):
            point = point.field(key, msg[key])
            wrote = True
    uptime = msg.get("uptime_s")
    if isinstance(uptime, int) and not isinstance(uptime, bool):
        point = point.field("uptime_s", int(uptime))
        wrote = True
    if not wrote:
        return False
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point,
                    write_precision=WritePrecision.NS)
    return True


def handle_ack(topic: str, msg: dict) -> bool:
    cid = msg.get("id")
    accepted = msg.get("accepted")
    if not isinstance(cid, int) or not isinstance(accepted, bool):
        return False
    point = Point("bench_ack").field("id", int(cid)).field("accepted", accepted)
    if isinstance(msg.get("reason"), str):
        point = point.tag("reason", msg["reason"])
    if isinstance(msg.get("actual"), str):
        point = point.field("actual", msg["actual"])
    device_id = msg.get("device_id", "unknown")
    point = point.tag("device_id", str(device_id))
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point,
                    write_precision=WritePrecision.NS)
    return True


HANDLERS = {
    "/telemetry": handle_telemetry,
    "/status": handle_status,
    "/ack": handle_ack,
}


def on_message(client, userdata, msg):
    topic = msg.topic
    suffix = next((s for s in HANDLERS if topic.endswith(s)), None)
    if suffix is None:
        return  # unrelated topic in the tree
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("payload is not a JSON object")
        if HANDLERS[suffix](topic, payload):
            stats[{"/telemetry": "telemetry", "/status": "status",
                   "/ack": "ack"}[suffix]] += 1
        else:
            stats["invalid"] += 1
            LOG.warning("rejected (schema) %s: %s", topic, msg.payload[:120])
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as exc:
        stats["invalid"] += 1
        LOG.warning("rejected (json) %s: %s (%s)", topic, msg.payload[:120], exc)


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        LOG.info("connected to broker %s:%d, subscribing to %s", MQTT_HOST,
                 MQTT_PORT, MQTT_TOPIC_FILTER)
        client.subscribe(MQTT_TOPIC_FILTER, qos=0)
    else:
        LOG.error("broker refused connection rc=%d", rc)


def main() -> int:
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    if not INFLUX_TOKEN:
        LOG.error("INFLUX_TOKEN is not set")
        return 2

    global write_api
    influx = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    write_api = influx.write_api(write_type=SYNCHRONOUS)

    client = mqtt.Client(client_id="benchwatch-ingestion",
                         clean_session=True, protocol=mqtt.MQTTv311)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()

    LOG.info("ingestion running: MQTT %s:%d -> InfluxDB %s/%s/%s", MQTT_HOST,
             MQTT_PORT, INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET)
    try:
        while True:
            time.sleep(30)
            LOG.info("stats: %s", stats)
    except KeyboardInterrupt:
        LOG.info("shutting down")
    finally:
        client.loop_stop()
        client.disconnect()
        write_api.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
