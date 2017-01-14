/*
 * encrypt.c - Manage the global encryptor
 *
 * Copyright (C) 2013 - 2016, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(USE_CRYPTO_MBEDTLS)

#include <mbedtls/md5.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/version.h>
#define CIPHER_UNSUPPORTED "unsupported"

#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <stdio.h>
#endif

#endif

#include <sodium.h>

#ifndef __MINGW32__
#include <arpa/inet.h>
#endif

#include "cache.h"
#include "encrypt.h"
#include "utils.h"

#define OFFSET_ROL(p, o) ((uint64_t)(*(p + o)) << (8 * o))

static uint8_t *enc_table;
static uint8_t *dec_table;
static uint8_t enc_key[MAX_KEY_LENGTH];
static int enc_key_len;
static int enc_iv_len;
static int enc_method;

static struct cache *iv_cache;

#ifdef DEBUG
static void
dump(char *tag, char *text, int len)
{
    int i;
    printf("%s: ", tag);
    for (i = 0; i < len; i++)
        printf("0x%02x ", (uint8_t)text[i]);
    printf("\n");
}

#endif

static const char *supported_ciphers[CIPHER_NUM] = {
    "table",
    "rc4",
    "rc4-md5",
    "aes-128-cfb",
    "aes-192-cfb",
    "aes-256-cfb",
    "aes-128-ctr",
    "aes-192-ctr",
    "aes-256-ctr",
    "bf-cfb",
    "camellia-128-cfb",
    "camellia-192-cfb",
    "camellia-256-cfb",
    "cast5-cfb",
    "des-cfb",
    "idea-cfb",
    "rc2-cfb",
    "seed-cfb",
    "salsa20",
    "chacha20",
    "chacha20-ietf"
};

#ifdef USE_CRYPTO_MBEDTLS
static const char *supported_ciphers_mbedtls[CIPHER_NUM] = {
    "table",
    "ARC4-128",
    "ARC4-128",
    "AES-128-CFB128",
    "AES-192-CFB128",
    "AES-256-CFB128",
    "AES-128-CTR",
    "AES-192-CTR",
    "AES-256-CTR",
    "BLOWFISH-CFB64",
    "CAMELLIA-128-CFB128",
    "CAMELLIA-192-CFB128",
    "CAMELLIA-256-CFB128",
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    CIPHER_UNSUPPORTED,
    "salsa20",
    "chacha20",
    "chacha20-ietf"
};
#endif

static const int supported_ciphers_iv_size[CIPHER_NUM] = {
    0, 0, 16, 16, 16, 16, 16, 16, 16, 8, 16, 16, 16, 8, 8, 8, 8, 16, 8, 8, 12
};

static const int supported_ciphers_key_size[CIPHER_NUM] = {
    0, 16, 16, 16, 24, 32, 16, 24, 32, 16, 16, 24, 32, 16, 8, 16, 16, 16, 32, 32, 32
};

static int
safe_memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *_s1 = (const unsigned char *)s1;
    const unsigned char *_s2 = (const unsigned char *)s2;
    int ret                  = 0;
    size_t i;
    for (i = 0; i < n; i++)
        ret |= _s1[i] ^ _s2[i];
    return !!ret;
}

int
balloc(buffer_t *ptr, size_t capacity)
{
    sodium_memzero(ptr, sizeof(buffer_t));
    ptr->data    = ss_malloc(capacity);
    ptr->capacity = capacity;
    return capacity;
}

int
brealloc(buffer_t *ptr, size_t len, size_t capacity)
{
    if (ptr == NULL)
        return -1;
    size_t real_capacity = max(len, capacity);
    if (ptr->capacity < real_capacity) {
        ptr->data    = ss_realloc(ptr->data, real_capacity);
        ptr->capacity = real_capacity;
    }
    return real_capacity;
}

void
bfree(buffer_t *ptr)
{
    if (ptr == NULL)
        return;
    ptr->idx      = 0;
    ptr->len      = 0;
    ptr->capacity = 0;
    if (ptr->data != NULL) {
        ss_free(ptr->data);
    }
}

static int
crypto_stream_xor_ic(uint8_t *c, const uint8_t *m, uint64_t mlen,
                     const uint8_t *n, uint64_t ic, const uint8_t *k,
                     int method)
{
    switch (method) {
    case SALSA20:
        return crypto_stream_salsa20_xor_ic(c, m, mlen, n, ic, k);
    case CHACHA20:
        return crypto_stream_chacha20_xor_ic(c, m, mlen, n, ic, k);
    case CHACHA20IETF:
        return crypto_stream_chacha20_ietf_xor_ic(c, m, mlen, n, (uint32_t)ic, k);
    }
    // always return 0
    return 0;
}

static int
random_compare(const void *_x, const void *_y, uint32_t i,
               uint64_t a)
{
    uint8_t x = *((uint8_t *)_x);
    uint8_t y = *((uint8_t *)_y);
    return a % (x + i) - a % (y + i);
}

static void
merge(uint8_t *left, int llength, uint8_t *right,
      int rlength, uint32_t salt, uint64_t key)
{
    uint8_t *ltmp = (uint8_t *)malloc(llength * sizeof(uint8_t));
    uint8_t *rtmp = (uint8_t *)malloc(rlength * sizeof(uint8_t));

    uint8_t *ll = ltmp;
    uint8_t *rr = rtmp;

    uint8_t *result = left;

    memcpy(ltmp, left, llength * sizeof(uint8_t));
    memcpy(rtmp, right, rlength * sizeof(uint8_t));

    while (llength > 0 && rlength > 0) {
        if (random_compare(ll, rr, salt, key) <= 0) {
            *result = *ll;
            ++ll;
            --llength;
        } else {
            *result = *rr;
            ++rr;
            --rlength;
        }
        ++result;
    }

    if (llength > 0) {
        while (llength > 0) {
            *result = *ll;
            ++result;
            ++ll;
            --llength;
        }
    } else {
        while (rlength > 0) {
            *result = *rr;
            ++result;
            ++rr;
            --rlength;
        }
    }

    ss_free(ltmp);
    ss_free(rtmp);
}

static void
merge_sort(uint8_t data[], int length,
           uint32_t salt, uint64_t key)
{
    uint8_t middle;
    uint8_t *left, *right;
    int llength;

    if (length <= 1) {
        return;
    }

    middle = length / 2;

    llength = length - middle;

    left  = data;
    right = data + llength;

    merge_sort(left, llength, salt, key);
    merge_sort(right, middle, salt, key);
    merge(left, llength, right, middle, salt, key);
}

int
enc_get_iv_len()
{
    return enc_iv_len;
}

unsigned char *
enc_md5(const unsigned char *d, size_t n, unsigned char *md)
{
#if defined(USE_CRYPTO_MBEDTLS)
    static unsigned char m[16];
    if (md == NULL) {
        md = m;
    }
    mbedtls_md5(d, n, md);
    return md;
#endif
}

void
enc_table_init(const char *pass)
{
    uint32_t i;
    uint64_t key = 0;
    uint8_t *digest;

    enc_table = ss_malloc(256);
    dec_table = ss_malloc(256);

    digest = enc_md5((const uint8_t *)pass, strlen(pass), NULL);

    for (i = 0; i < 8; i++)
        key += OFFSET_ROL(digest, i);

    for (i = 0; i < 256; ++i)
        enc_table[i] = i;
    for (i = 1; i < 1024; ++i)
        merge_sort(enc_table, 256, i, key);
    for (i = 0; i < 256; ++i)
        // gen decrypt table from encrypt table
        dec_table[enc_table[i]] = i;
}

int
cipher_iv_size(const cipher_t *cipher)
{
#if defined(USE_CRYPTO_MBEDTLS)
    if (cipher == NULL) {
        return 0;
    }
    return cipher->info->iv_size;
#endif
}

int
cipher_key_size(const cipher_t *cipher)
{
#if defined(USE_CRYPTO_MBEDTLS)
    if (cipher == NULL) {
        return 0;
    }
    /* From Version 1.2.7 released 2013-04-13 Default Blowfish keysize is now 128-bits */
    return cipher->info->key_bitlen / 8;
#endif
}

int
bytes_to_key(const cipher_t *cipher, const digest_type_t *md,
             const uint8_t *pass, uint8_t *key)
{
    size_t datal;
    datal = strlen((const char *)pass);

#if defined(USE_CRYPTO_MBEDTLS)

    mbedtls_md_context_t c;
    unsigned char md_buf[MAX_MD_SIZE];
    int nkey;
    int addmd;
    unsigned int i, j, mds;

    nkey = cipher_key_size(cipher);
    mds  = mbedtls_md_get_size(md);
    memset(&c, 0, sizeof(mbedtls_md_context_t));

    if (pass == NULL)
        return nkey;
    if (mbedtls_md_setup(&c, md, 1))
        return 0;

    for (j = 0, addmd = 0; j < nkey; addmd++) {
        mbedtls_md_starts(&c);
        if (addmd) {
            mbedtls_md_update(&c, md_buf, mds);
        }
        mbedtls_md_update(&c, pass, datal);
        mbedtls_md_finish(&c, &(md_buf[0]));

        for (i = 0; i < mds; i++, j++) {
            if (j >= nkey)
                break;
            key[j] = md_buf[i];
        }
    }

    mbedtls_md_free(&c);
    return nkey;
#endif
}

int
rand_bytes(void *output, int len)
{
    randombytes_buf(output, len);
    // always return success
    return 0;
}

const cipher_kt_t *
get_cipher_type(int method)
{
    if (method <= TABLE || method >= CIPHER_NUM) {
        LOGE("get_cipher_type(): Illegal method");
        return NULL;
    }

    if (method == RC4_MD5) {
        method = RC4;
    }

    if (method >= SALSA20) {
        return NULL;
    }

    const char *ciphername = supported_ciphers[method];
#if defined(USE_CRYPTO_MBEDTLS)
    const char *mbedtlsname = supported_ciphers_mbedtls[method];
    if (strcmp(mbedtlsname, CIPHER_UNSUPPORTED) == 0) {
        LOGE("Cipher %s currently is not supported by mbed TLS library",
             ciphername);
        return NULL;
    }
    return mbedtls_cipher_info_from_string(mbedtlsname);
#endif
}

const digest_type_t *
get_digest_type(const char *digest)
{
    if (digest == NULL) {
        LOGE("get_digest_type(): Digest name is null");
        return NULL;
    }

#if defined(USE_CRYPTO_MBEDTLS)
    return mbedtls_md_info_from_string(digest);
#endif
}

void
cipher_context_init(cipher_ctx_t *ctx, int method, int enc)
{
    if (method <= TABLE || method >= CIPHER_NUM) {
        LOGE("cipher_context_init(): Illegal method");
        return;
    }

    if (method >= SALSA20) {
        enc_iv_len = supported_ciphers_iv_size[method];
        return;
    }

    const char *ciphername = supported_ciphers[method];

    const cipher_kt_t *cipher = get_cipher_type(method);

#if defined(USE_CRYPTO_MBEDTLS)
    ctx->evp = ss_malloc(sizeof(cipher_evp_t));
    memset(ctx->evp, 0, sizeof(cipher_evp_t));
    cipher_evp_t *evp = ctx->evp;

    if (cipher == NULL) {
        LOGE("Cipher %s not found in mbed TLS library", ciphername);
        FATAL("Cannot initialize mbed TLS cipher");
    }
    mbedtls_cipher_init(evp);
    if (mbedtls_cipher_setup(evp, cipher) != 0) {
        FATAL("Cannot initialize mbed TLS cipher context");
    }
#endif
}

void
cipher_context_set_iv(cipher_ctx_t *ctx, uint8_t *iv, size_t iv_len,
                      int enc)
{
    const unsigned char *true_key;

    if (iv == NULL) {
        LOGE("cipher_context_set_iv(): IV is null");
        return;
    }

    if (!enc) {
        memcpy(ctx->iv, iv, iv_len);
    }

    if (enc_method >= SALSA20) {
        return;
    }

    if (enc_method == RC4_MD5) {
        unsigned char key_iv[32];
        memcpy(key_iv, enc_key, 16);
        memcpy(key_iv + 16, iv, 16);
        true_key = enc_md5(key_iv, 32, NULL);
        iv_len   = 0;
    } else {
        true_key = enc_key;
    }

    cipher_evp_t *evp = ctx->evp;
    if (evp == NULL) {
        LOGE("cipher_context_set_iv(): Cipher context is null");
        return;
    }
#if defined(USE_CRYPTO_MBEDTLS)
    if (mbedtls_cipher_setkey(evp, true_key, enc_key_len * 8, enc) != 0) {
        mbedtls_cipher_free(evp);
        FATAL("Cannot set mbed TLS cipher key");
    }

    if (mbedtls_cipher_set_iv(evp, iv, iv_len) != 0) {
        mbedtls_cipher_free(evp);
        FATAL("Cannot set mbed TLS cipher IV");
    }
    if (mbedtls_cipher_reset(evp) != 0) {
        mbedtls_cipher_free(evp);
        FATAL("Cannot finalize mbed TLS cipher context");
    }
#endif

#ifdef DEBUG
    dump("IV", (char *)iv, iv_len);
#endif
}

void
cipher_context_release(cipher_ctx_t *ctx)
{
    if (enc_method >= SALSA20) {
        return;
    }

#if defined(USE_CRYPTO_MBEDTLS)
    mbedtls_cipher_free(ctx->evp);
    ss_free(ctx->evp);
#endif
}

static int
cipher_context_update(cipher_ctx_t *ctx, uint8_t *output, size_t *olen,
                      const uint8_t *input, size_t ilen)
{
    cipher_evp_t *evp = ctx->evp;
#if defined(USE_CRYPTO_MBEDTLS)
    return !mbedtls_cipher_update(evp, (const uint8_t *)input, ilen,
                                  (uint8_t *)output, olen);
#endif
}

int
ss_onetimeauth(buffer_t *buf, uint8_t *iv, size_t capacity)
{
    uint8_t hash[ONETIMEAUTH_BYTES * 2];
    uint8_t auth_key[MAX_IV_LENGTH + MAX_KEY_LENGTH];
    memcpy(auth_key, iv, enc_iv_len);
    memcpy(auth_key + enc_iv_len, enc_key, enc_key_len);

    brealloc(buf, ONETIMEAUTH_BYTES + buf->len, capacity);

#if defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(
                        MBEDTLS_MD_SHA1), auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->data, buf->len,
                    (uint8_t *)hash);
#endif

    memcpy(buf->data + buf->len, hash, ONETIMEAUTH_BYTES);
    buf->len += ONETIMEAUTH_BYTES;

    return 0;
}

int
ss_onetimeauth_verify(buffer_t *buf, uint8_t *iv)
{
    uint8_t hash[ONETIMEAUTH_BYTES * 2];
    uint8_t auth_key[MAX_IV_LENGTH + MAX_KEY_LENGTH];
    memcpy(auth_key, iv, enc_iv_len);
    memcpy(auth_key + enc_iv_len, enc_key, enc_key_len);
    size_t len = buf->len - ONETIMEAUTH_BYTES;

#if defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(
                        MBEDTLS_MD_SHA1), auth_key, enc_iv_len + enc_key_len, (uint8_t *)buf->data, len, hash);
#endif

    return safe_memcmp(buf->data + len, hash, ONETIMEAUTH_BYTES);
}

int
ss_encrypt_all(buffer_t *plain, int method, int auth, size_t capacity)
{
    if (method > TABLE) {
        cipher_ctx_t evp;
        cipher_context_init(&evp, method, 1);

        size_t iv_len = enc_iv_len;
        int err       = 1;

        static buffer_t tmp = { 0, 0, 0, NULL };
        brealloc(&tmp, iv_len + plain->len, capacity);
        buffer_t *cipher = &tmp;
        cipher->len = plain->len;

        uint8_t iv[MAX_IV_LENGTH];

        rand_bytes(iv, iv_len);
        cipher_context_set_iv(&evp, iv, iv_len, 1);
        memcpy(cipher->data, iv, iv_len);

        if (auth) {
            ss_onetimeauth(plain, iv, capacity);
            cipher->len = plain->len;
        }

        if (method >= SALSA20) {
            crypto_stream_xor_ic((uint8_t *)(cipher->data + iv_len),
                                 (const uint8_t *)plain->data, (uint64_t)(plain->len),
                                 (const uint8_t *)iv,
                                 0, enc_key, method);
        } else {
            err = cipher_context_update(&evp, (uint8_t *)(cipher->data + iv_len),
                                        &cipher->len, (const uint8_t *)plain->data,
                                        plain->len);
        }

        if (!err) {
            bfree(plain);
            cipher_context_release(&evp);
            return -1;
        }

#ifdef DEBUG
        dump("PLAIN", plain->data, plain->len);
        dump("CIPHER", cipher->data + iv_len, cipher->len);
#endif

        cipher_context_release(&evp);

        brealloc(plain, iv_len + cipher->len, capacity);
        memcpy(plain->data, cipher->data, iv_len + cipher->len);
        plain->len = iv_len + cipher->len;

        return 0;
    } else {
        char *begin = plain->data;
        char *ptr   = plain->data;
        while (ptr < begin + plain->len) {
            *ptr = (char)enc_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

int
ss_encrypt(buffer_t *plain, enc_ctx_t *ctx, size_t capacity)
{
    if (ctx != NULL) {
        static buffer_t tmp = { 0, 0, 0, NULL };

        int err       = 1;
        size_t iv_len = 0;
        if (!ctx->init) {
            iv_len = enc_iv_len;
        }

        brealloc(&tmp, iv_len + plain->len, capacity);
        buffer_t *cipher = &tmp;
        cipher->len = plain->len;

        if (!ctx->init) {
            cipher_context_set_iv(&ctx->evp, ctx->evp.iv, iv_len, 1);
            memcpy(cipher->data, ctx->evp.iv, iv_len);
            ctx->counter = 0;
            ctx->init    = 1;
        }

        if (enc_method >= SALSA20) {
            int padding = ctx->counter % SODIUM_BLOCK_SIZE;
            brealloc(cipher, iv_len + (padding + cipher->len) * 2, capacity);
            if (padding) {
                brealloc(plain, plain->len + padding, capacity);
                memmove(plain->data + padding, plain->data, plain->len);
                sodium_memzero(plain->data, padding);
            }
            crypto_stream_xor_ic((uint8_t *)(cipher->data + iv_len),
                                 (const uint8_t *)plain->data,
                                 (uint64_t)(plain->len + padding),
                                 (const uint8_t *)ctx->evp.iv,
                                 ctx->counter / SODIUM_BLOCK_SIZE, enc_key,
                                 enc_method);
            ctx->counter += plain->len;
            if (padding) {
                memmove(cipher->data + iv_len,
                        cipher->data + iv_len + padding, cipher->len);
            }
        } else {
            err =
                cipher_context_update(&ctx->evp,
                                      (uint8_t *)(cipher->data + iv_len),
                                      &cipher->len, (const uint8_t *)plain->data,
                                      plain->len);
            if (!err) {
                return -1;
            }
        }

#ifdef DEBUG
        dump("PLAIN", plain->data, plain->len);
        dump("CIPHER", cipher->data + iv_len, cipher->len);
#endif

        brealloc(plain, iv_len + cipher->len, capacity);
        memcpy(plain->data, cipher->data, iv_len + cipher->len);
        plain->len = iv_len + cipher->len;

        return 0;
    } else {
        char *begin = plain->data;
        char *ptr   = plain->data;
        while (ptr < begin + plain->len) {
            *ptr = (char)enc_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

int
ss_decrypt_all(buffer_t *cipher, int method, int auth, size_t capacity)
{
    if (method > TABLE) {
        size_t iv_len = enc_iv_len;
        int ret       = 1;

        if (cipher->len <= iv_len) {
            return -1;
        }

        cipher_ctx_t evp;
        cipher_context_init(&evp, method, 0);

        static buffer_t tmp = { 0, 0, 0, NULL };
        brealloc(&tmp, cipher->len, capacity);
        buffer_t *plain = &tmp;
        plain->len = cipher->len - iv_len;

        uint8_t iv[MAX_IV_LENGTH];
        memcpy(iv, cipher->data, iv_len);
        cipher_context_set_iv(&evp, iv, iv_len, 0);

        if (method >= SALSA20) {
            crypto_stream_xor_ic((uint8_t *)plain->data,
                                 (const uint8_t *)(cipher->data + iv_len),
                                 (uint64_t)(cipher->len - iv_len),
                                 (const uint8_t *)iv, 0, enc_key, method);
        } else {
            ret = cipher_context_update(&evp, (uint8_t *)plain->data, &plain->len,
                                        (const uint8_t *)(cipher->data + iv_len),
                                        cipher->len - iv_len);
        }

        if (auth || (plain->data[0] & ONETIMEAUTH_FLAG)) {
            if (plain->len > ONETIMEAUTH_BYTES) {
                ret = !ss_onetimeauth_verify(plain, iv);
                if (ret) {
                    plain->len -= ONETIMEAUTH_BYTES;
                }
            } else {
                ret = 0;
            }
        }

        if (!ret) {
            bfree(cipher);
            cipher_context_release(&evp);
            return -1;
        }

#ifdef DEBUG
        dump("PLAIN", plain->data, plain->len);
        dump("CIPHER", cipher->data + iv_len, cipher->len - iv_len);
#endif

        cipher_context_release(&evp);

        brealloc(cipher, plain->len, capacity);
        memcpy(cipher->data, plain->data, plain->len);
        cipher->len = plain->len;

        return 0;
    } else {
        char *begin = cipher->data;
        char *ptr   = cipher->data;
        while (ptr < begin + cipher->len) {
            *ptr = (char)dec_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

int
ss_decrypt(buffer_t *cipher, enc_ctx_t *ctx, size_t capacity)
{
    if (ctx != NULL) {
        static buffer_t tmp = { 0, 0, 0, NULL };

        size_t iv_len = 0;
        int err       = 1;

        brealloc(&tmp, cipher->len, capacity);
        buffer_t *plain = &tmp;
        plain->len = cipher->len;

        if (!ctx->init) {
            uint8_t iv[MAX_IV_LENGTH];
            iv_len      = enc_iv_len;
            plain->len -= iv_len;

            memcpy(iv, cipher->data, iv_len);
            cipher_context_set_iv(&ctx->evp, iv, iv_len, 0);
            ctx->counter = 0;
            ctx->init    = 1;

            if (enc_method >= RC4_MD5) {
                if (cache_key_exist(iv_cache, (char *)iv, iv_len)) {
                    bfree(cipher);
                    return -1;
                } else {
                    cache_insert(iv_cache, (char *)iv, iv_len, NULL);
                }
            }
        }

        if (enc_method >= SALSA20) {
            int padding = ctx->counter % SODIUM_BLOCK_SIZE;
            brealloc(plain, (plain->len + padding) * 2, capacity);

            if (padding) {
                brealloc(cipher, cipher->len + padding, capacity);
                memmove(cipher->data + iv_len + padding, cipher->data + iv_len,
                        cipher->len - iv_len);
                sodium_memzero(cipher->data + iv_len, padding);
            }
            crypto_stream_xor_ic((uint8_t *)plain->data,
                                 (const uint8_t *)(cipher->data + iv_len),
                                 (uint64_t)(cipher->len - iv_len + padding),
                                 (const uint8_t *)ctx->evp.iv,
                                 ctx->counter / SODIUM_BLOCK_SIZE, enc_key,
                                 enc_method);
            ctx->counter += cipher->len - iv_len;
            if (padding) {
                memmove(plain->data, plain->data + padding, plain->len);
            }
        } else {
            err = cipher_context_update(&ctx->evp, (uint8_t *)plain->data, &plain->len,
                                        (const uint8_t *)(cipher->data + iv_len),
                                        cipher->len - iv_len);
        }

        if (!err) {
            bfree(cipher);
            return -1;
        }

#ifdef DEBUG
        dump("PLAIN", plain->data, plain->len);
        dump("CIPHER", cipher->data + iv_len, cipher->len - iv_len);
#endif

        brealloc(cipher, plain->len, capacity);
        memcpy(cipher->data, plain->data, plain->len);
        cipher->len = plain->len;

        return 0;
    } else {
        char *begin = cipher->data;
        char *ptr   = cipher->data;
        while (ptr < begin + cipher->len) {
            *ptr = (char)dec_table[(uint8_t)*ptr];
            ptr++;
        }
        return 0;
    }
}

void
enc_ctx_init(int method, enc_ctx_t *ctx, int enc)
{
    sodium_memzero(ctx, sizeof(enc_ctx_t));
    cipher_context_init(&ctx->evp, method, enc);

    if (enc) {
        rand_bytes(ctx->evp.iv, enc_iv_len);
    }
}

void
enc_key_init(int method, const char *pass)
{
    if (method <= TABLE || method >= CIPHER_NUM) {
        LOGE("enc_key_init(): Illegal method");
        return;
    }

    // Initialize cache
    cache_create(&iv_cache, 1024, NULL);

    cipher_kt_t cipher_info;

    cipher_t cipher;
    memset(&cipher, 0, sizeof(cipher_t));

    // Initialize sodium for random generator
    if (sodium_init() == -1) {
        FATAL("Failed to initialize sodium");
    }

    if (method == SALSA20 || method == CHACHA20 || method == CHACHA20IETF) {
#if defined(USE_CRYPTO_MBEDTLS)
        // XXX: key_length changed to key_bitlen in mbed TLS 2.0.0
        cipher.info             = &cipher_info;
        cipher.info->base       = NULL;
        cipher.info->key_bitlen = supported_ciphers_key_size[method] * 8;
        cipher.info->iv_size    = supported_ciphers_iv_size[method];
#endif
    } else {
        cipher.info = (cipher_kt_t *)get_cipher_type(method);
    }

    if (cipher.info == NULL && cipher.key_len == 0) {
        do {
            LOGE("Cipher %s not found in crypto library", supported_ciphers[method]);
            FATAL("Cannot initialize cipher");
        } while (0);
    }

    const digest_type_t *md = get_digest_type("MD5");
    if (md == NULL) {
        FATAL("MD5 Digest not found in crypto library");
    }

    enc_key_len = bytes_to_key(&cipher, md, (const uint8_t *)pass, enc_key);

    if (enc_key_len == 0) {
        FATAL("Cannot generate key and IV");
    }
    if (method == RC4_MD5) {
        enc_iv_len = 16;
    } else {
        enc_iv_len = cipher_iv_size(&cipher);
    }
    enc_method = method;
}

int
enc_init(const char *pass, const char *method)
{
    int m = TABLE;
    if (method != NULL) {
        for (m = TABLE; m < CIPHER_NUM; m++)
            if (strcmp(method, supported_ciphers[m]) == 0) {
                break;
            }
        if (m >= CIPHER_NUM) {
            LOGE("Invalid cipher name: %s, use rc4-md5 instead", method);
            m = RC4_MD5;
        }
    }
    if (m == TABLE) {
        enc_table_init(pass);
    } else {
        enc_key_init(m, pass);
    }
    return m;
}

int
ss_check_hash(buffer_t *buf, chunk_t *chunk, enc_ctx_t *ctx, size_t capacity)
{
    int i, j, k;
    ssize_t blen  = buf->len;
    uint32_t cidx = chunk->idx;

    brealloc(chunk->buf, chunk->len + blen, capacity);
    brealloc(buf, chunk->len + blen, capacity);

    for (i = 0, j = 0, k = 0; i < blen; i++) {
        chunk->buf->data[cidx++] = buf->data[k++];

        if (cidx == CLEN_BYTES) {
            uint16_t clen = ntohs(*((uint16_t *)chunk->buf->data));
            brealloc(chunk->buf, clen + AUTH_BYTES, capacity);
            chunk->len = clen;
        }

        if (cidx == chunk->len + AUTH_BYTES) {
            // Compare hash
            uint8_t hash[ONETIMEAUTH_BYTES * 2];
            uint8_t key[MAX_IV_LENGTH + sizeof(uint32_t)];

            uint32_t c = htonl(chunk->counter);
            memcpy(key, ctx->evp.iv, enc_iv_len);
            memcpy(key + enc_iv_len, &c, sizeof(uint32_t));
#if defined(USE_CRYPTO_MBEDTLS)
            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, enc_iv_len + sizeof(uint32_t),
                            (uint8_t *)chunk->buf->data + AUTH_BYTES, chunk->len, hash);
#endif

            if (safe_memcmp(hash, chunk->buf->data + CLEN_BYTES, ONETIMEAUTH_BYTES) != 0) {
                return 0;
            }

            // Copy chunk back to buffer
            memmove(buf->data + j + chunk->len, buf->data + k, blen - i - 1);
            memcpy(buf->data + j, chunk->buf->data + AUTH_BYTES, chunk->len);

            // Reset the base offset
            j   += chunk->len;
            k    = j;
            cidx = 0;
            chunk->counter++;
        }
    }

    buf->len   = j;
    chunk->idx = cidx;
    return 1;
}

int
ss_gen_hash(buffer_t *buf, uint32_t *counter, enc_ctx_t *ctx, size_t capacity)
{
    ssize_t blen       = buf->len;
    uint16_t chunk_len = htons((uint16_t)blen);
    uint8_t hash[ONETIMEAUTH_BYTES * 2];
    uint8_t key[MAX_IV_LENGTH + sizeof(uint32_t)];
    uint32_t c = htonl(*counter);

    brealloc(buf, AUTH_BYTES + blen, capacity);
    memcpy(key, ctx->evp.iv, enc_iv_len);
    memcpy(key + enc_iv_len, &c, sizeof(uint32_t));
#if defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(
                        MBEDTLS_MD_SHA1), key, enc_iv_len + sizeof(uint32_t), (uint8_t *)buf->data, blen, hash);
#endif

    memmove(buf->data + AUTH_BYTES, buf->data, blen);
    memcpy(buf->data + CLEN_BYTES, hash, ONETIMEAUTH_BYTES);
    memcpy(buf->data, &chunk_len, CLEN_BYTES);

    *counter = *counter + 1;
    buf->len = blen + AUTH_BYTES;

    return 0;
}