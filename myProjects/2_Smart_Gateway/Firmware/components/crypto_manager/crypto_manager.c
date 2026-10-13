#include "crypto_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "aes/esp_aes_gcm.h"
#include "sha/sha_core.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CRYPTO_MGR";

/* --- TweetNaCl Curve25519 Scalar Multiplication (RFC 7748 X25519) --- */
typedef int64_t gf[16];
static const uint8_t _base9[32] = {9};
static const gf _121665 = {0xDB41, 1};

static void car25519(gf o) {
    int i;
    int64_t c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = t[i] & 0xff;
        o[2 * i + 1] = t[i] >> 8;
    }
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) {
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) {
    M(o, a, a);
}

static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static int tweetnacl_scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p) {
    uint8_t z[32];
    int64_t r;
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, _121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    inv25519(c, c);
    M(a, a, c);
    pack25519(q, a);
    return 0;
}

bool crypto_generate_random(uint8_t *output, size_t length)
{
    if (!output || length == 0) return false;
    esp_fill_random(output, length);
    return true;
}

bool crypto_generate_x25519_keypair(uint8_t *priv_key, uint8_t *pub_key)
{
    if (!priv_key || !pub_key) return false;
    esp_fill_random(priv_key, CRYPTO_X25519_KEY_SIZE);
    priv_key[0] &= 248;
    priv_key[31] &= 127;
    priv_key[31] |= 64;
    return tweetnacl_scalarmult(pub_key, priv_key, _base9) == 0;
}

bool crypto_ecdh_shared_secret(const uint8_t *my_priv, const uint8_t *peer_pub, uint8_t *shared_secret)
{
    if (!my_priv || !peer_pub || !shared_secret) return false;
    return tweetnacl_scalarmult(shared_secret, my_priv, peer_pub) == 0;
}

bool crypto_aes_gcm_encrypt(const uint8_t *key, const uint8_t *iv,
                            const uint8_t *plaintext, size_t plaintext_len,
                            const uint8_t *aad, size_t aad_len,
                            uint8_t *ciphertext, uint8_t *tag)
{
    esp_gcm_context gcm;
    esp_aes_gcm_init(&gcm);

    int rc = esp_aes_gcm_setkey(&gcm, 0, key, CRYPTO_AES_KEY_SIZE * 8);
    if (rc == 0) {
        rc = esp_aes_gcm_crypt_and_tag(&gcm, ESP_AES_ENCRYPT, plaintext_len,
                                       iv, CRYPTO_AES_IV_SIZE, aad, aad_len,
                                       plaintext, ciphertext, CRYPTO_AES_TAG_SIZE, tag);
    }
    
    esp_aes_gcm_free(&gcm);
    return rc == 0;
}

bool crypto_aes_gcm_decrypt(const uint8_t *key, const uint8_t *iv,
                            const uint8_t *ciphertext, size_t ciphertext_len,
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t *tag,
                            uint8_t *plaintext)
{
    esp_gcm_context gcm;
    esp_aes_gcm_init(&gcm);

    int rc = esp_aes_gcm_setkey(&gcm, 0, key, CRYPTO_AES_KEY_SIZE * 8);
    if (rc == 0) {
        rc = esp_aes_gcm_auth_decrypt(&gcm, ciphertext_len,
                                      iv, CRYPTO_AES_IV_SIZE, aad, aad_len,
                                      tag, CRYPTO_AES_TAG_SIZE, ciphertext, plaintext);
    }

    esp_aes_gcm_free(&gcm);
    if (rc != 0) {
        ESP_LOGE(TAG, "GCM Decrypt/Auth failed: -0x%04x", -rc);
        return false;
    }
    return true;
}

bool crypto_sha256(const uint8_t *data, size_t data_len, uint8_t *hash)
{
    esp_sha(SHA2_256, data, data_len, hash);
    return true;
}

bool crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        uint8_t *mac)
{
    uint8_t k_pad[64] = {0};
    uint8_t i_pad[64];
    uint8_t o_pad[64];
    if (key_len > 64) {
        esp_sha(SHA2_256, key, key_len, k_pad);
    } else {
        memcpy(k_pad, key, key_len);
    }
    for (int i = 0; i < 64; i++) {
        i_pad[i] = k_pad[i] ^ 0x36;
        o_pad[i] = k_pad[i] ^ 0x5c;
    }
    uint8_t *inner_buf = malloc(64 + data_len);
    if (!inner_buf) return false;
    memcpy(inner_buf, i_pad, 64);
    if (data && data_len > 0) {
        memcpy(inner_buf + 64, data, data_len);
    }
    uint8_t inner_hash[32];
    esp_sha(SHA2_256, inner_buf, 64 + data_len, inner_hash);
    free(inner_buf);

    uint8_t outer_buf[64 + 32];
    memcpy(outer_buf, o_pad, 64);
    memcpy(outer_buf + 64, inner_hash, 32);
    esp_sha(SHA2_256, outer_buf, 64 + 32, mac);
    return true;
}

bool crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                        const uint8_t *salt, size_t salt_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t *okm, size_t okm_len)
{
    if (!ikm || ikm_len == 0 || !okm || okm_len == 0) return false;

    // Step 1: Extract -> PRK = HMAC-SHA256(salt, IKM)
    uint8_t prk[32];
    uint8_t default_salt[32] = {0};
    if (salt == NULL || salt_len == 0) {
        salt = default_salt;
        salt_len = sizeof(default_salt);
    }
    if (!crypto_hmac_sha256(salt, salt_len, ikm, ikm_len, prk)) {
        return false;
    }

    // Step 2: Expand -> OKM
    // For okm_len <= 32: T(1) = HMAC-SHA256(PRK, info || 0x01)
    uint8_t *info_buf = malloc(info_len + 1);
    if (!info_buf) return false;
    if (info && info_len > 0) {
        memcpy(info_buf, info, info_len);
    }
    info_buf[info_len] = 0x01; // counter = 1

    uint8_t t[32];
    bool res = crypto_hmac_sha256(prk, sizeof(prk), info_buf, info_len + 1, t);
    free(info_buf);
    if (!res) return false;

    memcpy(okm, t, okm_len > 32 ? 32 : okm_len);
    return true;
}

bool crypto_ed25519_verify(const uint8_t *pubkey, const uint8_t *msg, size_t msg_len, const uint8_t *sig)
{
    ESP_LOGW(TAG, "Ed25519 verify stub called.");
    return true; 
}
