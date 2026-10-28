/* Direct package reader adapted from EoWRecomp dd9c79d6c01d9eaddafc1fc5a07b6658bfc56505.
 * Copyright (c) 2026 Peiikai; Copyright (c) 2026 EoWRecomp contributors.
 * MIT: native/runtime/LICENSE-EoWRecomp. No game assets or keys included. */
// The player's game package read in place (nxpkg.h). Layout as in Atmosphere's fssystem (NcaFileSystemDriver,
// CompressedStorage, BucketTree) and Switchbrew's NCA / XCI / PFS0 / HFS0 pages, the same as tools/nca_extract.py:
//   package (NSP = PFS0 of NCAs; XCI = HFS0 root -> "secure" HFS0 of NCAs) -> program NCA (header: AES-XTS with
//   header_key) -> section (AES-CTR, key from the key area or the title key) -> hash layers -> [CompressedStorage]
//   -> PFS0 (ExeFS) / RomFS.
// Hashes are not verified (the files are the player's own dump; a bad one shows up as a game error).
#include "nxpkg.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#endif
#endif

#define WHY(...) (why && why_size ? (void)snprintf(why, why_size, __VA_ARGS__) : (void)0, false)
#define MAX_TABLE (64u << 20)
#define MAX_BLOCK (4u << 20)
static bool bounds(uint64_t off, uint64_t size, uint64_t total) { return off <= total && size <= total - off; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p+4)<<32; }
static void wipe(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }

// Local path. Android document URIs are opened by the app bridge and passed to
// nx_package_open_fd, so this core has no JNI/platform dependency.
static int open_package(const char *path) {
    if (!path) { errno = EINVAL; return -1; }
    return open(path, O_RDONLY);
}

// ------------------------------------------------------------------ AES-128 (software; ARMv8 AES for CTR)
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
static uint8_t INV_SBOX[256];

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

static void init_inv(void) {
    for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176]) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_inv);
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon, t[1] = SBOX[t[2]], t[2] = SBOX[t[3]], t[3] = SBOX[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = rk[i - 16 + k] ^ t[k];
    }
}
static void aes_encrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows (column-major)
        if (r < 10)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                q[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                q[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                q[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
    }
}

static void aes_decrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = INV_SBOX[s[i]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++) t[i] ^= rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                q[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                q[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                q[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
}

void nx_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len) {
    uint8_t rk[176];
    aes_expand(key, rk);
    for (size_t i = 0; i + 16 <= len; i += 16) aes_decrypt_block(rk, data + i);
}

static void ctr_inc(uint8_t c[16]) {
    for (int i = 15; i >= 0 && !++c[i]; i--) {}
}

#if defined(__aarch64__)
static bool have_arm_aes(void) {
#if defined(__APPLE__)
    return true;  // every Apple Silicon CPU
#elif defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
#else
    return false;
#endif
}

__attribute__((target("aes"))) static void ctr_arm(const uint8_t rk[176], uint8_t ctr[16], uint8_t *data, size_t len) {
    uint8x16_t k[11];
    for (int i = 0; i < 11; i++) k[i] = vld1q_u8(rk + 16 * i);
    while (len) {
        uint8x16_t b = vld1q_u8(ctr);
        for (int r = 0; r < 9; r++) b = vaesmcq_u8(vaeseq_u8(b, k[r]));
        b = veorq_u8(vaeseq_u8(b, k[9]), k[10]);
        size_t n = len < 16 ? len : 16;
        if (n == 16) vst1q_u8(data, veorq_u8(vld1q_u8(data), b));
        else {
            uint8_t ks[16];
            vst1q_u8(ks, b);
            for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        }
        data += n, len -= n;
        ctr_inc(ctr);
    }
}
#endif

void nx_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len) {
    uint8_t rk[176], ctr[16];
    aes_expand(key, rk);
    memcpy(ctr, iv, 16);
#if defined(__aarch64__)
    static int arm = -1;
    int use_arm = __atomic_load_n(&arm, __ATOMIC_RELAXED);
    if (use_arm < 0) { use_arm = have_arm_aes(); __atomic_store_n(&arm, use_arm, __ATOMIC_RELAXED); }
    if (use_arm) { ctr_arm(rk, ctr, data, len); return; }
#endif
    while (len) {
        uint8_t ks[16];
        memcpy(ks, ctr, 16);
        aes_encrypt_block(rk, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        data += n, len -= n;
        ctr_inc(ctr);
    }
}

// AES-XTS with Nintendo's tweak (the sector number big-endian), 0x200-byte sectors: the NCA header.
static void aes_xts_decrypt(const uint8_t key[32], uint8_t *data, size_t len, uint64_t sector) {
    uint8_t rk1[176], rk2[176];
    aes_expand(key, rk1);
    aes_expand(key + 16, rk2);
    for (size_t s = 0; s < len; s += 0x200, sector++) {
        uint8_t tw[16] = {0};
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sector >> (8 * i));
        aes_encrypt_block(rk2, tw);
        for (size_t b = 0; b < 0x200; b += 16) {
            uint8_t *p = data + s + b;
            for (int i = 0; i < 16; i++) p[i] ^= tw[i];
            aes_decrypt_block(rk1, p);
            for (int i = 0; i < 16; i++) p[i] ^= tw[i];
            uint8_t carry = tw[15] >> 7;  // tweak * x in GF(2^128), little-endian bytes
            for (int i = 15; i > 0; i--) tw[i] = (uint8_t)(tw[i] << 1 | tw[i - 1] >> 7);
            tw[0] = (uint8_t)(tw[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
}

// ------------------------------------------------------------------ LZ4 block
int nx_lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_len) {
    if (src_len < 0 || dst_len < 0 || (!src && src_len) || (!dst && dst_len)) return -1;
    if (!src_len) return 0;
    if (!dst_len) return -1;
    const uint8_t *ip = src, *iend = src + src_len;
    uint8_t *op = dst, *oend = dst + dst_len;
    while (ip < iend) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit) return -1;
        memcpy(op, ip, lit);
        op += lit, ip += lit;
        if (ip >= iend) break;  // the last sequence has literals only
        if (iend - ip < 2) return -1;
        size_t off = ip[0] | ip[1] << 8;
        ip += 2;
        size_t ml = tok & 15;
        if (ml == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255);
        }
        ml += 4;
        if (!off || off > (size_t)(op - dst) || (size_t)(oend - op) < ml) return -1;
        const uint8_t *m = op - off;
        if (off >= ml) memcpy(op, m, ml), op += ml;
        else while (ml--) *op++ = *m++;  // overlapping: byte by byte
    }
    return (int)(op - dst);
}

// ------------------------------------------------------------------ keys
typedef struct { char name[64]; uint8_t val[32]; int len; } Key;
typedef struct { Key *k; int n; } Keys;

static int unhex(const char *s, uint8_t *out, int cap) {
    int n = 0;
    while (s[0] && s[1] && n < cap) {
        int v = 0;
        for (int i = 0; i < 2; i++) {
            unsigned char c = (unsigned char)s[i];
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return n;
            v = (v << 4) | digit;
        }
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static bool keys_load(const char *path, Keys *ks) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[64], val[256];
        if (sscanf(line, " %63s", name) != 1 || sscanf(eq + 1, " %255s", val) != 1) continue;
        if (ks->n >= 8192) { fclose(f); return false; }
        Key *grown = realloc(ks->k, sizeof *ks->k * (ks->n + 1));
        if (!grown) { fclose(f); return false; }
        ks->k = grown;
        Key *k = &ks->k[ks->n++];
        snprintf(k->name, sizeof k->name, "%s", name);
        for (char *c = k->name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        k->len = unhex(val, k->val, 32);
    }
    fclose(f);
    return true;
}

static const uint8_t *key_get(const Keys *ks, const char *name, int len) {
    for (int i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].name, name) && ks->k[i].len == len) return ks->k[i].val;
    return NULL;
}

// ------------------------------------------------------------------ package -> program NCA
typedef struct { char name[256]; uint64_t off, size; } Entry;
typedef struct { Entry *e; int n; } Entries;

static bool entries_add(Entries *es, const char *name, uint64_t off, uint64_t size) {
    if (es->n >= 4096) return false;
    Entry *grown = realloc(es->e, sizeof *es->e * (es->n + 1));
    if (!grown) return false;
    es->e = grown;
    Entry *e = &es->e[es->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->off = off, e->size = size;
    return true;
}

static bool pread_all(int fd, void *dst, size_t n, uint64_t off) {
    if (off > INT64_MAX || n > (uint64_t)INT64_MAX - off) return false;
    uint8_t *p = dst;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r, n -= (size_t)r, off += (uint64_t)r;
    }
    return true;
}

// PFS0 (entry 0x18) / HFS0 (entry 0x40) at `base`: its files, offsets absolute in the package
static bool read_fs0(int fd, uint64_t base, uint64_t span, Entries *out) {
    uint8_t h[16];
    if (span < 16 || !pread_all(fd, h, 16, base)) return false;
    bool hfs = !memcmp(h, "HFS0", 4);
    if (memcmp(h, "PFS0", 4) && !hfs) return false;
    uint32_t n, strsz;
    memcpy(&n, h + 4, 4), memcpy(&strsz, h + 8, 4);
    if (n > 4096 || strsz > (1u << 20)) return false;
    size_t esz = hfs ? 0x40 : 0x18, meta = 16 + n * esz + strsz;
    if (meta > span) return false;
    uint8_t *m = malloc(meta);
    if (!m) return false;
    if (!pread_all(fd, m, meta, base)) { free(m); return false; }
    const char *strs = (const char *)m + 16 + n * esz;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off, size;
        uint32_t no;
        memcpy(&off, m + 16 + i * esz, 8), memcpy(&size, m + 16 + i * esz + 8, 8), memcpy(&no, m + 16 + i * esz + 16, 4);
        if (no >= strsz || !memchr(strs + no, 0, strsz - no) ||
            strnlen(strs + no, strsz - no) >= 256 || !bounds(off, size, span - meta) ||
            base > UINT64_MAX - meta - off) { free(m); return false; }
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)strnlen(strs + no, strsz - no), strs + no);
        if (!entries_add(out, name, base + meta + off, size)) { free(m); return false; }
    }
    free(m);
    return true;
}

typedef struct {
    int fd;
    uint64_t base;      // the NCA's offset in the package
    uint64_t size;
    uint8_t hdr[0xC00];  // decrypted header
    uint8_t ctr_key[16];
} Nca;

typedef struct {
    uint64_t off, size;           // in the NCA
    uint8_t fs_type, hash_type, enc_type;
    uint8_t ctr_hi[8];
    uint64_t data_off, data_size;  // the payload under the hash layers, relative to the section
    uint64_t comp_off, comp_size;  // compression table, relative to the payload
    uint8_t comp_hdr[16];
} Section;

static bool nca_header(int fd, uint64_t base, uint64_t size, const Keys *ks, Nca *nca) {
    const uint8_t *hk = key_get(ks, "header_key", 32);
    if (!hk || size < sizeof nca->hdr) return false;
    nca->fd = fd, nca->base = base, nca->size = size;
    if (!pread_all(fd, nca->hdr, 0xC00, base)) return false;
    aes_xts_decrypt(hk, nca->hdr, 0xC00, 0);
    return !memcmp(nca->hdr + 0x200, "NCA3", 4);
}

static bool section_info(const Nca *nca, int i, Section *s) {
    if (i < 0 || i >= 4) return false;
    uint32_t st, en;
    memcpy(&st, nca->hdr + 0x240 + i * 16, 4), memcpy(&en, nca->hdr + 0x244 + i * 16, 4);
    if (en <= st) return false;
    const uint8_t *fh = nca->hdr + 0x400 + i * 0x200;
    memset(s, 0, sizeof *s);
    s->off = (uint64_t)st * 0x200, s->size = (uint64_t)(en - st) * 0x200;
    s->fs_type = fh[2], s->hash_type = fh[3], s->enc_type = fh[4];
    for (int k = 0; k < 8; k++) s->ctr_hi[k] = fh[0x147 - k];
    if (s->hash_type == 2) {  // HierarchicalSha256: the last layer region is the data
        uint32_t layers;
        memcpy(&layers, fh + 0x8 + 0x24, 4);
        if (layers < 1 || layers > 5) return false;
        memcpy(&s->data_off, fh + 0x8 + 0x28 + (layers - 1) * 16, 8);
        memcpy(&s->data_size, fh + 0x8 + 0x30 + (layers - 1) * 16, 8);
    } else if (s->hash_type == 3) {  // HierarchicalIntegrity (IVFC): the last level is the data
        uint32_t levels;
        if (memcmp(fh + 0x8, "IVFC", 4)) return false;
        memcpy(&levels, fh + 0x8 + 0xC, 4);
        if (levels < 2 || levels > 7) return false;
        memcpy(&s->data_off, fh + 0x8 + 0x10 + (levels - 2) * 0x18, 8);
        memcpy(&s->data_size, fh + 0x8 + 0x18 + (levels - 2) * 0x18, 8);
    } else return false;
    memcpy(&s->comp_off, fh + 0x178, 8), memcpy(&s->comp_size, fh + 0x180, 8);
    memcpy(s->comp_hdr, fh + 0x188, 16);
    return bounds(s->off, s->size, nca->size) && bounds(s->data_off, s->data_size, s->size) &&
           (!s->comp_size || bounds(s->comp_off, s->comp_size, s->data_size)) &&
           (s->enc_type == 1 || s->enc_type == 3);
}

// decrypted bytes [off, off+n) of a section (off relative to the section)
static bool section_read(int fd, uint64_t nca_base, const Section *s, const uint8_t key[16], uint8_t *dst, uint64_t off,
                         size_t n) {
    uint64_t a = off & ~15ull, pre = off - a;
    if (!bounds(off, n, s->size) || s->off > UINT64_MAX - nca_base ||
        nca_base + s->off > INT64_MAX || off > (uint64_t)INT64_MAX - nca_base - s->off ||
        n > (uint64_t)INT64_MAX - nca_base - s->off - off || n > SIZE_MAX - pre) return false;
    if (s->enc_type == 1) return pread_all(fd, dst, n, nca_base + s->off + off);
    if (s->enc_type != 3) return false;
    uint8_t *tmp = pre ? malloc(n + pre) : dst;
    if (!tmp) return n == 0;
    if (!pread_all(fd, tmp, n + pre, nca_base + s->off + a)) { if (pre) free(tmp); return false; }
    uint8_t iv[16];
    memcpy(iv, s->ctr_hi, 8);
    uint64_t blk = (s->off + a) >> 4;
    for (int k = 0; k < 8; k++) iv[15 - k] = (uint8_t)(blk >> (8 * k));
    nx_aes128_ctr(key, iv, tmp, n + pre);
    if (pre) memcpy(dst, tmp + pre, n), free(tmp);
    return true;
}

// ------------------------------------------------------------------ CompressedStorage
// BucketTree of CompressedStorage::Entry {s64 virt; s64 phys; u8 type; s8 level; u16; u32 phys_size}, node size
// 0x4000. Types: 0 none (copy), 1 zeros, 3 LZ4.
typedef struct { int64_t virt, phys; uint8_t type; uint32_t psize; } CEntry;
typedef struct { uint8_t *data; uint64_t entry; uint32_t len; uint64_t stamp; } CBlock;
#define NCACHE 16

struct NxRomfs {
    int fd;
    uint64_t nca_base;
    Section sec;
    uint8_t key[16];
    uint64_t size;  // the RomFS image's
    CEntry *ent;
    uint64_t nent;
    pthread_mutex_t lock;
    CBlock cache[NCACHE];
    uint64_t stamp;
};

static bool comp_load(NxRomfs *r, char *why, size_t why_size) {
    const uint8_t *h = r->sec.comp_hdr;
    if (memcmp(h, "BKTR", 4)) return WHY("bad compression table header");
    int32_t count;
    memcpy(&count, h + 8, 4);
    const uint64_t NODE = 0x4000, ENTRY = 0x18;
    if (count <= 0 || count > (1 << 20) || r->sec.comp_size > MAX_TABLE)
        return WHY("unsupported compression table size");
    uint8_t *table = malloc(r->sec.comp_size);
    if (!table) return WHY("out of memory reading compression table");
    if (!section_read(r->fd, r->nca_base, &r->sec, r->key, table, r->sec.data_off + r->sec.comp_off, r->sec.comp_size)) {
        free(table);
        return WHY("can't read the compression table (package cut short?)");
    }
    uint64_t per_set = (NODE - 0x10) / ENTRY, sets = (count + per_set - 1) / per_set, per_node = (NODE - 0x10) / 8;
    uint64_t l2 = sets <= per_node ? 0 : (sets - per_node + per_node - 2) / (per_node - 1);
    uint64_t node_storage = (1 + l2) * NODE;
    r->ent = calloc((size_t)count + 1, sizeof *r->ent);
    if (!r->ent) { free(table); return WHY("out of memory reading compression entries"); }
    r->nent = 0;
    int64_t end = 0;
    for (uint64_t s = 0; s < sets; s++) {
        uint64_t b = node_storage + s * NODE;
        if (b + NODE > r->sec.comp_size) { free(table); return WHY("compression table cut short"); }
        int32_t idx, n;
        memcpy(&idx, table + b, 4), memcpy(&n, table + b + 4, 4), memcpy(&end, table + b + 8, 8);
        if ((uint64_t)idx != s || n < 0 || (uint64_t)n > per_set) { free(table); return WHY("bad compression table"); }
        for (int e = 0; e < n && r->nent < (uint64_t)count; e++) {
            const uint8_t *p = table + b + 0x10 + e * ENTRY;
            CEntry *c = &r->ent[r->nent++];
            memcpy(&c->virt, p, 8), memcpy(&c->phys, p + 8, 8), c->type = p[16], memcpy(&c->psize, p + 20, 4);
        }
    }
    free(table);
    if (r->nent != (uint64_t)count) return WHY("compression table: %llu entries of %d", (unsigned long long)r->nent, count);
    r->ent[r->nent].virt = end;  // (the sentinel: the end of the last entry)
    if (end <= 0 || (uint64_t)end > (64ull << 30) || r->ent[0].virt != 0)
        return WHY("invalid compression virtual image bounds");
    for (uint64_t i = 0; i < r->nent; i++) {
        CEntry *e = &r->ent[i];
        int64_t next = r->ent[i + 1].virt;
        if (e->virt < 0 || next <= e->virt || e->phys < 0 ||
            (e->type != 0 && e->type != 1 && e->type != 3))
            return WHY("invalid compression entry");
        uint64_t bytes = e->type == 3 ? e->psize : e->type == 0 ? (uint64_t)(next - e->virt) : 0;
        if (!bounds((uint64_t)e->phys, bytes, r->sec.data_size) ||
            (e->type == 3 && (!e->psize || e->psize > MAX_BLOCK || next - e->virt > MAX_BLOCK)))
            return WHY("compression block exceeds supported bounds");
    }
    r->size = (uint64_t)end;
    return true;
}

// the cached, decompressed block of entry i
static const uint8_t *comp_block(NxRomfs *r, uint64_t i, uint32_t *len) {
    CBlock *victim = &r->cache[0];
    for (int k = 0; k < NCACHE; k++) {
        CBlock *c = &r->cache[k];
        if (c->data && c->entry == i) { c->stamp = ++r->stamp; *len = c->len; return c->data; }
        if (!c->data || c->stamp < victim->stamp) victim = c;
    }
    const CEntry *e = &r->ent[i];
    uint32_t vlen = (uint32_t)(r->ent[i + 1].virt - e->virt);
    uint8_t *raw = malloc(e->psize ? e->psize : 1), *out = malloc(vlen ? vlen : 1);
    if (!raw || !out) { free(raw); free(out); return NULL; }
    if (!section_read(r->fd, r->nca_base, &r->sec, r->key, raw, r->sec.data_off + (uint64_t)e->phys, e->psize) ||
        nx_lz4_decompress(raw, (int)e->psize, out, (int)vlen) != (int)vlen) {
        free(raw), free(out);
        return NULL;
    }
    free(raw);
    free(victim->data);
    victim->data = out, victim->entry = i, victim->len = vlen, victim->stamp = ++r->stamp;
    *len = vlen;
    return out;
}

ssize_t nx_romfs_pread(NxRomfs *r, void *dst_, uint64_t size, uint64_t off) {
    if (!r || (!dst_ && size) || size > SSIZE_MAX || size > SIZE_MAX) { errno = EINVAL; return -1; }
    uint8_t *dst = dst_;
    if (!size || off >= r->size) return 0;
    if (size > r->size - off) size = r->size - off;
    if (!r->ent)
        return section_read(r->fd, r->nca_base, &r->sec, r->key, dst, r->sec.data_off + off, size) ? (ssize_t)size : -1;
    pthread_mutex_lock(&r->lock);
    uint64_t lo = 0, hi = r->nent;  // last entry with virt <= off
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if ((uint64_t)r->ent[mid].virt <= off) lo = mid;
        else hi = mid;
    }
    uint64_t done = 0;
    bool ok = true;
    for (uint64_t i = lo; done < size && i < r->nent && ok; i++) {
        const CEntry *e = &r->ent[i];
        uint64_t v = (uint64_t)e->virt, vend = (uint64_t)r->ent[i + 1].virt, o = off + done;
        if (o >= vend) continue;
        uint64_t n = vend - o < size - done ? vend - o : size - done;
        if (e->type == 0) ok = section_read(r->fd, r->nca_base, &r->sec, r->key, dst + done,
                                            r->sec.data_off + (uint64_t)e->phys + (o - v), n);
        else if (e->type == 1) memset(dst + done, 0, n);
        else if (e->type == 3) {
            uint32_t len;
            const uint8_t *b = comp_block(r, i, &len);
            if (b && bounds(o - v, n, len)) memcpy(dst + done, b + (o - v), n);
            else ok = false;
        } else ok = false;
        done += n;
    }
    pthread_mutex_unlock(&r->lock);
    return ok ? (ssize_t)done : -1;
}

uint64_t nx_romfs_size(const NxRomfs *r) { return r ? r->size : 0; }

/* Metadata traversal deliberately does not read any file payload. Record-start
 * maps reject pointers into names/padding; visited marks reject cycles, aliases
 * and orphaned metadata. Two passes prevent callbacks on an invalid tree. */
#define RF_NONE UINT32_MAX
#define RF_MAX_ENTRIES 262144u
#define RF_MAX_PATH 4096u
#define RF_MAX_DEPTH 64u
typedef struct { uint8_t *data, *map; size_t size; uint32_t count; } RfTable;
static bool rf_name(const uint8_t *s, uint32_t n, bool root) {
    if (!n) return root;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
    for (uint32_t i = 0; i < n; i++) if (!s[i] || s[i] == '/' || s[i] == '\\') return false;
    return true;
}
static bool rf_has(const RfTable *t, uint32_t at) {
    return at != RF_NONE && !(at & 3) && at < t->size && t->map[at / 4];
}
static bool rf_link(const RfTable *t, uint32_t at) { return at == RF_NONE || rf_has(t, at); }
static bool rf_parse(RfTable *t, bool directory) {
    size_t at = 0, fixed = directory ? 24 : 32;
    while (at < t->size) {
        if (t->count >= RF_MAX_ENTRIES || !bounds(at, fixed, t->size)) return false;
        uint32_t n = rd32(t->data + at + fixed - 4);
        if (!bounds(at + fixed, n, t->size) || n >= RF_MAX_PATH ||
            !rf_name(t->data + at + fixed, n, directory && !at)) return false;
        t->map[at / 4] = 1; t->count++;
        size_t next = (at + fixed + n + 3) & ~(size_t)3;
        if (next > t->size || next <= at) return false;
        at = next;
    }
    return at == t->size;
}
typedef struct { uint32_t dir, next_dir, next_file; size_t path_len; } RfFrame;
static bool rf_walk(RfTable *dirs, RfTable *files, uint64_t data_at, uint64_t image_size,
                    const uint64_t metadata_offsets[4], const uint64_t metadata_sizes[4],
                    NxRomfsFileVisit visit, void *ctx, const volatile int *cancel) {
    if (!rf_has(dirs, 0)) return false;
    const uint8_t *root = dirs->data;
    if (rd32(root + 20) || (rd32(root) != 0 && rd32(root) != RF_NONE) || rd32(root + 4) != RF_NONE || !rf_link(dirs, rd32(root + 16))) return false;
    RfFrame stack[RF_MAX_DEPTH]; size_t depth = 1; char path[RF_MAX_PATH] = {0};
    stack[0] = (RfFrame){0, rd32(root + 8), rd32(root + 12), 0}; dirs->map[0] = 2;
    uint32_t nd = 1, nf = 0;
    while (depth) {
        if (cancel && *cancel) return false;
        RfFrame *f = &stack[depth - 1];
        path[f->path_len] = 0;
        if (f->next_file != RF_NONE) {
            uint32_t at = f->next_file;
            if (!rf_has(files, at) || files->map[at / 4] != 1) return false;
            const uint8_t *e = files->data + at; uint32_t n = rd32(e + 28);
            if (rd32(e) != f->dir || !rf_link(files, rd32(e + 4)) ||
                !rf_link(files, rd32(e + 24))) return false;
            uint64_t off = rd64(e + 8), size = rd64(e + 16);
            if (!bounds(data_at, off, image_size) || !bounds(data_at + off, size, image_size)) return false;
            // RomFS can place metadata after file data. Its header's data base
            // is not a metadata boundary: prove each actual payload extent
            // against the bounded table extents before invoking any visitor.
            uint64_t absolute = data_at + off;
            for (size_t m = 0; m < 4; m++)
                if (size && metadata_sizes[m] && absolute < metadata_offsets[m] + metadata_sizes[m] &&
                    metadata_offsets[m] < absolute + size) return false;
            size_t sep = f->path_len ? 1 : 0;
            if (n >= sizeof path - f->path_len - sep) return false;
            if (sep) path[f->path_len] = '/';
            memcpy(path + f->path_len + sep, e + 32, n); path[f->path_len + sep + n] = 0;
            files->map[at / 4] = 2; nf++; f->next_file = rd32(e + 4);
            if (visit && !visit(ctx, path, data_at + off, size)) return false;
        } else if (f->next_dir != RF_NONE) {
            uint32_t at = f->next_dir;
            if (depth >= RF_MAX_DEPTH || !rf_has(dirs, at) || dirs->map[at / 4] != 1) return false;
            const uint8_t *e = dirs->data + at; uint32_t n = rd32(e + 20);
            if (rd32(e) != f->dir || !rf_link(dirs, rd32(e + 4)) ||
                !rf_link(dirs, rd32(e + 8)) || !rf_link(files, rd32(e + 12)) ||
                !rf_link(dirs, rd32(e + 16))) return false;
            size_t sep = f->path_len ? 1 : 0;
            if (n >= sizeof path - f->path_len - sep) return false;
            if (sep) path[f->path_len] = '/';
            memcpy(path + f->path_len + sep, e + 24, n); size_t len = f->path_len + sep + n; path[len] = 0;
            f->next_dir = rd32(e + 4); dirs->map[at / 4] = 2; nd++;
            stack[depth++] = (RfFrame){at, rd32(e + 8), rd32(e + 12), len};
        } else depth--;
    }
    return nd == dirs->count && nf == files->count;
}
static bool rf_read_chunks(NxReadAt read, void *reader, uint64_t offset, void *dst, size_t size, const volatile int *cancel) {
    size_t done = 0;
    while (done < size) {
        if (cancel && *cancel) return false;
        size_t n = size - done < 65536 ? size - done : 65536;
        if (!read(reader, offset + done, (uint8_t *)dst + done, n)) return false;
        done += n;
    }
    return true;
}
bool nx_romfs_visit_reader(NxReadAt read, void *reader, uint64_t image_size,
                          NxRomfsFileVisit visit, void *context, const volatile int *cancel,
                          char *why, size_t why_size) {
    uint8_t h[0x50]; RfTable dirs = {0}, files = {0}; bool ok = false;
    if (!read || !visit || image_size < sizeof h || (cancel && *cancel) || !read(reader, 0, h, sizeof h) || rd64(h) != sizeof h)
        return WHY("invalid or cancelled RomFS metadata request");
    uint64_t offsets[4], sizes[4], data_at = rd64(h + 0x48);
    for (size_t i = 0; i < 4; i++) { offsets[i] = rd64(h + 8 + 16 * i); sizes[i] = rd64(h + 16 + 16 * i); }
    uint64_t sum = 0;
    if (data_at < sizeof h || data_at > image_size) return WHY("invalid RomFS data offset");
    for (size_t i = 0; i < 4; i++) {
        if (sizes[i] > MAX_TABLE || sum > MAX_TABLE - sizes[i] || offsets[i] < sizeof h ||
            !bounds(offsets[i], sizes[i], image_size)) return WHY("RomFS metadata exceeds supported bounds");
        sum += sizes[i];
        for (size_t j = 0; j < i; j++) if (sizes[i] && sizes[j] && offsets[i] < offsets[j] + sizes[j] && offsets[j] < offsets[i] + sizes[i])
            return WHY("overlapping RomFS metadata tables");
    }
    dirs.size = (size_t)sizes[1]; files.size = (size_t)sizes[3];
    if (dirs.size < 24 || dirs.size % 4 || files.size % 4) return WHY("invalid RomFS metadata table sizes");
    dirs.data = malloc(dirs.size); files.data = malloc(files.size ? files.size : 1);
    dirs.map = calloc(dirs.size / 4, 1); files.map = calloc(files.size / 4 + 1, 1);
    if (!dirs.data || !files.data || !dirs.map || !files.map) { WHY("out of memory reading RomFS metadata"); goto done; }
    if (!rf_read_chunks(read, reader, offsets[1], dirs.data, dirs.size, cancel) || (files.size && !rf_read_chunks(read, reader, offsets[3], files.data, files.size, cancel)) ||
        !rf_parse(&dirs, true) || !rf_parse(&files, false) || dirs.count + files.count > RF_MAX_ENTRIES ||
        !rf_walk(&dirs, &files, data_at, image_size, offsets, sizes, NULL, NULL, cancel)) {
        WHY("invalid, cyclic, oversized or cancelled RomFS tree"); goto done;
    }
    for (size_t i = 0; i < dirs.size / 4; i++) if (dirs.map[i]) dirs.map[i] = 1;
    for (size_t i = 0; i < files.size / 4; i++) if (files.map[i]) files.map[i] = 1;
    ok = rf_walk(&dirs, &files, data_at, image_size, offsets, sizes, visit, context, cancel);
    if (!ok) WHY("RomFS file visitor cancelled");
done:
    free(dirs.data); free(dirs.map); free(files.data); free(files.map); return ok;
}
static bool rf_native_read(void *context, uint64_t offset, void *dst, size_t size) {
    return nx_romfs_pread(context, dst, size, offset) == (ssize_t)size;
}
bool nx_romfs_visit_files(NxRomfs *romfs, NxRomfsFileVisit visit, void *context,
                         const volatile int *cancel, char *why, size_t why_size) {
    return nx_romfs_visit_reader(rf_native_read, romfs, nx_romfs_size(romfs), visit, context, cancel, why, why_size);
}

NxRomfs *nx_romfs_clone(const NxRomfs *source, char *why, size_t why_size) {
    if (!source || source->fd < 0 || (source->ent && source->nent > (SIZE_MAX / sizeof *source->ent) - 1)) {
        WHY("invalid package reader to clone"); return NULL;
    }
    NxRomfs *r = calloc(1, sizeof *r);
    if (!r) { WHY("out of memory cloning game data reader"); return NULL; }
    r->fd = -1;
    if (pthread_mutex_init(&r->lock, NULL)) { free(r); WHY("can't initialize cloned game data reader"); return NULL; }
    r->fd = fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
    r->nca_base = source->nca_base; r->sec = source->sec; r->size = source->size;
    memcpy(r->key, source->key, sizeof r->key);
    if (r->fd < 0) { WHY("can't duplicate the game package reader"); nx_romfs_close(r); return NULL; }
    if (source->ent) {
        size_t bytes = ((size_t)source->nent + 1) * sizeof *source->ent;
        r->ent = malloc(bytes);
        if (!r->ent) { WHY("out of memory cloning compression metadata"); nx_romfs_close(r); return NULL; }
        memcpy(r->ent, source->ent, bytes); r->nent = source->nent;
    }
    return r;
}

void nx_romfs_close(NxRomfs *r) {
    if (!r) return;
    for (int k = 0; k < NCACHE; k++) free(r->cache[k].data);
    free(r->ent);
    if (r->fd >= 0) close(r->fd);
    pthread_mutex_destroy(&r->lock);
    wipe(r->key, sizeof r->key);
    free(r);
}

static void hex(char *out, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}
static void keys_free(Keys *keys) {
    if (keys->k) wipe(keys->k, (size_t)keys->n * sizeof *keys->k);
    free(keys->k);
    keys->k = NULL; keys->n = 0;
}
static bool title_key(const Nca *nca, int fd, const Entries *files, const Keys *ks, const char *keys_path, int keygen,
                      uint8_t out[16]) {
    const uint8_t *rights = nca->hdr + 0x230;
    uint8_t enc[16];
    bool found = false;
    for (int i = 0; i < files->n && !found; i++) {
        size_t l = strlen(files->e[i].name);
        if (l < 4 || strcmp(files->e[i].name + l - 4, ".tik") || files->e[i].size < 0x2b0) continue;
        uint8_t tik[0x2b0];
        if (pread_all(fd, tik, sizeof tik, files->e[i].off) && !memcmp(tik + 0x2a0, rights, 16))
            memcpy(enc, tik + 0x180, 16), found = true;
        wipe(tik, sizeof tik);
    }
    if (!found) {
        char tk[4096], name[33];
        if (strlen(keys_path) >= sizeof tk) return false;
        snprintf(tk, sizeof tk, "%s", keys_path);
        char *slash = strrchr(tk, '/');
        size_t pos = slash ? (size_t)(slash + 1 - tk) : 0;
        if (sizeof tk - pos < sizeof "title.keys") return false;
        memcpy(tk + pos, "title.keys", sizeof "title.keys");
        Keys t = {0};
        hex(name, rights, 16);
        if (keys_load(tk, &t)) {
            const uint8_t *v = key_get(&t, name, 16);
            if (v) memcpy(enc, v, 16), found = true;
        }
        keys_free(&t);
    }
    if (!found) return false;
    char kn[32];
    snprintf(kn, sizeof kn, "titlekek_%02x", keygen);
    const uint8_t *kek = key_get(ks, kn, 16);
    if (!kek) { wipe(enc, sizeof enc); return false; }
    memcpy(out, enc, 16);
    nx_aes128_ecb_decrypt(kek, out, 16);
    wipe(enc, sizeof enc);
    return true;
}

static bool nca_set_ctr_key(Nca *nca, int fd, const Entries *files, const Keys *keys,
                            const char *keys_path, char *why, size_t why_size) {
    int gen = nca->hdr[0x206] > nca->hdr[0x220] ? nca->hdr[0x206] : nca->hdr[0x220];
    int keygen = gen ? gen - 1 : 0;
    static const uint8_t zero[16];
    if (memcmp(nca->hdr + 0x230, zero, 16)) {
        if (!title_key(nca, fd, files, keys, keys_path, keygen, nca->ctr_key))
            return WHY("the package needs a title key from its ticket or title.keys");
    } else {
        char name[48]; snprintf(name, sizeof name, "key_area_key_application_%02x", keygen);
        const uint8_t *kaek = key_get(keys, name, 16);
        if (!kaek) return WHY("the keys file lacks the required application key generation");
        uint8_t area[0x40]; memcpy(area, nca->hdr + 0x300, sizeof area);
        nx_aes128_ecb_decrypt(kaek, area, sizeof area);
        memcpy(nca->ctr_key, area + 0x20, 16); wipe(area, sizeof area);
    }
    (void)why; (void)why_size;
    return true;
}

struct NxPackage { int fd; uint64_t title_id; Nca nca, control; Section exefs, romfs, control_romfs; bool has_control; };

NxPackage *nx_package_open_fd(int package_fd, const char *keys_path, char *why, size_t why_size) {
    Keys keys = {0}; Entries files = {0}; NxPackage *package = NULL;
    Nca nca = {0};
    int fd = -1;
    if (!keys_path || !keys_load(keys_path, &keys)) { WHY("can't read the keys file"); goto out; }
    fd = fcntl(package_fd, F_DUPFD_CLOEXEC, 0);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb) || sb.st_size < 0 || lseek(fd, 0, SEEK_CUR) < 0) {
        WHY("the game package must be a readable, seekable file"); goto out;
    }
    uint8_t head[0x200];
    if (!pread_all(fd, head, sizeof head, 0)) { WHY("the game package is too small"); goto out; }
    uint64_t size = (uint64_t)sb.st_size;
    if (!memcmp(head, "PFS0", 4)) {
        if (!read_fs0(fd, 0, size, &files)) { WHY("invalid NSP file table"); goto out; }
    } else if (!memcmp(head + 0x100, "HEAD", 4)) {
        uint64_t root;
        memcpy(&root, head + 0x130, 8);
        Entries parts = {0};
        bool parsed = bounds(root, 16, size) && read_fs0(fd, root, size - root, &parts);
        for (int i = 0; parsed && i < parts.n; i++)
            if (!strcmp(parts.e[i].name, "secure"))
                parsed = read_fs0(fd, parts.e[i].off, parts.e[i].size, &files);
        free(parts.e);
        if (!parsed || !files.n) { WHY("invalid XCI secure partition"); goto out; }
    } else if (!entries_add(&files, "program.nca", 0, size)) {
        WHY("out of memory reading package"); goto out;
    }
    bool found = false, control_found = false, any_nca = false, nsz = false;
    Nca control = {0};
    for (int i = 0; i < files.n; i++) {
        Entry *e = &files.e[i]; size_t len = strlen(e->name);
        if (len >= 4 && !strcmp(e->name + len - 4, ".ncz")) nsz = true;
        if (len < 4 || strcmp(e->name + len - 4, ".nca")) continue;
        Nca candidate = {0};
        if (!nca_header(fd, e->off, e->size, &keys, &candidate)) continue;
        any_nca = true;
        if (candidate.hdr[0x205] == 2) {
            if (control_found) { wipe(&candidate, sizeof candidate); WHY("multiple control NCAs in package"); goto out; }
            control = candidate; control_found = true; wipe(&candidate, sizeof candidate); continue;
        }
        if (candidate.hdr[0x205] != 0) { wipe(&candidate, sizeof candidate); continue; }
        if (found) { wipe(&candidate, sizeof candidate); WHY("multiple game programs: select a base-only package"); goto out; }
        nca = candidate; found = true;
        wipe(&candidate, sizeof candidate);
    }
    if (!found) { WHY(nsz ? "NSZ/NCZ compression is unsupported: use the original NSP" :
                                      any_nca ? "no game program in the package" : "can't decrypt the package (wrong or old keys?)"); goto out; }
    if (!nca_set_ctr_key(&nca, fd, &files, &keys, keys_path, why, why_size)) goto out;
    if (control_found && !nca_set_ctr_key(&control, fd, &files, &keys, keys_path, why, why_size))
        control_found = false; /* Program metadata remains useful; report NACP unavailable below. */
    Section exefs, romfs;
    if (!section_info(&nca, 0, &exefs) || !section_info(&nca, 1, &romfs) ||
        exefs.fs_type != 1 || romfs.fs_type != 0 || exefs.comp_size) {
        WHY("unsupported program section layout or patch/delta encryption"); goto out;
    }
    uint8_t magic[4];
    if (!section_read(fd, nca.base, &exefs, nca.ctr_key, magic, exefs.data_off, sizeof magic) || memcmp(magic, "PFS0", 4)) {
        WHY("can't decrypt the game code (wrong keys?)"); goto out;
    }
    package = calloc(1, sizeof *package);
    if (!package) { WHY("out of memory opening package"); goto out; }
    package->fd = fd; package->nca = nca; package->exefs = exefs; package->romfs = romfs;
    if (control_found) {
        for (int i=0;i<4&&!package->has_control;i++) { Section cr;
            if (section_info(&control,i,&cr) && cr.fs_type == 0 && !cr.comp_size) {
                package->control = control; package->control_romfs = cr; package->has_control = true;
            }
        }
    }
    memcpy(&package->title_id, nca.hdr + 0x210, 8);
    fd = -1;
out:
    if (fd >= 0) close(fd);
    free(files.e); keys_free(&keys); wipe(&control, sizeof control);
    wipe(&nca, sizeof nca);
    return package;
}

NxPackage *nx_package_open(const char *path, const char *keys_path, char *why, size_t why_size) {
    int fd = open_package(path);
    if (fd < 0) { WHY("can't open the game package"); return NULL; }
    NxPackage *p = nx_package_open_fd(fd, keys_path, why, why_size);
    close(fd); return p;
}
uint64_t nx_package_title_id(const NxPackage *p) { return p ? p->title_id : 0; }
void nx_package_close(NxPackage *p) { if (p) { close(p->fd); wipe(p, sizeof *p); free(p); } }

static bool romfs_file(NxPackage *p, const char *wanted, uint64_t *off, uint64_t *size) {
    if (!p->has_control) return false;
    Section *s=&p->control_romfs; uint8_t h[0x50];
    if (s->comp_size || s->data_size < sizeof h ||
        !section_read(p->fd,p->control.base,s,p->control.ctr_key,h,s->data_off,sizeof h)) return false;
    uint64_t dir_off=rd64(h+24),dir_size=rd64(h+32),file_off=rd64(h+56),file_size=rd64(h+64),data_off=rd64(h+72);
    if (!bounds(dir_off,dir_size,s->data_size)||!bounds(file_off,file_size,s->data_size)||data_off>s->data_size||dir_size<0x18||dir_size>MAX_TABLE||file_size>MAX_TABLE||
        s->data_off>UINT64_MAX-dir_off||s->data_off>UINT64_MAX-file_off) return false;
    uint8_t *dirs=malloc((size_t)dir_size),*files=malloc((size_t)file_size); if(!dirs||!files){free(dirs);free(files);return false;}
    bool ok=section_read(p->fd,p->control.base,s,p->control.ctr_key,dirs,s->data_off+dir_off,(size_t)dir_size)&&
            section_read(p->fd,p->control.base,s,p->control.ctr_key,files,s->data_off+file_off,(size_t)file_size);
    /* RomFS root directory metadata starts at offset zero. Follow only its direct file chain. */
    if(ok){uint32_t id=rd32(dirs+12);uint32_t walked=0;
        while(id!=UINT32_MAX&&walked++<4096){
            if(id>file_size||file_size-id<0x20){ok=false;break;}
            const uint8_t *e=files+id;uint32_t name_len=rd32(e+0x1c);uint32_t sibling=rd32(e+4);
            if(name_len>file_size-id-0x20){ok=false;break;}
            bool match=name_len==strlen(wanted)&&!memcmp(e+0x20,wanted,name_len);
            if(!strcmp(wanted,"@icon")&&name_len>=10&&!memcmp(e+0x20,"icon_",5)&&!memcmp(e+0x20+name_len-4,".dat",4))match=true;
            if(match){
                uint64_t rel=rd64(e+8),len=rd64(e+16);
                uint64_t data_size=s->data_size-data_off;
                if(!bounds(rel,len,data_size)||s->data_off>UINT64_MAX-data_off||s->data_off+data_off>UINT64_MAX-rel){ok=false;break;}
                *off=s->data_off+data_off+rel;*size=len;free(dirs);free(files);return true;
            }
            id=sibling;
        }
        if(id!=UINT32_MAX)ok=false;
    }
    free(dirs);free(files);return false;
}
static bool read_romfs_file(NxPackage*p,const char*name,uint8_t**data,size_t*size,size_t maximum){
    uint64_t off,len;if(!romfs_file(p,name,&off,&len)||len>maximum||len>SIZE_MAX)return false;
    uint8_t*b=malloc((size_t)len);if(!b)return false;
    if(!section_read(p->fd,p->control.base,&p->control_romfs,p->control.ctr_key,b,off,(size_t)len)){free(b);return false;}
    *data=b;*size=(size_t)len;return true;
}
bool nx_package_exefs_build_id(NxPackage*p,const char*wanted,uint8_t out[32]){
    if(!p||!wanted||!out)return false;uint8_t h[16];
    if(!section_read(p->fd,p->nca.base,&p->exefs,p->nca.ctr_key,h,p->exefs.data_off,sizeof h)||memcmp(h,"PFS0",4))return false;
    uint32_t count=rd32(h+4),strings=rd32(h+8);uint64_t meta=16ull+(uint64_t)count*24+strings;
    if(!count||count>256||!strings||strings>(1u<<20)||meta>p->exefs.data_size)return false;
    uint8_t*t=malloc((size_t)meta);if(!t)return false;
    bool ok=section_read(p->fd,p->nca.base,&p->exefs,p->nca.ctr_key,t,p->exefs.data_off,(size_t)meta),found=false;
    for(uint32_t i=0;ok&&i<count;i++){uint8_t*e=t+16+i*24;uint64_t off=rd64(e),size=rd64(e+8);uint32_t no=rd32(e+16);if(no>=strings){ok=false;break;}const char*name=(char*)t+16+count*24+no;size_t len=strnlen(name,strings-no);if(len==strings-no){ok=false;break;}if(strcmp(name,wanted))continue;
        if(size<0x60||!bounds(off,size,p->exefs.data_size-meta)){ok=false;break;}uint8_t id[0x60];if(!section_read(p->fd,p->nca.base,&p->exefs,p->nca.ctr_key,id,p->exefs.data_off+meta+off,sizeof id)||memcmp(id,"NSO0",4)){ok=false;break;}memcpy(out,id+0x40,32);found=true;break;}
    free(t);return ok&&found;
}
/* Complete validated ExeFS table with the same name/bound rules as extraction. */
static uint8_t *exefs_table(NxPackage*p,uint32_t*count,uint32_t*strings,uint64_t*meta){
    uint8_t h[16];
    if(!p||!section_read(p->fd,p->nca.base,&p->exefs,p->nca.ctr_key,h,p->exefs.data_off,sizeof h)||memcmp(h,"PFS0",4))return NULL;
    *count=rd32(h+4);*strings=rd32(h+8);*meta=16ull+(uint64_t)*count*24+*strings;
    if(!*count||*count>256||!*strings||*strings>(1u<<20)||*meta>p->exefs.data_size)return NULL;
    uint8_t*t=malloc((size_t)*meta);if(!t)return NULL;
    bool ok=section_read(p->fd,p->nca.base,&p->exefs,p->nca.ctr_key,t,p->exefs.data_off,(size_t)*meta);
    for(uint32_t i=0;ok&&i<*count;i++){
        const uint8_t*e=t+16+i*24;uint64_t off=rd64(e),size=rd64(e+8);uint32_t no=rd32(e+16);
        if(no>=*strings){ok=false;break;}
        const char*name=(const char*)t+16+*count*24+no;size_t len=strnlen(name,*strings-no);
        if(!len||len>=*strings-no||len>255||name[0]=='.'||strchr(name,'/')||strchr(name,'\\')||!bounds(off,size,p->exefs.data_size-*meta)){ok=false;break;}
        for(uint32_t j=0;j<i;j++)if(!strcmp(name,(const char*)t+16+*count*24+rd32(t+16+j*24+16))){ok=false;break;}
    }
    if(!ok){free(t);return NULL;}
    return t;
}
bool nx_package_exefs_list(NxPackage*p,bool(*visit)(void*,const char*,uint64_t),void*context){
    uint32_t count,strings;uint64_t meta;if(!visit)return false;
    uint8_t*t=exefs_table(p,&count,&strings,&meta);if(!t)return false;
    bool ok=true;
    for(uint32_t i=0;ok&&i<count;i++){const uint8_t*e=t+16+i*24;ok=visit(context,(const char*)t+16+count*24+rd32(e+16),rd64(e+8));}
    free(t);return ok;
}
bool nx_package_exefs_read(NxPackage*p,const char*wanted,uint64_t offset,void*dst,size_t size){
    uint32_t count,strings;uint64_t meta;if(!wanted||(!dst&&size))return false;
    uint8_t*t=exefs_table(p,&count,&strings,&meta);if(!t)return false;
    bool ok=false;
    for(uint32_t i=0;i<count;i++){
        const uint8_t*e=t+16+i*24;if(strcmp((const char*)t+16+count*24+rd32(e+16),wanted))continue;
        uint64_t off=rd64(e),length=rd64(e+8);
        ok=offset<=length&&size<=length-offset&&
           section_read(p->fd,p->nca.base,&p->exefs,p->nca.ctr_key,dst,p->exefs.data_off+meta+off+offset,size);
        break;
    }
    free(t);return ok;
}
bool nx_package_read_metadata(NxPackage*p,NxPackageMetadata*m,char*why,size_t why_size){
    if(m)memset(m,0,sizeof*m);if(!p||!m)return WHY("invalid package metadata request");
    uint8_t*nacp=NULL;size_t n=0;
    if(!read_romfs_file(p,"control.nacp",&nacp,&n,1u<<20)||n<0x3070){free(nacp);return WHY("Control NCA is missing a valid control.nacp");}
    /* Use the first populated language record; the NACP record is 0x300 bytes. */
    for(unsigned i=0;i<16;i++){const uint8_t*r=nacp+i*0x300;if(!r[0])continue;
        size_t a=strnlen((const char*)r,0x200),b=strnlen((const char*)r+0x200,0x100);
        memcpy(m->title,r,a);m->title[a]=0;memcpy(m->developer,r+0x200,b);m->developer[b]=0;break;}
    size_t v=strnlen((const char*)nacp+0x3060,0x10);memcpy(m->version,nacp+0x3060,v);m->version[v]=0;free(nacp);
    if(!m->title[0]||!m->developer[0])return WHY("Control NCA has no localized title/developer metadata");
    if(!nx_package_exefs_build_id(p,"main",m->main_build_id))return WHY("Cannot read the program main build identity");
    static const char*icons[]={"icon_AmericanEnglish.dat","icon_BritishEnglish.dat","icon_Japanese.dat","icon_AmericanSpanish.dat"};
    for(size_t i=0;i<sizeof icons/sizeof icons[0];i++)if(read_romfs_file(p,icons[i],&m->icon,&m->icon_size,16u<<20))break;
    if(!m->icon)read_romfs_file(p,"@icon",&m->icon,&m->icon_size,16u<<20);
    return true;
}
void nx_package_metadata_clear(NxPackageMetadata*m){if(m){free(m->icon);wipe(m,sizeof*m);}}

NxRomfs *nx_package_open_romfs(const NxPackage *p, char *why, size_t why_size) {
    if (!p) { WHY("no game package"); return NULL; }
    NxRomfs *r = calloc(1, sizeof *r);
    if (!r) { WHY("out of memory opening game data"); return NULL; }
    r->fd = -1;
    if (pthread_mutex_init(&r->lock, NULL)) { free(r); WHY("can't initialize game data reader"); return NULL; }
    r->fd = fcntl(p->fd, F_DUPFD_CLOEXEC, 0);
    r->nca_base = p->nca.base; r->sec = p->romfs; r->size = r->sec.data_size;
    memcpy(r->key, p->nca.ctr_key, sizeof r->key);
    if (r->fd < 0 || (r->sec.comp_size && !comp_load(r, why, why_size))) { nx_romfs_close(r); return NULL; }
    uint64_t header = 0;
    if (nx_romfs_pread(r, &header, sizeof header, 0) != sizeof header || header != 0x50) {
        WHY("invalid game data header (wrong keys or corrupt package)"); nx_romfs_close(r); return NULL;
    }
    return r;
}

// ExeFS is deliberately small and flat. Validate the complete table before
// publishing files; use O_EXCL and compare existing data rather than overwrite.
static bool exefs_write(NxPackage *p, const char *path, uint64_t off, uint64_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool existing = fd < 0 && errno == EEXIST;
    if (existing) fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat sb;
    bool ok = !fstat(fd, &sb) && S_ISREG(sb.st_mode) && (!existing || (uint64_t)sb.st_size == size);
    uint8_t *data = malloc(1 << 20), *other = existing ? malloc(1 << 20) : NULL;
    if (!data || (existing && !other)) ok = false;
    for (uint64_t done = 0; ok && done < size;) {
        size_t n = size - done < (1u << 20) ? (size_t)(size - done) : (1u << 20);
        ok = section_read(p->fd, p->nca.base, &p->exefs, p->nca.ctr_key, data, off + done, n);
        if (ok && existing) ok = pread_all(fd, other, n, done) && !memcmp(data, other, n);
        else if (ok) {
            size_t written = 0;
            while (written < n) {
                ssize_t got = write(fd, data + written, n - written);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) { ok = false; break; }
                written += (size_t)got;
            }
        }
        done += n;
    }
    free(data); free(other);
    if (!existing && ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (!ok && !existing) unlink(path);
    return ok;
}
bool nx_package_extract_exefs(NxPackage *p, const char *dir, char *why, size_t why_size) {
    if (!p || !dir || !*dir || strlen(dir) > 3800) return WHY("invalid game code cache path");
    uint8_t header[16];
    if (!section_read(p->fd, p->nca.base, &p->exefs, p->nca.ctr_key, header, p->exefs.data_off, sizeof header) ||
        memcmp(header, "PFS0", 4)) return WHY("invalid game code file table");
    uint32_t count, strings; memcpy(&count, header + 4, 4); memcpy(&strings, header + 8, 4);
    uint64_t meta = 16ull + (uint64_t)count * 24 + strings;
    if (!count || count > 256 || !strings || strings > (1u << 20) || meta > p->exefs.data_size)
        return WHY("game code file table exceeds supported bounds");
    uint8_t *table = malloc((size_t)meta);
    if (!table) return WHY("out of memory reading game code files");
    bool ok = section_read(p->fd, p->nca.base, &p->exefs, p->nca.ctr_key, table, p->exefs.data_off, (size_t)meta);
    uint64_t total = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *entry = table + 16 + i * 24;
        uint64_t off, size; uint32_t no;
        memcpy(&off, entry, 8); memcpy(&size, entry + 8, 8); memcpy(&no, entry + 16, 4);
        if (no >= strings) { ok = false; break; }
        const char *name = (const char *)table + 16 + count * 24 + no;
        size_t len = strnlen(name, strings - no);
        if (!len || len >= strings - no || len > 255 || name[0] == '.' || strchr(name, '/') || strchr(name, '\\') ||
            !bounds(off, size, p->exefs.data_size - meta) || size > (256u << 20) || total > (512u << 20) - size) { ok = false; break; }
        total += size;
        for (uint32_t j = 0; j < i; j++) {
            uint32_t prev; memcpy(&prev, table + 16 + j * 24 + 16, 4);
            if (!strcmp(name, (const char *)table + 16 + count * 24 + prev)) { ok = false; break; }
        }
    }
    if (!ok) { free(table); return WHY("invalid or unsupported game code file bounds/names"); }
    struct stat sb;
    if ((mkdir(dir, 0700) && errno != EEXIST) || lstat(dir, &sb) || !S_ISDIR(sb.st_mode)) {
        free(table); return WHY("can't open game code cache directory");
    }
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *entry = table + 16 + i * 24;
        uint64_t off, size; uint32_t no;
        memcpy(&off, entry, 8); memcpy(&size, entry + 8, 8); memcpy(&no, entry + 16, 4);
        const char *name = (const char *)table + 16 + count * 24 + no;
        char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
        ok = exefs_write(p, path, p->exefs.data_off + meta + off, size);
    }
    free(table);
    if (!ok) return WHY("can't write game code cache or existing files conflict");
    return true;
}
