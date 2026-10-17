// Pure-C mapper: SDP `a=fmtp` line -> ALAC "magic cookie" parameters.
//
// The RAOP ANNOUNCE SDP carries the ALAC codec configuration as 12 whitespace-
// separated integers, e.g.  "96 352 0 16 40 10 14 2 255 0 0 44100"
// (optionally prefixed with the SDP attribute name "a=fmtp:").
//
//   index  value  meaning                       David Hammerton alac_file field
//   -----  -----  ----------------------------  ------------------------------
//    [0]     96   RTP payload type (NOT a codec field — ignored)
//    [1]    352   frames per packet             setinfo_max_samples_per_frame
//    [2]      0   compatible version            setinfo_7a
//    [3]     16   bit depth                     setinfo_sample_size
//    [4]     40   rice history mult   (pb)      setinfo_rice_historymult
//    [5]     10   rice initial history(mb)      setinfo_rice_initialhistory
//    [6]     14   rice k modifier     (kb)      setinfo_rice_kmodifier
//    [7]      2   channels                      setinfo_7f
//    [8]    255   max run                       setinfo_80
//    [9]      0   max frame bytes               setinfo_82
//   [10]      0   avg bit rate                  setinfo_86
//   [11]  44100   sample rate                   setinfo_8a_rate
//
// This unit is intentionally free of alac.h / ESP-IDF includes so the mapping is
// host-unit-testable as pure logic. The target glue copies these fields into the
// vendored decoder's alac_file struct (matching shairport-sync's init_alac_decoder).
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t frame_length;      // fmtp[1]  setinfo_max_samples_per_frame
    uint8_t  compat_version;    // fmtp[2]  setinfo_7a
    uint8_t  bit_depth;         // fmtp[3]  setinfo_sample_size
    uint8_t  pb;                // fmtp[4]  setinfo_rice_historymult
    uint8_t  mb;                // fmtp[5]  setinfo_rice_initialhistory
    uint8_t  kb;                // fmtp[6]  setinfo_rice_kmodifier
    uint8_t  num_channels;      // fmtp[7]  setinfo_7f
    uint16_t max_run;           // fmtp[8]  setinfo_80
    uint32_t max_frame_bytes;   // fmtp[9]  setinfo_82
    uint32_t avg_bitrate;       // fmtp[10] setinfo_86
    uint32_t sample_rate;       // fmtp[11] setinfo_8a_rate
} alac_cfg_t;

// Parse the 12 whitespace-separated integers from `fmtp` (an optional leading
// "a=fmtp:" is skipped). ints[1..11] populate *out; ints[0] (payload type) is
// ignored. Returns 0 on success, -1 if fewer than 12 integers are present or any
// value is out of its destination field's range (bounds untrusted input).
int alac_cfg_from_fmtp(const char *fmtp, alac_cfg_t *out);
