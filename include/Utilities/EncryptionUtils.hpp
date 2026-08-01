#pragma once

#include <mbedtls/aes.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/ctr_drbg.h>
#include <esp_random.h>
#include <esp_log.h>
#include <string>
#include <cstring>
#include <algorithm>

class EncryptionUtils
{
public:
    static constexpr size_t KEY_SIZE = 16;
    static constexpr size_t IV_SIZE  = 16;

    // Derives a 128-bit AES key from a password via PBKDF2-HMAC-SHA256.
    // Uses a fixed application salt so all devices derive the same key from the same password.
    // Empty password must never be passed — callers must check EncryptionEnabled() first.
    static void DeriveKey(const std::string& password, uint8_t key[KEY_SIZE])
    {
        static constexpr uint8_t SALT[]     = "CelestialWayfinder-LoRa-v1";
        static constexpr size_t  SALT_LEN   = sizeof(SALT) - 1;
        static constexpr uint32_t ITERATIONS = 10000;

        mbedtls_pkcs5_pbkdf2_hmac_ext(
            MBEDTLS_MD_SHA256,
            reinterpret_cast<const uint8_t*>(password.c_str()), password.size(),
            SALT, SALT_LEN,
            ITERATIONS,
            KEY_SIZE, key);
    }

    // Seeds the CTR_DRBG that GenerateIV() draws from. Must be called once at
    // startup, from the window where a hardware entropy source is actually
    // running — either the RF subsystem is up, or bootloader_random_enable()
    // is in effect. esp_random() is only true-random under those conditions;
    // outside them it degrades to pseudo-random, and this device spends most
    // of its life with the radio off. Seeding once while entropy is real gives
    // unpredictable IVs forever after without touching the radio again.
    //
    // Returns false if seeding failed, in which case GenerateIV() falls back
    // to esp_random() directly.
    static bool SeedRng(const std::string& personalization = "")
    {
        mbedtls_ctr_drbg_init(&_Drbg());

        int rc = mbedtls_ctr_drbg_seed(
            &_Drbg(), _HardwareEntropy, nullptr,
            reinterpret_cast<const uint8_t*>(personalization.c_str()),
            personalization.size());

        if (rc != 0)
        {
            ESP_LOGE("EncryptionUtils", "mbedtls_ctr_drbg_seed failed: -0x%04X", -rc);
            mbedtls_ctr_drbg_free(&_Drbg());
            _Seeded() = false;
            return false;
        }

        _Seeded() = true;
        return true;
    }

    // Fills iv[IV_SIZE] with cryptographically random bytes.
    //
    // Not thread-safe: CONFIG_MBEDTLS_THREADING_C is off, so the DRBG has no
    // internal lock. Only the LoRa send task draws from it (GenerateIV is
    // reached solely via SerializeMessage from SendQueueTask). Calling this
    // from a second task means adding a mutex here.
    static void GenerateIV(uint8_t iv[IV_SIZE])
    {
        static_assert(IV_SIZE % 4 == 0, "IV_SIZE must be a multiple of 4");

        if (_Seeded() && mbedtls_ctr_drbg_random(&_Drbg(), iv, IV_SIZE) == 0)
        {
            return;
        }

        // Unseeded or the draw failed. Still better than a predictable IV, and
        // true-random whenever the radio happens to be up.
        for (size_t i = 0; i < IV_SIZE; i += 4)
        {
            uint32_t r = esp_random();
            memcpy(iv + i, &r, 4);
        }
    }

    // AES-128-CBC + PKCS7 padding. out must be at least inLen + 16 bytes.
    static bool Encrypt(const uint8_t* in, size_t inLen,
                        uint8_t* out, size_t& outLen,
                        const uint8_t key[KEY_SIZE], const uint8_t iv[IV_SIZE])
    {
        size_t padLen    = KEY_SIZE - (inLen % KEY_SIZE);
        size_t paddedLen = inLen + padLen;

        if (paddedLen > MAX_PAYLOAD_SIZE) { return false; }

        uint8_t padded[MAX_PAYLOAD_SIZE];
        memcpy(padded, in, inLen);
        memset(padded + inLen, static_cast<uint8_t>(padLen), padLen);

        uint8_t ivCopy[IV_SIZE];
        memcpy(ivCopy, iv, IV_SIZE);

        mbedtls_aes_context ctx;
        mbedtls_aes_init(&ctx);
        mbedtls_aes_setkey_enc(&ctx, key, 128);
        mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, paddedLen, ivCopy, padded, out);
        mbedtls_aes_free(&ctx);

        outLen = paddedLen;
        return true;
    }

    // AES-128-CBC + PKCS7 strip. inLen must be a multiple of 16.
    static bool Decrypt(const uint8_t* in, size_t inLen,
                        uint8_t* out, size_t& outLen,
                        const uint8_t key[KEY_SIZE], const uint8_t iv[IV_SIZE])
    {
        if (inLen == 0 || inLen % KEY_SIZE != 0) { return false; }

        uint8_t ivCopy[IV_SIZE];
        memcpy(ivCopy, iv, IV_SIZE);

        mbedtls_aes_context ctx;
        mbedtls_aes_init(&ctx);
        mbedtls_aes_setkey_dec(&ctx, key, 128);
        mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, inLen, ivCopy, in, out);
        mbedtls_aes_free(&ctx);

        uint8_t padLen = out[inLen - 1];
        if (padLen == 0 || padLen > KEY_SIZE) { return false; }

        outLen = inLen - padLen;
        return true;
    }

private:
    static constexpr size_t MAX_PAYLOAD_SIZE = 528;

    static mbedtls_ctr_drbg_context& _Drbg()
    {
        static mbedtls_ctr_drbg_context drbg;
        return drbg;
    }

    static bool& _Seeded()
    {
        static bool seeded = false;
        return seeded;
    }

    // Entropy callback for the initial seed. esp_fill_random() is the same
    // source ESP-IDF wires into mbedtls by default; the quality of what it
    // returns is entirely down to the caller opening an entropy window first
    // (see SeedRng).
    static int _HardwareEntropy(void* ctx, unsigned char* out, size_t len)
    {
        (void)ctx;
        esp_fill_random(out, len);
        return 0;
    }
};
