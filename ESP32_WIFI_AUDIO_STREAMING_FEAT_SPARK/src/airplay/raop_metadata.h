// Current-track metadata store (spec §5b: "log it (display later)"). Holds the last
// title/artist/album and logs a single line WHEN A FIELD CHANGES (senders re-send
// metadata redundantly; we don't spam the log). Lives in raop so the audio core
// stays transport-agnostic. Single writer (the RTSP task); not reentrant.
#pragma once
#include "dmap.h"

// Merge parsed metadata into the store; log "now playing" if anything changed.
void raop_metadata_update(const dmap_meta_t *m);

// Reset to empty (TEARDOWN).
void raop_metadata_clear(void);
