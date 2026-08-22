/**
 * \file config-gnos.h
 *
 * \brief Mbed TLS configuration for the GNOS curl app.
 *
 * A client-only TLS 1.2 configuration: ECDHE-ECDSA/ECDHE-RSA with
 * AES-128-GCM and SHA-256, certificate chain verification against a PEM
 * root bundle, no server-side, no TLS < 1.2, no legacy ciphers.  Entropy
 * comes from the kernel's getrandom(2) via mbedtls_hardware_poll() (see
 * curl.c); wall time for certificate validity from the musl libc.
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ---- system support ---------------------------------------------------- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME                 /* musl time(): RTC-backed  */
#define MBEDTLS_HAVE_TIME_DATE            /* cert validity windows    */

/* ---- feature support --------------------------------------------------- */
#define MBEDTLS_CIPHER_MODE_GCM
#define MBEDTLS_PKCS1_V15                 /* RSA signatures in certs  */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

/* ---- modules ------------------------------------------------------------ */
#define MBEDTLS_AES_C
#define MBEDTLS_AESNI_C                   /* -mno-sse is off for user */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C                  /* PEM decode              */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C                   /* mbedtls_strerror()      */
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C                /* curl exports the peer key   */
#define MBEDTLS_PKCS5_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C                  /* P-384 chains commonly   */
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C

/* The curves modern servers actually serve. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

/* Entropy: the kernel's getrandom(2), reached through the default
 * mbedtls_platform_entropy_poll() (patched in entropy_poll.c to use
 * getrandom on Linux regardless of libc). */

/* Legacy hashes/ciphers curl's DIGEST/NTLM auth still use. */
#define MBEDTLS_MD4_C
#define MBEDTLS_MD5_C
#define MBEDTLS_DES_C

#include "mbedtls/check_config.h"

#endif /* MBEDTLS_CONFIG_H */
