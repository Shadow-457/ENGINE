/*
 * tls.h — Minimal TLS 1.2 client for Systrix OS
 *
 * Cipher suite: TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009C)
 * Self-contained: AES-128, GCM, SHA-256, HMAC-SHA256, PRF, PKCS#1 v1.5 verify stub.
 * Wraps Systrix sys_connect / sys_send / sys_recv syscalls.
 *
 * Usage:
 *   TlsCtx ctx;
 *   int fd = tls_connect(&ctx, "93.184.216.34", 443, "example.com");
 *   tls_write(&ctx, "GET / HTTP/1.0\r\n\r\n", 18);
 *   char buf[4096]; int n = tls_read(&ctx, buf, sizeof(buf));
 *   tls_close(&ctx);
 */

#ifndef SYSTRIX_TLS_H
#define SYSTRIX_TLS_H

#include "libc.h"

/* ── Types ───────────────────────────────────────────────────────────── */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* ── AES-128 ─────────────────────────────────────────────────────────── */
#define AES_BLOCK 16
#define AES_ROUNDS 10

static const u8 _sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const u8 _rcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

typedef struct { u8 rk[11][16]; } AesKey;

static u8 _xtime(u8 a) { return (a<<1)^((a>>7)?0x1b:0); }
static u8 _gmul(u8 a, u8 b) {
    u8 r=0; for(int i=0;i<8;i++){if(b&1)r^=a;a=_xtime(a);b>>=1;} return r;
}

static void aes_key_expand(AesKey *k, const u8 *key) {
    memcpy(k->rk[0], key, 16);
    for (int i = 1; i <= AES_ROUNDS; i++) {
        u8 *prev = k->rk[i-1], *cur = k->rk[i];
        cur[0]  = prev[0] ^ _sbox[prev[13]] ^ _rcon[i];
        cur[1]  = prev[1] ^ _sbox[prev[14]];
        cur[2]  = prev[2] ^ _sbox[prev[15]];
        cur[3]  = prev[3] ^ _sbox[prev[12]];
        for (int j = 4; j < 16; j++) cur[j] = prev[j] ^ cur[j-4];
    }
}

static void aes_encrypt_block(const AesKey *k, u8 *b) {
    u8 s[16];
    for (int i=0;i<16;i++) b[i]^=k->rk[0][i];
    for (int r=1;r<=AES_ROUNDS;r++) {
        for (int i=0;i<16;i++) s[i]=_sbox[b[i]];
        /* ShiftRows */
        u8 t;
        t=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t;
        t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
        t=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t;
        if (r < AES_ROUNDS) {
            /* MixColumns */
            for (int c=0;c<4;c++) {
                u8 *col=s+4*c;
                u8 a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=_gmul(0x02,a0)^_gmul(0x03,a1)^a2^a3;
                col[1]=a0^_gmul(0x02,a1)^_gmul(0x03,a2)^a3;
                col[2]=a0^a1^_gmul(0x02,a2)^_gmul(0x03,a3);
                col[3]=_gmul(0x03,a0)^a1^a2^_gmul(0x02,a3);
            }
        }
        for (int i=0;i<16;i++) b[i]=s[i]^k->rk[r][i];
    }
}

/* ── GCM ─────────────────────────────────────────────────────────────── */
static void _gcm_gf_mul(u8 *x, const u8 *h) {
    u8 z[16]={0}, v[16];
    memcpy(v, h, 16);
    for (int i=0;i<128;i++) {
        if (x[i/8] & (0x80>>(i%8))) for(int j=0;j<16;j++) z[j]^=v[j];
        u8 lsb = v[15]&1;
        for (int j=15;j>0;j--) v[j]=(v[j]>>1)|((v[j-1]&1)<<7);
        v[0]>>=1;
        if (lsb) v[0]^=0xe1;
    }
    memcpy(x, z, 16);
}

static void _gcm_ghash(u8 *tag, const u8 *h, const u8 *data, u32 len) {
    u8 block[16];
    while (len >= 16) {
        for (int i=0;i<16;i++) tag[i]^=data[i];
        _gcm_gf_mul(tag, h);
        data+=16; len-=16;
    }
    if (len) {
        memset(block,0,16);
        memcpy(block,data,len);
        for (int i=0;i<16;i++) tag[i]^=block[i];
        _gcm_gf_mul(tag, h);
    }
}

typedef struct {
    AesKey key;
    u8     h[16];      /* H = AES(K, 0^128) */
    u8     iv[12];     /* fixed IV (4 bytes from handshake + 8 bytes explicit) */
    u64    seq;        /* sequence number for explicit nonce */
} GcmCtx;

static void gcm_init(GcmCtx *g, const u8 *key16, const u8 *iv4) {
    aes_key_expand(&g->key, key16);
    memset(g->h, 0, 16);
    aes_encrypt_block(&g->key, g->h);
    memcpy(g->iv, iv4, 4);
    g->seq = 0;
}

/* Encrypt plaintext in-place, append 16-byte tag. aad = additional data. */
static void gcm_seal(GcmCtx *g, u8 *ct, u32 len,
                     const u8 *aad, u32 aad_len, u8 *tag) {
    u8 j0[16], ek0[16], ctr[16], block[16];
    /* Build J0 = IV || seq (8 bytes explicit) || 0x00000001 */
    memcpy(j0, g->iv, 4);
    j0[4]=(g->seq>>56)&0xff; j0[5]=(g->seq>>48)&0xff;
    j0[6]=(g->seq>>40)&0xff; j0[7]=(g->seq>>32)&0xff;
    j0[8]=(g->seq>>24)&0xff; j0[9]=(g->seq>>16)&0xff;
    j0[10]=(g->seq>>8)&0xff; j0[11]=g->seq&0xff;
    j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
    memcpy(ek0, j0, 16); aes_encrypt_block(&g->key, ek0);
    /* CTR encrypt */
    memcpy(ctr, j0, 16);
    for (u32 i=0; i<len; i++) {
        if (i%16 == 0) {
            /* increment ctr */
            for(int c=15;c>=12;c--) if(++ctr[c]) break;
            memcpy(block, ctr, 16);
            aes_encrypt_block(&g->key, block);
        }
        ct[i] ^= block[i%16];
    }
    /* GHASH over AAD then ciphertext */
    u8 s[16]={0};
    _gcm_ghash(s, g->h, aad, aad_len);
    _gcm_ghash(s, g->h, ct, len);
    /* length block */
    u8 lb[16]={0};
    u64 aad_bits = (u64)aad_len*8, ct_bits = (u64)len*8;
    lb[0]=(aad_bits>>56)&0xff; lb[1]=(aad_bits>>48)&0xff;
    lb[2]=(aad_bits>>40)&0xff; lb[3]=(aad_bits>>32)&0xff;
    lb[4]=(aad_bits>>24)&0xff; lb[5]=(aad_bits>>16)&0xff;
    lb[6]=(aad_bits>>8)&0xff;  lb[7]=aad_bits&0xff;
    lb[8]=(ct_bits>>56)&0xff;  lb[9]=(ct_bits>>48)&0xff;
    lb[10]=(ct_bits>>40)&0xff; lb[11]=(ct_bits>>32)&0xff;
    lb[12]=(ct_bits>>24)&0xff; lb[13]=(ct_bits>>16)&0xff;
    lb[14]=(ct_bits>>8)&0xff;  lb[15]=ct_bits&0xff;
    for(int i=0;i<16;i++) s[i]^=lb[i];
    _gcm_gf_mul(s, g->h);
    for(int i=0;i<16;i++) tag[i]=s[i]^ek0[i];
    g->seq++;
}

/* Decrypt + verify tag. Returns 0 on success, -1 on tag mismatch. */
static int gcm_open(GcmCtx *g, u8 *pt, u32 len,
                    const u8 *aad, u32 aad_len, const u8 *tag) {
    u8 j0[16], ek0[16], ctr[16], block[16];
    memcpy(j0, g->iv, 4);
    j0[4]=(g->seq>>56)&0xff; j0[5]=(g->seq>>48)&0xff;
    j0[6]=(g->seq>>40)&0xff; j0[7]=(g->seq>>32)&0xff;
    j0[8]=(g->seq>>24)&0xff; j0[9]=(g->seq>>16)&0xff;
    j0[10]=(g->seq>>8)&0xff; j0[11]=g->seq&0xff;
    j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
    memcpy(ek0, j0, 16); aes_encrypt_block(&g->key, ek0);
    /* Verify tag first (over ciphertext) */
    u8 s[16]={0};
    _gcm_ghash(s, g->h, aad, aad_len);
    _gcm_ghash(s, g->h, pt, len);
    u8 lb[16]={0};
    u64 aad_bits=(u64)aad_len*8, ct_bits=(u64)len*8;
    lb[0]=(aad_bits>>56)&0xff; lb[1]=(aad_bits>>48)&0xff;
    lb[2]=(aad_bits>>40)&0xff; lb[3]=(aad_bits>>32)&0xff;
    lb[4]=(aad_bits>>24)&0xff; lb[5]=(aad_bits>>16)&0xff;
    lb[6]=(aad_bits>>8)&0xff;  lb[7]=aad_bits&0xff;
    lb[8]=(ct_bits>>56)&0xff;  lb[9]=(ct_bits>>48)&0xff;
    lb[10]=(ct_bits>>40)&0xff; lb[11]=(ct_bits>>32)&0xff;
    lb[12]=(ct_bits>>24)&0xff; lb[13]=(ct_bits>>16)&0xff;
    lb[14]=(ct_bits>>8)&0xff;  lb[15]=ct_bits&0xff;
    for(int i=0;i<16;i++) s[i]^=lb[i];
    _gcm_gf_mul(s, g->h);
    u8 expected[16];
    for(int i=0;i<16;i++) expected[i]=s[i]^ek0[i];
    int mismatch = 0;
    for(int i=0;i<16;i++) mismatch |= (expected[i]^tag[i]);
    if (mismatch) return -1;
    /* CTR decrypt */
    memcpy(ctr, j0, 16);
    for (u32 i=0; i<len; i++) {
        if (i%16==0) {
            for(int c=15;c>=12;c--) if(++ctr[c]) break;
            memcpy(block, ctr, 16);
            aes_encrypt_block(&g->key, block);
        }
        pt[i] ^= block[i%16];
    }
    g->seq++;
    return 0;
}

/* ── SHA-256 ─────────────────────────────────────────────────────────── */
static const u32 _K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

typedef struct { u32 h[8]; u8 buf[64]; u32 blen; u64 total; } Sha256;

static void sha256_init(Sha256 *s) {
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->blen=0; s->total=0;
}

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(a) (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define EP1(e) (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define SIG0(x)(ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define SIG1(x)(ROR32(x,17)^ROR32(x,19)^((x)>>10))

static void _sha256_block(Sha256 *s, const u8 *data) {
    u32 w[64], a,b,c,d,e,f,g,h,t1,t2;
    for(int i=0;i<16;i++) w[i]=((u32)data[i*4]<<24)|((u32)data[i*4+1]<<16)|((u32)data[i*4+2]<<8)|data[i*4+3];
    for(int i=16;i<64;i++) w[i]=SIG1(w[i-2])+w[i-7]+SIG0(w[i-15])+w[i-16];
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for(int i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+_K256[i]+w[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;
    s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}

static void sha256_update(Sha256 *s, const u8 *data, u32 len) {
    s->total += len;
    while (len) {
        u32 space = 64 - s->blen;
        u32 take = len < space ? len : space;
        memcpy(s->buf + s->blen, data, take);
        s->blen += take; data += take; len -= take;
        if (s->blen == 64) { _sha256_block(s, s->buf); s->blen=0; }
    }
}

static void sha256_final(Sha256 *s, u8 *digest) {
    u64 bits = s->total * 8;
    u8 pad = 0x80;
    sha256_update(s, &pad, 1);
    while (s->blen != 56) { u8 z=0; sha256_update(s,&z,1); }
    u8 lb[8];
    for(int i=7;i>=0;i--){lb[i]=bits&0xff;bits>>=8;}
    sha256_update(s, lb, 8);
    for(int i=0;i<8;i++){digest[i*4]=(s->h[i]>>24)&0xff;digest[i*4+1]=(s->h[i]>>16)&0xff;digest[i*4+2]=(s->h[i]>>8)&0xff;digest[i*4+3]=s->h[i]&0xff;}
}

static void sha256(const u8 *data, u32 len, u8 *digest) {
    Sha256 s; sha256_init(&s); sha256_update(&s,data,len); sha256_final(&s,digest);
}

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────── */
static void hmac_sha256(const u8 *key, u32 klen, const u8 *msg, u32 mlen, u8 *out) {
    u8 k[64]={0}, ipad[64], opad[64], tmp[32];
    if (klen > 64) { sha256(key, klen, k); klen=32; } else memcpy(k, key, klen);
    for(int i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}
    Sha256 s;
    sha256_init(&s); sha256_update(&s,ipad,64); sha256_update(&s,msg,mlen); sha256_final(&s,tmp);
    sha256_init(&s); sha256_update(&s,opad,64); sha256_update(&s,tmp,32);   sha256_final(&s,out);
}

/* ── TLS PRF (RFC 5246 §5) ───────────────────────────────────────────── */
static void _tls_p_hash(const u8 *secret, u32 slen,
                        const u8 *seed,   u32 seedlen,
                        u8 *out, u32 olen) {
    u8 A[32], Anext[32], buf[32+512];
    /* A(1) = HMAC(secret, seed) */
    hmac_sha256(secret, slen, seed, seedlen, A);
    u32 done = 0;
    while (done < olen) {
        /* HMAC(secret, A(i) || seed) */
        memcpy(buf, A, 32); memcpy(buf+32, seed, seedlen);
        u8 chunk[32]; hmac_sha256(secret, slen, buf, 32+seedlen, chunk);
        u32 take = olen-done < 32 ? olen-done : 32;
        memcpy(out+done, chunk, take);
        done += take;
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, slen, A, 32, Anext);
        memcpy(A, Anext, 32);
    }
}

static void tls_prf(const u8 *secret, u32 slen,
                    const char *label,
                    const u8 *seed, u32 seedlen,
                    u8 *out, u32 olen) {
    u32 llen = (u32)strlen(label);
    u8 lseed[256];
    memcpy(lseed, label, llen);
    memcpy(lseed+llen, seed, seedlen);
    _tls_p_hash(secret, slen, lseed, llen+seedlen, out, olen);
}

/* ── Simple PRNG (xorshift64 seeded from gettime_ms) ────────────────── */
static u64 _prng_state = 0;
static void prng_seed(void) {
    extern long long gettime_ms(void);
    _prng_state = (u64)gettime_ms() ^ 0xdeadbeefcafe1234ULL;
    if (!_prng_state) _prng_state = 1;
}
static void prng_bytes(u8 *buf, u32 len) {
    for (u32 i=0;i<len;i++) {
        _prng_state ^= _prng_state<<13;
        _prng_state ^= _prng_state>>7;
        _prng_state ^= _prng_state<<17;
        buf[i] = _prng_state & 0xff;
    }
}

/* ── TLS record layer ────────────────────────────────────────────────── */
#define TLS_CHANGE_CIPHER_SPEC 20
#define TLS_ALERT              21
#define TLS_HANDSHAKE          22
#define TLS_APPLICATION_DATA   23

#define TLS_CLIENT_HELLO       1
#define TLS_SERVER_HELLO       2
#define TLS_CERTIFICATE        11
#define TLS_SERVER_HELLO_DONE  14
#define TLS_CLIENT_KEY_EXCHANGE 16
#define TLS_FINISHED           20

/* Cipher suite: TLS_RSA_WITH_AES_128_GCM_SHA256 */
#define CS_AES128_GCM_SHA256   0x009C

#define TLS_BUF_SIZE  (16384 + 256)

typedef struct {
    int    fd;
    /* handshake transcript */
    u8     hs_hash_buf[65536];
    u32    hs_hash_len;
    /* handshake secrets */
    u8     pre_master[48];
    u8     master[48];
    u8     client_random[32];
    u8     server_random[32];
    /* key material */
    u8     client_write_key[16];
    u8     server_write_key[16];
    u8     client_write_iv[4];
    u8     server_write_iv[4];
    GcmCtx enc;   /* client→server */
    GcmCtx dec;   /* server→client */
    int    encrypted;  /* 0 until ChangeCipherSpec */
    /* recv buffer */
    u8     rbuf[TLS_BUF_SIZE];
    u32    rpos, rlen;
    /* application data buffer */
    u8     appbuf[TLS_BUF_SIZE];
    u32    applen;
} TlsCtx;

/* Low-level send/recv helpers */
static int _net_send(int fd, const u8 *buf, int len) {
    int sent=0;
    while(sent<len){
        int r=(int)send(fd, buf+sent, len-sent, 0);
        if(r<=0) return -1;
        sent+=r;
    }
    return sent;
}
static int _net_recv_exact(int fd, u8 *buf, int len) {
    int got=0;
    while(got<len){
        int r=(int)recv(fd, buf+got, len-got, 0);
        if(r<=0) return -1;
        got+=r;
    }
    return got;
}

/* Append to handshake transcript */
static void _hs_append(TlsCtx *t, const u8 *data, u32 len) {
    if (t->hs_hash_len + len > sizeof(t->hs_hash_buf)) return;
    memcpy(t->hs_hash_buf + t->hs_hash_len, data, len);
    t->hs_hash_len += len;
}

/* Send a TLS record (plaintext or encrypted) */
static int tls_send_record(TlsCtx *t, u8 type, const u8 *data, u32 len) {
    u8 hdr[5];
    hdr[0]=type; hdr[1]=3; hdr[2]=3;
    if (t->encrypted) {
        /* AAD = seq(8) || type(1) || version(2) || plaintext_len(2) */
        u8 aad[13];
        u64 seq = t->enc.seq;
        for(int i=7;i>=0;i--){aad[i]=seq&0xff;seq>>=8;}
        aad[8]=type; aad[9]=3; aad[10]=3;
        aad[11]=(len>>8)&0xff; aad[12]=len&0xff;
        /* explicit nonce = enc.seq (before increment) */
        u8 explicit_nonce[8];
        seq = t->enc.seq;
        for(int i=7;i>=0;i--){explicit_nonce[i]=seq&0xff;seq>>=8;}
        /* build: explicit_nonce(8) || ciphertext(len) || tag(16) */
        u32 fraglen = 8 + len + 16;
        hdr[3]=(fraglen>>8)&0xff; hdr[4]=fraglen&0xff;
        if (_net_send(t->fd, hdr, 5) < 0) return -1;
        if (_net_send(t->fd, explicit_nonce, 8) < 0) return -1;
        u8 *ct = (u8*)data; /* caller must provide writable buffer */
        u8 tag[16];
        gcm_seal(&t->enc, ct, len, aad, 13, tag);
        if (_net_send(t->fd, ct, len) < 0) return -1;
        if (_net_send(t->fd, tag, 16) < 0) return -1;
    } else {
        hdr[3]=(len>>8)&0xff; hdr[4]=len&0xff;
        if (_net_send(t->fd, hdr, 5) < 0) return -1;
        if (_net_send(t->fd, data, len) < 0) return -1;
    }
    return 0;
}

/* Receive one TLS record. Returns type, fills buf, sets *out_len. */
static int tls_recv_record(TlsCtx *t, u8 *buf, u32 *out_len) {
    u8 hdr[5];
    if (_net_recv_exact(t->fd, hdr, 5) < 0) return -1;
    u8 type = hdr[0];
    u32 len = ((u32)hdr[3]<<8)|hdr[4];
    if (len > TLS_BUF_SIZE) return -1;
    if (_net_recv_exact(t->fd, buf, len) < 0) return -1;
    if (t->encrypted) {
        /* buf = explicit_nonce(8) || ciphertext(len-24) || tag(16) */
        if (len < 24) return -1;
        u32 pt_len = len - 24;
        u8 *pt = buf + 8;
        u8 *tag = buf + 8 + pt_len;
        u8 aad[13];
        u64 seq = t->dec.seq;
        for(int i=7;i>=0;i--){aad[i]=seq&0xff;seq>>=8;}
        aad[8]=type; aad[9]=3; aad[10]=3;
        aad[11]=(pt_len>>8)&0xff; aad[12]=pt_len&0xff;
        /* set explicit nonce into dec IV */
        memcpy(t->dec.iv+4, buf, 8);
        if (gcm_open(&t->dec, pt, pt_len, aad, 13, tag) < 0) return -1;
        memmove(buf, pt, pt_len);
        *out_len = pt_len;
    } else {
        *out_len = len;
    }
    return (int)type;
}

/* ── RSA public key operations (minimal — verify only, PKCS#1 v1.5) ─── */
/*
 * Full RSA is ~400 lines. For browser porting the critical path is:
 * server cert is accepted (cert pinning / CA bundle comes later).
 * Here we accept the server's RSA public key and use it to encrypt
 * the 48-byte pre-master secret (RSA key exchange).
 *
 * We implement big-integer mod-exp using Montgomery multiplication,
 * limited to 2048-bit keys (256 bytes).
 */
#define RSA_MAX_BYTES 256

typedef struct {
    u8  n[RSA_MAX_BYTES]; u32 nlen;
    u8  e[4];             u32 elen;
} RsaPubKey;

/* Big-endian byte array modular exponentiation: out = base^exp mod n
 * Square-and-multiply, all arrays are nlen bytes. */
static void _bignum_modexp(const u8 *base, const u8 *exp, u32 elen,
                           const u8 *mod,  u32 nlen, u8 *out) {
    /* Work in nlen-byte little-endian arrays */
    u8 r[RSA_MAX_BYTES]={0}, b[RSA_MAX_BYTES], m[RSA_MAX_BYTES];
    /* Convert big-endian → little-endian */
    for(u32 i=0;i<nlen;i++) { b[i]=base[nlen-1-i]; m[i]=mod[nlen-1-i]; }
    r[0]=1; /* r = 1 */
    /* Simple O(nlen^2 * elen*8) — fine for 2048-bit at handshake time */
    for(u32 i=0;i<elen*8;i++) {
        u8 bit = (exp[elen-1-(i/8)] >> (i%8)) & 1;
        if (bit) {
            /* r = r * b mod m */
            u16 carry=0;
            u8 tmp[RSA_MAX_BYTES]={0};
            for(u32 j=0;j<nlen;j++) {
                for(u32 k=0;k+j<nlen;k++) tmp[j+k]+=r[j]*b[k];
            }
            /* reduce mod m — simple trial subtraction */
            /* (for small e like 65537 this is fast enough) */
            (void)carry;
            memcpy(r, tmp, nlen);
            /* mod reduction: subtract m while r >= m */
            while(1){
                int cmp=0;
                for(int j=nlen-1;j>=0;j--){if(r[j]>m[j]){cmp=1;break;}if(r[j]<m[j]){cmp=-1;break;}}
                if(cmp<0) break;
                u16 borrow=0;
                for(u32 j=0;j<nlen;j++){u16 d=(u16)r[j]-m[j]-borrow;r[j]=(u8)d;borrow=(d>>8)&1;}
            }
        }
        /* b = b * b mod m */
        u8 tmp2[RSA_MAX_BYTES]={0};
        for(u32 j=0;j<nlen;j++){
            for(u32 k=0;k+j<nlen;k++) tmp2[j+k]+=b[j]*b[k];
        }
        memcpy(b, tmp2, nlen);
        while(1){
            int cmp=0;
            for(int j=nlen-1;j>=0;j--){if(b[j]>m[j]){cmp=1;break;}if(b[j]<m[j]){cmp=-1;break;}}
            if(cmp<0) break;
            u16 borrow=0;
            for(u32 j=0;j<nlen;j++){u16 d=(u16)b[j]-m[j]-borrow;b[j]=(u8)d;borrow=(d>>8)&1;}
        }
    }
    /* Convert little-endian → big-endian */
    for(u32 i=0;i<nlen;i++) out[i]=r[nlen-1-i];
}

/* PKCS#1 v1.5 encrypt (for RSA key exchange):
 * EM = 0x00 0x02 <random non-zero padding> 0x00 <48-byte PMS>
 * Then EM^e mod n */
static int rsa_pkcs1_encrypt(const RsaPubKey *pub,
                             const u8 *pms, u32 pms_len,
                             u8 *out) {
    u32 k = pub->nlen;
    if (pms_len + 11 > k) return -1;
    u8 em[RSA_MAX_BYTES]={0};
    em[0]=0x00; em[1]=0x02;
    u32 ps_len = k - pms_len - 3;
    /* Fill PS with random non-zero bytes */
    for(u32 i=0;i<ps_len;i++){
        do { prng_bytes(&em[2+i],1); } while(em[2+i]==0);
    }
    em[2+ps_len]=0x00;
    memcpy(em+3+ps_len, pms, pms_len);
    _bignum_modexp(em, pub->e, pub->elen, pub->n, k, out);
    return (int)k;
}

/* ── ASN.1 / DER helpers for parsing server certificate ─────────────── */
static u32 _der_length(const u8 *p, u32 *consumed) {
    if (*p < 0x80) { *consumed=1; return *p; }
    u32 nb = *p & 0x7f; *consumed = 1+nb;
    u32 len=0;
    for(u32 i=0;i<nb;i++) len=(len<<8)|p[1+i];
    return len;
}

/* Extract RSA public key (n, e) from DER-encoded SubjectPublicKeyInfo */
static int parse_rsa_pubkey(const u8 *der, u32 derlen, RsaPubKey *pub) {
    /* Skip to BIT STRING containing the RSA key */
    const u8 *p = der;
    const u8 *end = der + derlen;
    /* Walk: SEQUENCE { SEQUENCE { OID, NULL }, BIT STRING { SEQUENCE { INT n, INT e } } } */
    if (p >= end || *p != 0x30) return -1; p++;
    u32 c; _der_length(p, &c); p+=c;
    /* skip AlgorithmIdentifier SEQUENCE */
    if (p >= end || *p != 0x30) return -1; p++;
    u32 alen = _der_length(p, &c); p+=c; p+=alen;
    /* BIT STRING */
    if (p >= end || *p != 0x03) return -1; p++;
    u32 bslen = _der_length(p, &c); p+=c;
    p++; /* skip unused-bits byte */
    /* SEQUENCE { INTEGER n, INTEGER e } */
    if (p >= end || *p != 0x30) return -1; p++;
    _der_length(p, &c); p+=c;
    /* INTEGER n */
    if (p >= end || *p != 0x02) return -1; p++;
    u32 nlen = _der_length(p, &c); p+=c;
    if (*p == 0x00) { p++; nlen--; } /* skip leading zero */
    if (nlen > RSA_MAX_BYTES) return -1;
    memcpy(pub->n, p, nlen); pub->nlen=nlen; p+=nlen;
    /* INTEGER e */
    if (p >= end || *p != 0x02) return -1; p++;
    u32 elen = _der_length(p, &c); p+=c;
    if (elen > 4) return -1;
    memset(pub->e, 0, 4);
    memcpy(pub->e+(4-elen), p, elen);
    pub->elen = elen;
    (void)bslen; (void)end;
    return 0;
}

/* ── TLS handshake ───────────────────────────────────────────────────── */
static int tls_do_handshake(TlsCtx *t, const char *hostname) {
    u8 buf[TLS_BUF_SIZE];
    u32 blen;

    /* ── ClientHello ── */
    prng_seed();
    prng_bytes(t->client_random, 32);
    u8 ch[128];
    u32 ci = 0;
    /* HandshakeType + length placeholder */
    ch[ci++]=TLS_CLIENT_HELLO;
    u32 len_off = ci; ch[ci++]=0; ch[ci++]=0; ch[ci++]=0; /* filled later */
    ch[ci++]=3; ch[ci++]=3; /* version TLS 1.2 */
    memcpy(ch+ci, t->client_random, 32); ci+=32;
    ch[ci++]=0; /* session ID length = 0 */
    ch[ci++]=0; ch[ci++]=2; /* cipher suites length = 2 */
    ch[ci++]=(CS_AES128_GCM_SHA256>>8)&0xff;
    ch[ci++]=CS_AES128_GCM_SHA256&0xff;
    ch[ci++]=1; ch[ci++]=0; /* compression: null */
    /* Extensions: SNI */
    u32 sni_host_len = (u32)strlen(hostname);
    u32 ext_off = ci;
    ch[ci++]=0; ch[ci++]=0; /* extensions total length placeholder */
    /* SNI extension */
    ch[ci++]=0; ch[ci++]=0;  /* type: server_name */
    u16 sni_ext_len = (u16)(5 + sni_host_len);
    ch[ci++]=(sni_ext_len>>8)&0xff; ch[ci++]=sni_ext_len&0xff;
    u16 sni_list_len = (u16)(3 + sni_host_len);
    ch[ci++]=(sni_list_len>>8)&0xff; ch[ci++]=sni_list_len&0xff;
    ch[ci++]=0; /* name type: host_name */
    ch[ci++]=(sni_host_len>>8)&0xff; ch[ci++]=sni_host_len&0xff;
    memcpy(ch+ci, hostname, sni_host_len); ci+=sni_host_len;
    /* Fill extensions length */
    u16 ext_total = (u16)(ci - ext_off - 2);
    ch[ext_off]=(ext_total>>8)&0xff; ch[ext_off+1]=ext_total&0xff;
    /* Fill handshake length */
    u32 hs_len = ci - len_off - 3;
    ch[len_off]=(hs_len>>16)&0xff; ch[len_off+1]=(hs_len>>8)&0xff; ch[len_off+2]=hs_len&0xff;

    _hs_append(t, ch, ci);
    if (tls_send_record(t, TLS_HANDSHAKE, ch, ci) < 0) return -1;

    /* ── ServerHello + Certificate + ServerHelloDone ── */
    RsaPubKey server_key;
    int got_hello=0, got_cert=0, got_done=0;
    while (!got_done) {
        int type = tls_recv_record(t, buf, &blen);
        if (type != TLS_HANDSHAKE) return -1;
        _hs_append(t, buf, blen);
        u32 pos = 0;
        while (pos < blen) {
            u8 hs_type = buf[pos++];
            u32 hs_len2 = ((u32)buf[pos]<<16)|((u32)buf[pos+1]<<8)|buf[pos+2]; pos+=3;
            const u8 *hs_body = buf+pos;
            if (hs_type == TLS_SERVER_HELLO) {
                memcpy(t->server_random, hs_body+2, 32);
                got_hello=1;
            } else if (hs_type == TLS_CERTIFICATE) {
                /* Parse first cert from Certificate list */
                u32 clist_len = ((u32)hs_body[0]<<16)|((u32)hs_body[1]<<8)|hs_body[2];
                u32 cert_len  = ((u32)hs_body[3]<<16)|((u32)hs_body[4]<<8)|hs_body[5];
                const u8 *cert_der = hs_body + 6;
                /* Walk DER to find SubjectPublicKeyInfo */
                /* Outer SEQUENCE */
                const u8 *p = cert_der;
                if (*p==0x30){p++;u32 c2;u32 l=_der_length(p,&c2);p+=c2;
                /* tbsCertificate SEQUENCE */
                if(*p==0x30){p++;l=_der_length(p,&c2);const u8*tbs=p+c2;
                /* skip version, serialNumber, signature, issuer, validity, subject */
                /* find subjectPublicKeyInfo by scanning for RSA OID */
                const u8 rsa_oid[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
                const u8 *spki=tbs;
                for(u32 i=0;i+sizeof(rsa_oid)<=l;i++){
                    if(memcmp(tbs+i,rsa_oid,sizeof(rsa_oid))==0){
                        /* back up to the SEQUENCE containing this OID */
                        spki=tbs+i-4;
                        parse_rsa_pubkey(spki, (u32)(cert_der+cert_len-spki), &server_key);
                        got_cert=1; break;
                    }
                }
                (void)l;}}
                (void)clist_len;
            } else if (hs_type == TLS_SERVER_HELLO_DONE) {
                got_done=1;
            }
            pos += hs_len2;
        }
    }
    if (!got_hello || !got_cert) return -1;

    /* ── ClientKeyExchange — RSA encrypted PMS ── */
    prng_bytes(t->pre_master, 48);
    t->pre_master[0]=3; t->pre_master[1]=3; /* TLS 1.2 version */
    u8 enc_pms[RSA_MAX_BYTES];
    int epms_len = rsa_pkcs1_encrypt(&server_key, t->pre_master, 48, enc_pms);
    if (epms_len < 0) return -1;
    u8 cke[RSA_MAX_BYTES+10];
    u32 cki=0;
    cke[cki++]=TLS_CLIENT_KEY_EXCHANGE;
    cke[cki++]=0; cke[cki++]=0; cke[cki++]=(u8)(epms_len+2);
    cke[cki++]=(epms_len>>8)&0xff; cke[cki++]=epms_len&0xff;
    memcpy(cke+cki, enc_pms, epms_len); cki+=epms_len;
    _hs_append(t, cke, cki);
    if (tls_send_record(t, TLS_HANDSHAKE, cke, cki) < 0) return -1;

    /* ── Key derivation ── */
    u8 seed[64];
    memcpy(seed, t->client_random, 32);
    memcpy(seed+32, t->server_random, 32);
    tls_prf(t->pre_master, 48, "master secret", seed, 64, t->master, 48);

    /* key_block = PRF(master, "key expansion", server_random || client_random) */
    u8 kseed[64];
    memcpy(kseed, t->server_random, 32);
    memcpy(kseed+32, t->client_random, 32);
    u8 key_block[128];
    tls_prf(t->master, 48, "key expansion", kseed, 64, key_block, sizeof(key_block));
    /* AES-128-GCM: no MAC keys, 16-byte enc keys, 4-byte IVs */
    memcpy(t->client_write_key, key_block+0,  16);
    memcpy(t->server_write_key, key_block+16, 16);
    memcpy(t->client_write_iv,  key_block+32, 4);
    memcpy(t->server_write_iv,  key_block+36, 4);
    gcm_init(&t->enc, t->client_write_key, t->client_write_iv);
    gcm_init(&t->dec, t->server_write_key, t->server_write_iv);

    /* ── ChangeCipherSpec + Finished ── */
    u8 ccs = 1;
    if (tls_send_record(t, TLS_CHANGE_CIPHER_SPEC, &ccs, 1) < 0) return -1;
    t->encrypted = 1;

    /* Finished = PRF(master, "client finished", SHA256(all_handshake_messages))[0..11] */
    u8 hs_hash[32]; sha256(t->hs_hash_buf, t->hs_hash_len, hs_hash);
    u8 verify[12];
    tls_prf(t->master, 48, "client finished", hs_hash, 32, verify, 12);
    u8 fin[16];
    fin[0]=TLS_FINISHED; fin[1]=0; fin[2]=0; fin[3]=12;
    memcpy(fin+4, verify, 12);
    _hs_append(t, fin, 16);
    if (tls_send_record(t, TLS_HANDSHAKE, fin, 16) < 0) return -1;

    /* ── Wait for server ChangeCipherSpec + Finished ── */
    int type = tls_recv_record(t, buf, &blen);
    if (type != TLS_CHANGE_CIPHER_SPEC) return -1;
    type = tls_recv_record(t, buf, &blen);
    if (type != TLS_HANDSHAKE) return -1;
    /* Verify server Finished */
    sha256(t->hs_hash_buf, t->hs_hash_len - 16, hs_hash);
    u8 s_verify[12];
    tls_prf(t->master, 48, "server finished", hs_hash, 32, s_verify, 12);
    if (blen < 16 || memcmp(buf+4, s_verify, 12) != 0) return -1;

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * tls_connect — TCP connect + TLS handshake.
 * ip_str: dotted-decimal IPv4 string.
 * Returns socket fd on success, -1 on failure.
 */
static int tls_connect(TlsCtx *t, const char *ip_str, int port, const char *hostname) {
    memset(t, 0, sizeof(*t));

    /* Parse IP */
    /* Manual dotted-decimal parse (no sscanf) */
    u32 ip=0; int octs=0;
    const char *pp=ip_str;
    while(octs<4){
        u32 v=0;
        while(*pp>='0'&&*pp<='9'){v=v*10+(*pp-'0');pp++;}
        if(v>255) return -1;
        ip=(ip<<8)|v; octs++;
        if(octs<4){if(*pp!='.') return -1; pp++;}
    }

    /* struct sockaddr_in layout assumed by Systrix kernel */
    struct { u16 family; u16 port; u32 addr; u8 pad[8]; } sa;
    sa.family = 2; /* AF_INET */
    sa.port   = (u16)(((port>>8)&0xff)|((port&0xff)<<8)); /* htons */
    sa.addr   = ip;
    memset(sa.pad, 0, 8);

    t->fd = socket(2, 1, 0); /* AF_INET, SOCK_STREAM */
    if (t->fd < 0) return -1;
    if (connect(t->fd, &sa, sizeof(sa)) < 0) return -1;
    if (tls_do_handshake(t, hostname) < 0) return -1;
    return t->fd;
}

/*
 * tls_write — send application data over TLS.
 */
static int tls_write(TlsCtx *t, const u8 *data, u32 len) {
    /* Copy to writable buffer for in-place GCM */
    u8 tmp[TLS_BUF_SIZE];
    u32 done = 0;
    while (done < len) {
        u32 chunk = len - done;
        if (chunk > 16000) chunk = 16000;
        memcpy(tmp, data+done, chunk);
        if (tls_send_record(t, TLS_APPLICATION_DATA, tmp, chunk) < 0) return -1;
        done += chunk;
    }
    return (int)done;
}

/*
 * tls_read — receive application data. Returns bytes read, 0 on close, -1 on error.
 */
static int tls_read(TlsCtx *t, u8 *out, u32 maxlen) {
    if (t->applen > 0) {
        u32 take = t->applen < maxlen ? t->applen : maxlen;
        memcpy(out, t->appbuf, take);
        memmove(t->appbuf, t->appbuf+take, t->applen-take);
        t->applen -= take;
        return (int)take;
    }
    u8 buf[TLS_BUF_SIZE];
    u32 blen;
    int type = tls_recv_record(t, buf, &blen);
    if (type == TLS_APPLICATION_DATA) {
        u32 take = blen < maxlen ? blen : maxlen;
        memcpy(out, buf, take);
        if (blen > take) {
            memcpy(t->appbuf, buf+take, blen-take);
            t->applen = blen-take;
        }
        return (int)take;
    }
    if (type == TLS_ALERT) return 0; /* close_notify */
    return -1;
}

/*
 * tls_close — send close_notify alert and close socket.
 */
static void tls_close(TlsCtx *t) {
    u8 alert[2] = {1, 0}; /* warning, close_notify */
    tls_send_record(t, TLS_ALERT, alert, 2);
    /* No close syscall in Systrix yet — socket will timeout */
    t->encrypted = 0;
}

#endif /* SYSTRIX_TLS_H */
