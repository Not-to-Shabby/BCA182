#include "setparam.h"
#include <ctype.h>

// Case-insensitive compare of the media type; the match must be followed by end,
// whitespace, or ';' so a longer type ("text/parametersX") does not match.
static int media_is(const char *ct, const char *want) {
    while (*want) {
        if (!*ct) return 0;
        if (tolower((unsigned char)*ct) != tolower((unsigned char)*want)) return 0;
        ct++; want++;
    }
    return (*ct == '\0' || *ct == ';' || *ct == ' ' || *ct == '\t' ||
            *ct == '\r' || *ct == '\n');
}

setparam_kind_t setparam_classify(const char *content_type) {
    if (!content_type) return SETPARAM_OTHER;
    while (*content_type == ' ' || *content_type == '\t') content_type++;
    if (media_is(content_type, "text/parameters"))          return SETPARAM_VOLUME;
    if (media_is(content_type, "application/x-dmap-tagged")) return SETPARAM_METADATA;
    return SETPARAM_OTHER;
}
