#pragma once

#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <string>
#include <cstring>
#include <cstdint>

// Key derivation only. The legacy AES-128-CBC transport cipher (Encrypt/
// Decrypt/GenerateIV + its CTR_DRBG) was removed when the mesh moved to
// MeshCore, whose group channel does encrypt-then-MAC over the secret below.
class EncryptionUtils
{
public:
    static constexpr size_t KEY_SIZE = 16;   // legacy AES key width; still the
    static constexpr size_t IV_SIZE  = 16;   // size LoraMessageInterface reserves

    // PBKDF2-HMAC-SHA256 parameters. Fixed application salt so every device
    // derives the same key material from the same passphrase.
    static constexpr uint8_t  PBKDF2_SALT[]   = "CelestialWayfinder-LoRa-v1";
    static constexpr size_t   PBKDF2_SALT_LEN = sizeof(PBKDF2_SALT) - 1;
    static constexpr uint32_t PBKDF2_ITERS    = 10000;

    // Stretches a passphrase into `outLen` bytes of key material. Used for the
    // 32-byte MeshCore GroupChannel secret; the same passphrase yields a
    // matching secret across the fleet.
    static void DeriveKey(const std::string& password, uint8_t* out, size_t outLen)
    {
        mbedtls_pkcs5_pbkdf2_hmac_ext(
            MBEDTLS_MD_SHA256,
            reinterpret_cast<const uint8_t*>(password.c_str()), password.size(),
            PBKDF2_SALT, PBKDF2_SALT_LEN,
            PBKDF2_ITERS,
            outLen, out);
    }

    static void DeriveKey(const std::string& password, uint8_t key[KEY_SIZE])
    {
        DeriveKey(password, key, KEY_SIZE);
    }

    // No-op: kept only because CompassUtils calls it once at boot (it used to
    // seed the CBC IV DRBG). Harmless to leave in place.
    static bool SeedRng(const std::string& /*personalization*/ = "") { return true; }
};
