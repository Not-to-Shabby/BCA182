// Phase 4 RTP audio receive path — GLUE (sockets + PSA crypto + FreeRTOS + the
// vendored ALAC decoder). The host-testable logic it relies on lives in the pure
// units rtp_parser.c, aes_frame.c, alac_config.c, rtp_reorder.c, rtp_resend.c and
// rtp_timing.c; this file only wires them to the network, the crypto and the audio
// ring.
//
// Wire behavior follows shairport-sync rtp.c / player.c EXACTLY (see
// research-phase3-5.md agent 3 + critic G1/G2/G3, fetched & quoted 2026-07-17):
//   * dispatch on packet[1] & 0x7f; audio=0x60, resend-response=0x56 (wire 0xd6),
//     sync=0x54 (ignored, free-run), timing-request=0x52, timing-response=0x53.
//   * decrypt ONLY the largest multiple of 16 payload bytes; the trailing
//     (payload_len % 16) bytes are plaintext and copied verbatim (aes_frame). The
//     AES-CBC IV is RESET to the constant session IV for EVERY packet — CBC chains
//     within a packet, NEVER across packets.
//   * the decrypted+tail bytes are one raw ALAC frame -> alac_decode_frame ->
//     interleaved LE int16 stereo PCM -> audio_play_pcm.
//
// Phase 4 upgrade over Phase 3's arrival-order loop:
//   1. SEQ-INDEXED REORDER (rtp_reorder): audio payloads are buffered ENCRYPTED,
//      keyed by 16-bit RTP seq, and drained in seq order before decode. Late /
//      out-of-order / retransmitted packets slot into place; an unfillable gap
//      conceals with silence and advances (never stalls). Reorder-before-decode is
//      correct because each RAOP packet is one independently-decodable ALAC frame
//      and a resend response is itself an encrypted audio packet — so recovered
//      packets flow through the SINGLE decode path with no duplicate branch.
//   2. RETRANSMIT (rtp_resend): on a front gap we send an 8-byte 0xd5 resend
//      request to the SENDER's control port and inject 0xd6 responses by seq.
//   3. TIMING (rtp_timing): RECEIVER-INITIATED. We periodically send 0xd2 requests
//      to the sender's timing port and consume 0xd3 responses (discarded — we
//      free-run, spec §5c). The task brief's "reply to 0x53 requests" premise was
//      inverted; verified against shairport rtp_timing_sender/_receiver. A
//      documented defensive belt still answers an inbound 0xd2 with a 0xd3.
//
// The peer address is learned from the FIRST audio datagram's source (shairport
// does the same); resend/timing requests are addressed to that IP + the sender's
// control/timing ports from the SETUP Transport header.
//
// Untrusted LAN input (spec §9): the datagram size is capped, the RTP length is
// bounded before use, and every downstream length is checked against a buffer.

#include "raop_rtp.h"
#include "rtp_parser.h"
#include "aes_frame.h"
#include "rtp_reorder.h"
#include "rtp_resend.h"
#include "rtp_timing.h"
#include "alac.h"
#include "alac_config.h"
#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mbedtls/aes.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "raop_rtp";

// RAOP audio datagrams are ~1.4 KB; 2048 caps a hostile oversize packet (§9).
#define RTP_RX_CAP   2048
// Worst-case decoded PCM: frameLength * numChannels * int16. Bounds the decoder
// output for up to 4096-sample stereo (16 KB); the RAOP default 352 uses 1408 B.
#define PCM_MAX_INT16 (4096 * 2)

// Reorder / jitter window (spec §6c). 256 packets @ 352 samples ≈ 2.04 s — the
// budget for how long a gap is held before conceal. In steady state the drain
// keeps it near-empty (the PCM ring backpressures), so it adds no latency; it only
// fills when loss forces a hold so retransmit has time to land.
#define REORDER_WINDOW 256
#define REORDER_HOLD   192      // conceal after holding ~1.5 s (retransmit RTT ≪ this)

// Don't re-request the same front-gap more than this often while a retransmit is in
// flight (keeps the control channel from flooding on a persistent hole).
#define RESEND_MIN_INTERVAL_MS 30
// Receiver-initiated timing cadence (shairport uses ~3 s).
#define TIMING_INTERVAL_MS     3000

// ---- module state (single session, spec §8) ------------------------------
static alac_file           *s_alac        = NULL;
static mbedtls_aes_context   s_aes;
static bool                  s_aes_key_ok  = false;
static uint8_t               s_iv[16];             // constant session IV (per-packet reset)
static uint8_t               s_num_channels = 2;
static uint16_t              s_frame_length = 352; // ALAC frameLength (conceal size)

static int                   s_audio_fd    = -1;
static int                   s_control_fd  = -1;
static int                   s_timing_fd   = -1;
static int                   s_client_control_port = 0;
static int                   s_client_timing_port  = 0;

static struct sockaddr_in    s_peer;               // learned from the first audio recvfrom
static bool                  s_peer_known  = false;

static rtp_reorder_t         s_reorder;
static rtp_reorder_slot_t   *s_slots       = NULL;  // heap_caps_malloc(SPIRAM), WINDOW entries

// Resend throttle: the last front-gap we requested + when.
static uint16_t              s_resend_first = 0;
static bool                  s_resend_valid = false;
static TickType_t            s_resend_tick  = 0;

static TaskHandle_t          s_task        = NULL;
static volatile bool         s_run         = false;
static volatile bool         s_done        = false;

// static (off the task stack); single-instance task -> not reentrant.
static uint8_t               s_rx[RTP_RX_CAP];               // recvfrom scratch
static uint8_t               s_enc[RTP_REORDER_MAX_PKT];     // popped encrypted payload
static uint8_t               s_plain[RTP_RX_CAP];            // decrypted ALAC frame
static int16_t               s_pcm[PCM_MAX_INT16];           // decoded PCM

// Free-run NTP-64 clock (sec-since-1900 unused; we discard timing responses, so the
// epoch is irrelevant — the value only needs to be well-formed and monotonic). Built
// from the boot microsecond clock.
static uint64_t now_ntp64(void) {
    uint64_t us   = (uint64_t)esp_timer_get_time();
    uint64_t sec  = us / 1000000ULL;
    uint64_t frac = ((us % 1000000ULL) << 32) / 1000000ULL;
    return (sec << 32) | frac;
}

// AES-128-CBC decrypt of `len` (multiple-of-16) ciphertext bytes with a fresh IV
// per call using hardware-accelerated mbedtls_aes_crypt_cbc.
static int aes_cbc_decrypt(const uint8_t *in, size_t len, const uint8_t iv[16],
                           uint8_t *out, size_t out_cap, size_t *out_len) {
    if (len > out_cap || !s_aes_key_ok) {
        return -1;
    }
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int rc = mbedtls_aes_crypt_cbc(&s_aes, MBEDTLS_AES_DECRYPT, len, iv_copy, in, out);
    if (rc != 0) {
        ESP_LOGW(TAG, "mbedtls_aes_crypt_cbc failed: %d", rc);
        return -1;
    }
    *out_len = len;
    return 0;
}

// Backpressured feed of decoded PCM (a full ring yields+retries, like the diag
// tone) so samples are never dropped mid-stream. Interleaved stereo frames.
static void feed_pcm(const int16_t *pcm, size_t frames) {
    size_t off = 0;
    while (off < frames && s_run) {
        off += audio_play_pcm(pcm + off * s_num_channels, frames - off);
        if (off < frames) vTaskDelay(1);
    }
}

// Push `frames` of silence through the SAME backpressured feed so a concealed gap
// preserves stream duration (holds timing) instead of time-compressing playback.
static void feed_silence(size_t frames) {
    static const int16_t zeros[256 * 2] = {0};
    size_t remaining = frames;
    while (remaining > 0 && s_run) {
        size_t chunk = remaining > 256 ? 256 : remaining;
        size_t off = 0;
        while (off < chunk && s_run) {          // zeros -> offset is a no-op
            off += audio_play_pcm(zeros, chunk - off);
            if (off < chunk) vTaskDelay(1);
        }
        remaining -= chunk;
    }
}

// One raw ENCRYPTED ALAC payload (as popped from the reorder buffer) ->
// AES-CBC decrypt -> ALAC decode -> feed. Identical to the Phase 3 body; only the
// source of the payload changed (reorder pop instead of the raw datagram).
static void decode_and_feed(const uint8_t *payload, size_t payload_len) {
    if (payload_len == 0 || payload_len > sizeof(s_plain)) return;

    aes_frame_split_t sp;
    aes_frame_split(payload_len, &sp);

    size_t decoded = 0;
    if (sp.cipher_len > 0) {
        if (aes_cbc_decrypt(payload, sp.cipher_len, s_iv,
                            s_plain, sizeof(s_plain), &decoded) != 0) {
            return;
        }
    }
    if (sp.plain_len > 0) {
        memcpy(s_plain + sp.plain_off, payload + sp.plain_off, sp.plain_len);
    }

    int outsize = (int)sizeof(s_pcm);
    alac_decode_frame(s_alac, s_plain, s_pcm, &outsize);
    if (outsize <= 0) return;

    size_t frames = (size_t)outsize / ((size_t)s_num_channels * sizeof(int16_t));
    if (frames == 0) return;
    feed_pcm(s_pcm, frames);
}

// Insert one encrypted audio payload into the reorder buffer by seq. Classification
// is intentionally ignored — DUP / TOO_OLD / TOO_NEW / BADLEN are all safely dropped
// (a retransmit of a packet we already played, or one outside the window).
static void insert_audio(uint16_t seq, const uint8_t *payload, size_t len) {
    if (payload == NULL || len == 0) return;
    rtp_reorder_insert(&s_reorder, seq, payload, len);
}

// Drain the reorder buffer as far as in-order data allows: pop -> decode/feed, or
// conceal one frame of silence on an unrecoverable gap, then stop and wait for more.
static void drain_decode(void) {
    for (;;) {
        size_t len = 0; uint16_t seq = 0;
        rtp_pop_t r = rtp_reorder_pop(&s_reorder, s_enc, sizeof s_enc, &len, &seq);
        if (r == RTP_POP_OK) {
            decode_and_feed(s_enc, len);
        } else if (r == RTP_POP_CONCEAL) {
            feed_silence(s_frame_length);   // hold stream duration across the hole
        } else {
            break;                          // WAIT (front gap held) or EMPTY
        }
    }
}

// On a front gap, send an 0xd5 resend request to the sender's control port for the
// missing [first, count] run — throttled so a persistent gap doesn't flood while a
// retransmit is in flight.
static void maybe_request_resend(void) {
    if (!s_peer_known || s_control_fd < 0) return;
    uint16_t first, count;
    if (!rtp_reorder_gap(&s_reorder, &first, &count)) return;

    TickType_t now = xTaskGetTickCount();
    if (s_resend_valid && first == s_resend_first &&
        (now - s_resend_tick) < pdMS_TO_TICKS(RESEND_MIN_INTERVAL_MS)) {
        return;   // same front-gap, requested too recently
    }

    uint8_t req[RESEND_REQ_LEN];
    rtp_resend_build(req, first, count);
    struct sockaddr_in dst = s_peer;
    dst.sin_port = htons((uint16_t)s_client_control_port);
    sendto(s_control_fd, req, sizeof req, 0, (struct sockaddr *)&dst, sizeof dst);

    s_resend_first = first; s_resend_tick = now; s_resend_valid = true;
}

// audio_fd ready: parse, learn the peer + anchor on the first packet, buffer by
// seq, drain, and (re)request any front gap.
static void handle_audio(void) {
    struct sockaddr_in src;
    socklen_t sl = sizeof src;
    int packets_processed = 0;

    while (packets_processed < 16) {
        int n = recvfrom(s_audio_fd, s_rx, sizeof s_rx, MSG_DONTWAIT, (struct sockaddr *)&src, &sl);
        if (n <= 0) break;

        rtp_header_t h;
        if (rtp_parse(s_rx, (size_t)n, &h) != 0) continue;
        if (h.payload_type != RTP_PT_AUDIO) continue;

        // Anchor the reorder buffer on the first audio packet (independent of the peer:
        // the peer is normally seeded from the RTSP sender at RECORD).
        if (!s_reorder.started) {
            rtp_reorder_anchor(&s_reorder, h.seq);   // authoritative anchor
            ESP_LOGI(TAG, "anchor seq=%u ctrl=%d timing=%d",
                     h.seq, s_client_control_port, s_client_timing_port);
        }
        // Fallback only: if RECORD had no sender IP, learn the peer from the audio source.
        if (!s_peer_known) {
            s_peer = src;
            s_peer_known = true;
        }

        insert_audio(h.seq, h.payload, h.payload_len);
        packets_processed++;
    }

    if (packets_processed > 0) {
        drain_decode();
        maybe_request_resend();
    }
}

// control_fd ready: inject 0xd6 resend responses by seq; ignore sync (0x54) and any
// other control type (free-run receiver, spec §5c).
static void handle_control(void) {
    int n = recvfrom(s_control_fd, s_rx, sizeof s_rx, 0, NULL, NULL);
    if (n < 2) return;

    uint8_t type = s_rx[1] & 0x7f;
    if (type == RESEND_RESP_TYPE) {
        const uint8_t *inner; size_t ilen;
        if (rtp_resend_unwrap(s_rx, (size_t)n, &inner, &ilen) != 0) return;
        rtp_header_t h;
        if (rtp_parse(inner, ilen, &h) != 0) return;
        if (h.payload_type != RTP_PT_AUDIO) return;
        insert_audio(h.seq, h.payload, h.payload_len);   // recovered packet -> its slot
        drain_decode();
    }
    // else: sync (0x54) / unknown control — logged-and-dropped (free-run).
}

// timing_fd ready: standard senders send us 0xd3 RESPONSES (consumed + discarded,
// free-run). The 0xd2-request branch is the documented defensive belt for a
// non-standard sender that polls us — standard senders never do.
static void handle_timing(void) {
    struct sockaddr_in src;
    socklen_t sl = sizeof src;
    int n = recvfrom(s_timing_fd, s_rx, sizeof s_rx, 0, (struct sockaddr *)&src, &sl);
    if (n <= 0) return;

    uint8_t type; timing_stamps_t st;
    if (rtp_timing_parse(s_rx, (size_t)n, &type, &st) != 0) return;

    if (type == TIMING_REQ_TYPE) {           // defensive belt only
        uint64_t now = now_ntp64();
        uint8_t resp[TIMING_PKT_LEN];
        rtp_timing_build_response(resp, st.transmit, now, now);  // echo their t1 as origin
        sendto(s_timing_fd, resp, sizeof resp, 0, (struct sockaddr *)&src, sl);
    }
    // TIMING_RESP_TYPE (0x53): consumed and DISCARDED — no clock discipline.
}

// Receiver-initiated timing: emit a 0xd2 request to the sender's timing port at a
// steady cadence so a real sender never tears us down for a silent timing channel.
static void send_timing_request(void) {
    if (!s_peer_known || s_timing_fd < 0 || s_client_timing_port <= 0) return;
    uint8_t req[TIMING_PKT_LEN];
    rtp_timing_build_request(req, now_ntp64());
    struct sockaddr_in dst = s_peer;
    dst.sin_port = htons((uint16_t)s_client_timing_port);
    sendto(s_timing_fd, req, sizeof req, 0, (struct sockaddr *)&dst, sizeof dst);
}

static void rtp_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "RTP receive task up (core %d): 3-socket select loop (audio/control/timing)",
             audio_producer_core());
    // Send the FIRST timing request right away (peer already seeded from the RTSP
    // sender at RECORD): iOS drops the session at ~2 s if it never sees timing, and
    // the steady cadence below is 3 s. Backdate last_timing so iteration 1 fires.
    if (s_peer_known) send_timing_request();
    TickType_t last_timing = xTaskGetTickCount();

    while (s_run) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s_audio_fd, &rd);
        int maxfd = s_audio_fd;
        if (s_control_fd >= 0) { FD_SET(s_control_fd, &rd); if (s_control_fd > maxfd) maxfd = s_control_fd; }
        if (s_timing_fd  >= 0) { FD_SET(s_timing_fd,  &rd); if (s_timing_fd  > maxfd) maxfd = s_timing_fd;  }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };  // re-check s_run + timing cadence
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "select failed: errno %d", errno);
            vTaskDelay(1);
            continue;
        }
        if (r > 0) {
            if (FD_ISSET(s_audio_fd, &rd))                        handle_audio();
            if (s_control_fd >= 0 && FD_ISSET(s_control_fd, &rd)) handle_control();
            if (s_timing_fd  >= 0 && FD_ISSET(s_timing_fd,  &rd)) handle_timing();
        }

        // Periodic receiver-initiated timing request (keeps the sender happy).
        TickType_t now = xTaskGetTickCount();
        if (s_peer_known && (now - last_timing) >= pdMS_TO_TICKS(TIMING_INTERVAL_MS)) {
            send_timing_request();
            last_timing = now;
        }
    }

    audio_producer_release();
    s_task = NULL;
    s_done = true;              // publish AFTER releasing: stop() waits on this
    vTaskDelete(NULL);
}

// Build the decoder from the SDP fmtp and set the setinfo_* fields directly
// (matches shairport-sync init_alac_decoder). Returns 0 on success.
static int build_decoder(const char *fmtp) {
    alac_cfg_t c;
    if (alac_cfg_from_fmtp(fmtp, &c) != 0) {
        ESP_LOGE(TAG, "fmtp parse failed: \"%s\"", fmtp ? fmtp : "(null)");
        return -1;
    }
    // The audio ring is 16-bit stereo; bound the decoder to that contract and to
    // the PCM buffer size.
    if (c.bit_depth != 16 || c.num_channels != 2) {
        ESP_LOGE(TAG, "unsupported ALAC: bit_depth=%u channels=%u (want 16/2)",
                 c.bit_depth, c.num_channels);
        return -1;
    }
    if (c.frame_length == 0 ||
        (size_t)c.frame_length * c.num_channels > PCM_MAX_INT16) {
        ESP_LOGE(TAG, "ALAC frame_length %u out of range", (unsigned)c.frame_length);
        return -1;
    }

    s_alac = alac_create(c.bit_depth, c.num_channels);
    if (s_alac == NULL) {
        ESP_LOGE(TAG, "alac_create failed");
        return -1;
    }
    s_alac->setinfo_max_samples_per_frame = c.frame_length;
    s_alac->setinfo_7a                    = c.compat_version;
    s_alac->setinfo_sample_size           = c.bit_depth;
    s_alac->setinfo_rice_historymult      = c.pb;
    s_alac->setinfo_rice_initialhistory   = c.mb;
    s_alac->setinfo_rice_kmodifier        = c.kb;
    s_alac->setinfo_7f                    = c.num_channels;
    s_alac->setinfo_80                    = c.max_run;
    s_alac->setinfo_82                    = c.max_frame_bytes;
    s_alac->setinfo_86                    = c.avg_bitrate;
    s_alac->setinfo_8a_rate               = c.sample_rate;
    alac_allocate_buffers(s_alac);

    s_num_channels = c.num_channels;
    s_frame_length = c.frame_length;
    ESP_LOGI(TAG, "ALAC decoder: frameLength=%u %u-bit %uch @%uHz",
             (unsigned)c.frame_length, c.bit_depth, c.num_channels, (unsigned)c.sample_rate);
    return 0;
}

static int import_aes_key(const uint8_t key[16]) {
    mbedtls_aes_init(&s_aes);
    int rc = mbedtls_aes_setkey_dec(&s_aes, key, 128);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_setkey_dec failed: %d", rc);
        return -1;
    }
    s_aes_key_ok = true;
    return 0;
}

static void teardown_crypto_decoder(void) {
    if (s_aes_key_ok) {
        mbedtls_aes_free(&s_aes);
        s_aes_key_ok = false;
    }
    if (s_alac) {
        alac_free(s_alac);
        s_alac = NULL;
    }
    if (s_slots) {
        heap_caps_free(s_slots);
        s_slots = NULL;
    }
}

int raop_rtp_start(raop_session_t *s) {
    if (s_task != NULL) {
        ESP_LOGW(TAG, "raop_rtp_start: already running");
        return -1;
    }
    if (!s->have_key || s->audio_fd < 0) {
        ESP_LOGE(TAG, "raop_rtp_start: missing key or unbound audio socket");
        return -1;
    }
    if (build_decoder(s->fmtp) != 0) return -1;
    if (import_aes_key(s->aeskey) != 0) { teardown_crypto_decoder(); return -1; }

    // Reorder buffer in PSRAM (spec §6c), or fallback to internal RAM
    uint16_t window = REORDER_WINDOW;
    uint16_t hold = REORDER_HOLD;
    s_slots = (rtp_reorder_slot_t *)heap_caps_malloc((size_t)window * sizeof(rtp_reorder_slot_t),
                               MALLOC_CAP_SPIRAM);
    if (s_slots == NULL) {
        // Fallback: 40 slots (~61 KB) fits in internal RAM; hold=3 (24 ms) prevents huge silence stalls
        window = 40;
        hold = 3;
        s_slots = (rtp_reorder_slot_t *)malloc((size_t)window * sizeof(rtp_reorder_slot_t));
        if (s_slots != NULL) {
            ESP_LOGI(TAG, "reorder buffer using internal RAM (%u slots, hold=%u)", (unsigned)window, (unsigned)hold);
        }
    } else {
        ESP_LOGI(TAG, "reorder buffer allocated in PSRAM (%u slots)", (unsigned)window);
    }
    if (s_slots == NULL) {
        ESP_LOGE(TAG, "reorder buffer alloc failed");
        teardown_crypto_decoder();
        return -1;
    }
    rtp_reorder_init(&s_reorder, s_slots, window, hold);

    memcpy(s_iv, s->aesiv, sizeof(s_iv));
    s_audio_fd   = s->audio_fd;
    s_control_fd = s->control_fd;
    s_timing_fd  = s->timing_fd;
    s_client_control_port = s->client_control_port;
    s_client_timing_port  = s->client_timing_port;
    // Seed the peer from the RTSP sender IP (captured at RECORD) so timing/resend
    // requests can flow to the sender's control/timing ports IMMEDIATELY, before
    // any audio arrives. iOS tears the session down at ~2 s if the receiver's
    // timing channel is silent, and the first timing request otherwise waited on
    // the first audio packet -> deadlock. handle_audio() still refines the peer.
    if (s->client_ip != 0) {
        memset(&s_peer, 0, sizeof(s_peer));
        s_peer.sin_family = AF_INET;
        s_peer.sin_addr.s_addr = s->client_ip;
        s_peer_known = true;
    } else {
        s_peer_known = false;
    }
    s_resend_valid = false;

    // Bound recvfrom on every socket so a spurious wake never blocks the task and it
    // periodically re-checks s_run (select() is the primary wait).
    struct timeval rcv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(s_audio_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    if (s_control_fd >= 0) setsockopt(s_control_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    if (s_timing_fd  >= 0) setsockopt(s_timing_fd,  SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    // Claim the single-producer slot BEFORE the task starts (the caller has
    // already stopped the diag tone, so the slot is free). Prio above the diag
    // tone; pinned to the audio core so the SPSC ring stays synchronized.
    if (!audio_producer_acquire("raop_rtp")) {
        teardown_crypto_decoder();
        return -1;
    }
    s_run  = true;
    s_done = false;
    // 16 KB: per packet this task runs PSA AES-128-CBC decrypt + a full ALAC frame
    // decode (Hammerton decoder uses sizable temp buffers), which overflows a
    // smaller stack — same failure class as the RTSP-task RSA overflow. Sized to
    // match the RTSP task.
    if (xTaskCreatePinnedToCore(rtp_task, "raop_rtp", 16384, NULL, 6,
                                &s_task, audio_producer_core()) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(raop_rtp) failed");
        audio_producer_release();
        s_run = false;
        s_task = NULL;
        teardown_crypto_decoder();
        return -1;
    }
    ESP_LOGI(TAG, "RTP receive started (audio_fd=%d control_fd=%d timing_fd=%d)",
             s_audio_fd, s_control_fd, s_timing_fd);
    return 0;
}

bool raop_rtp_stop(void) {
    if (s_task == NULL) {
        // Task not running; still clean up any decoder/key/reorder left from a failed start.
        teardown_crypto_decoder();
        return false;
    }
    s_run = false;
    while (!s_done) vTaskDelay(1);   // block until the task released the producer
    teardown_crypto_decoder();
    s_audio_fd = s_control_fd = s_timing_fd = -1;
    s_peer_known = false;
    ESP_LOGI(TAG, "RTP receive stopped");
    return true;
}
