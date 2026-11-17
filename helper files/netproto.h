/* netproto.h — shared networking helpers for NM, SS and client
 *
 *  - send_all / recv_all     : loop until every byte is transferred
 *  - send_frame / recv_frame : 4-byte big-endian length + payload, so
 *                              message boundaries never depend on how TCP
 *                              happens to split or merge segments
 *  - SHA-256 / HMAC-SHA-256  : dependency-free
 *  - nfs_auth_accept/connect : mutual challenge-response using a shared
 *                              secret (the secret itself never crosses the wire)
 *  - nfs_valid_name          : single definition of what a file name may be
 *
 * NOTE: this authenticates peers but does NOT encrypt traffic.
 */
#ifndef NETPROTO_H
#define NETPROTO_H

#include "nfs_compat.h"   /* Winsock / POSIX differences live in here */

#define NFS_MAX_NAME    1000                 /* longest accepted file name   */
#define NFS_CTRL_MAX    (1u << 20)           /* largest control frame (1 MB) */
#define NFS_CHUNK       65536                /* data chunk size              */
#define NFS_MAX_WRITE   (16u * 1024u * 1024u)/* max bytes per write request  */
#define NFS_MIN_SECRET  8

/* ------------------------------------------------------------------ */
/* Socket I/O                                                          */
/* ------------------------------------------------------------------ */
/* One chunk at a time: Winsock's send()/recv() take an int length, so the
 * transfer is capped per call and the loop handles the rest. */
#define NFS_IO_STEP (1 << 20)

static inline int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len)
    {
        int want = len > NFS_IO_STEP ? NFS_IO_STEP : (int)len;
        int n = (int)send(fd, p, want, MSG_NOSIGNAL);
        if (n < 0) { if (nfs_sock_eintr()) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static inline int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len)
    {
        int want = len > NFS_IO_STEP ? NFS_IO_STEP : (int)len;
        int n = (int)recv(fd, p, want, 0);
        if (n < 0) { if (nfs_sock_eintr()) continue; return -1; }
        if (n == 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* Writes to a FILE descriptor (not a socket). */
static inline int fd_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len)
    {
        unsigned want = len > NFS_IO_STEP ? NFS_IO_STEP : (unsigned)len;
#ifdef _WIN32
        int n = _write(fd, p, want);
#else
        ssize_t n = write(fd, p, want);
#endif
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* Reads from a FILE descriptor (not a socket). Returns bytes read, 0 at EOF, -1 on error. */
static inline int fd_read_some(int fd, void *buf, size_t len)
{
    unsigned want = len > NFS_IO_STEP ? NFS_IO_STEP : (unsigned)len;
#ifdef _WIN32
    return _read(fd, buf, want);
#else
    return (int)read(fd, buf, want);
#endif
}

/* Frame = uint32 big-endian length, then payload. Zero-length frames are legal. */
static inline int send_frame(int fd, const void *buf, uint32_t len)
{
    uint32_t be = htonl(len);
    if (len <= 2048)
    {
        char tmp[4 + 2048];
        memcpy(tmp, &be, 4);
        if (len) memcpy(tmp + 4, buf, len);
        return send_all(fd, tmp, 4 + (size_t)len);
    }
    if (send_all(fd, &be, 4)) return -1;
    return send_all(fd, buf, len);
}

/* Returns 0 ok, -1 I/O error, -2 frame larger than cap (protocol violation). */
static inline int recv_frame(int fd, void *buf, uint32_t cap, uint32_t *out)
{
    uint32_t be;
    if (recv_all(fd, &be, 4)) return -1;
    uint32_t len = ntohl(be);
    if (len > cap) return -2;
    if (len && recv_all(fd, buf, len)) return -1;
    *out = len;
    return 0;
}

static inline int send_str(int fd, const char *s)
{
    return send_frame(fd, s, (uint32_t)strlen(s));
}

/* Receives a text frame into buf (always NUL-terminated). cap includes the NUL. */
static inline int recv_str(int fd, char *buf, size_t cap)
{
    uint32_t n = 0;
    int r = recv_frame(fd, buf, (uint32_t)(cap - 1), &n);
    if (r) return r;
    buf[n] = '\0';
    return 0;
}

static inline void nfs_tune(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

static inline void nfs_reuseaddr(int fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
}

/* secs == 0 disables the timeout */
static inline void nfs_set_timeout(int fd, int secs)
{
#ifdef _WIN32
    DWORD ms = (DWORD)secs * 1000u;                 /* Winsock wants milliseconds */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv; tv.tv_sec = secs; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#endif
}

/* ------------------------------------------------------------------ */
/* SHA-256 / HMAC-SHA-256                                              */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t h[8]; uint64_t total; uint8_t buf[64]; size_t blen; } nfs_sha256;

static const uint32_t NFS_K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define NFS_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void nfs_sha256_block(nfs_sha256 *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = NFS_ROR(w[i-15], 7) ^ NFS_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = NFS_ROR(w[i-2], 17) ^ NFS_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t S1 = NFS_ROR(e, 6) ^ NFS_ROR(e, 11) ^ NFS_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + NFS_K256[i] + w[i];
        uint32_t S0 = NFS_ROR(a, 2) ^ NFS_ROR(a, 13) ^ NFS_ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void nfs_sha256_init(nfs_sha256 *c)
{
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv)); c->total = 0; c->blen = 0;
}

static void nfs_sha256_update(nfs_sha256 *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len)
    {
        size_t take = 64 - c->blen; if (take > len) take = len;
        memcpy(c->buf + c->blen, p, take);
        c->blen += take; p += take; len -= take;
        if (c->blen == 64) { nfs_sha256_block(c, c->buf); c->blen = 0; }
    }
}

static void nfs_sha256_final(nfs_sha256 *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    nfs_sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->blen != 56) nfs_sha256_update(c, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    nfs_sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++)
    {
        out[4*i]   = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16);
        out[4*i+2] = (uint8_t)(c->h[i] >> 8);  out[4*i+3] = (uint8_t)(c->h[i]);
    }
}

static void nfs_hmac_sha256(const uint8_t *key, size_t klen,
                            const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[32];
    if (klen > 64) { nfs_sha256 t; nfs_sha256_init(&t); nfs_sha256_update(&t, key, klen); nfs_sha256_final(&t, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    nfs_sha256 c;
    nfs_sha256_init(&c); nfs_sha256_update(&c, ipad, 64); nfs_sha256_update(&c, msg, mlen); nfs_sha256_final(&c, inner);
    nfs_sha256_init(&c); nfs_sha256_update(&c, opad, 64); nfs_sha256_update(&c, inner, 32); nfs_sha256_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* Shared-secret mutual authentication                                 */
/*                                                                     */
/*   S -> C : Ns (32 random bytes)                                     */
/*   C -> S : Nc (32 random) || HMAC(K, 'C' || Ns || Nc)               */
/*   S -> C : HMAC(K, 'S' || Ns || Nc)     (sent only if C verified)   */
/*                                                                     */
/* Fresh nonces per connection stop replay; the role tags stop a peer  */
/* from reflecting our own answer back at us.                          */
/* ------------------------------------------------------------------ */
/* nfs_random() lives in nfs_compat.h: /dev/urandom on POSIX,
 * BCryptGenRandom() on Windows. */

static inline int nfs_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static inline int nfs_auth_accept(int fd, const char *secret)
{
    uint8_t ns[32], in[64], msg[65], expect[32], reply[32];
    if (nfs_random(ns, 32) || send_all(fd, ns, 32) || recv_all(fd, in, 64)) return -1;
    msg[0] = 'C'; memcpy(msg + 1, ns, 32); memcpy(msg + 33, in, 32);   /* in[0..31] = Nc */
    nfs_hmac_sha256((const uint8_t *)secret, strlen(secret), msg, 65, expect);
    if (!nfs_ct_equal(expect, in + 32, 32)) return -1;
    msg[0] = 'S';
    nfs_hmac_sha256((const uint8_t *)secret, strlen(secret), msg, 65, reply);
    return send_all(fd, reply, 32);
}

static inline int nfs_auth_connect(int fd, const char *secret)
{
    uint8_t ns[32], out[64], msg[65], got[32], expect[32];
    if (recv_all(fd, ns, 32) || nfs_random(out, 32)) return -1;        /* out[0..31] = Nc */
    msg[0] = 'C'; memcpy(msg + 1, ns, 32); memcpy(msg + 33, out, 32);
    nfs_hmac_sha256((const uint8_t *)secret, strlen(secret), msg, 65, out + 32);
    if (send_all(fd, out, 64) || recv_all(fd, got, 32)) return -1;     /* server closes on bad MAC */
    msg[0] = 'S';
    nfs_hmac_sha256((const uint8_t *)secret, strlen(secret), msg, 65, expect);
    return nfs_ct_equal(expect, got, 32) ? 0 : -1;
}

/* Reads the shared secret from $NFS_SECRET (or prompts when interactive).
 * Returns NULL (after printing why) when missing or too short.          */
static char nfs_secret_buf[256];
static inline const char *nfs_load_secret(int interactive)
{
    const char *s = getenv("NFS_SECRET");
    if ((!s || !*s) && interactive) s = nfs_getpass("Shared secret: ");
    if (!s || !*s)
    {
        fprintf(stderr, "[!] No shared secret. Set NFS_SECRET (at least %d characters).\n", NFS_MIN_SECRET);
        return NULL;
    }
    if (strlen(s) < NFS_MIN_SECRET || strlen(s) >= sizeof(nfs_secret_buf))
    {
        fprintf(stderr, "[!] Shared secret must be %d-%d characters.\n", NFS_MIN_SECRET, (int)sizeof(nfs_secret_buf) - 1);
        return NULL;
    }
    snprintf(nfs_secret_buf, sizeof(nfs_secret_buf), "%s", s);
    return nfs_secret_buf;
}

/* ------------------------------------------------------------------ */
/* File-name validation (used by client, NM and — authoritatively — SS)*/
/*                                                                     */
/* Any printable name is fine (dots, upper-case, spaces, sub-folders). */
/* Rejected: empty, absolute, backslash/control chars, "." or ".."     */
/* components, empty components ("a//b"), over-long names.             */
/* A trailing '/' means "directory".                                   */
/* ------------------------------------------------------------------ */
static inline int nfs_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > NFS_MAX_NAME || name[0] == '/') return 0;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '\\') return 0;
    }
    const char *p = name;
    while (*p)
    {
        const char *e = strchr(p, '/');
        size_t cl = e ? (size_t)(e - p) : strlen(p);
        if (cl == 0) return 0;
        if (cl == 1 && p[0] == '.') return 0;
        if (cl == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!e) break;
        p = e + 1;
    }
    return 1;
}

#endif /* NETPROTO_H */
