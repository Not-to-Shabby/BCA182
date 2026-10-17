#include "raop_crypto.h"

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/md.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "raop_crypto";
static mbedtls_pk_context s_pk;
static bool s_pk_loaded = false;

esp_err_t raop_crypto_init(void) {
    if (s_pk_loaded) {
        return ESP_OK;
    }

    mbedtls_pk_init(&s_pk);
    // mbedTLS signature: (ctx, key, keylen, pwd, pwdlen). keylen includes NUL for PEM.
    int rc = mbedtls_pk_parse_key(&s_pk, (const unsigned char *)raop_key_pem, raop_key_pem_len,
                                  NULL, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_parse_key: -0x%04x (embedded RAOP key corrupt?)", (unsigned)(-rc));
        mbedtls_pk_free(&s_pk);
        return ESP_FAIL;
    }

    s_pk_loaded = true;

    // --- Self-test: sign a 32-byte buffer -> expect 256-byte signature ---
    uint8_t buf32[32] = {0};
    uint8_t sig[256];
    size_t siglen = 0;
    if (raop_crypto_sign_challenge(buf32, sig, sizeof(sig), &siglen) != 0 || siglen != 256) {
        ESP_LOGE(TAG, "self-test sign failed (siglen=%u)", (unsigned)siglen);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "RAOP 2048-bit RSA key loaded; PKCS1-v1.5 sign & OAEP ready");
    return ESP_OK;
}

int raop_crypto_sign_challenge(const uint8_t buf32[32],
                               uint8_t *sig, size_t sig_cap, size_t *sig_len) {
    if (!s_pk_loaded || sig_cap < 256) {
        return -1;
    }

    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(s_pk);
    if (rsa == NULL) {
        return -1;
    }

    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    int rc = mbedtls_rsa_pkcs1_sign(rsa, NULL, NULL, MBEDTLS_RSA_PRIVATE,
                                   MBEDTLS_MD_NONE, 32, buf32, sig);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_rsa_pkcs1_sign failed: -0x%04x", (unsigned)(-rc));
        return -1;
    }

    *sig_len = 256;
    return 0;
}

int raop_crypto_decrypt_aeskey(const uint8_t *in, size_t in_len, uint8_t out16[16]) {
    if (!s_pk_loaded || in_len != 256) {
        return -1;
    }

    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(s_pk);
    if (rsa == NULL) {
        return -1;
    }

    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);

    size_t olen = 0;
    uint8_t tmp[256];
    int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, NULL, NULL, MBEDTLS_RSA_PRIVATE,
                                           NULL, 0, &olen, in, tmp, sizeof(tmp));
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_rsa_rsaes_oaep_decrypt failed: -0x%04x", (unsigned)(-rc));
        return -1;
    }

    if (olen != 16) {
        ESP_LOGE(TAG, "decrypted AES key length %u != 16", (unsigned)olen);
        return -1;
    }

    memcpy(out16, tmp, 16);
    return 0;
}
