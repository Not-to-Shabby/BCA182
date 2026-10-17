// AirPlay-1 (RAOP) RTSP control server — Phase 2.
//
// One FreeRTOS task owns a listening TCP socket on RAOP_RTSP_PORT (5000). It
// walks a single sender through OPTIONS -> ANNOUNCE -> SETUP -> RECORD, answers
// the RSA Apple-Challenge, RSA/OAEP-decrypts the AES session key, and binds the
// three receiver-side UDP sockets. The RTP audio receive/decrypt/decode loop is
// Phase 3; here we only negotiate the session.
//
// Single session (spec §8): the task select()s on the listen socket AND the
// active client socket at once, so while one session is live a second sender is
// accepted just long enough to be answered 453 (Not Enough Bandwidth) and closed.
//
// Untrusted LAN input (spec §9): the recv buffer is capped; an over-long request
// gets 400 and the connection is dropped; every parsed length is bounded by the
// pure rtsp_parser/base64/sdp units.
//
// Dead/silent peer (spec §8): the accepted client socket carries SO_KEEPALIVE +
// SO_RCVTIMEO, and the task tracks last-activity so a sender that vanishes without
// TEARDOWN (out of Wi-Fi range / crash) — or any LAN peer that connects to :5000
// and then says nothing — is torn down after RAOP_CLIENT_IDLE_MS, freeing the
// single-session lock instead of holding it until reboot.

#include "raop.h"

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "rtsp_parser.h"
#include "sdp.h"
#include "base64.h"
#include "raop_challenge.h"
#include "raop_crypto.h"
#include "rtsp_session.h"
#include "raop_rtp.h"       // Phase 3 RTP receive/decrypt/decode task
#include "audio.h"          // diag-tone handoff + audio_set_volume (Phase 5)
#include "mdns_service.h"   // RAOP_RTSP_PORT
#include "setparam.h"       // Phase 5: SET_PARAMETER Content-Type dispatch
#include "raop_volume.h"    // Phase 5: "volume:"/"progress:" line parsers
#include "dmap.h"           // Phase 5: DMAP/DAAP TLV metadata parser
#include "raop_metadata.h"  // Phase 5: current-track store (log on change)

static const char *TAG = "raop";

// Session lifecycle callback for the status LED (spec §7). Set by main before
// start; fired from the RTSP task on RECORD (STREAMING) and teardown (IDLE).
static raop_event_cb_t s_ev_cb = NULL;

void raop_set_event_cb(raop_event_cb_t cb) { s_ev_cb = cb; }

#define RAOP_RX_CAP 2048

// Reclaim the single-session lock from a peer that stops talking without TEARDOWN
// (spec §8 dead-RTSP detection; §9 unauthenticated-LAN DoS defense). Generous
// enough to never interrupt a real OPTIONS->ANNOUNCE->SETUP->RECORD negotiation
// (which completes in well under a second), yet bounded so the slot always frees.
#define RAOP_CLIENT_IDLE_MS 30000

static TaskHandle_t     s_task     = NULL;
static volatile bool    s_running  = false;
static int              s_listen_fd = -1;
static raop_session_t   s_session;

// Reverse the RECORD handoff and clear the session. If a stream is live, stop the
// RTP decoder producer and resume the pre-stream diag tone (single-producer
// arbitration: exactly one producer feeds audio_play_pcm() at all times). The
// stop joins the decode task BEFORE raop_session_reset() closes the UDP sockets
// it read from — order matters. Idempotent; a no-op when not streaming.
static void session_teardown_full(void) {
    if (raop_rtp_stop()) {
        // audio_diag_tone_start();  // DEBUG: diag tone disabled to isolate RAOP streaming
    }
    raop_metadata_clear();         // forget the last now-playing (spec §5b)
    raop_session_reset(&s_session);
    if (s_ev_cb) s_ev_cb(RAOP_EV_IDLE);   // LED -> CONNECTED_IDLE (no live stream)
}

// ---------------------------------------------------------------------------
static void send_response(int fd, int status, const char *reason, int cseq,
                          const char *extra, const char *body, size_t body_len) {
    char out[1024];
    int n = rtsp_build_response(out, sizeof(out), status, reason,
                                cseq < 0 ? 0 : cseq, extra, body, body_len);
    if (n > 0) {
        send(fd, out, (size_t)n, 0);
    } else {
        ESP_LOGE(TAG, "response build overflow (status %d)", status);
    }
}

// Build + send the OPTIONS reply, answering an Apple-Challenge if present.
static void handle_options(int fd, const rtsp_request_t *req, int cseq) {
    char extra[512];
    size_t eo = 0;
    int n = snprintf(extra + eo, sizeof(extra) - eo,
                     "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
                     "OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
    if (n > 0 && (size_t)n < sizeof(extra) - eo) eo += (size_t)n;

    const char *chal = rtsp_header_get(req, "Apple-Challenge");
    if (chal) {
        uint8_t cbuf[32];
        int clen = base64_decode(chal, strlen(chal), cbuf, sizeof(cbuf));
        if (clen == 16) {
            uint8_t ip4[4] = {0};
            struct sockaddr_in local;
            socklen_t ll = sizeof(local);
            if (getsockname(fd, (struct sockaddr *)&local, &ll) == 0)
                memcpy(ip4, &local.sin_addr.s_addr, 4);  // network byte order
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_WIFI_STA);

            uint8_t buf32[RAOP_CHALLENGE_BUF_LEN];
            if (raop_challenge_assemble(cbuf, 16, ip4, mac, buf32) >= 0) {
                uint8_t sig[256];
                size_t siglen = 0;
                if (raop_crypto_sign_challenge(buf32, sig, sizeof(sig), &siglen) == 0) {
                    char b64[400];
                    if (base64_encode(sig, siglen, b64, sizeof(b64)) > 0) {
                        char *pad = strchr(b64, '=');  // shairport strips the padding
                        if (pad) *pad = '\0';
                        int m = snprintf(extra + eo, sizeof(extra) - eo,
                                         "Apple-Response: %s\r\n", b64);
                        if (m > 0 && (size_t)m < sizeof(extra) - eo) eo += (size_t)m;
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Apple-Challenge decoded to %d bytes (want 16)", clen);
        }
    }
    send_response(fd, 200, "OK", cseq, extra, NULL, 0);
}

// Parse SDP, RSA-decrypt the AES key, store key/iv/fmtp. 200 on success, 400 on
// a decode/decrypt failure (never crash — §8/§9).
static void handle_announce(int fd, const rtsp_request_t *req, int cseq) {
    sdp_media_t m;
    sdp_parse(req->body ? req->body : "", req->body_len, &m);

    bool ok = true;

    if (m.has_rsaaeskey) {
        uint8_t enc[256];
        int el = base64_decode(m.rsaaeskey, strlen(m.rsaaeskey), enc, sizeof(enc));
        if (el <= 0 || raop_crypto_decrypt_aeskey(enc, (size_t)el, s_session.aeskey) != 0) {
            ESP_LOGW(TAG, "ANNOUNCE: rsaaeskey decrypt failed");
            ok = false;
        }
    } else {
        ok = false;  // an encrypted RAOP stream must carry the key
    }

    if (ok && m.has_aesiv) {
        int il = base64_decode(m.aesiv, strlen(m.aesiv), s_session.aesiv, sizeof(s_session.aesiv));
        if (il != 16) {
            ESP_LOGW(TAG, "ANNOUNCE: aesiv decoded to %d bytes (want 16)", il);
            ok = false;
        }
    }

    if (ok) {
        s_session.have_key = m.has_rsaaeskey && m.has_aesiv;
        if (m.has_fmtp) snprintf(s_session.fmtp, sizeof(s_session.fmtp), "%s", m.fmtp);
        s_session.state = RAOP_ANNOUNCED;
        ESP_LOGI(TAG, "ANNOUNCE ok: AES key(16) decrypted, iv(16), fmtp=\"%s\"", s_session.fmtp);
        send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
    } else {
        send_response(fd, 400, "Bad Request", cseq, NULL, NULL, 0);
    }
}

// Parse the sender's Transport ports, bind our three UDP sockets, return ours.
static void handle_setup(int fd, const rtsp_request_t *req, int cseq) {
    const char *t = rtsp_header_get(req, "Transport");
    if (t) {
        s_session.client_control_port = rtsp_transport_port(t, "control_port");
        s_session.client_timing_port  = rtsp_transport_port(t, "timing_port");
    }
    if (raop_session_bind_udp(&s_session) != 0) {
        send_response(fd, 500, "Internal Server Error", cseq, NULL, NULL, 0);
        return;
    }
    s_session.state = RAOP_SETUP;

    char extra[256];
    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;"
             "control_port=%u;timing_port=%u\r\n"
             "Session: 1\r\n",
             s_session.audio_port, s_session.control_port, s_session.timing_port);
    ESP_LOGI(TAG, "SETUP ok: client control=%d timing=%d; our audio=%u control=%u timing=%u",
             s_session.client_control_port, s_session.client_timing_port,
             s_session.audio_port, s_session.control_port, s_session.timing_port);
    send_response(fd, 200, "OK", cseq, extra, NULL, 0);
}

// Handle SET_PARAMETER (spec §5b). Dispatch on Content-Type (untrusted input,
// §9: every parsed length bounded by the pure parsers): text/parameters carries
// the volume slider (+ optional progress); application/x-dmap-tagged carries the
// DMAP/DAAP track metadata; artwork (image/*) and anything else is ACKed + ignored.
// Always answers 200 OK — a sender that gets no ack for a control message stalls.
static void handle_set_parameter(int fd, const rtsp_request_t *req, int cseq) {
    const char *ct = rtsp_header_get(req, "Content-Type");
    switch (setparam_classify(ct)) {
        case SETPARAM_VOLUME: {
            float db;
            if (req->body && raop_parse_volume(req->body, req->body_len, &db)) {
                audio_set_volume(db);            // software gain in the playback drain
            }
            uint32_t s, c, e;
            if (req->body && raop_parse_progress(req->body, req->body_len, &s, &c, &e)) {
                // Three RTP timestamps @44100 Hz; log elapsed/total once (free-run
                // receiver: no seek UI, values are otherwise ignored).
                ESP_LOGI(TAG, "progress %us / %us", (unsigned)((c - s) / 44100),
                         (unsigned)((e - s) / 44100));
            }
            break;
        }
        case SETPARAM_METADATA: {
            if (req->body && req->body_len) {
                dmap_meta_t m;
                dmap_parse((const uint8_t *)req->body, req->body_len, &m);
                raop_metadata_update(&m);        // logs "now playing" on change
            }
            break;
        }
        case SETPARAM_OTHER:
        default:
            break;                               // artwork / unknown: ignore (Phase 5 scope)
    }
    send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
}

// Dispatch one parsed request. Returns true if the connection should be closed
// afterwards (TEARDOWN).
static bool dispatch(int fd, const rtsp_request_t *req) {
    int cseq = rtsp_cseq(req);
    ESP_LOGI(TAG, "%s (CSeq %d)", req->method, cseq);

    if (strcmp(req->method, "OPTIONS") == 0) {
        handle_options(fd, req, cseq);
    } else if (strcmp(req->method, "ANNOUNCE") == 0) {
        handle_announce(fd, req, cseq);
    } else if (strcmp(req->method, "SETUP") == 0) {
        handle_setup(fd, req, cseq);
    } else if (strcmp(req->method, "RECORD") == 0) {
        if (s_session.state == RAOP_RECORDING) {
            // Idempotent re-RECORD: already streaming, just re-ack.
            send_response(fd, 200, "OK", cseq, "Audio-Latency: 11025\r\n", NULL, 0);
        } else if (!s_session.have_key || s_session.fmtp[0] == '\0') {
            // No AES key / ALAC config -> cannot decode (spec §9: bound bad input).
            ESP_LOGW(TAG, "RECORD without key/fmtp -> 400");
            send_response(fd, 400, "Bad Request", cseq, NULL, NULL, 0);
        } else {
            // Single-producer handoff: STOP the diag tone (blocks until it has
            // released the audio path) BEFORE starting the decoder — the two
            // producers must never feed audio_play_pcm() concurrently.
            audio_diag_tone_stop();
            // Capture the sender's IP from the RTSP TCP connection so the RTP task
            // can start the timing/resend channels immediately (not wait for the
            // first audio packet). The sender's timing/control servers live at this
            // IP + the ports from SETUP.
            struct sockaddr_in praddr;
            socklen_t prlen = sizeof(praddr);
            if (getpeername(fd, (struct sockaddr *)&praddr, &prlen) == 0) {
                s_session.client_ip = praddr.sin_addr.s_addr;
            }
            if (raop_rtp_start(&s_session) == 0) {
                s_session.state = RAOP_RECORDING;
                if (s_ev_cb) s_ev_cb(RAOP_EV_STREAMING);   // LED -> STREAMING (green)
                ESP_LOGI(TAG, "RECORD: RTP decode streaming (first AirPlay audio)");
            } else {
                ESP_LOGE(TAG, "RECORD: RTP start failed; resuming diag tone");
                // audio_diag_tone_start();  // DEBUG: diag tone disabled to isolate RAOP streaming
            }
            send_response(fd, 200, "OK", cseq, "Audio-Latency: 11025\r\n", NULL, 0);
        }
    } else if (strcmp(req->method, "SET_PARAMETER") == 0) {
        // Phase 5: volume (software gain), DAAP metadata, progress. Dispatches on
        // Content-Type; always acks 200. Must NOT tear the decoder down.
        handle_set_parameter(fd, req, cseq);
    } else if (strcmp(req->method, "FLUSH") == 0 ||
               strcmp(req->method, "PAUSE") == 0 ||
               strcmp(req->method, "GET_PARAMETER") == 0) {
        // FLUSH affects buffering only (jitter buffer is Phase 4) and must NOT
        // tear the decoder down. Ack now.
        send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
    } else if (strcmp(req->method, "TEARDOWN") == 0) {
        session_teardown_full();   // stop decoder, resume tone, reset session
        send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
        ESP_LOGI(TAG, "TEARDOWN: session released");
        return true;
    } else {
        send_response(fd, 501, "Not Implemented", cseq, NULL, NULL, 0);
    }
    return false;
}

// Harden an accepted RTSP client socket against a vanished/silent peer (spec §8/§9).
// The loop's last-activity timeout is the primary reclaim (it fires even for a peer
// that never sends a byte, since select() never marks such a fd readable); these
// socket options are defense-in-depth: SO_RCVTIMEO caps any recv() we do make, and
// TCP keepalive probes let lwip surface a silently-dead peer as a recv() error.
static void configure_client_socket(int fd) {
    struct timeval rcv = { .tv_sec = RAOP_CLIENT_IDLE_MS / 1000, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 10;   // begin probing after 10 s of silence
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = 5;   // probe every 5 s
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = 3;     // drop after 3 unanswered probes
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

// ---------------------------------------------------------------------------
// Case-insensitive prefix match over a bounded buffer (no NUL assumptions).
static int ci_startswith(const char *s, const char *key, size_t klen) {
    for (size_t i = 0; i < klen; i++) {
        char a = s[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

// Find header `key` in the bounded RTSP header block and return its base-10 value,
// or -1 if absent. Used to drain oversized bodies we don't buffer (artwork).
static long hdr_scan_long(const char *buf, size_t len, const char *key) {
    size_t klen = strlen(key);
    if (len < klen) return -1;
    for (size_t i = 0; i + klen < len; i++) {
        if (ci_startswith(buf + i, key, klen)) {
            const char *v = buf + i + klen;
            const char *e = buf + len;
            while (v < e && (*v == ':' || *v == ' ' || *v == '\t')) v++;
            return strtol(v, NULL, 10);
        }
    }
    return -1;
}

static void server_task(void *arg) {
    (void)arg;

    // Crypto init (RSA-2048 self-test: sign + encrypt + decrypt round-trip) runs
    // HERE, on this task's large stack — NOT in raop_server_start()'s caller, which
    // is the wifi GOT_IP handler running on the sys_evt event task (~2.3 KB stack).
    // mbedTLS/PSA bignum ops need ~6-8 KB and overflow sys_evt -> panic/reboot loop.
    // Fail loud: without the key we cannot answer the Apple-Challenge, so don't serve.
    if (raop_crypto_init() != ESP_OK) {
        ESP_LOGE(TAG, "raop_crypto_init failed; RTSP server not serving "
                      "(cannot answer the Apple-Challenge without the key)");
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "listen socket() failed: errno %d", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(RAOP_RTSP_PORT);
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d failed: errno %d", RAOP_RTSP_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    // Backlog 2: enough to accept a second sender solely to answer it 453.
    listen(s_listen_fd, 2);
    ESP_LOGI(TAG, "RTSP listening on :%d", RAOP_RTSP_PORT);

    int       client_fd = -1;
    char      rx[RAOP_RX_CAP];
    size_t    used = 0;
    TickType_t last_activity = 0;   // tick of the last accept/recv on client_fd

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_listen_fd, &rfds);
        int maxfd = s_listen_fd;
        if (client_fd >= 0) {
            FD_SET(client_fd, &rfds);
            if (client_fd > maxfd) maxfd = client_fd;
        }
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select failed: errno %d", errno);
            break;
        }
        // Reclaim the single-session lock from a peer that went silent without
        // TEARDOWN — out of range, crashed, or a LAN peer that connected to :5000
        // and then sent nothing (spec §8 dead-RTSP detection; §9 DoS defense).
        // Checked on every wake, including the select() timeout, so a peer that
        // never triggers readability is still dropped.
        if (client_fd >= 0 &&
            (xTaskGetTickCount() - last_activity) >= pdMS_TO_TICKS(RAOP_CLIENT_IDLE_MS)) {
            ESP_LOGW(TAG, "RTSP client idle > %d ms -> teardown", RAOP_CLIENT_IDLE_MS);
            close(client_fd);
            client_fd = -1;
            session_teardown_full();
            continue;
        }

        if (r == 0) continue;  // timeout -> re-check s_running

        // New inbound connection.
        if (FD_ISSET(s_listen_fd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t cl = sizeof(cli);
            int nfd = accept(s_listen_fd, (struct sockaddr *)&cli, &cl);
            if (nfd >= 0) {
                if (client_fd >= 0) {
                    // Single session (spec §8): refuse the second sender.
                    char busy[128];
                    int bn = rtsp_build_response(busy, sizeof(busy), 453,
                                                 "Not Enough Bandwidth", 0, NULL, NULL, 0);
                    if (bn > 0) send(nfd, busy, (size_t)bn, 0);
                    close(nfd);
                    ESP_LOGW(TAG, "second sender refused (453 busy)");
                } else {
                    client_fd = nfd;
                    used = 0;
                    last_activity = xTaskGetTickCount();
                    configure_client_socket(client_fd);
                    session_teardown_full();
                    ESP_LOGI(TAG, "sender connected");
                }
            }
        }

        // Data (or EOF) on the active connection.
        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            if (used >= sizeof(rx)) {
                // Buffer full but the request is still incomplete: an oversized body,
                // almost always a SET_PARAMETER carrying album ARTWORK (a JPEG, tens
                // of KB) or large DAAP metadata we don't need. Rather than 400 +
                // teardown (which kills a live stream), find the header terminator,
                // read Content-Length, DRAIN the rest of the body off the socket, and
                // ack 200 OK so iOS keeps streaming. Headers always fit in rx.
                const char *hdr_end = NULL;
                for (size_t i = 0; i + 4 <= used; i++) {
                    if (memcmp(rx + i, "\r\n\r\n", 4) == 0) { hdr_end = rx + i + 4; break; }
                }
                if (hdr_end) {
                    size_t hlen = (size_t)(hdr_end - rx);
                    long clen = hdr_scan_long(rx, hlen, "Content-Length");
                    long cseq = hdr_scan_long(rx, hlen, "CSeq");
                    long to_drain = (clen > 0) ? clen - (long)(used - hlen) : 0;
                    char tmp[512];
                    while (to_drain > 0) {
                        size_t want = (to_drain < (long)sizeof(tmp)) ? (size_t)to_drain : sizeof(tmp);
                        int dn = recv(client_fd, tmp, want, 0);
                        if (dn <= 0) break;
                        to_drain -= dn;
                    }
                    ESP_LOGW(TAG, "oversized request (Content-Length=%ld) drained -> 200 OK", clen);
                    send_response(client_fd, 200, "OK", (int)(cseq < 0 ? 0 : cseq), NULL, NULL, 0);
                    used = 0;
                    last_activity = xTaskGetTickCount();
                    continue;
                }
                // No header terminator in the whole buffer -> genuinely malformed huge
                // headers (untrusted input, §9): reject and drop.
                ESP_LOGW(TAG, "request headers exceed %d bytes -> 400", (int)sizeof(rx));
                send_response(client_fd, 400, "Bad Request", 0, NULL, NULL, 0);
                close(client_fd);
                client_fd = -1;
                session_teardown_full();
                continue;
            }
            int n = recv(client_fd, rx + used, sizeof(rx) - used, 0);
            if (n <= 0) {
                // Sender vanished without TEARDOWN (§8): drop + reset.
                ESP_LOGI(TAG, "sender disconnected");
                close(client_fd);
                client_fd = -1;
                session_teardown_full();
                continue;
            }
            used += (size_t)n;
            last_activity = xTaskGetTickCount();

            // Drain every complete pipelined request currently buffered.
            bool close_conn = false;
            for (;;) {
                rtsp_request_t req;
                if (!rtsp_parse_request(rx, used, &req)) break;  // need more bytes
                close_conn = dispatch(client_fd, &req);
                size_t consumed = req.total_len;
                if (consumed == 0 || consumed > used) consumed = used;
                memmove(rx, rx + consumed, used - consumed);
                used -= consumed;
                if (close_conn) break;
            }
            if (close_conn) {
                close(client_fd);
                client_fd = -1;
                // session already reset in the TEARDOWN handler
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    session_teardown_full();
    ESP_LOGI(TAG, "RTSP server stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
void raop_server_start(void) {
    if (s_task != NULL) {
        ESP_LOGW(TAG, "raop_server_start: already running");
        return;
    }
    // NB: called from the wifi GOT_IP handler (sys_evt task, small stack). Do NO
    // heavy work here — just spawn the server task. Crypto init (RSA-2048) runs
    // inside server_task on its 16 KB stack; the RTSP handlers also do per-request
    // RSA sign/decrypt, so the task stack must accommodate mbedTLS bignum ops.
    raop_session_reset(&s_session);
    s_running = true;
    if (xTaskCreate(server_task, "raop_rtsp", 16384, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(raop_rtsp) failed");
        s_running = false;
        s_task = NULL;
    }
}

void raop_server_stop(void) {
    s_running = false;
    if (s_listen_fd >= 0) {
        // Nudge the task out of select()/accept() so it can observe s_running.
        shutdown(s_listen_fd, SHUT_RDWR);
    }
}
