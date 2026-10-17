#include "raop_txt.h"

size_t raop_txt_build(raop_txt_item_t *items, size_t max_items) {
    static const raop_txt_item_t kRaop[RAOP_TXT_COUNT] = {
        {"tp", "UDP"},    // transport: audio over UDP/RTP
        {"sr", "44100"},  // sample rate
        {"ss", "16"},     // sample size (bits)
        {"ch", "2"},      // channels (stereo)
        {"cn", "1"},      // codecs: ALAC
        {"et", "0,1"},    // encryption: none + RSA/AES
        {"sv", "false"},  // not a "server" advertising extra features
        {"da", "true"},   // digest auth available
        {"vn", "3"},      // AirTunes protocol version
        {"md", "0,1,2"},  // metadata: text (0), artwork(1)/progress(2) declared
    };
    size_t n = (max_items < RAOP_TXT_COUNT) ? max_items : RAOP_TXT_COUNT;
    for (size_t i = 0; i < n; i++) items[i] = kRaop[i];
    return n;
}
