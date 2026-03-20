/*
 * now_manifest.c — Incremental build manifest (§2.6)
 *
 * Uses a simple SHA-256 implementation (public domain) for hashing.
 */
#include "now_manifest.h"
#include "now_fs.h"
#include "pasta.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* ---- Minimal SHA-256 (public domain, from Brad Conte) ---- */

typedef struct {
    unsigned char data[64];
    unsigned int  datalen;
    unsigned long long bitlen;
    unsigned int  state[8];
} SHA256_CTX;

static const unsigned int sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(a,b) (((a)>>(b))|((a)<<(32-(b))))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)  (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x)  (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static void sha256_transform(SHA256_CTX *ctx, const unsigned char data[]) {
    unsigned int a,b,c,d,e,f,g,h,t1,t2,m[64];
    int i;
    for (i=0; i<16; i++)
        m[i] = ((unsigned int)data[i*4]<<24)|((unsigned int)data[i*4+1]<<16)|
               ((unsigned int)data[i*4+2]<<8)|((unsigned int)data[i*4+3]);
    for (; i<64; i++)
        m[i] = SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i=0; i<64; i++) {
        t1=h+EP1(e)+CH(e,f,g)+sha256_k[i]+m[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen=0; ctx->bitlen=0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const unsigned char data[], size_t len) {
    for (size_t i=0; i<len; i++) {
        ctx->data[ctx->datalen]=data[i];
        ctx->datalen++;
        if (ctx->datalen==64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen+=512;
            ctx->datalen=0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, unsigned char hash[32]) {
    unsigned int i=ctx->datalen;
    if (i<56) {
        ctx->data[i++]=0x80;
        while (i<56) ctx->data[i++]=0;
    } else {
        ctx->data[i++]=0x80;
        while (i<64) ctx->data[i++]=0;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (unsigned long long)ctx->datalen * 8;
    ctx->data[63]=(unsigned char)(ctx->bitlen);
    ctx->data[62]=(unsigned char)(ctx->bitlen>>8);
    ctx->data[61]=(unsigned char)(ctx->bitlen>>16);
    ctx->data[60]=(unsigned char)(ctx->bitlen>>24);
    ctx->data[59]=(unsigned char)(ctx->bitlen>>32);
    ctx->data[58]=(unsigned char)(ctx->bitlen>>40);
    ctx->data[57]=(unsigned char)(ctx->bitlen>>48);
    ctx->data[56]=(unsigned char)(ctx->bitlen>>56);
    sha256_transform(ctx, ctx->data);
    for (i=0; i<4; i++) {
        hash[i]    =(unsigned char)((ctx->state[0]>>(24-i*8))&0xff);
        hash[i+4]  =(unsigned char)((ctx->state[1]>>(24-i*8))&0xff);
        hash[i+8]  =(unsigned char)((ctx->state[2]>>(24-i*8))&0xff);
        hash[i+12] =(unsigned char)((ctx->state[3]>>(24-i*8))&0xff);
        hash[i+16] =(unsigned char)((ctx->state[4]>>(24-i*8))&0xff);
        hash[i+20] =(unsigned char)((ctx->state[5]>>(24-i*8))&0xff);
        hash[i+24] =(unsigned char)((ctx->state[6]>>(24-i*8))&0xff);
        hash[i+28] =(unsigned char)((ctx->state[7]>>(24-i*8))&0xff);
    }
}

/* ---- Hex encoding ---- */

static char *to_hex(const unsigned char *hash, size_t len) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return NULL;
    for (size_t i = 0; i < len; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);
    hex[len * 2] = '\0';
    return hex;
}

/* ---- Public SHA-256 API ---- */

NOW_API char *now_sha256_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    SHA256_CTX ctx;
    sha256_init(&ctx);

    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        sha256_update(&ctx, buf, n);
    fclose(fp);

    unsigned char hash[32];
    sha256_final(&ctx, hash);
    return to_hex(hash, 32);
}

NOW_API char *now_sha256_string(const char *data, size_t len) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char *)data, len);
    unsigned char hash[32];
    sha256_final(&ctx, hash);
    return to_hex(hash, 32);
}

/* ---- Manifest operations ---- */

NOW_API void now_manifest_init(NowManifest *m) {
    memset(m, 0, sizeof(*m));
}

NOW_API void now_manifest_free(NowManifest *m) {
    for (size_t i = 0; i < m->count; i++) {
        free(m->entries[i].source);
        free(m->entries[i].object);
        free(m->entries[i].source_hash);
        free(m->entries[i].flags_hash);
        for (size_t d = 0; d < m->entries[i].dep_count; d++) {
            free(m->entries[i].deps[d]);
            free(m->entries[i].dep_hashes[d]);
        }
        free(m->entries[i].deps);
        free(m->entries[i].dep_hashes);
        free(m->entries[i].dep_mtimes);
    }
    free(m->entries);
    free(m->link_flags_hash);
    now_manifest_init(m);
}

NOW_API const NowManifestEntry *now_manifest_find(const NowManifest *m,
                                            const char *source) {
    if (!m || !source) return NULL;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].source, source) == 0)
            return &m->entries[i];
    }
    return NULL;
}

NOW_API int now_manifest_set(NowManifest *m, const char *source,
                      const char *object, const char *source_hash,
                      const char *flags_hash, long long mtime) {
    /* Update existing entry if found */
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].source, source) == 0) {
            NowManifestEntry *e = &m->entries[i];
            free(e->object);
            free(e->source_hash);
            free(e->flags_hash);
            e->object      = object ? strdup(object) : NULL;
            e->source_hash = source_hash ? strdup(source_hash) : NULL;
            e->flags_hash  = flags_hash ? strdup(flags_hash) : NULL;
            e->mtime       = mtime;
            return 0;
        }
    }

    /* Add new entry */
    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity ? m->capacity * 2 : 16;
        NowManifestEntry *tmp = realloc(m->entries, new_cap * sizeof(NowManifestEntry));
        if (!tmp) return -1;
        m->entries = tmp;
        m->capacity = new_cap;
    }

    NowManifestEntry *e = &m->entries[m->count];
    memset(e, 0, sizeof(*e));
    e->source      = strdup(source);
    e->object      = object ? strdup(object) : NULL;
    e->source_hash = source_hash ? strdup(source_hash) : NULL;
    e->flags_hash  = flags_hash ? strdup(flags_hash) : NULL;
    e->mtime       = mtime;
    m->count++;
    return 0;
}

NOW_API int now_manifest_set_deps(NowManifest *m, const char *source,
                                   const char **deps, const char **dep_hashes,
                                   size_t dep_count) {
    if (!m || !source) return -1;

    /* Find existing entry */
    NowManifestEntry *e = NULL;
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].source, source) == 0) {
            e = &m->entries[i];
            break;
        }
    }
    if (!e) return -1;

    /* Free old deps */
    for (size_t i = 0; i < e->dep_count; i++) {
        free(e->deps[i]);
        free(e->dep_hashes[i]);
    }
    free(e->deps);
    free(e->dep_hashes);
    free(e->dep_mtimes);
    e->deps = NULL;
    e->dep_hashes = NULL;
    e->dep_mtimes = NULL;
    e->dep_count = 0;
    e->dep_mtime_count = 0;

    if (dep_count == 0 || !deps || !dep_hashes) return 0;

    e->deps = (char **)malloc(dep_count * sizeof(char *));
    e->dep_hashes = (char **)malloc(dep_count * sizeof(char *));
    e->dep_mtimes = (long long *)malloc(dep_count * sizeof(long long));
    if (!e->deps || !e->dep_hashes || !e->dep_mtimes) {
        free(e->deps);
        free(e->dep_hashes);
        free(e->dep_mtimes);
        e->deps = NULL;
        e->dep_hashes = NULL;
        e->dep_mtimes = NULL;
        return -1;
    }

    for (size_t i = 0; i < dep_count; i++) {
        e->deps[i] = strdup(deps[i]);
        e->dep_hashes[i] = strdup(dep_hashes[i]);
        e->dep_mtimes[i] = 0; /* populated by save/load cycle, not by set_deps */
    }
    e->dep_count = dep_count;
    e->dep_mtime_count = 0; /* no valid mtimes until loaded from manifest */
    return 0;
}

NOW_API int now_manifest_needs_rebuild(const NowManifestEntry *entry,
                                const char *basedir,
                                const char *source,
                                const char *flags_hash) {
    if (!entry) return 1;  /* no previous entry → rebuild */

    /* Check flags changed */
    if (!entry->flags_hash || !flags_hash ||
        strcmp(entry->flags_hash, flags_hash) != 0)
        return 1;

    /* Check source mtime changed (fast path) */
    char *full = now_path_join(basedir, source);
    if (!full) return 1;

    struct stat st;
    if (stat(full, &st) != 0) { free(full); return 1; }

    long long cur_mtime = (long long)st.st_mtime;
    if (cur_mtime != entry->mtime) {
        /* mtime differs — hash to confirm */
        char *cur_hash = now_sha256_file(full);
        free(full);
        if (!cur_hash) return 1;

        int changed = !entry->source_hash ||
                      strcmp(cur_hash, entry->source_hash) != 0;
        free(cur_hash);
        return changed;
    }

    free(full);

    /* Check object file exists */
    if (entry->object && !now_path_exists(entry->object))
        return 1;

    /* Check header dependencies — mtime fast path, then hash */
    for (size_t d = 0; d < entry->dep_count; d++) {
        if (!entry->deps[d] || !entry->dep_hashes[d])
            return 1;
        struct stat dst;
        if (stat(entry->deps[d], &dst) != 0)
            return 1;  /* dep deleted or unreadable */
        /* Fast path: if dep has mtime stored and it matches, skip hash */
        if (d < entry->dep_mtime_count && entry->dep_mtimes &&
            (long long)dst.st_mtime == entry->dep_mtimes[d])
            continue;
        /* mtime differs or not tracked — hash to confirm */
        char *dh = now_sha256_file(entry->deps[d]);
        if (!dh) return 1;
        int changed = strcmp(dh, entry->dep_hashes[d]) != 0;
        free(dh);
        if (changed) return 1;
    }

    return 0;  /* up to date */
}

/* ---- Serialization (Pasta format) ---- */

NOW_API int now_manifest_load(NowManifest *m, const char *path) {
    now_manifest_init(m);

    if (!now_path_exists(path)) return 0;  /* no manifest yet → empty */

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);
    if (!root || pr.code != PASTA_OK) return -1;
    if (pasta_type(root) != PASTA_MAP) { pasta_free(root); return -1; }

    /* Read link_flags_hash */
    const PastaValue *lfh = pasta_map_get(root, "link_flags_hash");
    if (lfh && pasta_type(lfh) == PASTA_STRING)
        m->link_flags_hash = strdup(pasta_get_string(lfh));

    /* Read entries array */
    const PastaValue *entries = pasta_map_get(root, "entries");
    if (entries && pasta_type(entries) == PASTA_ARRAY) {
        size_t n = pasta_count(entries);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *e = pasta_array_get(entries, i);
            if (!e || pasta_type(e) != PASTA_MAP) continue;

            const PastaValue *v;
            const char *src = NULL, *obj = NULL, *sh = NULL, *fh = NULL;
            long long mt = 0;

            v = pasta_map_get(e, "source");
            if (v && pasta_type(v) == PASTA_STRING) src = pasta_get_string(v);
            v = pasta_map_get(e, "object");
            if (v && pasta_type(v) == PASTA_STRING) obj = pasta_get_string(v);
            v = pasta_map_get(e, "source_hash");
            if (v && pasta_type(v) == PASTA_STRING) sh = pasta_get_string(v);
            v = pasta_map_get(e, "flags_hash");
            if (v && pasta_type(v) == PASTA_STRING) fh = pasta_get_string(v);
            v = pasta_map_get(e, "mtime");
            if (v && pasta_type(v) == PASTA_NUMBER) mt = (long long)pasta_get_number(v);

            if (src) {
                now_manifest_set(m, src, obj, sh, fh, mt);

                /* Load deps if present */
                const PastaValue *darr = pasta_map_get(e, "deps");
                if (darr && pasta_type(darr) == PASTA_ARRAY) {
                    size_t dc = pasta_count(darr);
                    if (dc > 0) {
                        char **dpaths = (char **)malloc(dc * sizeof(char *));
                        char **dhashes = (char **)malloc(dc * sizeof(char *));
                        size_t actual = 0;
                        if (dpaths && dhashes) {
                            for (size_t d = 0; d < dc; d++) {
                                const PastaValue *de = pasta_array_get(darr, d);
                                if (!de || pasta_type(de) != PASTA_MAP) continue;
                                const PastaValue *dp = pasta_map_get(de, "path");
                                const PastaValue *dh = pasta_map_get(de, "hash");
                                if (dp && dh &&
                                    pasta_type(dp) == PASTA_STRING &&
                                    pasta_type(dh) == PASTA_STRING) {
                                    dpaths[actual] = strdup(pasta_get_string(dp));
                                    dhashes[actual] = strdup(pasta_get_string(dh));
                                    actual++;
                                }
                            }
                            if (actual > 0) {
                                now_manifest_set_deps(m, src,
                                    (const char **)dpaths,
                                    (const char **)dhashes, actual);
                                /* Overwrite dep_mtimes with stored values (not re-stat) */
                                NowManifestEntry *me = NULL;
                                for (size_t mi = 0; mi < m->count; mi++) {
                                    if (strcmp(m->entries[mi].source, src) == 0) {
                                        me = &m->entries[mi];
                                        break;
                                    }
                                }
                                if (me && me->dep_mtimes) {
                                    size_t di2 = 0;
                                    for (size_t d = 0; d < dc && di2 < actual; d++) {
                                        const PastaValue *de = pasta_array_get(darr, d);
                                        if (!de || pasta_type(de) != PASTA_MAP) continue;
                                        const PastaValue *dp = pasta_map_get(de, "path");
                                        const PastaValue *dh = pasta_map_get(de, "hash");
                                        if (!dp || !dh) continue;
                                        const PastaValue *dm = pasta_map_get(de, "mtime");
                                        if (dm && pasta_type(dm) == PASTA_NUMBER)
                                            me->dep_mtimes[di2] = (long long)pasta_get_number(dm);
                                        di2++;
                                    }
                                    me->dep_mtime_count = actual;
                                }
                            }
                            for (size_t d = 0; d < actual; d++) {
                                free(dpaths[d]);
                                free(dhashes[d]);
                            }
                        }
                        free(dpaths);
                        free(dhashes);
                    }
                }
            }
        }
    }

    pasta_free(root);
    return 0;
}

NOW_API int now_manifest_save(const NowManifest *m, const char *path) {
    /* Build Pasta tree */
    PastaValue *root = pasta_new_map();
    if (!root) return -1;

    if (m->link_flags_hash)
        pasta_set(root, "link_flags_hash", pasta_new_string(m->link_flags_hash));

    PastaValue *entries = pasta_new_array();
    for (size_t i = 0; i < m->count; i++) {
        const NowManifestEntry *e = &m->entries[i];
        PastaValue *entry = pasta_new_map();

        if (e->source)      pasta_set(entry, "source", pasta_new_string(e->source));
        if (e->object)      pasta_set(entry, "object", pasta_new_string(e->object));
        if (e->source_hash) pasta_set(entry, "source_hash", pasta_new_string(e->source_hash));
        if (e->flags_hash)  pasta_set(entry, "flags_hash", pasta_new_string(e->flags_hash));
        pasta_set(entry, "mtime", pasta_new_number((double)e->mtime));

        if (e->dep_count > 0 && e->deps && e->dep_hashes) {
            PastaValue *dep_arr = pasta_new_array();
            for (size_t d = 0; d < e->dep_count; d++) {
                PastaValue *dep = pasta_new_map();
                pasta_set(dep, "path", pasta_new_string(e->deps[d]));
                pasta_set(dep, "hash", pasta_new_string(e->dep_hashes[d]));
                /* Always stat dep at save time to get current mtime */
                {
                    struct stat dep_st;
                    if (stat(e->deps[d], &dep_st) == 0)
                        pasta_set(dep, "mtime", pasta_new_number((double)(long long)dep_st.st_mtime));
                }
                pasta_push(dep_arr, dep);
            }
            pasta_set(entry, "deps", dep_arr);
        }

        pasta_push(entries, entry);
    }
    pasta_set(root, "entries", entries);

    /* Write to file */
    char *out = pasta_write(root, PASTA_PRETTY | PASTA_SORTED);
    pasta_free(root);
    if (!out) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(out); return -1; }
    fputs(out, fp);
    fclose(fp);
    free(out);

    return 0;
}

/* ---- Per-build hash memoization cache ---- */

NOW_API NowHashMemo *now_hash_memo_global = NULL;

static unsigned int memo_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

NOW_API void now_hash_memo_init(NowHashMemo *memo, size_t bucket_count) {
    if (!memo) return;
    if (bucket_count == 0) bucket_count = 512;
    memo->buckets = (NowHashMemoEntry **)calloc(bucket_count, sizeof(NowHashMemoEntry *));
    memo->bucket_count = bucket_count;
    memo->count = 0;
}

NOW_API void now_hash_memo_free(NowHashMemo *memo) {
    if (!memo || !memo->buckets) return;
    for (size_t i = 0; i < memo->bucket_count; i++) {
        NowHashMemoEntry *e = memo->buckets[i];
        while (e) {
            NowHashMemoEntry *next = e->next;
            free(e->path);
            free(e->hash);
            free(e);
            e = next;
        }
    }
    free(memo->buckets);
    memo->buckets = NULL;
    memo->bucket_count = 0;
    memo->count = 0;
}

NOW_API char *now_sha256_file_memo(const char *path, NowHashMemo *memo) {
    if (!path) return NULL;

    /* No memo → fall back to direct hash */
    if (!memo || !memo->buckets)
        return now_sha256_file(path);

    /* Stat the file for mtime */
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    long long cur_mtime = (long long)st.st_mtime;

    /* Look up in memo */
    unsigned int idx = memo_hash(path) % memo->bucket_count;
    for (NowHashMemoEntry *e = memo->buckets[idx]; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            if (e->mtime == cur_mtime && e->hash)
                return strdup(e->hash);  /* cache hit */
            /* mtime changed — rehash and update */
            char *h = now_sha256_file(path);
            if (h) {
                free(e->hash);
                e->hash = strdup(h);
                e->mtime = cur_mtime;
            }
            return h;
        }
    }

    /* Cache miss — hash and insert */
    char *h = now_sha256_file(path);
    if (!h) return NULL;

    NowHashMemoEntry *ne = (NowHashMemoEntry *)malloc(sizeof(NowHashMemoEntry));
    if (ne) {
        ne->path = strdup(path);
        ne->hash = strdup(h);
        ne->mtime = cur_mtime;
        ne->next = memo->buckets[idx];
        memo->buckets[idx] = ne;
        memo->count++;
    }
    return h;
}
