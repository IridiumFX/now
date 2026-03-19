/*
 * now_trust.h — Signing, integrity, and trust (§17)
 *
 * Trust store management, signature verification,
 * and trust policy enforcement during procure.
 */
#ifndef NOW_TRUST_H
#define NOW_TRUST_H

#include "now.h"

/* Trust level (ascending strictness) */
typedef enum {
    NOW_TRUST_NONE = 0,     /* SHA-256 integrity only (default) */
    NOW_TRUST_SIGNED,       /* must be signed; key need not be known */
    NOW_TRUST_TRUSTED        /* must be signed by a key in trust store */
} NowTrustLevel;

/* Trust policy from now.pasta trust: section */
typedef struct {
    int require_signatures;  /* reject unsigned packages */
    int require_known_keys;  /* reject packages with unknown publisher keys */
} NowTrustPolicy;

/* A trusted key entry in the trust store */
typedef struct {
    char *scope;             /* group prefix, "group:artifact", or "*" */
    char *key;               /* public key (minisign format, base64) */
    char *comment;           /* human-readable description */
} NowTrustKey;

/* The trust store (~/.now/trust.pasta) */
typedef struct {
    NowTrustKey *keys;
    size_t       count;
    size_t       capacity;
} NowTrustStore;

/* ---- Trust store operations ---- */

/* Initialize an empty trust store */
NOW_API void now_trust_init(NowTrustStore *store);

/* Free a trust store and all keys */
NOW_API void now_trust_free(NowTrustStore *store);

/* Add a key to the trust store. Returns 0 on success. */
NOW_API int now_trust_add(NowTrustStore *store, const char *scope,
                           const char *key, const char *comment);

/* Find a key matching a coordinate (group or group:artifact).
 * Returns the first matching key, or NULL if none. */
NOW_API const NowTrustKey *now_trust_find(const NowTrustStore *store,
                                            const char *group,
                                            const char *artifact);

/* Check if a scope pattern matches a given group[:artifact].
 * Scope rules:
 *   "*"           — matches everything
 *   "org.acme"    — matches group prefix (dot-boundary)
 *   "org:artifact" — matches exact group:artifact */
NOW_API int now_trust_scope_matches(const char *scope, const char *group,
                                      const char *artifact);

/* Load trust store from ~/.now/trust.pasta. Returns 0 on success.
 * If file doesn't exist, store is left empty (not an error). */
NOW_API int now_trust_load(NowTrustStore *store, NowResult *result);

/* Save trust store to ~/.now/trust.pasta. Returns 0 on success. */
NOW_API int now_trust_save(const NowTrustStore *store, NowResult *result);

/* ---- Trust policy ---- */

/* Parse trust policy from a NowProject. */
NOW_API NowTrustPolicy now_trust_policy_from_project(const void *project);

/* Get the effective trust level from a policy. */
NOW_API NowTrustLevel now_trust_level(const NowTrustPolicy *policy);

/* ---- Ed25519 digital signatures (native, no external deps) ---- */

/* Generate an Ed25519 keypair from a 32-byte seed.
 * pub receives the 32-byte public key.
 * priv receives the 64-byte private key (seed || public key).
 * Returns 0 on success. */
NOW_API int now_ed25519_keypair(unsigned char pub[32],
                                 unsigned char priv[64],
                                 const unsigned char seed[32]);

/* Sign a message with an Ed25519 private key.
 * sig receives the 64-byte signature.
 * Returns 0 on success. */
NOW_API int now_ed25519_sign(unsigned char sig[64],
                               const unsigned char *msg, size_t msg_len,
                               const unsigned char priv[64]);

/* Verify an Ed25519 signature (64 bytes) over a message against a public key (32 bytes).
 * Returns 0 if valid, -1 if invalid. */
NOW_API int now_ed25519_verify(const unsigned char *sig,
                                const unsigned char *msg, size_t msg_len,
                                const unsigned char *pub_key);

/* Verify a .sig file (Ed25519 detached signature) against a file
 * using a base64-encoded public key string.
 * Returns 0 if valid, non-zero on error or invalid signature. */
NOW_API int now_verify_file(const char *archive_path, const char *sig_path,
                             const char *pubkey_b64, NowResult *result);

#endif /* NOW_TRUST_H */
