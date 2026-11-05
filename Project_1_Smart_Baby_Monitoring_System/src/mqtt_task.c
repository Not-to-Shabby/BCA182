/**
 * @file mqtt_task.c
 * @brief MQTT client task: publishes sensor readings and cry/env events,
 *        subscribes to the command topic (ack/mute). P4 Smart Baby Monitor.
 *
 * Pipeline (implements the frozen contract in mqtt_task.h):
 *   1. Park until net_task_is_online() (RW007 associated + DHCP lease +
 *      carrier), polled at 200 ms. No socket work happens before that.
 *   2. Resolve the broker host (secrets.h MQTT_BROKER_HOST, which may be a
 *      hostname or dotted quad) with zsock_getaddrinfo() and connect with
 *      the standard Zephyr MQTT client library (Zephyr 4.4 mqtt_socket,
 *      MQTT 3.1.1, mirrors samples/net/mqtt_publisher). Client id is
 *      "babymonitor-<4 random hex digits>" so parallel simulators can
 *      share one broker without client-id collisions; keepalive 30 s;
 *      clean session; no auth (baseline, documented trade-off).
 *   3. On connect: SUBSCRIBE "babymonitor/cmd" at QoS 1, replay up to the
 *      8 queued offline events, then enter the service loop.
 *   4. Service loop (mirrors the mqtt_publisher sample's poll/input/live
 *      trio): zsock_poll() the transport socket (200 ms), mqtt_input() on
 *      data, mqtt_live() for automatic keep-alive PINGREQ. Every 5 s a
 *      reading is built from a monitor_state snapshot (under
 *      monitor_state_lock()) and published at QoS 0. Event edges are
 *      evaluated every iteration from a fresh snapshot.
 *   5. Events are EDGE-TRIGGERED (transition only, one line each):
 *      cry_detected rising/falling -> {"type":"cry","on":..,"conf":..};
 *      BABY_STATE_FAULT rising -> {"type":"fault","on":true};
 *      (state==ALERT && !cry) rising -> {"type":"env","on":true};
 *      state returning to BABY_STATE_NORMAL -> {"type":"allclear","on":true}.
 *      While offline, events are pushed into a small RAM ring (8 x 160 B,
 *      oldest dropped when full) and replayed on reconnect. An SD-backed
 *      queue with a deep history is a later phase (documented in the spec).
 *   6. Commands from "babymonitor/cmd": {"cmd":"ack"} and
 *      {"cmd":"mute","sec":N} (N clamped 0..3600) refresh the mute window
 *      exactly like the physical KEY0 acknowledge path (buttons.c): under
 *      the monitor-state lock, baby_logic_acknowledge() + the
 *      mute_until_ms mirror + a buzzer_on re-evaluation. Parsing is a tiny
 *      hand-rolled strstr/strncmp scanner -- no cJSON dependency.
 *
 * JSON wire formats (fixed-point, no float formatting dependency):
 *   reading: {"t":<temp C 1dp>,"h":<%RH 1dp>,"lux":<u16>,"px":<u16>,
 *             "mg":<int32>,"ts":<uptime seconds>}
 *   event:   {"type":"cry|env|fault|allclear","on":true|false,
 *             "conf":<0..100, cry only>,"ts":<uptime seconds>}
 *   cmd:     {"cmd":"ack"} | {"cmd":"mute","sec":<0..3600>}
 * "ts" is k_uptime_seconds() on both publish topics (device uptime, no
 * wall clock in the baseline). The 1 dp decimals are emitted with integer
 * math (x/10 and x%10) so output does not depend on printf float support.
 *
 * RAM budget: the thread stack (2048 B) lives in the otherwise unused
 * 64 KB DTCM via the raw-buffer pattern from net_task.c (K_THREAD_STACK
 * macros carry their own section attribute and cannot be redirected);
 * the 1280 B event ring + 512 B MQTT rx/tx buffers + client context stay
 * in main SRAM (~2.1 KB total, within the RAM plan's connectivity share).
 *
 * Mute semantics note: the frozen baby_logic API silences the buzzer for
 * its internal fixed ALERT_MUTE_MS (60 s) window; a remote
 * {"cmd":"mute","sec":N} mirrors the requested N-second window into
 * mute_until_ms (what the dashboard shows) and still refreshes the logic
 * window. Extending the buzzer-level window beyond 60 s needs a
 * baby_logic API extension and is out of scope for the frozen header.
 */

#include "mqtt_task.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "baby_logic.h"
#include "monitor_state.h"
#include "net_task.h"

/* ----------------------------------------------------------- configuration */

/* Broker: secrets.h (git-ignored) overrides the public-test defaults.
 * Mirrors the __has_include pattern documented in secrets.h.example --
 * the project must still compile without credentials for grading. */
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "test.mosquitto.org"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883u
#endif

#define MQTT_THREAD_STACK_SIZE 2048u
#define MQTT_THREAD_PRIORITY   7
#define MQTT_THREAD_NAME       "mqtt_thread"

#define MQTT_KEEPALIVE_S        30u
#define MQTT_POLL_INTERVAL_MS   200  /* service loop / park granularity  */
#define MQTT_PUBLISH_PERIOD_MS  5000u
#define MQTT_CONNECT_TIMEOUT_MS 10000

#define MQTT_RXBUF_SIZE 256u
#define MQTT_TXBUF_SIZE 256u
#define MQTT_CMD_MAX    128u /* received cmd payload cap (rest is dropped) */

#define MQTT_TOPIC_READING "babymonitor/reading"
#define MQTT_TOPIC_EVENT   "babymonitor/event"
#define MQTT_TOPIC_CMD     "babymonitor/cmd"

#define MQTT_EVT_QUEUE_LEN 8u
#define MQTT_EVT_SLOT_SIZE 160u

#define MQTT_BACKOFF_START_MS 2000u
#define MQTT_BACKOFF_MAX_MS   30000u

/* ----------------------------------------------------------------- state */

/* MQTT client context + buffers (main SRAM, ~2.1 KB with the ring). */
static struct mqtt_client s_client;
static struct sockaddr_storage s_broker;
static uint8_t s_rxbuf[MQTT_RXBUF_SIZE];
static uint8_t s_txbuf[MQTT_TXBUF_SIZE];
static char s_client_id[sizeof("babymonitor-XXXX")]; /* 4 hex digits + NUL */

static struct zsock_pollfd s_fds[1];

/* Cross-thread flags: mqtt_task_is_connected() is polled by other tasks,
 * so the connected marker is an atomic, not a plain bool. */
static atomic_t s_connected;

/* Offline event ring (oldest-dropped when full). Guarded by its own mutex
 * so an offline event push from this thread's edge scan never races the
 * replay drain. */
static K_MUTEX_DEFINE(s_evt_lock);
static char s_evt_ring[MQTT_EVT_QUEUE_LEN][MQTT_EVT_SLOT_SIZE];
static uint8_t s_evt_head;  /* index of the OLDEST slot */
static uint8_t s_evt_count; /* slots currently occupied (0..8)          */

/* Thread plumbing: raw DTCM stack buffer (K_THREAD_STACK_DEFINE cannot be
 * wrapped in a custom-section attribute -- see net_task.c). */
static uint8_t __attribute__((section(".dtcm_noinit"), aligned(32)))
	s_mqtt_stack[MQTT_THREAD_STACK_SIZE + 64];
static struct k_thread s_mqtt_thread_cb;
static atomic_t s_thread_started;

/* ------------------------------------------------------------ event ring */

/** Push one event JSON into the ring; drops the OLDEST entry when full. */
static void evt_enqueue(const char *json)
{
	const size_t len = strlen(json); /* built locally, always < slot */

	k_mutex_lock(&s_evt_lock, K_FOREVER);
	uint8_t slot;
	if (s_evt_count < MQTT_EVT_QUEUE_LEN) {
		slot = (uint8_t)((s_evt_head + s_evt_count) %
				 MQTT_EVT_QUEUE_LEN);
		s_evt_count++;
	} else {
		/* Full: overwrite the oldest and advance the head. */
		slot = s_evt_head;
		s_evt_head = (uint8_t)((s_evt_head + 1u) % MQTT_EVT_QUEUE_LEN);
	}
	memcpy(s_evt_ring[slot], json, len);
	s_evt_ring[slot][len] = '\0';
	k_mutex_unlock(&s_evt_lock);
}

/** Pop the oldest queued event JSON; false when the ring is empty. */
static bool evt_dequeue(char *out, size_t out_size)
{
	bool ok = false;

	k_mutex_lock(&s_evt_lock, K_FOREVER);
	if (s_evt_count > 0u) {
		const size_t len = strlen(s_evt_ring[s_evt_head]);
		if (len < out_size) {
			memcpy(out, s_evt_ring[s_evt_head], len + 1u);
			ok = true;
		}
		s_evt_head = (uint8_t)((s_evt_head + 1u) % MQTT_EVT_QUEUE_LEN);
		s_evt_count--;
	}
	k_mutex_unlock(&s_evt_lock);
	return ok;
}

/* ------------------------------------------------------------ publishing */

/** Publish one JSON payload on a topic (QoS 0). */
static int publish_json(const char *topic, const char *payload,
			enum mqtt_qos qos)
{
	struct mqtt_publish_param param = { 0 };

	param.message.topic.topic.utf8 = (const uint8_t *)topic;
	param.message.topic.topic.size = (uint32_t)strlen(topic);
	param.message.topic.qos = qos;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = (uint32_t)strlen(payload);
	param.message_id = (uint16_t)sys_rand32_get();
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	return mqtt_publish(&s_client, &param);
}

/** Publish an event now, or park it in the RAM ring while offline. */
static void publish_or_queue_event(const char *json)
{
	if (atomic_get(&s_connected) != 0) {
		if (publish_json(MQTT_TOPIC_EVENT, json,
				 MQTT_QOS_0_AT_MOST_ONCE) == 0) {
			return;
		}
		/* Publish failure while connected: fall through and queue
		 * it so a short broker hiccup still delivers the event. */
	}
	evt_enqueue(json);
	printk("[MQTT] event queued (offline): %s\n", json);
}

/* Reading snapshot -> JSON. Integer-math 1 dp decimals (x/10, x%10), so
 * the output never depends on printf float formatting. */
static void publish_reading(void)
{
	int16_t temp_x10;
	uint16_t hum_x10;
	uint16_t lux;
	uint16_t proximity;
	int32_t accel_mg;

	monitor_state_lock();
	const monitor_state_t *st = monitor_state_get();
	temp_x10 = st->temp_x10;
	hum_x10 = st->hum_x10;
	lux = st->lux;
	proximity = st->proximity;
	accel_mg = st->accel_mg;
	monitor_state_unlock();

	int t_int = temp_x10 / 10;
	int t_frac = temp_x10 % 10;
	if (t_frac < 0) {
		t_frac = -t_frac; /* sign lives on the integer part */
	}

	char json[MQTT_EVT_SLOT_SIZE];
	(void)snprintf(json, sizeof(json),
		       "{\"t\":%d.%d,\"h\":%u.%u,\"lux\":%u,\"px\":%u,"
		       "\"mg\":%d,\"ts\":%u}",
		       t_int, t_frac, (unsigned)(hum_x10 / 10u),
		       (unsigned)(hum_x10 % 10u), (unsigned)lux,
		       (unsigned)proximity, (int)accel_mg,
		       (unsigned)k_uptime_seconds());

	if (publish_json(MQTT_TOPIC_READING, json,
			 MQTT_QOS_0_AT_MOST_ONCE) == 0) {
		printk("[MQTT] pub reading\n");
	}
}

/* ---------------------------------------------------------- event edges */

/* Prev-edge flags for the transition compare (all false = matches the
 * zero-initialized shared state, EXCEPT normal: a fresh boot in NORMAL
 * must not broadcast a spurious all-clear on every connect). */
static bool s_prev_cry;
static bool s_prev_fault;
static bool s_prev_env;
static bool s_prev_normal = true;

/** Snapshot the shared state under the lock and publish edge transitions. */
static void check_events(void)
{
	bool cry;
	bool fault;
	bool env;
	bool normal;
	uint32_t conf;

	monitor_state_lock();
	const monitor_state_t *st = monitor_state_get();
	cry = st->cry_detected;
	conf = st->cry_confidence;
	fault = (st->state == BABY_STATE_FAULT);
	/* Environmental alert only while the cry classifier is not the
	 * reason for ALERT (cry owns its own event stream). */
	env = ((st->state == BABY_STATE_ALERT) && !st->cry_detected);
	normal = (st->state == BABY_STATE_NORMAL);
	monitor_state_unlock();

	const bool cry_on = (cry && !s_prev_cry);
	const bool cry_off = (!cry && s_prev_cry);
	const bool fault_on = (fault && !s_prev_fault);
	const bool env_on = (env && !s_prev_env);
	const bool allclear = (normal && !s_prev_normal);

	s_prev_cry = cry;
	s_prev_fault = fault;
	s_prev_env = env;
	s_prev_normal = normal;

	char json[MQTT_EVT_SLOT_SIZE];

	if (cry_on || cry_off) {
		(void)snprintf(json, sizeof(json),
			       "{\"type\":\"cry\",\"on\":%s,\"conf\":%u,"
			       "\"ts\":%u}",
			       cry_on ? "true" : "false", (unsigned)conf,
			       (unsigned)k_uptime_seconds());
		publish_or_queue_event(json);
		printk("[MQTT] pub event cry %s\n", cry_on ? "on" : "off");
	}
	if (fault_on) {
		(void)snprintf(json, sizeof(json),
			       "{\"type\":\"fault\",\"on\":true,\"ts\":%u}",
			       (unsigned)k_uptime_seconds());
		publish_or_queue_event(json);
		printk("[MQTT] pub event fault on\n");
	}
	if (env_on) {
		(void)snprintf(json, sizeof(json),
			       "{\"type\":\"env\",\"on\":true,\"ts\":%u}",
			       (unsigned)k_uptime_seconds());
		publish_or_queue_event(json);
		printk("[MQTT] pub event env on\n");
	}
	if (allclear) {
		(void)snprintf(json, sizeof(json),
			       "{\"type\":\"allclear\",\"on\":true,\"ts\":%u}",
			       (unsigned)k_uptime_seconds());
		publish_or_queue_event(json);
		printk("[MQTT] pub event allclear on\n");
	}
}

/* ----------------------------------------------------------- cmd handling */

/** Apply a remote ack/mute exactly like the KEY0 path (buttons.c). */
static void apply_cmd(uint32_t mute_ms)
{
	if (mute_ms > 3600u * 1000u) {
		mute_ms = 3600u * 1000u; /* clamp to the contract */
	}

	const uint32_t now = k_uptime_get_32();

	monitor_state_lock();
	monitor_state_t *st = monitor_state_get();
	if (mute_ms > 0u) {
		/* Parametrized logic mute window (baby_logic_acknowledge_for):
		 * the buzzer-level window now matches the requested duration,
		 * not just the dashboard mirror. */
		baby_logic_acknowledge_for(now, mute_ms);
		st->mute_until_ms = now + mute_ms;
	} else {
		/* sec=0: request unmute */
		baby_logic_acknowledge_for(now, 0u);
		st->mute_until_ms = now;
	}
	st->buzzer_on = baby_logic_buzzer_on(st->state, now);
	monitor_state_unlock();
}

/** Minimal JSON scanner for {"cmd":"ack"} / {"cmd":"mute","sec":N}. */
static void handle_cmd_payload(const char *payload)
{
	if (strstr(payload, "\"cmd\":\"ack\"") != NULL) {
		apply_cmd(ALERT_MUTE_MS);
		printk("[MQTT] cmd ack\n");
		return;
	}
	if (strstr(payload, "\"cmd\":\"mute\"") != NULL) {
		const char *p = strstr(payload, "\"sec\":");
		long sec = 0;
		if (p != NULL) {
			sec = atol(p + 6); /* stops at first non-digit */
			if (sec < 0) {
				sec = 0;
			} else if (sec > 3600) {
				sec = 3600;
			}
		}
		apply_cmd((uint32_t)sec * 1000u);
		printk("[MQTT] cmd mute (%u s)\n", (unsigned)sec);
		return;
	}
	printk("[MQTT] cmd ignored (unknown)\n");
}

/* ------------------------------------------------------------- event cb */

static bool s_subscribed; /* SUBACK seen for the current connection */

static void mqtt_evt_handler(struct mqtt_client *const client,
			     const struct mqtt_evt *evt)
{
	ARG_UNUSED(client);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			atomic_set(&s_connected, 1);
			printk("[MQTT] connected\n");
		}
		/* result != 0: the retry loop below owns the backoff print */
		break;

	case MQTT_EVT_DISCONNECT:
		/* Library closed the transport; the service loop notices
		 * via s_connected and prints the retry line once. */
		atomic_set(&s_connected, 0);
		s_fds[0].fd = -1;
		break;

	case MQTT_EVT_SUBACK:
		s_subscribed = true;
		printk("[MQTT] subscribed %s\n", MQTT_TOPIC_CMD);
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;
		char buf[MQTT_CMD_MAX];
		const uint32_t plen = pub->message.payload.len;

		/* Payload is read separately (see mqtt.h MQTT_EVT_PUBLISH).
		 * Oversized payloads are truncated: the two known commands
		 * are far below the cap. */
		size_t want = (plen < sizeof(buf) - 1u) ? plen
							: (sizeof(buf) - 1u);
		int n = mqtt_readall_publish_payload(client, buf, want);
		if (n == 0) {
			buf[want] = '\0';
			handle_cmd_payload(buf);
		}

		if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = {
				.message_id = pub->message_id
			};
			(void)mqtt_publish_qos1_ack(client, &ack);
		}
		break;
	}

	case MQTT_EVT_PINGRESP:
	default:
		break;
	}
}

/* -------------------------------------------------------------- connect */

/** Resolve MQTT_BROKER_HOST (name or dotted quad) into the broker addr. */
static bool broker_resolve(void)
{
	struct zsock_addrinfo *res = NULL;
	char port_str[8];

	(void)snprintf(port_str, sizeof(port_str), "%u",
		       (unsigned)MQTT_BROKER_PORT);

	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};

	const int rc = zsock_getaddrinfo(MQTT_BROKER_HOST, port_str, &hints,
					 &res);
	if (rc != 0 || res == NULL) {
		return false;
	}

	const net_socklen_t len =
		(res->ai_addrlen < (net_socklen_t)sizeof(s_broker))
			? res->ai_addrlen
			: (net_socklen_t)sizeof(s_broker);
	memcpy(&s_broker, res->ai_addr, len);
	zsock_freeaddrinfo(res);
	return true;
}

/* Mirrors the mqtt_publisher sample's client_init(). */
static void client_init(void)
{
	mqtt_client_init(&s_client);

	s_client.broker = &s_broker;
	s_client.evt_cb = mqtt_evt_handler;
	s_client.client_id.utf8 = (const uint8_t *)s_client_id;
	s_client.client_id.size = (uint32_t)strlen(s_client_id);
	s_client.password = NULL;
	s_client.user_name = NULL;
	s_client.protocol_version = MQTT_VERSION_3_1_1;
	s_client.keepalive = (uint16_t)MQTT_KEEPALIVE_S;
	s_client.clean_session = 1u; /* fresh session each connect */

	s_client.rx_buf = s_rxbuf;
	s_client.rx_buf_size = sizeof(s_rxbuf);
	s_client.tx_buf = s_txbuf;
	s_client.tx_buf_size = sizeof(s_txbuf);

	/* Plaintext TCP baseline (LAN/public broker, no TLS): documented
	 * trade-off in mqtt_task.h -- LAN Mosquitto + auth is the P5 path. */
	s_client.transport.type = MQTT_TRANSPORT_NON_SECURE;
}

static void prepare_fds(void)
{
	if (s_client.transport.type == MQTT_TRANSPORT_NON_SECURE) {
		s_fds[0].fd = s_client.transport.tcp.sock;
	}
	s_fds[0].events = ZSOCK_POLLIN;
}

/** Subscribe babymonitor/cmd at QoS 1 and wait briefly for the SUBACK. */
static bool subscribe_cmd(void)
{
	struct mqtt_topic cmd_topic = {
		.topic = {
			.utf8 = (const uint8_t *)MQTT_TOPIC_CMD,
			.size = (uint32_t)strlen(MQTT_TOPIC_CMD),
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	struct mqtt_subscription_list sub = {
		.list = &cmd_topic,
		.list_count = 1u,
		.message_id = (uint16_t)sys_rand32_get(),
	};

	s_subscribed = false;
	if (mqtt_subscribe(&s_client, &sub) != 0) {
		return false;
	}

	/* SUBACK arrives asynchronously; service the socket for up to 2 s. */
	const int64_t deadline = k_uptime_get() + 2000;
	while (!s_subscribed && (k_uptime_get() < deadline)) {
		if (zsock_poll(s_fds, 1, MQTT_POLL_INTERVAL_MS) > 0) {
			if (mqtt_input(&s_client) != 0) {
				return false;
			}
		}
	}
	return s_subscribed;
}

/** Replay the offline ring (oldest first, up to the 8 queued events). */
static void replay_evt_queue(void)
{
	char json[MQTT_EVT_SLOT_SIZE];
	unsigned replayed = 0;

	while (evt_dequeue(json, sizeof(json))) {
		if (publish_json(MQTT_TOPIC_EVENT, json,
				 MQTT_QOS_0_AT_MOST_ONCE) != 0) {
			/* Mid-flush publish failure: leave the rest queued
			 * for the next reconnect (the one in-flight event is
			 * dropped -- QoS 0, documented trade-off). */
			break;
		}
		replayed++;
	}
	if (replayed > 0u) {
		printk("[MQTT] replayed %u queued event(s)\n", replayed);
	}
}

/* --------------------------------------------------------------- thread */

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	s_fds[0].fd = -1;
	s_fds[0].events = ZSOCK_POLLIN;

	uint32_t backoff_ms = MQTT_BACKOFF_START_MS;

	for (;;) {
		/* 1. Park until the network supervisor reports online. */
		while (!net_task_is_online()) {
			k_sleep(K_MSEC(MQTT_POLL_INTERVAL_MS));
		}

		/* 2. Resolve the broker (hostname or dotted quad). */
		if (!broker_resolve()) {
			printk("[MQTT] disconnected (retry in %u s)\n",
			       (unsigned)(backoff_ms / 1000u));
			k_sleep(K_MSEC(backoff_ms));
			backoff_ms = MIN(backoff_ms * 2u, MQTT_BACKOFF_MAX_MS);
			continue;
		}

		/* 3. Connect and wait for the CONNACK (sample pattern). */
		client_init();
		if (mqtt_connect(&s_client) != 0) {
			s_fds[0].fd = -1;
			printk("[MQTT] disconnected (retry in %u s)\n",
			       (unsigned)(backoff_ms / 1000u));
			k_sleep(K_MSEC(backoff_ms));
			backoff_ms = MIN(backoff_ms * 2u, MQTT_BACKOFF_MAX_MS);
			continue;
		}

		prepare_fds();
		const int64_t deadline = k_uptime_get() + MQTT_CONNECT_TIMEOUT_MS;
		while ((atomic_get(&s_connected) == 0) &&
		       (k_uptime_get() < deadline)) {
			if (zsock_poll(s_fds, 1, MQTT_POLL_INTERVAL_MS) > 0) {
				(void)mqtt_input(&s_client);
			}
		}

		if (atomic_get(&s_connected) == 0) {
			(void)mqtt_abort(&s_client);
			s_fds[0].fd = -1;
			printk("[MQTT] disconnected (retry in %u s)\n",
			       (unsigned)(backoff_ms / 1000u));
			k_sleep(K_MSEC(backoff_ms));
			backoff_ms = MIN(backoff_ms * 2u, MQTT_BACKOFF_MAX_MS);
			continue;
		}

		/* 4. Connected: reset backoff, subscribe, replay queue. */
		backoff_ms = MQTT_BACKOFF_START_MS;
		(void)subscribe_cmd();
		replay_evt_queue();

		/* 5. Service loop: poll/input/live trio (mqtt_publisher
		 * pattern) + 5 s readings + event edges per iteration. */
		uint32_t last_pub = 0u; /* publish one reading immediately */
		for (;;) {
			if (zsock_poll(s_fds, 1, MQTT_POLL_INTERVAL_MS) > 0) {
				if (mqtt_input(&s_client) != 0) {
					break;
				}
			}

			/* mqtt_live() sends PINGREQ when the keepalive
			 * window demands it; 0 means a packet was sent and
			 * the PINGRESP needs servicing (sample behavior). */
			const int live_rc = mqtt_live(&s_client);
			if ((live_rc != 0) && (live_rc != -EAGAIN)) {
				break;
			}
			if (live_rc == 0) {
				if (mqtt_input(&s_client) != 0) {
					break;
				}
			}

			if (atomic_get(&s_connected) == 0) {
				break; /* broker-initiated DISCONNECT */
			}
			if (!net_task_is_online()) {
				break; /* Wi-Fi/link lost */
			}

			check_events();

			if ((k_uptime_get_32() - last_pub) >=
			    MQTT_PUBLISH_PERIOD_MS) {
				last_pub = k_uptime_get_32();
				publish_reading();
			}
		}

		/* 6. Down: release the transport and back off. */
		if (atomic_get(&s_connected) != 0) {
			(void)mqtt_disconnect(&s_client, NULL);
		}
		atomic_set(&s_connected, 0);
		s_fds[0].fd = -1;
		printk("[MQTT] disconnected (retry in %u s)\n",
		       (unsigned)(backoff_ms / 1000u));
		k_sleep(K_MSEC(backoff_ms));
		backoff_ms = MIN(backoff_ms * 2u, MQTT_BACKOFF_MAX_MS);
	}
}

/* --------------------------------------------------------- public API */

void mqtt_task_start(void)
{
	/* Idempotent: safe against a double invocation from bring-up code. */
	if (!atomic_cas(&s_thread_started, 0, 1)) {
		return;
	}

	/* Client id "babymonitor-<4 random hex>": parallel boards and
	 * simulators can share one broker without id collisions. */
	(void)snprintf(s_client_id, sizeof(s_client_id), "babymonitor-%04x",
		       (unsigned)(sys_rand32_get() & 0xFFFFu));

	/* House style: static thread, no dynamic allocation (prio 7), stack
	 * in DTCM (main SRAM is nearly full -- see net_task.c). */
	k_tid_t tid =
		k_thread_create(&s_mqtt_thread_cb, (k_thread_stack_t *)s_mqtt_stack,
				MQTT_THREAD_STACK_SIZE, mqtt_thread_fn, NULL,
				NULL, NULL, MQTT_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (tid != NULL) {
		/* CONFIG_THREAD_NAME=y publishes "mqtt_thread" at runtime. */
		(void)k_thread_name_set(tid, MQTT_THREAD_NAME);
	}
}

bool mqtt_task_is_connected(void)
{
	return atomic_get(&s_connected) != 0;
}
