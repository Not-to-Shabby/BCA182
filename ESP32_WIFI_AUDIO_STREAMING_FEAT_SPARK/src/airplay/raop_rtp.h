// Phase 4 RTP audio receive path (target-only glue). Owns ONE FreeRTOS task that
// select()s over the whole session's three UDP sockets (audio / control / timing):
//   * audio_fd   — reads RTP audio, inserts the ENCRYPTED payload into a
//                  seq-indexed reorder/jitter buffer, then drains it in RTP-seq
//                  order → AES-128-CBC decrypt → ALAC decode → audio_play_pcm().
//   * control_fd — sends resend REQUESTS (0xd5) on a detected gap and injects
//                  resend RESPONSES (0xd6, an encrypted audio packet) back into the
//                  reorder buffer by seq. Sync (0xd4) is ignored (free-run).
//   * timing_fd  — RECEIVER-INITIATED: periodically sends 0xd2 timing requests to
//                  the sender and consumes 0xd3 responses (discarded — we free-run,
//                  no clock discipline). A documented defensive belt answers an
//                  inbound 0xd2 with a 0xd3.
//
// Reorder-before-decode: each RAOP audio packet is one independently-decodable
// ALAC frame, and a resend response is just an encrypted audio packet, so buffering
// encrypted-by-seq lets late/out-of-order/recovered packets flow through the SINGLE
// decode path (spec §6c/§5c). An unfillable gap conceals (silence) and advances —
// it never stalls forever.
//
// Single-producer contract: raop_rtp_start() claims the audio producer slot and
// pins its task to audio_producer_core(); the caller MUST stop the diagnostic
// tone (the other producer) before calling it. raop_rtp_stop() blocks until the
// task has fully exited and released the producer slot.
#pragma once

#include "rtsp_session.h"

// Build the ALAC decoder from s->fmtp, import the AES key, allocate the reorder
// buffer (PSRAM), stash all three sockets + the sender's control/timing ports,
// claim the audio producer, and spawn the receive task on audio_producer_core().
// Requires s->have_key and a valid s->fmtp + bound s->audio_fd. Returns 0 on
// success, -1 on failure (on failure the producer slot is NOT held, so the caller
// can resume the tone).
int  raop_rtp_start(raop_session_t *s);

// Stop the receive task and BLOCK until it has exited and released the producer
// slot; free the reorder buffer, the decoder and the AES key. Idempotent — a no-op
// if the task is not running. Returns true if a running stream was actually stopped
// (so the caller knows to resume the pre-stream producer), false otherwise.
bool raop_rtp_stop(void);
