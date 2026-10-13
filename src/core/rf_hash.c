/*
 * rf_hash.c -- MD5 / SHA-1 / SHA-256 / SHA-512 的可移植实现
 *
 * 说明：上游 Rufus 的 hash.c 与 Win32 深度耦合（crtdbg / intrin / 注册表）。
 * 移植时保留相同的算法与摘要长度，但以纯 C 重写，去掉平台加速分支，
 * 从而在任意 POSIX 系统上给出与 Windows 版完全一致的摘要值。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rf_hash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ==================== 通用小工具 ==================== */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static uint32_t rd32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr32be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void wr32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static uint32_t rd32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64be(const unsigned char *p)
{
    return ((uint64_t)rd32be(p) << 32) | rd32be(p + 4);
}
static void wr64be(unsigned char *p, uint64_t v)
{
    wr32be(p, (uint32_t)(v >> 32));
    wr32be(p + 4, (uint32_t)v);
}

/* ==================== MD5 ==================== */

static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const unsigned char md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void md5_block(rf_hash_ctx *c, const unsigned char *p)
{
    uint32_t a = c->state32[0], b = c->state32[1], cc = c->state32[2], d = c->state32[3];
    uint32_t m[16], f, tmp;
    int i, g;

    for (i = 0; i < 16; i++)
        m[i] = rd32le(p + i * 4);

    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (b & cc) | (~b & d);        g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);        g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d;                 g = (3 * i + 5) & 15; }
        else             { f = cc ^ (b | ~d);              g = (7 * i) & 15; }
        tmp = d;
        d = cc;
        cc = b;
        b = b + ROTL32(a + f + md5_k[i] + m[g], md5_s[i]);
        a = tmp;
    }
    c->state32[0] += a; c->state32[1] += b; c->state32[2] += cc; c->state32[3] += d;
}

/* ==================== SHA-1 ==================== */

static void sha1_block(rf_hash_ctx *c, const unsigned char *p)
{
    uint32_t w[80], a, b, cc, d, e, f, k, tmp;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = rd32be(p + i * 4);
    for (i = 16; i < 80; i++)
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->state32[0]; b = c->state32[1]; cc = c->state32[2];
    d = c->state32[3]; e = c->state32[4];

    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else             { f = b ^ cc ^ d;                   k = 0xca62c1d6; }
        tmp = ROTL32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROTL32(b, 30); b = a; a = tmp;
    }
    c->state32[0] += a; c->state32[1] += b; c->state32[2] += cc;
    c->state32[3] += d; c->state32[4] += e;
}

/* ==================== SHA-256 ==================== */

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(rf_hash_ctx *c, const unsigned char *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = rd32be(p + i * 4);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->state32[0]; b = c->state32[1]; cc = c->state32[2]; d = c->state32[3];
    e = c->state32[4]; f = c->state32[5]; g = c->state32[6]; h = c->state32[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + sha256_k[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state32[0] += a; c->state32[1] += b; c->state32[2] += cc; c->state32[3] += d;
    c->state32[4] += e; c->state32[5] += f; c->state32[6] += g; c->state32[7] += h;
}

/* ==================== SHA-512 ==================== */

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

static void sha512_block(rf_hash_ctx *c, const unsigned char *p)
{
    uint64_t w[80], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = rd64be(p + i * 8);
    for (i = 16; i < 80; i++) {
        uint64_t s0 = ROTR64(w[i - 15], 1) ^ ROTR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROTR64(w[i - 2], 19) ^ ROTR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->state64[0]; b = c->state64[1]; cc = c->state64[2]; d = c->state64[3];
    e = c->state64[4]; f = c->state64[5]; g = c->state64[6]; h = c->state64[7];

    for (i = 0; i < 80; i++) {
        uint64_t S1 = ROTR64(e, 14) ^ ROTR64(e, 18) ^ ROTR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t S0 = ROTR64(a, 28) ^ ROTR64(a, 34) ^ ROTR64(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + sha512_k[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state64[0] += a; c->state64[1] += b; c->state64[2] += cc; c->state64[3] += d;
    c->state64[4] += e; c->state64[5] += f; c->state64[6] += g; c->state64[7] += h;
}

/* ==================== 元信息 ==================== */

size_t rf_hash_size(rf_hash_algo algo)
{
    switch (algo) {
    case RF_HASH_MD5:    return RF_MD5_SIZE;
    case RF_HASH_SHA1:   return RF_SHA1_SIZE;
    case RF_HASH_SHA256: return RF_SHA256_SIZE;
    case RF_HASH_SHA512: return RF_SHA512_SIZE;
    default:             return 0;
    }
}

const char *rf_hash_name(rf_hash_algo algo)
{
    switch (algo) {
    case RF_HASH_MD5:    return "MD5";
    case RF_HASH_SHA1:   return "SHA1";
    case RF_HASH_SHA256: return "SHA256";
    case RF_HASH_SHA512: return "SHA512";
    default:             return "?";
    }
}

int rf_hash_by_name(const char *name, rf_hash_algo *out)
{
    int i;
    if (!name || !out)
        return RF_ERR_INVALID_ARG;
    for (i = 0; i < RF_HASH_COUNT; i++) {
        if (strcasecmp(name, rf_hash_name((rf_hash_algo)i)) == 0) {
            *out = (rf_hash_algo)i;
            return RF_OK;
        }
    }
    return RF_ERR_INVALID_ARG;
}

/* ==================== 流式接口 ==================== */

int rf_hash_init(rf_hash_ctx *c, rf_hash_algo algo)
{
    if (!c)
        return RF_ERR_INVALID_ARG;
    memset(c, 0, sizeof(*c));
    c->algo = algo;
    switch (algo) {
    case RF_HASH_MD5:
        c->state32[0] = 0x67452301; c->state32[1] = 0xefcdab89;
        c->state32[2] = 0x98badcfe; c->state32[3] = 0x10325476;
        break;
    case RF_HASH_SHA1:
        c->state32[0] = 0x67452301; c->state32[1] = 0xefcdab89;
        c->state32[2] = 0x98badcfe; c->state32[3] = 0x10325476;
        c->state32[4] = 0xc3d2e1f0;
        break;
    case RF_HASH_SHA256:
        c->state32[0] = 0x6a09e667; c->state32[1] = 0xbb67ae85;
        c->state32[2] = 0x3c6ef372; c->state32[3] = 0xa54ff53a;
        c->state32[4] = 0x510e527f; c->state32[5] = 0x9b05688c;
        c->state32[6] = 0x1f83d9ab; c->state32[7] = 0x5be0cd19;
        break;
    case RF_HASH_SHA512:
        c->state64[0] = 0x6a09e667f3bcc908ULL; c->state64[1] = 0xbb67ae8584caa73bULL;
        c->state64[2] = 0x3c6ef372fe94f82bULL; c->state64[3] = 0xa54ff53a5f1d36f1ULL;
        c->state64[4] = 0x510e527fade682d1ULL; c->state64[5] = 0x9b05688c2b3e6c1fULL;
        c->state64[6] = 0x1f83d9abfb41bd6bULL; c->state64[7] = 0x5be0cd19137e2179ULL;
        break;
    default:
        return RF_ERR_NOT_SUPPORTED;
    }
    return RF_OK;
}

#define BLOCK_BYTES 128

int rf_hash_update(rf_hash_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t block = (c && c->algo == RF_HASH_SHA512) ? 128 : 64;

    if (!c || (!data && len))
        return RF_ERR_INVALID_ARG;

    c->bitlen += (uint64_t)len * 8;

    if (c->buflen) {
        size_t need = block - c->buflen;
        size_t take = len < need ? len : need;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint32_t)take;
        p += take;
        len -= take;
        if (c->buflen == block) {
            switch (c->algo) {
            case RF_HASH_MD5:    md5_block(c, c->buf); break;
            case RF_HASH_SHA1:   sha1_block(c, c->buf); break;
            case RF_HASH_SHA256: sha256_block(c, c->buf); break;
            case RF_HASH_SHA512: sha512_block(c, c->buf); break;
            default: break;
            }
            c->buflen = 0;
        }
    }

    while (len >= block) {
        switch (c->algo) {
        case RF_HASH_MD5:    md5_block(c, p); break;
        case RF_HASH_SHA1:   sha1_block(c, p); break;
        case RF_HASH_SHA256: sha256_block(c, p); break;
        case RF_HASH_SHA512: sha512_block(c, p); break;
        default: break;
        }
        p += block;
        len -= block;
    }

    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = (uint32_t)len;
    }
    return RF_OK;
}

int rf_hash_final(rf_hash_ctx *c, unsigned char *out)
{
    unsigned char pad[256];
    uint64_t bits = c->bitlen;
    size_t block = (c->algo == RF_HASH_SHA512) ? 128 : 64;
    size_t padlen, i;
    size_t lenlen = (c->algo == RF_HASH_SHA512) ? 16 : 8;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    /* 需要追加的字节数 k 满足 buflen + k ≡ block - lenlen (mod block)，
     * 且 k >= 1（0x80 本身占 1 字节）。 */
    padlen = block - ((c->buflen + lenlen) % block);
    if (padlen == 0)
        padlen = block;

    {
        /* 构造最后的填充块 */
        unsigned char tail[256];
        size_t total = 0;
        memcpy(tail, pad, padlen);
        total = padlen;
        if (c->algo == RF_HASH_SHA512) {
            /* SHA-512 使用 128 位大端长度；高位恒为 0（我们只处理 < 2^64 位） */
            wr64be(tail + total, 0);
            wr64be(tail + total + 8, bits);
        } else if (c->algo == RF_HASH_MD5) {
            /* MD5 使用 64 位小端长度 */
            wr32le(tail + total, (uint32_t)bits);
            wr32le(tail + total + 4, (uint32_t)(bits >> 32));
        } else {
            /* SHA-1 / SHA-256 使用 64 位大端长度 */
            wr32be(tail + total, (uint32_t)(bits >> 32));
            wr32be(tail + total + 4, (uint32_t)bits);
        }
        total += lenlen;
        rf_hash_update(c, tail, total);
        /* rf_hash_update 已把 bitlen 增加；下面不再使用它 */
    }

    switch (c->algo) {
    case RF_HASH_MD5:
        for (i = 0; i < 4; i++)
            wr32le(out + i * 4, c->state32[i]);
        break;
    case RF_HASH_SHA1:
        for (i = 0; i < 5; i++)
            wr32be(out + i * 4, c->state32[i]);
        break;
    case RF_HASH_SHA256:
        for (i = 0; i < 8; i++)
            wr32be(out + i * 4, c->state32[i]);
        break;
    case RF_HASH_SHA512:
        for (i = 0; i < 8; i++)
            wr64be(out + i * 8, c->state64[i]);
        break;
    default:
        return RF_ERR_NOT_SUPPORTED;
    }
    return RF_OK;
}

int rf_hash_buffer(rf_hash_algo algo, const void *data, size_t len, unsigned char *out)
{
    rf_hash_ctx c;
    int rc = rf_hash_init(&c, algo);
    if (rc != RF_OK)
        return rc;
    rf_hash_update(&c, data, len);
    return rf_hash_final(&c, out);
}

void rf_hash_hex(const unsigned char *digest, size_t len, char *out)
{
    static const char *hex = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* ==================== 文件 / 设备摘要 ==================== */

#define HASH_CHUNK (4u * 1024 * 1024)

int rf_hash_fd(rf_hash_algo algo, int fd, uint64_t limit, unsigned char *out,
               const char *phase, rf_progress_cb progress, void *user)
{
    rf_hash_ctx c;
    unsigned char *buf;
    uint64_t done = 0;
    double t0 = rf_monotonic();
    int rc;

    rc = rf_hash_init(&c, algo);
    if (rc != RF_OK)
        return rc;

    buf = malloc(HASH_CHUNK);
    if (!buf)
        return RF_ERR_NO_MEMORY;

    if (lseek(fd, 0, SEEK_SET) < 0 && errno != ESPIPE) {
        free(buf);
        return RF_ERR_IO;
    }

    for (;;) {
        size_t want = HASH_CHUNK;
        ssize_t n;
        if (limit && (uint64_t)(limit - done) < (uint64_t)want)
            want = (size_t)(limit - done);
        if (want == 0)
            break;
        n = read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return RF_ERR_IO;
        }
        if (n == 0)
            break;
        rf_hash_update(&c, buf, (size_t)n);
        done += (uint64_t)n;

        if (progress && limit) {
            rf_progress p;
            double el = rf_monotonic() - t0;
            memset(&p, 0, sizeof(p));
            p.done = done;
            p.total = limit;
            p.speed_bps = (el > 0.001) ? (double)done / el : 0.0;
            p.eta_sec = (p.speed_bps > 0) ? (double)(limit - done) / p.speed_bps : -1.0;
            p.percent = (int)((double)done * 100.0 / (double)limit);
            p.phase = phase ? phase : "计算摘要";
            if (progress(&p, user))
                break;
        }
    }
    free(buf);
    return rf_hash_final(&c, out);
}

int rf_hash_file(rf_hash_algo algo, const char *path, unsigned char *out,
                 rf_progress_cb progress, void *user)
{
    struct stat st;
    uint64_t limit = 0;
    int fd, rc;

    if (!path || !out)
        return RF_ERR_INVALID_ARG;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return (errno == EACCES) ? RF_ERR_PERMISSION : RF_ERR_IO;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        limit = (uint64_t)st.st_size;
    else if (lseek(fd, 0, SEEK_END) >= 0)
        limit = (uint64_t)lseek(fd, 0, SEEK_SET);
    rc = rf_hash_fd(algo, fd, limit, out, "计算摘要", progress, user);
    close(fd);
    return rc;
}

/* ==================== CRC32 ==================== */

static uint32_t crc_table[256];
static int crc_table_ready = 0;

uint32_t rf_crc32(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t i;

    if (!crc_table_ready) {
        uint32_t c;
        int n, k;
        for (n = 0; n < 256; n++) {
            c = (uint32_t)n;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crc_table[n] = c;
        }
        crc_table_ready = 1;
    }
    crc = ~crc;
    for (i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
