// Mbed TLS 3.x configuration: only what crypto.c needs - P-256 ECDSA and
// point multiplication, SHA-256 / HMAC, AES-256-CBC and a CTR-DRBG.
// Picked up through pico_mbedtls_config.h ("mbedtls_config.h" on the include
// path of this target).
#ifndef KGFIDO_MBEDTLS_CONFIG_H
#define KGFIDO_MBEDTLS_CONFIG_H

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES  // tables in flash, not built in RAM
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CTR_DRBG_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C  // required by MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C

#endif  // KGFIDO_MBEDTLS_CONFIG_H
