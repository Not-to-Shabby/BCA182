#include "raop_txt.h"

size_t raop_txt_build(raop_txt_item_t *items, size_t max_items) {
    static const raop_txt_item_t kRaop[RAOP_TXT_COUNT] = {
        {"txtvers", "1"},             // TXT record version (must be 1 for Apple parsers)
        {"ch", "2"},                  // audio channels: 2
        {"cn", "0,1"},                // audio codecs: PCM, ALAC
        {"et", "0,1"},                // encryption: none, RSA/AES
        {"sv", "false"},              // not a server
        {"da", "true"},               // digest authentication
        {"sr", "44100"},              // sample rate: 44100
        {"ss", "16"},                 // sample size: 16
        {"pw", "false"},              // password: false
        {"vn", "65537"},              // protocol version (Apple expects 65537)
        {"tp", "TCP,UDP"},            // transport
        {"md", "0,1,2"},              // metadata support
        {"am", "AirPort4,107"},       // Apple model identifier (required by Apple Music / iTunes)
        {"vs", "105.1"},              // AirPlay version string
        {"sf", "0x4"},                // status flags
        {"ek", "1"},                  // encryption key type
    };
    size_t n = (max_items < RAOP_TXT_COUNT) ? max_items : RAOP_TXT_COUNT;
    for (size_t i = 0; i < n; i++) items[i] = kRaop[i];
    return n;
}
