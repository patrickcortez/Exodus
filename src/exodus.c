#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * Compile Command:
 * gcc -Wall -Wextra -O2 exodus.c cortez-mesh.o ctz-json.a ctz-set.a cortez_ipc.o -o exodus -pthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>

#include "cortez-mesh.h"
#include "exodus-common.h"
#include "ctz-json.h"
#include "cortez_ipc.h"
#include "ctz-set.h"
#include <libgen.h>


#include <ftw.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <stddef.h> 

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <dirent.h>


#define PID_FILE "/tmp/exodus.pid"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

//Foward Declarations
static int read_string_from_file(const char* fpath, char* buf, size_t buf_size);
static void get_trunk_head_file(const char* node_path, char* path_buf, size_t buf_size);
static void get_subsection_head_file(const char* node_path, const char* subsection_name, char* path_buf, size_t buf_size);
static int get_commit_hash_for_subsection(const char* node_path, const char* subsection_name, char* hash_out, size_t hash_size);

// ============================================================================
// BEGIN: EMBEDDED TINY-SHA256 (Public Domain)
// Source: https://github.com/amosnier/sha-2
// ============================================================================
#define SHA256_BLOCK_SIZE 32
typedef struct {
	uint8_t  data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} SHA256_CTX;

#define DBL_INT_ADD(a,b,c) \
	if (a > 0xffffffff - (c)) ++(b); \
	a += c;
#define ROTLEFT(a,b)  (((a) << (b)) | ((a) >> (32-(b))))
#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static const uint32_t k[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
	uint32_t a,b,c,d,e,f,g,h,i,j,t1,t2,m[64];
	for (i=0,j=0; i < 16; ++i, j += 4)
		m[i] = (data[j] << 24) | (data[j+1] << 16) | (data[j+2] << 8) | (data[j+3]);
	for ( ; i < 64; ++i)
		m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
	for (i = 0; i < 64; ++i) {
		t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
		t2 = EP0(a) + MAJ(a,b,c);
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
	ctx->datalen = 0;
	ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
	uint32_t i;
	for (i=0; i < len; ++i) {
		ctx->data[ctx->datalen] = data[i];
		ctx->datalen++;
		if (ctx->datalen == 64) {
			sha256_transform(ctx,ctx->data);
			DBL_INT_ADD(ctx->bitlen,ctx->bitlen,512);
			ctx->datalen = 0;
		}
	}
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
	uint32_t i;
	i = ctx->datalen;
	if (ctx->datalen < 56) {
		ctx->data[i++] = 0x80;
		while (i < 56) ctx->data[i++] = 0x00;
	}
	else {
		ctx->data[i++] = 0x80;
		while (i < 64) ctx->data[i++] = 0x00;
		sha256_transform(ctx,ctx->data);
		memset(ctx->data,0,56);
	}
	DBL_INT_ADD(ctx->bitlen,ctx->bitlen,ctx->datalen * 8);
	ctx->data[63] = ctx->bitlen;
	ctx->data[62] = ctx->bitlen >> 8;
	ctx->data[61] = ctx->bitlen >> 16;
	ctx->data[60] = ctx->bitlen >> 24;
	ctx->data[59] = ctx->bitlen >> 32;
	ctx->data[58] = ctx->bitlen >> 40;
	ctx->data[57] = ctx->bitlen >> 48;
	ctx->data[56] = ctx->bitlen >> 56;
	sha256_transform(ctx,ctx->data);
	for (i=0; i < 4; ++i) {
		hash[i]    = (ctx->state[0] >> (24-i*8)) & 0xff;
		hash[i+4]  = (ctx->state[1] >> (24-i*8)) & 0xff;
		hash[i+8]  = (ctx->state[2] >> (24-i*8)) & 0xff;
		hash[i+12] = (ctx->state[3] >> (24-i*8)) & 0xff;
		hash[i+16] = (ctx->state[4] >> (24-i*8)) & 0xff;
		hash[i+20] = (ctx->state[5] >> (24-i*8)) & 0xff;
		hash[i+24] = (ctx->state[6] >> (24-i*8)) & 0xff;
		hash[i+28] = (ctx->state[7] >> (24-i*8)) & 0xff;
	}
}
// ============================================================================
// END: EMBEDDED TINY-SHA256
// ============================================================================

// ============================================================================
// BEGIN: EMBEDDED TINY-AES (Public Domain)
// Source: https://github.com/kokke/tiny-AES-c
// Mode: 256-bit CBC
// ============================================================================

#define AES_KEYLEN 256
#define AES_KEYEXPSIZE (240) // (AES_KEYLEN/8 * (14 + 1))
#define AES_BLOCKLEN 16

struct AES_ctx {
  uint8_t RoundKey[AES_KEYEXPSIZE];
  uint8_t Iv[AES_BLOCKLEN];
};

static const uint8_t sbox[256] = {
  //0     1    2      3     4    5     6     7      8    9     A     B     C     D     E     F
  0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
  0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
  0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
  0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
  0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
  0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
  0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
  0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
  0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
  0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
  0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
  0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
  0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
  0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
  0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
  0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16 };

static const uint8_t rsbox[256] = {
  0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
  0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
  0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
  0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
  0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
  0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
  0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
  0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
  0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
  0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
  0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
  0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
  0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
  0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
  0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
  0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d };

static const uint8_t Rcon[255] = {
  0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d, 0x9a,
  0x2f, 0x5e, 0xbc, 0x63, 0xc6, 0x97, 0x35, 0x6a, 0xd4, 0xb3, 0x7d, 0xfa, 0xef, 0xc5, 0x91, 0x39,
  0x72, 0xe4, 0xd3, 0xbd, 0x61, 0xc2, 0x9f, 0x25, 0x4a, 0x94, 0x33, 0x66, 0xcc, 0x83, 0x1d, 0x3a,
  0x74, 0xe8, 0xcb, 0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8,
  0xab, 0x4d, 0x9a, 0x2f, 0x5e, 0xbc, 0x63, 0xc6, 0x97, 0x35, 0x6a, 0xd4, 0xb3, 0x7d, 0xfa, 0xef,
  0xc5, 0x91, 0x39, 0x72, 0xe4, 0xd3, 0xbd, 0x61, 0xc2, 0x9f, 0x25, 0x4a, 0x94, 0x33, 0x66, 0xcc,
  0x83, 0x1d, 0x3a, 0x74, 0xe8, 0xcb, 0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b,
  0x36, 0x6c, 0xd8, 0xab, 0x4d, 0x9a, 0x2f, 0x5e, 0xbc, 0x63, 0xc6, 0x97, 0x35, 0x6a, 0xd4, 0xb3,
  0x7d, 0xfa, 0xef, 0xc5, 0x91, 0x39, 0x72, 0xe4, 0xd3, 0xbd, 0x61, 0xc2, 0x9f, 0x25, 0x4a, 0x94,
  0x33, 0x66, 0xcc, 0x83, 0x1d, 0x3a, 0x74, 0xe8, 0xcb, 0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20,
  0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d, 0x9a, 0x2f, 0x5e, 0xbc, 0x63, 0xc6, 0x97, 0x35,
  0x6a, 0xd4, 0xb3, 0x7d, 0xfa, 0xef, 0xc5, 0x91, 0x39, 0x72, 0xe4, 0xd3, 0xbd, 0x61, 0xc2, 0x9f,
  0x25, 0x4a, 0x94, 0x33, 0x66, 0xcc, 0x83, 0x1d, 0x3a, 0x74, 0xe8, 0xcb, 0x8d, 0x01, 0x02, 0x04,
  0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d, 0x9a, 0x2f, 0x5e, 0xbc, 0x63,
  0xc6, 0x97, 0x35, 0x6a, 0xd4, 0xb3, 0x7d, 0xfa, 0xef, 0xc5, 0x91, 0x39, 0x72, 0xe4, 0xd3, 0xbd,
  0x61, 0xc2, 0x9f, 0x25, 0x4a, 0x94, 0x33, 0x66, 0xcc, 0x83, 0x1d, 0x3a, 0x74, 0xe8, 0xcb };

#define getSBoxValue(num) (sbox[(num)])
#define getSBoxInvert(num) (rsbox[(num)])

static void KeyExpansion(uint8_t* RoundKey, const uint8_t* Key) {
  unsigned i, j, k;
  uint8_t tempa[4];
  
  for (i = 0; i < 8; ++i) {
    RoundKey[(i * 4) + 0] = Key[(i * 4) + 0];
    RoundKey[(i * 4) + 1] = Key[(i * 4) + 1];
    RoundKey[(i * 4) + 2] = Key[(i * 4) + 2];
    RoundKey[(i * 4) + 3] = Key[(i * 4) + 3];
  }

  for (i = 8; i < 60; ++i) {
    {
      k = (i - 1) * 4;
      tempa[0]=RoundKey[k + 0];
      tempa[1]=RoundKey[k + 1];
      tempa[2]=RoundKey[k + 2];
      tempa[3]=RoundKey[k + 3];
    }

    if (i % 8 == 0) {   
      {
        const uint8_t u8tmp = tempa[0];
        tempa[0] = tempa[1];
        tempa[1] = tempa[2];
        tempa[2] = tempa[3];
        tempa[3] = u8tmp;
      }
      {
        tempa[0] = getSBoxValue(tempa[0]);
        tempa[1] = getSBoxValue(tempa[1]);
        tempa[2] = getSBoxValue(tempa[2]);
        tempa[3] = getSBoxValue(tempa[3]);
      }
      tempa[0] = tempa[0] ^ Rcon[i/8];
    }
    
    if (i % 8 == 4) {
      {
        tempa[0] = getSBoxValue(tempa[0]);
        tempa[1] = getSBoxValue(tempa[1]);
        tempa[2] = getSBoxValue(tempa[2]);
        tempa[3] = getSBoxValue(tempa[3]);
      }
    }
    
    j = i * 4; k=(i - 8) * 4;
    RoundKey[j + 0] = RoundKey[k + 0] ^ tempa[0];
    RoundKey[j + 1] = RoundKey[k + 1] ^ tempa[1];
    RoundKey[j + 2] = RoundKey[k + 2] ^ tempa[2];
    RoundKey[j + 3] = RoundKey[k + 3] ^ tempa[3];
  }
}

static void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv) {
  KeyExpansion(ctx->RoundKey, key);
  memcpy(ctx->Iv, iv, AES_BLOCKLEN);
}

// --- FIX: Removed incorrect '(*state)' wrapper ---
static void AddRoundKey(uint8_t round, uint8_t (*state)[4], const uint8_t* RoundKey) {
  uint8_t i,j;
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      state[i][j] ^= RoundKey[(round * 16) + (i * 4) + j];
    }
  }
}

static void SubBytes(uint8_t (*state)[4]) {
  uint8_t i, j;
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      state[j][i] = getSBoxValue(state[j][i]);
    }
  }
}

static void ShiftRows(uint8_t (*state)[4]) {
  uint8_t temp;
  temp         = state[0][1];
  state[0][1] = state[1][1];
  state[1][1] = state[2][1];
  state[2][1] = state[3][1];
  state[3][1] = temp;
  temp         = state[0][2];
  state[0][2] = state[2][2];
  state[2][2] = temp;
  temp         = state[1][2];
  state[1][2] = state[3][2];
  state[3][2] = temp;
  temp         = state[0][3];
  state[0][3] = state[3][3];
  state[3][3] = state[2][3];
  state[2][3] = state[1][3];
  state[1][3] = temp;
}

static uint8_t xtime(uint8_t x) {
  return ((x<<1) ^ (((x>>7) & 1) * 0x1b));
}


static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    uint8_t hi_bit_set;
    int i;
    for (i = 0; i < 8; ++i) {
        if ((b & 1) == 1) {
            p ^= a;
        }
        hi_bit_set = (a & 0x80);
        a <<= 1;
        if (hi_bit_set) {
            a ^= 0x1b;
        }
        b >>= 1;
    }
    return p;
}

// --- FIX: Removed incorrect '(*state)' wrapper ---
static void MixColumns(uint8_t (*state)[4]) {
  uint8_t i;
  uint8_t Tmp, Tm, t;
  for (i = 0; i < 4; ++i) {  
    t   = state[i][0];
    Tmp = state[i][0] ^ state[i][1] ^ state[i][2] ^ state[i][3] ;
    Tm  = state[i][0] ^ state[i][1] ; Tm = xtime(Tm);  state[i][0] ^= Tm ^ Tmp ;
    Tm  = state[i][1] ^ state[i][2] ; Tm = xtime(Tm);  state[i][1] ^= Tm ^ Tmp ;
    Tm  = state[i][2] ^ state[i][3] ; Tm = xtime(Tm);  state[i][2] ^= Tm ^ Tmp ;
    Tm  = state[i][3] ^ t ; Tm = xtime(Tm);  state[i][3] ^= Tm ^ Tmp ;
  }
}

static void InvMixColumns(uint8_t (*state)[4]) {
  int i;
  uint8_t a, b, c, d;
  for (i = 0; i < 4; ++i) { 
    a = state[i][0];
    b = state[i][1];
    c = state[i][2];
    d = state[i][3];

    state[i][0] = gmul(a, 0x0e) ^ gmul(b, 0x0b) ^ gmul(c, 0x0d) ^ gmul(d, 0x09);
    state[i][1] = gmul(a, 0x09) ^ gmul(b, 0x0e) ^ gmul(c, 0x0b) ^ gmul(d, 0x0d);
    state[i][2] = gmul(a, 0x0d) ^ gmul(b, 0x09) ^ gmul(c, 0x0e) ^ gmul(d, 0x0b);
    state[i][3] = gmul(a, 0x0b) ^ gmul(b, 0x0d) ^ gmul(c, 0x09) ^ gmul(d, 0x0e);
  }
}

static void InvSubBytes(uint8_t (*state)[4]) {
  uint8_t i, j;
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      state[j][i] = getSBoxInvert(state[j][i]);
    }
  }
}

static void write_to_handle(cortez_write_handle_t* h, const void* data, size_t size) {
    size_t part1_size;
    char* part1 = cortez_write_handle_get_part1(h, &part1_size);
    
    if (size <= part1_size) {
        memcpy(part1, data, size);
    } else {
        size_t part2_size;
        char* part2 = cortez_write_handle_get_part2(h, &part2_size);
        memcpy(part1, data, part1_size);
        memcpy(part2, (const char*)data + part1_size, size - part1_size);
    }
}

static void InvShiftRows(uint8_t (*state)[4]) {
  uint8_t temp;
  temp = state[3][1];
  state[3][1] = state[2][1];
  state[2][1] = state[1][1];
  state[1][1] = state[0][1];
  state[0][1] = temp;
  temp = state[0][2];
  state[0][2] = state[2][2];
  state[2][2] = temp;
  temp = state[1][2];
  state[1][2] = state[3][2];
  state[3][2] = temp;
  temp = state[0][3];
  state[0][3] = state[1][3];
  state[1][3] = state[2][3];
  state[2][3] = state[3][3];
  state[3][3] = temp;
}

static void Cipher(uint8_t (*state)[4], const uint8_t* RoundKey) {
  uint8_t round = 0;
  AddRoundKey(0, state, RoundKey); 
  for (round = 1; round < 14; ++round) {
    SubBytes(state);
    ShiftRows(state);
    MixColumns(state);
    AddRoundKey(round, state, RoundKey);
  }
  SubBytes(state);
  ShiftRows(state);
  AddRoundKey(14, state, RoundKey);
}

static void InvCipher(uint8_t (*state)[4], const uint8_t* RoundKey) {
  uint8_t round = 0;
  AddRoundKey(14, state, RoundKey);
  for (round = 13; round > 0; --round) {
    InvShiftRows(state);
    InvSubBytes(state);
    AddRoundKey(round, state, RoundKey);
    InvMixColumns(state);
  }
  InvShiftRows(state);
  InvSubBytes(state);
  AddRoundKey(0, state, RoundKey);
}

static void XorWithIv(uint8_t* buf, const uint8_t* Iv) {
  uint8_t i;
  for (i = 0; i < AES_BLOCKLEN; ++i) {
    buf[i] ^= Iv[i];
  }
}

static void AES_CBC_encrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, uint32_t length) {
  uintptr_t i;
  uint8_t* Iv = ctx->Iv;
  for (i = 0; i < length; i += AES_BLOCKLEN) {
    XorWithIv(buf + i, Iv);
    Cipher((uint8_t (*)[4])(buf + i), ctx->RoundKey);
    Iv = buf + i;
  }
  memcpy(ctx->Iv, Iv, AES_BLOCKLEN);
}

static void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, uint32_t length) {
  uintptr_t i;
  uint8_t storeNextIv[AES_BLOCKLEN];
  for (i = 0; i < length; i += AES_BLOCKLEN) {
    memcpy(storeNextIv, buf + i, AES_BLOCKLEN);
    InvCipher((uint8_t (*)[4])(buf + i), ctx->RoundKey);
    XorWithIv(buf + i, ctx->Iv);
    memcpy(ctx->Iv, storeNextIv, AES_BLOCKLEN);
  }
}
// ============================================================================
// END: EMBEDDED TINY-AES
// ============================================================================



#define ENODE_MAGIC "ENODEv2" 

typedef struct __attribute__((packed)) {
    char magic[8];
    char node_name[MAX_NODE_NAME_LEN];
    uint8_t iv[AES_BLOCKLEN];
    char author[MAX_ATTR_LEN];
    char desc[MAX_ATTR_LEN];
    char tag[MAX_ATTR_LEN];
    char current_version[MAX_NODE_NAME_LEN];
} EnodeHeader;


typedef struct __attribute__((packed)) {
    char relative_path[PATH_MAX];
    uint64_t data_size; 
    mode_t mode;        
    char link_target[PATH_MAX]; 
} EnodeFileHeader;

// Special marker for the end of the archive
static const uint64_t ENODE_EOF_MARKER = 0xDEADBEEFCAFED00D;

static int get_home_from_uid(uid_t uid, char* home_buf, size_t home_size) {
    FILE* f = fopen("/etc/passwd", "r");
    if (!f) {
        perror("[exodus] Error: Could not open /etc/passwd");
        return -1;
    }
    char line[1024];
    char* saveptr;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char* line_copy = strdup(line);
        if (line_copy == NULL) continue;

        strtok_r(line_copy, ":", &saveptr); // name
        strtok_r(NULL, ":", &saveptr);      // pass
        char* uid_str = strtok_r(NULL, ":", &saveptr);

        if (uid_str && atoi(uid_str) == (int)uid) {
            strtok_r(NULL, ":", &saveptr); // gid
            strtok_r(NULL, ":", &saveptr); // gecos
            char* home_dir = strtok_r(NULL, ":", &saveptr);
            if (home_dir) {
                strncpy(home_buf, home_dir, home_size - 1);
                home_buf[home_size - 1] = '\0';
                found = 1;
            }
            free(line_copy);
            break;
        }
        free(line_copy);
    }
    fclose(f);
    return found ? 0 : -1;
}

// Gets the owner UID of the node path itself
static int get_user_uid_from_path(const char* path, uid_t* user_id) {
    struct stat st;
    if (stat(path, &st) != 0) {
        perror("[exodus] stat failed on node path");
        return -1;
    }
    *user_id = st.st_uid;
    return 0;
}

        typedef enum {
            NET_STATE_NONE,         
            NET_STATE_CREATED,
            NET_STATE_MODIFIED,
            NET_STATE_DELETED,
            NET_STATE_TEMP_DELETED ,
            NET_STATE_MOVED
        } FileNetState;

        typedef struct FileStatusNode {
            char path[PATH_MAX];
            FileNetState state;
            int modify_count;
            struct FileStatusNode* next;
            char from_path[PATH_MAX];
        } FileStatusNode;

        FileStatusNode* find_or_create_status(FileStatusNode** head, const char* path) {
            FileStatusNode* current = *head;
            while (current) {
                if (strcmp(current->path, path) == 0) {
                    return current;
                }
                current = current->next;
            }
            
            // Not found, create new one
            FileStatusNode* new_node = (FileStatusNode*)calloc(1, sizeof(FileStatusNode));
            if (!new_node) {
                fprintf(stderr, "Out of memory processing status.\n");
                return NULL; 
            }
            strncpy(new_node->path, path, PATH_MAX - 1);
            new_node->state = NET_STATE_NONE; // Represents a committed file by default
            new_node->next = *head;
            *head = new_node;
            return new_node;
        }

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const unsigned char* data, size_t input_length, size_t* output_length) {
    *output_length = 4 * ((input_length + 2) / 3);
    char* encoded_data = malloc(*output_length + 1);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = b64_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 0 * 6) & 0x3F];
    }

    // Add padding
    int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[*output_length - 1 - i] = '=';

    encoded_data[*output_length] = '\0';
    return encoded_data;
}

// --- NEW: Helper to read an entire file into a buffer ---
static unsigned char* read_file_for_sync(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buffer = malloc(*size);
    if (buffer) {
        if (fread(buffer, 1, *size, f) != *size) {
            free(buffer);
            buffer = NULL;
        }
    }
    fclose(f);
    return buffer;
}

int get_executable_dir(char* buffer, size_t size) {
    char path_buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);
    if (len == -1) {
        perror("readlink failed");
        return -1;
    }
    path_buf[len] = '\0';
    char *last_slash = strrchr(path_buf, '/');
    if (last_slash == NULL) {
        fprintf(stderr, "Could not find slash in executable path\n");
        return -1;
    }
    *last_slash = '\0';
    strncpy(buffer, path_buf, size - 1);
    buffer[size - 1] = '\0';
    return 0;
}

static int read_string_from_file(const char* fpath, char* buf, size_t buf_size) {
    FILE* f = fopen(fpath, "r");
    if (!f) return -1;
    if (fgets(buf, buf_size, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

static void get_trunk_head_file(const char* node_path, char* path_buf, size_t buf_size) {
    snprintf(path_buf, buf_size, "%s/.log/TRUNK_HEAD", node_path);
}

static void get_subsection_head_file(const char* node_path, const char* subsection_name, char* path_buf, size_t buf_size) {
    snprintf(path_buf, buf_size, "%s/.log/subsections/%s.subsec", node_path, subsection_name);
}

static int get_commit_hash_for_subsection(const char* node_path, const char* subsection_name, char* hash_out, size_t hash_size) {
    char head_file_path[PATH_MAX];
    
    if (strcmp(subsection_name, "master") == 0) {
        get_trunk_head_file(node_path, head_file_path, sizeof(head_file_path));
    } else {
        get_subsection_head_file(node_path, subsection_name, head_file_path, sizeof(head_file_path));
    }

    if (read_string_from_file(head_file_path, hash_out, hash_size) != 0) {
        hash_out[0] = '\0'; // Ensure it's empty on failure (e.g., unborn branch)
        return -1;
    }
    return 0;
}

void start_daemons() {
    FILE* f = fopen(PID_FILE, "r");
    if (f) {
        fclose(f);
        fprintf(stderr, "PID file %s already exists. Are daemons running? Use 'stop' first.\n", PID_FILE);
        return;
    }

    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Could not determine executable directory. Aborting.\n");
        return;
    }

    char cloud_daemon_path[PATH_MAX];
    char query_daemon_path[PATH_MAX];
    snprintf(cloud_daemon_path, sizeof(cloud_daemon_path), "%s/cloud_daemon", exe_dir);
    snprintf(query_daemon_path, sizeof(query_daemon_path), "%s/query_daemon", exe_dir);
    printf("Daemon path prefix: %s\n", exe_dir);

    pid_t pids[2];
    pids[0] = fork();
    if (pids[0] == 0) { 
        execl(cloud_daemon_path, "cloud_daemon", (char*)NULL);
        perror("execl cloud_daemon failed");
        exit(1);
    } else if (pids[0] < 0) {
        perror("fork for cloud_daemon failed");
        return;
    }
    
    printf("Waiting for cloud daemon to initialize...\n");
    sleep(3);

    pids[1] = fork();
    if (pids[1] == 0) { 
        execl(query_daemon_path, "query_daemon", (char*)NULL);
        perror("execl query_daemon failed");
        exit(1);
    } else if (pids[1] < 0) {
        perror("fork for query_daemon failed");
        kill(pids[0], SIGTERM); // Clean up the first one
        return;
    }

    f = fopen(PID_FILE, "w");
    if (!f) {
        perror("Could not create PID file");
        kill(pids[0], SIGTERM);
        kill(pids[1], SIGTERM);
        return;
    }
    fprintf(f, "%d\n%d\n", pids[0], pids[1]);
    fclose(f);
    printf("Daemons started with PIDs: %d (cloud), %d (query)\n", pids[0], pids[1]);
}

int update_node_current_version(const char* node_name, const char* version_tag) {
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Error: Could not determine executable directory to find config.\n");
        return -1;
    }
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/nodewatch.json", exe_dir);

    FILE* f = fopen(config_path, "r");
    if (!f) {
        fprintf(stderr, "Error: nodewatch.json not found in %s.\n", exe_dir);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(f); return -1; }
    fread(buffer, 1, length, f);
    buffer[length] = '\0';
    fclose(f);

    char error_buf[256];
    ctz_json_value* root = ctz_json_parse(buffer, error_buf, sizeof(error_buf));
    free(buffer);
    if (!root) {
        fprintf(stderr, "Error parsing nodewatch.json: %s\n", error_buf);
        return -1;
    }

    int result = -1;
    ctz_json_value* node_obj = ctz_json_find_object_value(root, node_name);
    if (node_obj && ctz_json_get_type(node_obj) == CTZ_JSON_OBJECT) {
        ctz_json_object_set_value(node_obj, "current_version", ctz_json_new_string(version_tag));
        result = 0;
    } else {
        fprintf(stderr, "Error: Node '%s' not found in %s.\n", node_name, config_path);
    }
    
    if (result == 0) {
        char* json_string = ctz_json_stringify(root, 1);
        if (json_string) {
            FILE* out = fopen(config_path, "w");
            if (out) {
                fprintf(out, "%s", json_string);
                fclose(out);
            } else {
                fprintf(stderr, "Error: Could not write updated nodewatch.json.\n");
                result = -1;
            }
            free(json_string);
        }
    }
    ctz_json_free(root);
    return result;
}



void stop_daemons() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) {
        fprintf(stderr, "PID file %s not found. Are daemons running?\n", PID_FILE);
        return;
    }
    
    pid_t pids[2];
    if (fscanf(f, "%d\n%d\n", &pids[0], &pids[1]) != 2) {
        fprintf(stderr, "Could not read PIDs from file. File may be corrupt.\n");
        fclose(f);
        
        if (remove(PID_FILE) == 0) {
            printf("Removed corrupt PID file.\n");
        }
        return;
    }
    fclose(f);

    printf("Sending SIGTERM to PIDs: %d (cloud), %d (query)\n", pids[0], pids[1]);

    int kill_cloud_result = kill(pids[0], SIGTERM);
    int kill_query_result = kill(pids[1], SIGTERM);

    if (kill_cloud_result != 0) {
        
        perror("Warning: Failed to send SIGTERM to cloud_daemon");
    }
    if (kill_query_result != 0) {
        perror("Warning: Failed to send SIGTERM to query_daemon");
    }

    if (kill_cloud_result == 0 && kill_query_result == 0) {
        printf("Termination signals sent successfully.\n");
    }

    // Give the daemons a moment to shut down before removing the PID file
    sleep(1); 
    
    remove(PID_FILE);
    printf("Daemons stopped.\n");
}

pid_t find_query_daemon_pid() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) return 0;
    pid_t cloud_pid, query_pid;
    if (fscanf(f, "%d\n%d\n", &cloud_pid, &query_pid) != 2) {
        query_pid = 0;
    }
    fclose(f);
    return query_pid;
}

static int get_node_conf_path(const char* node_name, const char* node_path, char* conf_path_buffer, size_t buffer_size) {
    if (snprintf(conf_path_buffer, buffer_size, "%s/.log/%s.conf", node_path, node_name) >= (int)buffer_size) {
        fprintf(stderr, "Error: Config path is too long.\n");
        return -1;
    }
    
    // Ensure the .log directory exists
    char log_dir_path[PATH_MAX];
    snprintf(log_dir_path, sizeof(log_dir_path), "%s/.log", node_path);
    
    struct stat st = {0};
    if (stat(log_dir_path, &st) == -1) {
        if (mkdir(log_dir_path, 0755) != 0) {
            perror("  Error creating .log directory");
            return -1;
        }
    }
    return 0;
}


int find_node_path_in_config(const char* node_name, char* path_buffer, size_t buffer_size) {
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Error: Could not determine executable directory to find config.\n");
        return -1;
    }
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/nodewatch.json", exe_dir);
    FILE* f = fopen(config_path, "r");
    if (!f) {
        fprintf(stderr, "Error: nodewatch.json not found in %s.\n", exe_dir);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(f); return -1; }
    fread(buffer, 1, length, f);
    buffer[length] = '\0';
    fclose(f);
    char error_buf[256];
    ctz_json_value* root = ctz_json_parse(buffer, error_buf, sizeof(error_buf));
    free(buffer);
    if (!root) {
        fprintf(stderr, "Error parsing nodewatch.json: %s\n", error_buf);
        return -1;
    }
    int result = -1;
    ctz_json_value* node_obj = ctz_json_find_object_value(root, node_name);
    if (node_obj && ctz_json_get_type(node_obj) == CTZ_JSON_OBJECT) {
        ctz_json_value* path_val = ctz_json_find_object_value(node_obj, "path");
        if (path_val && ctz_json_get_type(path_val) == CTZ_JSON_STRING) {
            strncpy(path_buffer, ctz_json_get_string(path_val), buffer_size - 1);
            path_buffer[buffer_size - 1] = '\0';
            result = 0;
        }
    }
    ctz_json_free(root);
    if (result != 0) {
        fprintf(stderr, "Error: Node '%s' not found in %s.\n", node_name, config_path);
    }
    return result;
}

static int get_current_subsection(const char* node_path, char* subsec_buffer, size_t buffer_size) {
    char subsec_file_path[PATH_MAX];
    snprintf(subsec_file_path, sizeof(subsec_file_path), "%s/.log/CURRENT_SUBSECTION", node_path);

    FILE* f = fopen(subsec_file_path, "r");
    if (!f) {
        // File doesn't exist, default to "master"
        strncpy(subsec_buffer, "master", buffer_size - 1);
        subsec_buffer[buffer_size - 1] = '\0';
        return 0; // Not an error, just default
    }

    if (fgets(subsec_buffer, buffer_size, f) == NULL) {
        fclose(f);
        // File is empty or unreadable, default to "master"
        strncpy(subsec_buffer, "master", buffer_size - 1);
        subsec_buffer[buffer_size - 1] = '\0';
        return 0;
    }
    fclose(f);
    subsec_buffer[strcspn(subsec_buffer, "\n")] = 0; // Remove newline
    
    if (strlen(subsec_buffer) == 0) {
        // File was empty, default to "master"
        strncpy(subsec_buffer, "master", buffer_size - 1);
        subsec_buffer[buffer_size - 1] = '\0';
    }
    
    return 0;
}

static int parse_node_path(const char* input_str, const char* default_node, 
                           char* out_node, size_t node_size, 
                           char* out_path, size_t path_size) {
    const char* colon = strchr(input_str, ':');
    if (colon) {
        // Inter-node format: "Node:path"
        size_t node_len = colon - input_str;
        if (node_len >= node_size) node_len = node_size - 1;
        strncpy(out_node, input_str, node_len);
        out_node[node_len] = '\0';
        
        const char* path_start = colon + 1;
        strncpy(out_path, path_start, path_size - 1);
        out_path[path_size - 1] = '\0';
    } else {
        // Intra-node format: "path"
        strncpy(out_node, default_node, node_size - 1);
        out_node[node_size - 1] = '\0';
        
        strncpy(out_path, input_str, path_size - 1);
        out_path[path_size - 1] = '\0';
    }
    
    // Basic validation
    if (out_node[0] == '\0' || out_path[0] == '\0') {
        return -1; // Invalid format
    }
    return 0;
}

static void run_node_man(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: exodus node-man <node_name> <operation> [args...]\n");
        fprintf(stderr, "Operations:\n");
        fprintf(stderr, "  --create <file|dir> <path/to/create>\n");
        fprintf(stderr, "  --delete <path/to/delete>\n");
        fprintf(stderr, "  --move <src_path> <dest_path_or_node:path>\n");
        fprintf(stderr, "  --copy <src_path> <dest_path_or_node:path>\n");
        return;
    }

    char* context_node = argv[2];
    char* operation = argv[3];

    void* req_payload = NULL;
    uint32_t payload_size = 0;
    uint16_t msg_type = 0;

    if (strcmp(operation, "--create") == 0 && argc == 6) {
        node_man_create_req_t req = {0};
        strncpy(req.node_name, context_node, sizeof(req.node_name) - 1);
        strncpy(req.path, argv[5], sizeof(req.path) - 1);
        if (strcmp(argv[4], "dir") == 0) {
            req.is_directory = 1;
        } else if (strcmp(argv[4], "file") != 0) {
            fprintf(stderr, "Error: --create type must be 'file' or 'dir'.\n");
            return;
        }
        
        msg_type = MSG_NODE_MAN_CREATE;
        payload_size = sizeof(req);
        req_payload = malloc(payload_size);
        memcpy(req_payload, &req, payload_size);

    } else if (strcmp(operation, "--delete") == 0 && argc == 5) {
        node_man_delete_req_t req = {0};
        strncpy(req.node_name, context_node, sizeof(req.node_name) - 1);
        strncpy(req.path, argv[4], sizeof(req.path) - 1);
        
        msg_type = MSG_NODE_MAN_DELETE;
        payload_size = sizeof(req);
        req_payload = malloc(payload_size);
        memcpy(req_payload, &req, payload_size);

    } else if ((strcmp(operation, "--move") == 0 || strcmp(operation, "--copy") == 0) && argc == 6) {
        node_man_move_copy_req_t req = {0};
        
        strncpy(req.src_node, context_node, sizeof(req.src_node) - 1);
        strncpy(req.src_path, argv[4], sizeof(req.src_path) - 1);

        if (parse_node_path(argv[5], context_node, req.dest_node, sizeof(req.dest_node), req.dest_path, sizeof(req.dest_path)) != 0) {
            fprintf(stderr, "Error: Invalid destination format '%s'.\n", argv[5]);
            return;
        }

        msg_type = (strcmp(operation, "--move") == 0) ? MSG_NODE_MAN_MOVE : MSG_NODE_MAN_COPY;
        payload_size = sizeof(req);
        req_payload = malloc(payload_size);
        memcpy(req_payload, &req, payload_size);

    } else {
        fprintf(stderr, "Error: Invalid operation or argument count for 'node-man'.\n");
        return;
    }
    // --- End of payload building ---

    cortez_mesh_t* mesh = cortez_mesh_init("exodus_client", NULL);
    if (!mesh) {
        fprintf(stderr, "Could not connect to exodus mesh. Are daemons running?\n");
        free(req_payload); // Free payload on early exit
        return;
    }

    int operation_complete = 0; // Flag to see if we ever got an ACK

    // --- NEW: Main retry loop ---
    for (int attempt = 1; attempt <= 5; attempt++) {
        
        // 1. Find the daemon (needs to be inside the loop)
        pid_t target_pid = find_query_daemon_pid();
        if (target_pid == 0) {
             fprintf(stderr, "Attempt %d: Could not find the query daemon.\n", attempt);
             if (attempt < 5) sleep(2); // Wait 2 seconds
             continue; // Retry
        }

        // 2. Send the request (this has its own *internal* 0.2s retry)
        int sent_ok = 0;
        for (int i = 0; i < 5; i++) {
            cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
            if (h) {
                size_t part1_size;
                void* buffer = cortez_write_handle_get_part1(h, &part1_size);
                memcpy(buffer, req_payload, payload_size); // Assuming payload fits in part1
                cortez_mesh_commit_send_zc(h, msg_type);
                sent_ok = 1;
                break;
            }
            usleep(200000); // 0.2s sleep for send buffer contention
        }

        // 3. Wait for response
        if (sent_ok) {
            printf("Waiting for response...\n");
            cortez_msg_t* msg = cortez_mesh_read(mesh, 10000); // 10 second timeout
            if(msg) {
                if(cortez_msg_type(msg) == MSG_OPERATION_ACK) {
                    const ack_t* ack = cortez_msg_payload(msg);
                    printf("Result: %s (%s)\n", ack->success ? "Success" : "Failure", ack->details);
                    operation_complete = 1; // We got a response, so the op is done
                    cortez_mesh_msg_release(mesh, msg);
                    break; // Exit main retry loop (success or hard failure)
                }
                cortez_mesh_msg_release(mesh, msg);
            } else {
                // Read timed out
                printf("Attempt %d: No response from daemon (timeout).\n", attempt);
                if (attempt < 5) sleep(2); // Wait 2 seconds
                // continue to next attempt...
            }
        } else {
             // Send failed
             fprintf(stderr, "Attempt %d: Failed to send message to query daemon.\n", attempt);
             if (attempt < 5) sleep(2); // Wait 2 seconds
             // continue to next attempt...
        }
    } // --- End of main retry loop ---
    
    free(req_payload); // Free the heap-allocated payload

    if (!operation_complete) {
        fprintf(stderr, "Operation failed after 5 attempts.\n");
    }
    
    cortez_mesh_shutdown(mesh);
}

static void run_node_edit() {
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Error: Could not determine executable directory to find exodus-tui.\n");
        return;
    }

    char gui_path[PATH_MAX];
    snprintf(gui_path, sizeof(gui_path), "%s/exodus-tui", exe_dir);

    // Check if the GUI executable exists and is executable
    if (access(gui_path, X_OK) != 0) {
        fprintf(stderr, "Error: 'exodus-tui' not found or not executable.\n");
        fprintf(stderr, "Please ensure 'exodus-tui' is in the same directory as 'exodus':\n%s\n", gui_path);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) { 

        execl(gui_path, "exodus-tui", (char*)NULL);
        
        // If execl returns, an error occurred
        perror("execl exodus-tui failed");
        exit(1); 
    
    } else if (pid < 0) {
        // --- FORK ERROR ---
        perror("fork for exodus-tui failed");
        return;
    
    } else {
        // --- PARENT PROCESS ---
        printf("Launching Exodus TUI... (PID: %d)\n", pid);
        
        // Wait for the child process (the TUI) to finish
        int status;
        waitpid(pid, &status, 0);
        
        printf("Exodus TUI exited.\n");
    }
}


// --- NEW: Simple cross-platform getpass ---
static char* getpass_custom(const char* prompt) {
    static char buffer[256];
    struct termios oldt, newt;
    int i = 0;

    memset(buffer, 0, sizeof(buffer));

    printf("%s", prompt);
    fflush(stdout);
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return NULL;
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) return NULL;
    while (i < (int)sizeof(buffer) - 1) {
        char c = getchar();
        if (c == '\n' || c == EOF) break;
        buffer[i++] = c;
    }
    buffer[i] = '\0';
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    printf("\n");
    return buffer;
}

// --- NEW: Helper to generate a 32-byte key from a password ---
static void generate_key_from_password(const char* password, uint8_t key[SHA256_BLOCK_SIZE]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t*)password, strlen(password));
    sha256_final(&ctx, key);
}

// --- NEW: Helper to generate a random IV ---
static void generate_random_iv(uint8_t iv[AES_BLOCKLEN]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1) {
        perror("Error opening /dev/urandom. Using pseudo-random fallback.");
        // Fallback for systems without /dev/urandom
        srand((unsigned int)time(NULL));
        for (int i = 0; i < AES_BLOCKLEN; i++) {
            iv[i] = (uint8_t)rand();
        }
        return;
    }
    if (read(fd, iv, AES_BLOCKLEN) != AES_BLOCKLEN) {
        fprintf(stderr, "Warning: Could not read full IV from /dev/urandom.\n");
    }
    close(fd);
}

// --- NEW: PKCS7 Padding ---
// Adds padding to data. Returns new size.
static size_t pkcs7_pad(uint8_t** data, size_t len) {
    size_t pad_len = AES_BLOCKLEN - (len % AES_BLOCKLEN);
    uint8_t* new_data = realloc(*data, len + pad_len);
    if (!new_data) return 0; // realloc failed
    for (size_t i = 0; i < pad_len; i++) {
        new_data[len + i] = (uint8_t)pad_len;
    }
    *data = new_data;
    return len + pad_len;
}

// Unpads data. Returns new size. On error, returns 0.
static size_t pkcs7_unpad(uint8_t* data, size_t len) {
    if (len == 0 || len % AES_BLOCKLEN != 0) return 0;
    uint8_t pad_len = data[len - 1];
    if (pad_len == 0 || pad_len > AES_BLOCKLEN) return 0;
    for (size_t i = 0; i < pad_len; i++) {
        if (data[len - 1 - i] != pad_len) return 0;
    }
    return len - pad_len;
}

// --- NEW: Globals for the pack callback ---
static FILE* g_pack_file_temp = NULL; // We write to a temp file first
static const char* g_pack_root_path = NULL;
static size_t g_pack_root_len = 0;

// --- NEW: ftw() callback function for packing ---
static int ftw_pack_callback(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf) {
    (void)ftwbuf; // Unused
    
    EnodeFileHeader header = {0};
    const char* relative_path = fpath + g_pack_root_len;
    if (*relative_path == '/') relative_path++;
    if (strlen(relative_path) == 0) return 0; // Skip root
    
    strncpy(header.relative_path, relative_path, sizeof(header.relative_path) - 1);
    header.mode = sb->st_mode;

    printf("  Archiving: %s\n", relative_path);

    if (typeflag == FTW_F) {
        header.data_size = sb->st_size;
    } else if (typeflag == FTW_D) {
        header.data_size = 0;
    } else if (typeflag == FTW_SL) {
        ssize_t len = readlink(fpath, header.link_target, sizeof(header.link_target) - 1);
        if (len != -1) header.link_target[len] = '\0';
        else return 0; // Skip bad link
        header.data_size = 0;
    } else {
        return 0; // Skip other types
    }

    // Write the unencrypted header to the temp file
    if (fwrite(&header, sizeof(header), 1, g_pack_file_temp) != 1) {
        fprintf(stderr, "  Error writing header to temp archive.\n");
        return -1; // Stop ftw
    }

    // Write the unencrypted file data (if any)
    if (typeflag == FTW_F && header.data_size > 0) {
        FILE* in_file = fopen(fpath, "rb");
        if (!in_file) return 0; // Skip
        
        char buffer[4096];
        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, sizeof(buffer), in_file)) > 0) {
            if (fwrite(buffer, 1, bytes_read, g_pack_file_temp) != bytes_read) {
                fprintf(stderr, "  Error writing data to temp archive.\n");
                fclose(in_file);
                return -1;
            }
        }
        fclose(in_file);
    }
    return 0; // Continue
}

static void run_clean_history(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exodus clean <node_name>\n");
        return;
    }
    char* node_name = argv[2];
    char node_path[PATH_MAX];

    if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) {
        return; // Error message already printed by the function
    }

    char history_path[PATH_MAX];
    snprintf(history_path, sizeof(history_path), "%s/.log/history.json", node_path);

    // Open in "w" mode to truncate the file to zero bytes
    FILE* f = fopen(history_path, "w");
    if (!f) {
        perror("Error: Could not open history.json to clear it");
        fprintf(stderr, "Path: %s\n", history_path);
        return;
    }
    
    fclose(f);

    printf("Successfully cleared uncommitted history for node '%s'.\n", node_name);
}

static void run_node_conf(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: exodus node-conf <node_name> [options...]\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  --auto <0|1>                Enable or disable auto-surveillance guardian.\n");
        fprintf(stderr, "  -h <1>                      (With --auto) Use headless (systemd) mode instead of XDG (desktop).\n");
        fprintf(stderr, "  --time <Unix|Real>          Set event timestamp format (Unix timestamp or Realtime string).\n");
        fprintf(stderr, "  --filter [.ext1 .ext2 ...]  Set file extensions to auto-delete (guardian only).\n");
        return;
    }

    char* node_name = argv[2];
    char node_path[PATH_MAX];
    if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) {
        return;
    }

    char conf_path[PATH_MAX];
    if (get_node_conf_path(node_name, node_path, conf_path, sizeof(conf_path)) != 0) {
        return;
    }

    // --- Config loading/updating logic ---
    char conf_auto[16] = "auto=0";
    char conf_time[32] = "time=Unix";
    char conf_filter[PATH_MAX] = "filter=";

    // 1. Read existing config file to load current values
    FILE* f = fopen(conf_path, "r");
    if (f) {
        char line[PATH_MAX + 10];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0; // strip newline
            if (strncmp(line, "auto=", 5) == 0) {
                strncpy(conf_auto, line, sizeof(conf_auto) - 1);
            } else if (strncmp(line, "time=", 5) == 0) {
                strncpy(conf_time, line, sizeof(conf_time) - 1);
            } else if (strncmp(line, "filter=", 7) == 0) {
                strncpy(conf_filter, line, sizeof(conf_filter) - 1);
            }
        }
        fclose(f);
    }

    // 2. Parse argv to update values
    int i = 3;
    int auto_changed = 0; // Flag to track if --auto was specified
    int new_auto_val = 0;
    int is_headless = 0; // Flag for systemd mode

    while (i < argc) {
        if (strcmp(argv[i], "--auto") == 0) {
            if (i + 1 < argc) {
                // --- Use strcmp, not atoi, to validate input ---
                if (strcmp(argv[i + 1], "0") == 0) {
                    snprintf(conf_auto, sizeof(conf_auto), "auto=0");
                    printf("Setting auto=0\n");
                    auto_changed = 1;
                    new_auto_val = 0;
                    i += 2; // Consume both
                } else if (strcmp(argv[i + 1], "1") == 0) {
                    snprintf(conf_auto, sizeof(conf_auto), "auto=1");
                    printf("Setting auto=1\n");
                    auto_changed = 1;
                    new_auto_val = 1;
                    i += 2; // Consume both
                } else {
                    fprintf(stderr, "Error: --auto value must be 0 or 1. Got '%s'.\n", argv[i+1]);
                    i++; // Only consume '--auto'
                    continue; 
                }
            } else {
                 fprintf(stderr, "Error: --auto requires an argument.\n"); i++;
            }
        } else if (strcmp(argv[i], "-h") == 0) {
            if (i + 1 < argc && strcmp(argv[i+1], "1") == 0) {
                is_headless = 1;
                i += 2;
            } else {
                fprintf(stderr, "Error: -h requires '1' as an argument.\n"); i++;
            }
        } else if (strcmp(argv[i], "--time") == 0) {
            if (i + 1 < argc) {
                if (strcmp(argv[i+1], "Unix") == 0 || strcmp(argv[i+1], "Real") == 0) {
                    snprintf(conf_time, sizeof(conf_time), "time=%s", argv[i+1]);
                    printf("Setting time=%s\n", argv[i+1]);
                } else {
                    fprintf(stderr, "Error: --time value must be 'Unix' or 'Real'.\n");
                }
                i += 2;
            } else {
                 fprintf(stderr, "Error: --time requires an argument.\n"); i++;
            }
        } else if (strcmp(argv[i], "--filter") == 0) {
            if (i + 1 < argc) {
                snprintf(conf_filter, sizeof(conf_filter), "filter="); // Reset filter list
                i++; // Move to first filter argument
                while(i < argc && argv[i][0] != '-') {
                    if (strlen(conf_filter) + strlen(argv[i]) + 2 < sizeof(conf_filter)) {
                        strcat(conf_filter, argv[i]);
                        strcat(conf_filter, " "); // Space-separated list
                    }
                    i++;
                }
                // Trim trailing space
                if (strlen(conf_filter) > 7 && conf_filter[strlen(conf_filter) - 1] == ' ') {
                    conf_filter[strlen(conf_filter) - 1] = '\0';
                }
                printf("Setting %s\n", conf_filter);
                // i is now at the next option or at argc
            } else {
                fprintf(stderr, "Info: To clear filter, use --filter with no arguments (this is not an error).\n");
                snprintf(conf_filter, sizeof(conf_filter), "filter="); // Clear filter
                i++;
            }
        } else {
            fprintf(stderr, "Warning: Ignoring unknown option '%s'\n", argv[i]);
            i++;
        }
    }

    // 3. Write the updated config file
    f = fopen(conf_path, "w");
    if (!f) {
        perror("  Error writing config file");
        return;
    }
    fprintf(f, "%s\n", conf_auto);
    fprintf(f, "%s\n", conf_time);
    fprintf(f, "%s\n", conf_filter);
    fclose(f);

    printf("Node '%s' config updated at '%s'.\n", node_name, conf_path);

    // 4. Handle Autostart logic
    if (auto_changed) {
        char exec_path[PATH_MAX]; // Path to the guardian executable
        char cmd[PATH_MAX * 2];
        
        // Find and copy the guardian executable (common to both modes)
        char exe_dir[PATH_MAX];
        if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
            fprintf(stderr, "  Error: Could not determine executable directory.\n");
            return;
        }
        char guardian_source_path[PATH_MAX];
        snprintf(guardian_source_path, sizeof(guardian_source_path), "%s/exodus-node-guardian", exe_dir);
        snprintf(exec_path, sizeof(exec_path), "%s/.log/%s-guardian", node_path, node_name);
        
        snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\"", guardian_source_path, exec_path);
        if (system(cmd) != 0) {
            fprintf(stderr, "  Error: Failed to copy 'exodus-node-guardian' to '%s'.\n", exec_path);
            fprintf(stderr, "  Make sure 'exodus-node-guardian' is compiled and in the same directory as 'exodus'.\n");
            return;
        }
        snprintf(cmd, sizeof(cmd), "chmod +x \"%s\"", exec_path);
        system(cmd);

        // Find the correct user and home directory based on the node's path
        uid_t node_owner_uid;
        char node_owner_home[PATH_MAX];
        const char* home_dir_fallback = getenv("HOME");
        if (home_dir_fallback == NULL) home_dir_fallback = "/tmp";

        if (get_user_uid_from_path(node_path, &node_owner_uid) != 0 || get_home_from_uid(node_owner_uid, node_owner_home, sizeof(node_owner_home)) != 0) {
            fprintf(stderr, "Warning: Could not determine node owner's home directory. Fallback to $HOME.\n");
            strncpy(node_owner_home, home_dir_fallback, sizeof(node_owner_home));
            node_owner_uid = getuid(); 
        }

        if (is_headless) {
            printf("Headless mode (-h 1) detected. Configuring systemd --user service...\n");
            
            char systemd_dir_path[PATH_MAX];
            char service_file_path[PATH_MAX];
            
            // Use the robust home path
            snprintf(systemd_dir_path, sizeof(systemd_dir_path), "%s/.config/systemd/user", node_owner_home); 
            snprintf(service_file_path, sizeof(service_file_path), "%s/%s.service", systemd_dir_path, node_name);

            if (new_auto_val == 1) {
                // Create systemd user directory
                snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", systemd_dir_path);
                system(cmd);

                // Create and write the service file
                const char* service_template =
                    "[Unit]\n"
                    "Description=Exodus Self-Surveillance Guardian for %s\n"
                    "After=network.target\n\n" // Using network.target from your old code
                    "[Service]\n"
                    "ExecStart=%s\n"
                    "Restart=always\n"
                    "RestartSec=10\n\n" // Using 10s from your old code
                    "[Install]\n"
                    "WantedBy=default.target\n";
                
                char service_content[2048];
                snprintf(service_content, sizeof(service_content), service_template, node_name, exec_path);
                
                f = fopen(service_file_path, "w");
                if (!f) { perror("  Error writing systemd service file"); return; }
                fprintf(f, "%s", service_content);
                fclose(f);
                
                printf("  Reloading systemd user daemon...\n");
                system("systemctl --user daemon-reload");
                snprintf(cmd, sizeof(cmd), "systemctl --user enable %s.service", node_name);
                system(cmd);
                snprintf(cmd, sizeof(cmd), "systemctl --user start %s.service", node_name);
                system(cmd);
                
                printf("Successfully enabled auto-surveillance for '%s'.\n", node_name);
                printf("Manage with: systemctl --user status %s.service\n", node_name);

            } else {
                printf("Disabling auto-surveillance for node '%s'...\n", node_name);
                
                // --- Using the simple logic from your "old" version ---
                snprintf(cmd, sizeof(cmd), "systemctl --user stop %s.service", node_name);
                system(cmd);
                snprintf(cmd, sizeof(cmd), "systemctl --user disable %s.service", node_name);
                system(cmd);
                // --- End simple logic ---

                remove(service_file_path);
                remove(exec_path);
                
                printf("  Reloading systemd user daemon...\n");
                system("systemctl --user daemon-reload");
                printf("Successfully disabled auto-surveillance for '%s'.\n", node_name);
            }

        } else {

            printf("Desktop mode detected. Configuring XDG Autostart...\n");

            char autostart_dir[PATH_MAX];
            char desktop_file_path[PATH_MAX];
            snprintf(autostart_dir, sizeof(autostart_dir), "%s/.config/autostart", node_owner_home);
            snprintf(desktop_file_path, sizeof(desktop_file_path), "%s/exodus-guardian-%s.desktop", autostart_dir, node_name);

            if (new_auto_val == 1) {
                // Create autostart directory
                snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", autostart_dir);
                system(cmd);

                // Create and write .desktop file
                const char* desktop_template = 
                    "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=Exodus Guardian (%s)\n"
                    "Comment=Exodus self-surveillance for node %s\n"
                    "Exec=%s\n"
                    "Terminal=false\n"
                    "X-GNOME-Autostart-enabled=true\n";
                
                char desktop_content[2048];
                snprintf(desktop_content, sizeof(desktop_content), desktop_template, node_name, node_name, exec_path);
                
                f = fopen(desktop_file_path, "w");
                if (!f) { perror("  Error writing .desktop file"); return; }
                fprintf(f, "%s", desktop_content);
                fclose(f);

                // Start the service *now* for the current session by forking
                printf("  Starting guardian for current session...\n");
                pid_t pid = fork();
                if (pid == 0) {
                    if (setuid(node_owner_uid) != 0) exit(1); // Drop privileges
                    execl(exec_path, exec_path, (char*)NULL);
                    perror("  execl failed");
                    exit(1);
                } else if (pid < 0) {
                    perror("  fork failed");
                }
                printf("Successfully enabled auto-surveillance for '%s'.\n", node_name);

            } else {
                printf("Disabling auto-surveillance for node '%s'...\n", node_name);
                
                printf("  Stopping running guardian process...\n");
                snprintf(cmd, sizeof(cmd), "pkill -f \"%s\"", exec_path);
                system(cmd);
                
                sleep(1);

                // Remove .desktop file and executable
                remove(desktop_file_path);
                remove(exec_path);
                
                printf("Successfully disabled auto-surveillance for '%s'.\n", node_name);
            }
        }
    }
}

static void print_key(const char* title, uint8_t key[SHA256_BLOCK_SIZE]) {
    printf("DEBUG KEY (%s): ", title);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        printf("%02x", key[i]);
    }
    printf("\n");
}

static void run_pack(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exodus pack <node_name>\n");
        return;
    }
    char* node_name = argv[2];
    char node_path[PATH_MAX];
    if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) return;

    char out_file[MAX_NODE_NAME_LEN + 7]; // .enode + \0
    snprintf(out_file, sizeof(out_file), "%s.enode", node_name);
    char tmp_file[] = "exodus.pack.tmp";

    char password_buffer[256]; // Create a local buffer
    char* password_ptr = getpass_custom("Enter encryption password: ");
    if (strlen(password_ptr) == 0) { 
        fprintf(stderr, "Password cannot be empty. Aborting.\n"); 
        return; 
    }
    strncpy(password_buffer, password_ptr, sizeof(password_buffer) - 1);
    password_buffer[sizeof(password_buffer) - 1] = '\0';
    
    char* password_confirm = getpass_custom("Verify password: ");
    
    if (strcmp(password_buffer, password_confirm) != 0) { 
        fprintf(stderr, "Passwords do not match. Aborting.\n"); 
        return; 
    }
    
    uint8_t key[SHA256_BLOCK_SIZE];
    generate_key_from_password(password_buffer, key);

    // --- Pass 1: Write all data unencrypted to a temporary file ---
    g_pack_file_temp = fopen(tmp_file, "wb");
    if (!g_pack_file_temp) { perror("Error opening temp file"); return; }
    
    printf("Packing node '%s' from '%s'...\n", node_name, node_path);
    g_pack_root_path = node_path;
    g_pack_root_len = strlen(node_path);

    if (nftw(node_path, ftw_pack_callback, 20, FTW_PHYS) != 0) {
        fprintf(stderr, "Error during file tree walk. Archive may be incomplete.\n");
        fclose(g_pack_file_temp);
        remove(tmp_file);
        return;
    }
    
    // Write EOF marker to temp file
    uint64_t eof_marker = ENODE_EOF_MARKER;
    fwrite(&eof_marker, sizeof(eof_marker), 1, g_pack_file_temp);
    fclose(g_pack_file_temp); // Close temp file

    // --- Pass 2: Stream-encrypt from the temporary file to the final file ---
    printf("Encrypting archive...\n");
    FILE* f_tmp_in = fopen(tmp_file, "rb"); // Re-open temp file for reading
    if (!f_tmp_in) { perror("Error reading temp file"); remove(tmp_file); return; }
    
    FILE* f_out = fopen(out_file, "wb"); // Open final output file
    if (!f_out) { perror("Error opening output file"); fclose(f_tmp_in); remove(tmp_file); return; }

    // Prepare Header
    EnodeHeader header = {0};
    strncpy(header.magic, ENODE_MAGIC, sizeof(header.magic));
    strncpy(header.node_name, node_name, sizeof(header.node_name) - 1);
    generate_random_iv(header.iv); // Generate IV for the header

    // --- Fill header metadata ---
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) == 0) {
        char config_path[PATH_MAX];
        snprintf(config_path, sizeof(config_path), "%s/nodewatch.json", exe_dir);
        FILE* f_conf = fopen(config_path, "r");
        if (f_conf) {
            fseek(f_conf, 0, SEEK_END); long length = ftell(f_conf); fseek(f_conf, 0, SEEK_SET);
            char* buffer = malloc(length + 1);
            if (buffer) {
                fread(buffer, 1, length, f_conf); buffer[length] = '\0';
                ctz_json_value* root = ctz_json_parse(buffer, NULL, 0);
                if (root) {
                    ctz_json_value* node_obj = ctz_json_find_object_value(root, node_name);
                    if (node_obj) {
                        ctz_json_value* val;
                        val = ctz_json_find_object_value(node_obj, "author");
                        if (val) strncpy(header.author, ctz_json_get_string(val), sizeof(header.author) - 1);
                        val = ctz_json_find_object_value(node_obj, "desc");
                        if (val) strncpy(header.desc, ctz_json_get_string(val), sizeof(header.desc) - 1);
                        val = ctz_json_find_object_value(node_obj, "tag");
                        if (val) strncpy(header.tag, ctz_json_get_string(val), sizeof(header.tag) - 1);
                        val = ctz_json_find_object_value(node_obj, "current_version");
                        if (val) strncpy(header.current_version, ctz_json_get_string(val), sizeof(header.current_version) - 1);
                    }
                    ctz_json_free(root);
                }
                free(buffer);
            }
            fclose(f_conf);
        } else { fprintf(stderr, "Warning: Could not open nodewatch.json. Metadata will be blank.\n"); }
    } else { fprintf(stderr, "Warning: Could not find nodewatch.json path. Metadata will be blank.\n"); }
    // --- End header metadata ---

    // Write the unencrypted header to the final file
    if (fwrite(&header, sizeof(header), 1, f_out) != 1) {
        fprintf(stderr, "Error writing archive header.\n");
        fclose(f_tmp_in); fclose(f_out); remove(tmp_file); return;
    }

    // --- Start streaming encryption ---
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, header.iv); // Init AES with the IV from the header

    uint8_t buffer[4096]; // 4KB chunk buffer
    size_t bytes_read;
    
    while (1) {
        bytes_read = fread(buffer, 1, sizeof(buffer), f_tmp_in);
        
        if (bytes_read == 4096) {
            // Full chunk - encrypt and write
            AES_CBC_encrypt_buffer(&ctx, buffer, (uint32_t)bytes_read);
            if (fwrite(buffer, 1, bytes_read, f_out) != bytes_read) {
                fprintf(stderr, "Error writing encrypted data chunk.\n");
                break; // Exit loop on error
            }
        } else {
            // Last chunk (could be 0 to 4095 bytes)
            size_t final_data_len = bytes_read;
            size_t pad_len = AES_BLOCKLEN - (final_data_len % AES_BLOCKLEN);
            size_t padded_final_size = final_data_len + pad_len;

            // Use a temporary buffer for this final padded block
            uint8_t* final_buf = malloc(padded_final_size);
            if (!final_buf) {
                fprintf(stderr, "Out of memory for final padding.\n");
                break;
            }
            
            // Copy the last bytes
            memcpy(final_buf, buffer, final_data_len);
            
            // Apply PKCS7 padding
            for (size_t i = 0; i < pad_len; i++) {
                final_buf[final_data_len + i] = (uint8_t)pad_len;
            }
            
            // Encrypt and write the final padded block
            AES_CBC_encrypt_buffer(&ctx, final_buf, (uint32_t)padded_final_size);
            if (fwrite(final_buf, 1, padded_final_size, f_out) != padded_final_size) {
                 fprintf(stderr, "Error writing final encrypted chunk.\n");
            } else {
                printf("\nSuccessfully packed and encrypted node to '%s'.\n", out_file);
            }
            
            free(final_buf);
            break; // End of file, break from loop
        }
    }
    
    // --- Cleanup ---
    fclose(f_out);
    fclose(f_tmp_in);
    remove(tmp_file);
}

static void run_unit_list(cortez_mesh_t* mesh, pid_t target_pid) {
    int sent_ok = 0;
    for (int i = 0; i < 5; i++) {
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, 1);
        if (h) {
            cortez_mesh_commit_send_zc(h, MSG_SIG_REQUEST_UNIT_LIST);
            sent_ok = 1;
            break;
        }
        usleep(100000);
    }
    
    if (!sent_ok) {
        fprintf(stderr, "Failed to send LIST_UNITS request.\n");
        return;
    }

    printf("Waiting for response [10s]...\n");
    cortez_msg_t* msg = cortez_mesh_read(mesh, 10000);
    if (msg) {
        if (cortez_msg_type(msg) == MSG_SIG_RESPONSE_UNIT_LIST) {
            const char* json_body = (const char*)cortez_msg_payload(msg);
            
            ctz_json_value* root = ctz_json_parse(json_body, NULL, 0);
            if (root && ctz_json_get_type(root) == CTZ_JSON_ARRAY) {
                printf("--- Registered Units ---\n");
                for (size_t i = 0; i < ctz_json_get_array_size(root); i++) {
                    ctz_json_value* item = ctz_json_get_array_element(root, i);
                    const char* name = ctz_json_get_string(ctz_json_find_object_value(item, "name"));
                    const char* status = ctz_json_get_string(ctz_json_find_object_value(item, "status"));
                    printf("  %s (%s)\n", name ? name : "??", status ? status : "??");
                }
                ctz_json_free(root);
            } else {
                fprintf(stderr, "Received invalid JSON from daemon.\n");
            }
        } else if (cortez_msg_type(msg) == MSG_OPERATION_ACK) {
            const ack_t* ack = cortez_msg_payload(msg);
            fprintf(stderr, "Error from daemon: %s\n", ack->details);
        } else {
            fprintf(stderr, "Received unexpected response type: %d\n", cortez_msg_type(msg));
        }
        cortez_mesh_msg_release(mesh, msg);
    } else {
        printf("No response from daemon (timeout).\n");
    }
}

static void run_connect(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exodus connect <coord-name>\n");
        return;
    }
    char* target_name = argv[2];
    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) return;

    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/exodus-coord.set", exe_dir);

    SetConfig* cfg = set_load(conf_path);
    if (!cfg) {
        fprintf(stderr, "Error: No coordinators configured. Use 'unit-set --coord' first.\n");
        return;
    }

    // Check if target exists
    const char* check_ip = set_get_string(cfg, target_name, "ip", NULL);
    if (!check_ip) {
        fprintf(stderr, "Error: Coordinator '%s' not found in configuration.\n", target_name);
        set_free(cfg);
        return;
    }

    // Iterate through all sections to unset 'current' and set it for target
    SetSection* sec = cfg->sections;
    while (sec) {
        if (strcmp(sec->name, "global") != 0) {
            if (strcmp(sec->name, target_name) == 0) {
                set_set_bool(cfg, sec->name, "current", 1);
            } else {
                set_set_bool(cfg, sec->name, "current", 0);
            }
        }
        sec = sec->next;
    }

    if (set_save(cfg) == 0) {
        printf("Switched to coordinator '%s' (%s).\n", target_name, check_ip);
        printf("Please restart daemons to apply.\n");
    } else {
        perror("Error saving config");
    }
    set_free(cfg);
}

static int confirm_overwrite(const char* label, const char* current_val, const char* new_val) {
    // Case 1: No existing value (First time setup) -> Auto-approve
    if (current_val == NULL || strlen(current_val) == 0) {
        return 1; 
    }

    // Case 2: Value is identical -> No change needed
    if (strcmp(current_val, new_val) == 0) {
        printf("Info: %s is already set to '%s'. No changes made.\n", label, new_val);
        return 0; // Return 0 to skip write
    }

    // Case 3: Overwrite -> Prompt user
    fprintf(stderr, "----------------------------------------\n");
    fprintf(stderr, "Configuration Change Requested:\n");
    fprintf(stderr, "  Current %-12s: %s\n", label, current_val);
    fprintf(stderr, "  New     %-12s: %s\n", label, new_val);
    fprintf(stderr, "----------------------------------------\n");
    fprintf(stderr, "Overwrite this setting? [y/N] ");
    
    char response[32];
    if (fgets(response, sizeof(response), stdin)) {
        if (response[0] == 'y' || response[0] == 'Y') {
            return 1;
        }
    }
    printf("Operation cancelled.\n");
    return -1; // -1 indicates cancellation
}

static void run_unit_set(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: exodus unit-set <flag> [arguments...]\n");
        fprintf(stderr, "  --name <New Unit Name>      Set this unit's display name\n");
        fprintf(stderr, "  --desg <path/to/storage>    Set the designated folder for incoming Pushes\n");
        fprintf(stderr, "  --coord <coord-name> <ip> <port>         Set the Coordinator address\n");
        return;
    }

    char exe_dir[PATH_MAX];
    if (get_executable_dir(exe_dir, sizeof(exe_dir)) != 0) {
        fprintf(stderr, "Error: Could not determine executable directory.\n");
        return;
    }

    // --- Handle Coordinator Config (Separate File) ---
if (strcmp(argv[2], "--coord") == 0) {
        // Usage: --coord <Name> <IP> <Port>
        if (argc != 6) {
            fprintf(stderr, "Usage: exodus unit-set --coord <Name> <IP> <Port>\n");
            return;
        }
        char* coord_name = argv[3];
        char* ip = argv[4];
        int port = atoi(argv[5]);

        char conf_path[PATH_MAX];
        snprintf(conf_path, sizeof(conf_path), "%s/exodus-coord.set", exe_dir);
        
        SetConfig* cfg = set_load(conf_path);
        if (!cfg) cfg = set_create(conf_path);

        // Check if this is an overwrite
        const char* old_ip = set_get_string(cfg, coord_name, "ip", "");
        if (strlen(old_ip) > 0) {
            printf("Updating existing coordinator '%s'...\n", coord_name);
            // Optional: Add confirm_overwrite logic here if you want
        }

        // Set values
        set_set_string(cfg, coord_name, "ip", ip);
        set_set_int(cfg, coord_name, "port", port);
        
        SetSection* sec = cfg->sections;
        while (sec) {
            set_set_bool(cfg, sec->name, "current", 0); // Reset all others
            sec = sec->next;
        }
        set_set_bool(cfg, coord_name, "current", 1);

        if (set_save(cfg) == 0) {
            printf("Coordinator '%s' set to %s:%d (Active).\n", coord_name, ip, port);
            printf("Please restart daemons.\n");
        }
        set_free(cfg);
        return;
    }

    // --- Handle Unit Config (Shared File: Name & Storage) ---
    char conf_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/exodus-unit.set", exe_dir);

    // 1. Load existing config using ctz-set
    SetConfig* config = set_load(conf_path);
    if (!config) {
        fprintf(stderr, "Error loading configuration.\n");
        return;
    }

    int save_needed = 0;

    // 2. Process Changes
    if (strcmp(argv[2], "--name") == 0) {
        if (argc != 4) { fprintf(stderr, "Usage: --name <name>\n"); set_free(config); return; }
        
        const char* current_name = set_get_string(config, "unit", "name", "My-Exodus-Unit");
        int result = confirm_overwrite("Unit Name", current_name, argv[3]);
        
        if (result == 1) {
            set_set_string(config, "unit", "name", argv[3]);
            save_needed = 1;
        } else if (result == -1) {
            set_free(config);
            return; // Cancelled
        }

    } else if (strcmp(argv[2], "--desg") == 0 || strcmp(argv[2], "--desig") == 0) {
        if (argc != 4) { fprintf(stderr, "Usage: --desg <path>\n"); set_free(config); return; }
        
        char resolved_path[PATH_MAX];
        if (realpath(argv[3], resolved_path) == NULL) {
            // If path doesn't exist, ask to create it or abort
            fprintf(stderr, "Warning: Path '%s' does not exist.\n", argv[3]);
            // Try to use absolute path anyway if user provided absolute
            if (argv[3][0] == '/') {
                 strncpy(resolved_path, argv[3], sizeof(resolved_path)-1);
            } else {
                 fprintf(stderr, "Error: Please provide a valid, absolute path, or create the directory first.\n");
                 set_free(config);
                 return;
            }
        }

        const char* current_storage = set_get_string(config, "storage", "path", "");
        int result = confirm_overwrite("Storage Path", current_storage, resolved_path);
        
        if (result == 1) {
            set_set_string(config, "storage", "path", resolved_path);
            save_needed = 1;
        } else if (result == -1) {
            set_free(config);
            return; // Cancelled
        }

    } else {
        fprintf(stderr, "Unknown flag: %s\n", argv[2]);
        set_free(config);
        return;
    }

    // 3. Safe Write (Only if changed)
    if (save_needed) {
        if (set_save(config) == 0) {
            printf("Configuration updated successfully.\n");
            printf("Please restart daemons for the change to take effect:\n");
            printf("  sudo ./exodus stop && sudo ./exodus start\n");
        } else {
            perror("Error saving configuration");
        }
    }

    set_free(config);
}

static void run_view_unit(cortez_mesh_t* mesh, pid_t target_pid, const char* unit_name) {
    uint32_t payload_size = sizeof(sig_view_unit_req_t);
    int sent_ok = 0;
    for (int i = 0; i < 5; i++) {
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
        if (h) {
            size_t part1_size;
            sig_view_unit_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
            strncpy(req->unit_name, unit_name, sizeof(req->unit_name) - 1);
            cortez_mesh_commit_send_zc(h, MSG_SIG_REQUEST_VIEW_UNIT);
            sent_ok = 1;
            break;
        }
        usleep(100000);
    }
    
    if (!sent_ok) {
        fprintf(stderr, "Failed to send VIEW_UNIT request.\n");
        return;
    }

    printf("Waiting for response [10s]...\n");
    cortez_msg_t* msg = cortez_mesh_read(mesh, 10000);
    if (msg) {
        if (cortez_msg_type(msg) == MSG_SIG_RESPONSE_VIEW_UNIT) {
            if (cortez_msg_payload_size(msg) <= sizeof(uint64_t)) {
                fprintf(stderr, "Received invalid (too small) response from daemon.\n");
                cortez_mesh_msg_release(mesh, msg);
                return;
            }
            const char* json_body = (const char*)cortez_msg_payload(msg);
            
            ctz_json_value* root = ctz_json_parse(json_body, NULL, 0);
            if (root && ctz_json_get_type(root) == CTZ_JSON_ARRAY) {
                printf("--- Nodes on Unit '%s' ---\n", unit_name);
                for (size_t i = 0; i < ctz_json_get_array_size(root); i++) {
                    ctz_json_value* item = ctz_json_get_array_element(root, i);
                    const char* name = ctz_json_get_string(ctz_json_find_object_value(item, "name"));
                    printf("  - %s\n", name ? name : "??");
                }
                ctz_json_free(root);
            } else {
                 fprintf(stderr, "Received invalid JSON from daemon.\n");
            }
        } else if (cortez_msg_type(msg) == MSG_OPERATION_ACK) {
            const ack_t* ack = cortez_msg_payload(msg);
            fprintf(stderr, "Error from daemon: %s\n", ack->details);
        } else {
            fprintf(stderr, "Received unexpected response type: %d\n", cortez_msg_type(msg));
        }
        cortez_mesh_msg_release(mesh, msg);
    } else {
        printf("No response from daemon (timeout).\n");
    }
}

static void run_push_node(cortez_mesh_t* mesh, pid_t target_pid, const char* node_name, const char* target_unit) {
    // 1. Resolve Target IP
    printf("Resolving address for unit '%s'...\n", target_unit);
    
    resolve_unit_req_t req;
    strncpy(req.target_unit_name, target_unit, sizeof(req.target_unit_name) - 1);
    
    int sent_ok = 0;
    for(int i=0; i<5; i++) {
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, sizeof(req));
        if(h) {
            size_t part1_size;
            void* buffer = cortez_write_handle_get_part1(h, &part1_size);
            memcpy(buffer, &req, sizeof(req));
            cortez_mesh_commit_send_zc(h, MSG_SIG_REQUEST_RESOLVE_UNIT);
            sent_ok = 1; break;
        }
        usleep(100000);
    }
    
    if(!sent_ok) { fprintf(stderr, "Failed to contact daemon.\n"); return; }

    char target_ip[64] = {0};
    int target_port = 0;
    
    printf("Waiting for resolution...\n");
    cortez_msg_t* msg = cortez_mesh_read(mesh, 5000); // 5s timeout
    if(msg) {
        if (cortez_msg_type(msg) == MSG_SIG_RESPONSE_RESOLVE_UNIT) {
            const resolve_unit_resp_t* resp = (const resolve_unit_resp_t*)cortez_msg_payload(msg);
            if(resp->success) {
                strncpy(target_ip, resp->ip_addr, 63);
                target_port = resp->port;
            }
        }
        cortez_mesh_msg_release(mesh, msg);
    }

    if(target_port == 0) {
        fprintf(stderr, "Error: Unit '%s' not found or offline.\n", target_unit);
        return;
    }

    // 2. Find Local Node Path
    char node_path[PATH_MAX];
    if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) return;

    printf("Pushing node '%s' to %s:%d...\n", node_name, target_ip, target_port);

    // 3. Create Temporary Archive (TAR)
    // Using tar is fast and preserves structure
    char tmp_file[64];
    snprintf(tmp_file, sizeof(tmp_file), "/tmp/exodus_push_%d.tar", getpid());
    char cmd[PATH_MAX*2];
    // Create tar of the node directory
    snprintf(cmd, sizeof(cmd), "tar -cf \"%s\" -C \"%s\" .", tmp_file, node_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error: Failed to pack node data.\n");
        return;
    }

    // 4. Upload Archive
    struct sockaddr_in serv_addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) { perror("Socket creation failed"); remove(tmp_file); return; }
    
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(target_port);
    if(inet_pton(AF_INET, target_ip, &serv_addr.sin_addr)<=0) { perror("Invalid address"); remove(tmp_file); return; }
    
    if(connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed"); remove(tmp_file); return;
    }

    // Calculate file size
    FILE* f = fopen(tmp_file, "rb");
    if(!f) { perror("Failed to open temp archive"); close(sock); remove(tmp_file); return; }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Send Headers
    char header[1024];
    snprintf(header, sizeof(header), 
        "POST /push_incoming HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Node-Name: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        target_ip, target_port, node_name, file_size);
    write(sock, header, strlen(header));

    // Send File Data
    char buf[65536]; // 64KB buffer for speed
    size_t n;
    size_t total_sent = 0;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        write(sock, buf, n);
        total_sent += n;
        // Optional: Print progress bar here
    }
    fclose(f);
    remove(tmp_file);

    // Read Response
    char resp[1024];
    ssize_t rn = read(sock, resp, sizeof(resp)-1);
    if (rn > 0) {
        resp[rn] = '\0';
        if (strstr(resp, "200 OK")) {
            printf("Push successful! Node delivered to designated storage.\n");
        } else {
            printf("Server reported error:\n%s\n", resp);
        }
    } else {
        printf("No response from server (connection closed).\n");
    }
    
    close(sock);
}

static void run_sync_node(cortez_mesh_t* mesh, pid_t target_pid, 
                          const char* unit_name, const char* remote_node, const char* local_node) {
    
    // 1. Find local node path
    char local_node_path[PATH_MAX];
    if (find_node_path_in_config(local_node, local_node_path, sizeof(local_node_path)) != 0) {
        fprintf(stderr, "Error: Local node '%s' not found in config.\n", local_node);
        return;
    }
    
    // 2. Read local history.json
    char local_history_path[PATH_MAX];
    snprintf(local_history_path, sizeof(local_history_path), "%s/.log/history.json", local_node_path);
    char error_buf[256];
    ctz_json_value* history_json = ctz_json_load_file(local_history_path, error_buf, sizeof(error_buf));
    if (!history_json) {
        fprintf(stderr, "Error: Could not read local history file: %s (%s)\n", local_history_path, error_buf);
        history_json = ctz_json_new_array(); // Send empty history
    }

    // 3. Create the master payload object: { "history": [...], "files": {} }
    ctz_json_value* payload_obj = ctz_json_new_object();
    ctz_json_object_set_value(payload_obj, "history", history_json);
    ctz_json_value* files_obj = ctz_json_new_object();
    ctz_json_object_set_value(payload_obj, "files", files_obj);

    // 4. Find all "Created" and "Modified" files and add them to the payload
    printf("Analyzing local history for new files...\n");
    for (size_t i = 0; i < ctz_json_get_array_size(history_json); i++) {
        ctz_json_value* event = ctz_json_get_array_element(history_json, i);
        const char* event_str = ctz_json_get_string(ctz_json_find_object_value(event, "event"));
        const char* name_str = ctz_json_get_string(ctz_json_find_object_value(event, "name"));
        
        if (!event_str || !name_str) continue;

        if (strcmp(event_str, "Created") == 0 || strcmp(event_str, "Modified") == 0) {
            // Check if we already added this file (to avoid duplicates)
            if (ctz_json_find_object_value(files_obj, name_str) != NULL) {
                continue;
            }

            char file_full_path[PATH_MAX];
            snprintf(file_full_path, sizeof(file_full_path), "%s/%s", local_node_path, name_str);
            
            size_t file_size;
            unsigned char* file_content = read_file_for_sync(file_full_path, &file_size);
            if (!file_content) {
                fprintf(stderr, "Warning: Could not read file %s, skipping...\n", name_str);
                continue;
            }
            
            printf("  > Bundling: %s (%zu bytes)\n", name_str, file_size);
            
            size_t b64_len;
            char* b64_content = base64_encode(file_content, file_size, &b64_len);
            free(file_content);
            
            if (!b64_content) {
                fprintf(stderr, "Warning: Failed to encode file %s, skipping...\n", name_str);
                continue;
            }
            
            // --- FIXED: Use ctz_json_new_string and free the original b64 string ---
            ctz_json_object_set_value(files_obj, name_str, ctz_json_new_string(b64_content));
            free(b64_content); // Free the original, ctz-json has its own copy now.
        }
    }

    // 5. Stringify the final payload
    char* payload_json_string = ctz_json_stringify(payload_obj, 0); // 0 = compact
    ctz_json_free(payload_obj); // Frees history_json, files_obj, and the *copies* of b64 strings
    
    if (!payload_json_string) {
        fprintf(stderr, "Error: Failed to create final JSON payload.\n");
        return;
    }

    size_t json_string_len = strlen(payload_json_string);
    uint32_t total_payload_size = sizeof(sig_sync_req_t) + json_string_len + 1;
    
    printf("Total sync payload: %.2f KB\n", (double)total_payload_size / 1024.0);
    
    // 6. Prepare and send the request
    sig_sync_req_t* req_header = calloc(1, total_payload_size);
    if (!req_header) {
        free(payload_json_string);
        return;
    }
    
    strncpy(req_header->target_unit, unit_name, sizeof(req_header->target_unit) - 1);
    strncpy(req_header->remote_node, remote_node, sizeof(req_header->remote_node) - 1);
    strncpy(req_header->local_node, local_node, sizeof(req_header->local_node) - 1);
    memcpy(req_header->sync_payload_json, payload_json_string, json_string_len + 1);
    free(payload_json_string);

    // 7. Send
    int sent_ok = 0;
    for (int i = 0; i < 5; i++) {
        cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, total_payload_size);
        if (h) {
            write_to_handle(h, req_header, total_payload_size); // <-- This will now link correctly
            cortez_mesh_commit_send_zc(h, MSG_SIG_REQUEST_SYNC_NODE);
            sent_ok = 1;
            break;
        }
        usleep(100000);
    }
    free(req_header);
    
    if (!sent_ok) {
        fprintf(stderr, "Failed to send SYNC_NODE request.\n");
        return;
    }

    // 8. Wait for ACK
    printf("Waiting for sync ACK [30s]...\n");
    cortez_msg_t* msg = cortez_mesh_read(mesh, 30000);
    if (msg) {
        if (cortez_msg_type(msg) == MSG_OPERATION_ACK) {
            const ack_t* ack = cortez_msg_payload(msg);
            printf("Result: %s (%s)\n", ack->success ? "Success" : "Failure", ack->details);
        } else {
            fprintf(stderr, "Received unexpected response type: %d\n", cortez_msg_type(msg));
        }
        cortez_mesh_msg_release(mesh, msg);
    } else {
        printf("No response from daemon (timeout).\n");
    }
}

static void run_unpack(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exodus unpack <path/to/.enode file>\n");
        return;
    }
    char* node_file = argv[2];
    if (access(node_file, F_OK) == -1) { fprintf(stderr, "Error: File not found: %s\n", node_file); return; }
    
    char password_buffer[256]; // Create a local buffer
    char* password_ptr = getpass_custom("Enter decryption password: ");
    if (strlen(password_ptr) == 0) { 
        fprintf(stderr, "Password cannot be empty. Aborting.\n"); 
        return; 
    }
    // Copy the password into our local buffer
    strncpy(password_buffer, password_ptr, sizeof(password_buffer) - 1);
    password_buffer[sizeof(password_buffer) - 1] = '\0';
    
    uint8_t key[SHA256_BLOCK_SIZE];
    // Generate the key from the local buffer, not the static pointer
    generate_key_from_password(password_buffer, key);

    FILE* f = fopen(node_file, "rb");
    if (!f) { perror("Error opening archive file"); return; }

    EnodeHeader header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error: Could not read archive header.\n"); fclose(f); return;
    }
    if (memcmp(header.magic, ENODE_MAGIC, strlen(ENODE_MAGIC)) != 0) {
    fprintf(stderr, "Error: Not a valid .enode file (magic mismatch).\n");
    fclose(f);
    return;
    }


    // Read all encrypted data :)

struct stat st;
if (fstat(fileno(f), &st) != 0) {
    perror("Error: fstat failed");
    fclose(f);
    return;
}

if (st.st_size < (off_t)sizeof(EnodeHeader)) {
    fprintf(stderr, "Error: File too small or corrupt.\n");
    fclose(f);
    return;
}

size_t data_size = (size_t)(st.st_size - (off_t)sizeof(EnodeHeader));
if (data_size == 0 || data_size % AES_BLOCKLEN != 0) {
    fprintf(stderr, "Error: Corrupt file or wrong password (invalid length).\n");
    fclose(f);
    return;
}

/* position stream to start of encrypted data and read it */
if (fseek(f, (long)sizeof(EnodeHeader), SEEK_SET) != 0) {
    perror("Error: fseek failed");
    fclose(f);
    return;
}
    uint8_t* data = malloc(data_size);
    if (!data) { fprintf(stderr, "Out of memory.\n"); fclose(f); return; }
    if (fread(data, 1, data_size, f) != data_size) {
        fprintf(stderr, "Error reading encrypted data.\n"); free(data); fclose(f); return;
    }
    fclose(f);

    // Decrypt
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, header.iv);
    AES_CBC_decrypt_buffer(&ctx, data, (uint32_t)data_size);
    
    // Unpad
    size_t unpadded_size = pkcs7_unpad(data, data_size);
    if (unpadded_size == 0) {
        fprintf(stderr, "Error: Wrong password or corrupt data (padding error).\n");
        free(data); return;
    }

    printf("Unpacking node '%s' from '%s' into current directory...\n", header.node_name, node_file);

    // --- Process the decrypted data in memory ---
    uint8_t* ptr = data;
    uint8_t* end = data + unpadded_size;
    char* file_data_buffer = NULL;
    size_t file_buffer_size = 0;

    while (ptr < end) {
        // Check for EOF marker
        if (end - ptr == sizeof(uint64_t)) {
            if (*((uint64_t*)ptr) == ENODE_EOF_MARKER) {
                printf("\nSuccessfully decrypted and unpacked node.\n");
                break; // Clean exit
            }
        }
        
        if (end - ptr < sizeof(EnodeFileHeader)) {
            fprintf(stderr, "Error: Ran out of data. Archive corrupt.\n");
            break;
        }

        EnodeFileHeader* file_header = (EnodeFileHeader*)ptr;
        ptr += sizeof(EnodeFileHeader);

        printf("  Extracting: %s\n", file_header->relative_path);

        if (S_ISDIR(file_header->mode)) {
            mkdir(file_header->relative_path, file_header->mode);
            chmod(file_header->relative_path, file_header->mode);
        } else if (S_ISLNK(file_header->mode)) {
            unlink(file_header->relative_path); // Remove if it exists
            if (symlink(file_header->link_target, file_header->relative_path) != 0) {
                fprintf(stderr, "  Warning: could not create symlink '%s' -> '%s'\n", file_header->relative_path, file_header->link_target);
            }
        } else if (S_ISREG(file_header->mode)) {
            if (file_header->data_size > 0) {
                if (ptr + file_header->data_size > end) {
                    fprintf(stderr, "  Error: Incomplete file data. Archive corrupt.\n");
                    break;
                }
                
                FILE* out_file = fopen(file_header->relative_path, "wb");
                if (!out_file) {
                    fprintf(stderr, "  Error: Could not create file %s\n", file_header->relative_path);
                    ptr += file_header->data_size; // Skip data
                    continue;
                }
                fwrite(ptr, 1, file_header->data_size, out_file);
                fclose(out_file);
                chmod(file_header->relative_path, file_header->mode);
                ptr += file_header->data_size;
            } else {
                FILE* out_file = fopen(file_header->relative_path, "wb");
                if (out_file) fclose(out_file);
                chmod(file_header->relative_path, file_header->mode);
            }
        }
    }
    
    free(data);
    free(file_data_buffer);
}

// --- NEW: Self-contained send (client) function ---
static void run_send(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: exodus send <path/to/.enode file> <http://ip:port>\n");
        return;
    }
    char* node_file = argv[2];
    char* target_url = argv[3];

    // We must pass a writable copy to basename()
    char node_file_copy[PATH_MAX];
    strncpy(node_file_copy, node_file, sizeof(node_file_copy) - 1);
    node_file_copy[sizeof(node_file_copy) - 1] = '\0';
    char* file_basename = basename(node_file_copy);
    
    // Parse URL
    char* host = strstr(target_url, "://");
    if (!host) {
        fprintf(stderr, "Error: Invalid URL format. Must be http://ip:port\n");
        return;
    }
    host += 3; // Skip "://"
    char* port_str = strrchr(host, ':');
    if (!port_str) {
        fprintf(stderr, "Error: Invalid URL format. Port is required.\n");
        return;
    }
    
    char host_copy[256];
    strncpy(host_copy, host, sizeof(host_copy) - 1);
    host_copy[port_str - host] = '\0'; // Split host
    int port = atoi(port_str + 1);

    // Open file
    int file_fd = open(node_file, O_RDONLY);
    if (file_fd == -1) {
        perror("Error opening file to send");
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        perror("Error getting file stats");
        close(file_fd);
        return;
    }
    size_t file_size = st.st_size;

    // Map file into memory
    void* file_data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
    if (file_data == MAP_FAILED) {
        perror("Error mapping file to memory");
        close(file_fd);
        return;
    }
    close(file_fd); // fd no longer needed after mmap

    // Resolve host
    struct hostent* server = gethostbyname(host_copy);
    if (server == NULL) {
        fprintf(stderr, "Error: Could not resolve host: %s\n", host_copy);
        munmap(file_data, file_size);
        return;
    }

    // Create socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Error creating socket");
        munmap(file_data, file_size);
        return;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    // Connect
    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error connecting to server");
        close(sock_fd);
        munmap(file_data, file_size);
        return;
    }
    
    printf("Sending pre-encrypted file '%s' (%ld bytes) to '%s:%d'...\n", file_basename, file_size, host_copy, port);
    printf("(Payload is already encrypted with AES-256. Network is plain HTTP.)\n");

    // Send HTTP POST request
    char header_buf[4096];
    int header_len = snprintf(header_buf, sizeof(header_buf),
        "POST / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "X-Filename: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        host_copy, port, file_basename, file_size
    );

    if (write(sock_fd, header_buf, header_len) != header_len) {
        perror("Error writing headers to socket");
    } else {
        // Send file data
        if (write(sock_fd, file_data, file_size) != (ssize_t)file_size) {
            perror("Error writing file data to socket");
        } else {
            // Read response
            char resp_buf[1024];
            ssize_t n = read(sock_fd, resp_buf, sizeof(resp_buf) - 1);
            if (n > 0) {
                resp_buf[n] = '\0';
                if (strstr(resp_buf, "HTTP/1.1 200 OK")) {
                    printf("\nSuccessfully sent file.\n");
                } else {
                    fprintf(stderr, "\nServer responded with an error:\n%s\n", resp_buf);
                }
            } else {
                perror("Error reading response from server");
            }
        }
    }

    // Cleanup
    close(sock_fd);
    munmap(file_data, file_size);
}

// --- NEW: Helper for server to find a header value ---
static char* find_header_value(const char* request, const char* header_name) {
    char* header_start = strcasestr(request, header_name);
    if (!header_start) return NULL;
    
    char* value_start = strchr(header_start, ':');
    if (!value_start) return NULL;
    value_start++; // Move past ':'
    
    while (*value_start == ' ') value_start++; // Skip whitespace
    
    char* value_end = strstr(value_start, "\r\n");
    if (!value_end) return NULL;
    
    static char value_buf[PATH_MAX]; // Not thread-safe, but fine for this
    size_t len = value_end - value_start;
    if (len >= sizeof(value_buf)) len = sizeof(value_buf) - 1;
    
    strncpy(value_buf, value_start, len);
    value_buf[len] = '\0';
    return value_buf;
}

// --- NEW: Self-contained expose-node (server) function ---
static void run_expose_node(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: exodus expose-node <target_directory> <port>\n");
        return;
    }
    char* target_dir = argv[2];
    int port = atoi(argv[3]);

    struct stat st_dir;
    if (stat(target_dir, &st_dir) != 0 || !S_ISDIR(st_dir.st_mode)) {
        fprintf(stderr, "Error: Target directory '%s' does not exist or is not a directory.\n", target_dir);
        return;
    }

    int server_fd, client_sock;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed"); return;
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt"); close(server_fd); return;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed"); close(server_fd); return;
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen"); close(server_fd); return;
    }

    printf("Exodus receiver live on port %d, saving to %s\n", port, target_dir);
    printf("(Receiving pre-encrypted files over plain HTTP.)\n");
    printf("Press Ctrl+C to stop.\n");

    char* http_buf = malloc(65536); // 64k buffer
    if (!http_buf) {
        fprintf(stderr, "Failed to allocate buffer.\n");
        close(server_fd); return;
    }

    while (1) {
        if ((client_sock = accept(server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
            perror("accept"); continue;
        }

        printf("\nAccepted connection from %s:%d\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));

        ssize_t total_read = 0;
        char* body_ptr = NULL;
        long content_length = 0;
        char* filename = NULL;

        while (total_read < 8192) { // Read max 8k for headers
            ssize_t n = read(client_sock, http_buf + total_read, 65536 - total_read);
            if (n <= 0) break;
            total_read += n;
            http_buf[total_read] = '\0';
            body_ptr = strstr(http_buf, "\r\n\r\n");
            if (body_ptr) {
                body_ptr += 4; // Move pointer to start of body
                break;
            }
        }

        if (!body_ptr) {
            fprintf(stderr, "Error: Failed to find request body.\n");
            close(client_sock); continue;
        }

        filename = find_header_value(http_buf, "X-Filename");
        char* cl_str = find_header_value(http_buf, "Content-Length");

        if (!filename || !cl_str) {
            fprintf(stderr, "Error: Missing X-Filename or Content-Length header.\n");
            const char* resp = "HTTP/1.1 400 Bad Request\r\n\r\nMissing headers.";
            write(client_sock, resp, strlen(resp));
            close(client_sock); continue;
        }
        
        content_length = atol(cl_str);
        char safe_filename[PATH_MAX];
        char* final_slash = strrchr(filename, '/');
        strncpy(safe_filename, final_slash ? final_slash + 1 : filename, sizeof(safe_filename) - 1);
        safe_filename[sizeof(safe_filename) - 1] = '\0'; // Ensure null termination

        char out_path[PATH_MAX * 2];
        snprintf(out_path, sizeof(out_path), "%s/%s", target_dir, safe_filename);
        
        printf("Receiving file: '%s' (%ld bytes) -> %s\n", safe_filename, content_length, out_path);

        FILE* out_file = fopen(out_path, "wb");
        if (!out_file) {
            perror("Error opening output file");
            const char* resp = "HTTP/1.1 500 Server Error\r\n\r\nCould not open file.";
            write(client_sock, resp, strlen(resp));
            close(client_sock); continue;
        }

        ssize_t body_in_buffer = total_read - (body_ptr - http_buf);
        if (body_in_buffer > 0) {
            fwrite(body_ptr, 1, body_in_buffer, out_file);
        }
        
        ssize_t remaining_to_read = content_length - body_in_buffer;
        while (remaining_to_read > 0) {
            ssize_t n = read(client_sock, http_buf, (remaining_to_read > 65536) ? 65536 : remaining_to_read);
            if (n <= 0) {
                fprintf(stderr, "Error: Connection closed before all data was received.\n");
                break;
            }
            fwrite(http_buf, 1, n, out_file);
            remaining_to_read -= n;
        }
        fclose(out_file);
        
        if (remaining_to_read == 0) {
            printf("OK: Received %s (%ld bytes)\n", safe_filename, content_length);
            const char* resp = "HTTP/1.1 200 OK\r\n\r\nFile received.";
            write(client_sock, resp, strlen(resp));
        } else {
             fprintf(stderr, "Error: File transfer incomplete.\n");
        }
        close(client_sock);
    }
    free(http_buf);
    close(server_fd);
}

static void run_pack_info(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: exodus pack-info <path/to/.enode file>\n");
        return;
    }
    char* node_file = argv[2];
    if (access(node_file, F_OK) == -1) {
        fprintf(stderr, "Error: File not found: %s\n", node_file);
        return;
    }

    FILE* f = fopen(node_file, "rb");
    if (!f) {
        perror("Error opening archive file");
        return;
    }

    EnodeHeader header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error: Could not read archive header. File may be corrupt or too small.\n");
        fclose(f);
        return;
    }
    fclose(f);

    if (memcmp(header.magic, ENODE_MAGIC, strlen(ENODE_MAGIC)) != 0) {
        fprintf(stderr, "Error: Not a valid .enode file (magic mismatch).\n");
        return;
    }

    // Helper macro to print a field or "[not set]" if it's empty
    #define print_field(label, value, len) \
        printf("%-12s: %s\n", label, (value[0] != '\0' && strnlen(value, len) > 0) ? value : "[not set]")

    printf("--- Archive Info for: %s ---\n", node_file);
    printf("%-12s: %s\n", "Node Name", header.node_name);
    print_field("Version", header.current_version, sizeof(header.current_version));
    print_field("Author", header.author, sizeof(header.author));
    print_field("Tag", header.tag, sizeof(header.tag));
    print_field("Description", header.desc, sizeof(header.desc));

    #undef print_field
}

static void print_detailed_usage(void) {
    fprintf(stderr, "usage: exodus <command> [<args>...]\n\n");

    fprintf(stderr, "Daemon & Service Management\n");
    fprintf(stderr, "  %-12s Start the Exodus cloud and query daemons\n", "start");
    fprintf(stderr, "  %-12s Stop the Exodus daemons\n", "stop");
    fprintf(stderr, "\n");

    fprintf(stderr, "Node Configuration & TUI\n");
    fprintf(stderr, "  %-12s Configure a node's auto-surveillance and settings\n", "node-conf");
    fprintf(stderr, "  %-12s Show uncommitted changes for a node\n", "node-status");
    fprintf(stderr, "  %-12s Open the TUI to browse and edit files in nodes with built in Text Editor\n", "node-edit");
    fprintf(stderr, "  %-12s Create, delete, move, or copy files/dirs within a node\n", "node-man");
    fprintf(stderr, "\n");

    fprintf(stderr, "Snapshot & History Management\n");
    fprintf(stderr, "  %-12s Create a permanent, versioned snapshot of a node\n", "commit");
    fprintf(stderr, "  %-12s Restore a node to a specific snapshot version (destructive)\n", "rebuild");
    fprintf(stderr, "  %-12s Restore a single file from a specific snapshot\n", "checkout");
    fprintf(stderr, "  %-12s Show changes between two snapshot versions\n", "diff");
    fprintf(stderr, "  %-12s View History of a node(what changed in a node e.g: Modified, Created, Moved or Deleted)\n", "history");
    fprintf(stderr, "  %-12s Show the commit history for the active subsection\n", "log");
    fprintf(stderr, "  %-12s Clear the uncommitted change history for a node\n", "clean");
    fprintf(stderr, "\n");

    fprintf(stderr, "Subsection (Branch) Management\n");
    fprintf(stderr, "  %-12s List all subsections for a node\n", "list-subs");
    fprintf(stderr, "  %-12s Create a new subsection\n", "add-subs");
    fprintf(stderr, "  %-12s Remove a subsection (cannot remove 'master' or active subsection)\n", "remove-subs");
    fprintf(stderr, "  %-12s Switch active subsection (rebuilds node to new subsection's HEAD)\n", "switch");
    fprintf(stderr, "  %-12s Promote (merge) a subsection into 'master' (Trunk)\n", "promote");
    fprintf(stderr, "\n");

    fprintf(stderr, "Archiving & Data Transfer\n");
    fprintf(stderr, "  %-12s Encrypt and archive a node into a .enode file\n", "pack");
    fprintf(stderr, "  %-12s Decrypt and extract a .enode file\n", "unpack");
    fprintf(stderr, "  %-12s Show metadata from an encrypted .enode file header\n", "pack-info");
    fprintf(stderr, "  %-12s Send a .enode file to a remote receiver\n", "send");
    fprintf(stderr, "  %-12s Start a receiver to accept .enode files\n", "expose-node");
    fprintf(stderr, "\n");

    fprintf(stderr, "Node Management\n");
    fprintf(stderr, "  %-12s Adds your project/directory as a new node\n", "add-node");
    fprintf(stderr, "  %-12s List all added nodes\n", "list-nodes");
    fprintf(stderr, "  %-12s Deletes a node and remove it from the config\n", "remove-node");
    fprintf(stderr, "  %-12s View recent events of a node\n", "view-node");
    fprintf(stderr, "  %-12s Start real-time surveillance on an inactive node\n", "activate");
    fprintf(stderr, "  %-12s Stop real-time surveillance on an active node\n", "deactivate");
    fprintf(stderr, "  %-12s Set metadata (author, tag, desc) for a node\n", "attr-node");
    fprintf(stderr, "  %-12s View metadata for a node\n", "info-node");
    fprintf(stderr, "  %-12s Find nodes by author or tag\n", "search-attr");
    fprintf(stderr, "  %-12s Find a file/folder, or pin it with 'look <file> --pin <name>'\n", "look");
    fprintf(stderr, "  %-12s Remove a pinned shortcut\n", "unpin");
    fprintf(stderr, "\n");
    
    fprintf(stderr, "File Indexing\n");
    fprintf(stderr, "  %-12s Upload a file for word indexing\n", "upload");
    fprintf(stderr, "  %-12s Find a word in the last indexed file\n", "find");
    fprintf(stderr, "  %-12s Find and replace a word in the last indexed file\n", "change");
    fprintf(stderr, "  %-12s Get the word count of the last indexed file\n", "wc");
    fprintf(stderr, "  %-12s Get the line count of the last indexed file\n", "wl");
    fprintf(stderr, "  %-12s Get the non-space character count of the last indexed file\n", "cc");
    fprintf(stderr, "\n");

    fprintf(stderr, "Unit & Network Synchronization\n");
    fprintf(stderr, "  %-12s List all connected Units on the network\n", "unit-list");
    fprintf(stderr, "  %-12s List all nodes on a specific remote Unit\n", "view-unit");
    fprintf(stderr, "  %-12s Sync history with a remote node (e.g., sync <unit> <remote-node> <local-node>)\n", "sync");
    fprintf(stderr, "  %-12s Set this machine's name or coordinator (--name, --coord)\n", "unit-set");
    fprintf(stderr, "  %-12s -For Debugging- View the signal daemon's local node cache\n", "view-cache");
    fprintf(stderr, "  %-12s Push a node's full data to a remote unit's designated storage\n", "push");
    fprintf(stderr, "\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_detailed_usage();
        return 1;
    }

    if (strcmp(argv[1], "start") == 0) {
        start_daemons();
    } else if (strcmp(argv[1], "stop") == 0) {
        stop_daemons();
    
    } else if (strcmp(argv[1], "unit-set") == 0) {
        run_unit_set(argc, argv);    
    }else if (strcmp(argv[1], "pack") == 0) {
        run_pack(argc, argv);

    
    } else if (strcmp(argv[1], "unpack") == 0) {
        run_unpack(argc, argv);
        
    
    }else if (strcmp(argv[1], "pack-info") == 0) {
        run_pack_info(argc, argv);
    
    } else if (strcmp(argv[1], "send") == 0) {
        run_send(argc, argv);

    
    } else if (strcmp(argv[1], "expose-node") == 0) {
        run_expose_node(argc, argv);

    } else if (strcmp(argv[1], "node-conf") == 0) {
        run_node_conf(argc, argv);

    } else if (strcmp(argv[1], "connect") == 0) {
        run_connect(argc, argv);
    }else if (strcmp(argv[1], "clean") == 0) {
        run_clean_history(argc, argv);

    } else if (strcmp(argv[1], "node-edit") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Usage: exodus node-edit\n");
            return 1;
        }
        run_node_edit();
    
    } else if (strcmp(argv[1], "list-subs") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: exodus list-subs <node_name>\n");
            return 1;
        }
        char node_path[PATH_MAX];
        if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) return 1;

        char subsec_dir_path[PATH_MAX];
        snprintf(subsec_dir_path, sizeof(subsec_dir_path), "%s/.log/subsections", node_path);
        
        DIR* d = opendir(subsec_dir_path);
        if (!d) {
            // If dir doesn't exist, 'master' is the only one
            printf("Subsections for node '%s':\n", argv[2]);
            printf("* master (Trunk)\n");
            return 0;
        }

        char current_subsec[MAX_NODE_NAME_LEN];
        get_current_subsection(node_path, current_subsec, sizeof(current_subsec));

        printf("Subsections for node '%s':\n", argv[2]);
        
        // Always list master
        if (strcmp("master", current_subsec) == 0) {
            printf("* master (Trunk)\n");
        } else {
            printf("  master (Trunk)\n");
        }

        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            
            // Check if it ends in .subsec
            char* ext = strrchr(dir->d_name, '.');
            if (ext && strcmp(ext, ".subsec") == 0) {
                // Get name without extension
                char sub_name[NAME_MAX + 1];
                size_t name_len = ext - dir->d_name;
                strncpy(sub_name, dir->d_name, name_len);
                sub_name[name_len] = '\0';

                if (strcmp(sub_name, current_subsec) == 0) {
                    printf("* %s\n", sub_name);
                } else {
                    printf("  %s\n", sub_name);
                }
            }
        }
        closedir(d);

    } else if (strcmp(argv[1], "add-subs") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: exodus add-subs <node_name> <new_subsection_name>\n");
            return 1;
        }
        char* node_name = argv[2];
        char* new_sub_name = argv[3];
        char node_path[PATH_MAX];
        if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) return 1;

        if (strcmp(new_sub_name, "master") == 0) {
            fprintf(stderr, "Error: Cannot create subsection 'master'. It is reserved.\n");
            return 1;
        }

        printf("Sending 'add-subs' command for new subsection '%s'...\n", new_sub_name);
        int result = cortez_ipc_send("./exodus_snapshot",
                                     CORTEZ_TYPE_STRING, "add-subs",
                                     CORTEZ_TYPE_STRING, node_name,
                                     CORTEZ_TYPE_STRING, node_path,
                                     CORTEZ_TYPE_STRING, "master",     // Dummy subsection
                                     CORTEZ_TYPE_STRING, new_sub_name, // arg1
                                     0);
        if (result != 0) {
             fprintf(stderr, "Failed to start add-subs process.\n");
        }

    }else if (strcmp(argv[1], "switch") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: exodus switch <node_name> <subsection_name>\n");
            return 1;
        }
        char* node_name = argv[2];
        char* new_sub_name = argv[3];
        char node_path[PATH_MAX];
        if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) return 1;

        // 1. Check if subsection exists (unless it's "master")
        if (strcmp(new_sub_name, "master") != 0) {
            char subsec_file_path[PATH_MAX];
            snprintf(subsec_file_path, sizeof(subsec_file_path), "%s/.log/subsections/%s.subsec", node_path, new_sub_name);
            if (access(subsec_file_path, F_OK) != 0) {
                fprintf(stderr, "Error: Subsection '%s' does not exist.\n", new_sub_name);
                fprintf(stderr, "Use 'exodus add-subs %s %s' to create it.\n", node_name, new_sub_name);
                return 1;
            }
        }
        
        // 2. Get current subsection to check if we're already on it
        char current_subsec_name[MAX_NODE_NAME_LEN];
        get_current_subsection(node_path, current_subsec_name, sizeof(current_subsec_name));
        if (strcmp(new_sub_name, current_subsec_name) == 0) {
            printf("Already on subsection '%s'.\n", new_sub_name);
            return 0;
        }

        // 3. Check for uncommitted changes
        char history_path[PATH_MAX];
        snprintf(history_path, sizeof(history_path), "%s/.log/history.json", node_path);
        struct stat st_hist = {0};
        if (stat(history_path, &st_hist) == 0 && st_hist.st_size > 5) {
            fprintf(stderr, "Error: Cannot switch subsection with uncommitted changes.\n");
            fprintf(stderr, "Please run 'exodus commit' or 'exodus node-status' to review changes.\n");
            return 1;
        }
        // 4. Get the commit hash for the CURRENT subsection (the "old" one)
        char old_commit_hash[PATH_MAX]; // Using PATH_MAX for safety
        if (get_commit_hash_for_subsection(node_path, current_subsec_name, old_commit_hash, sizeof(old_commit_hash)) != 0) {
            // This means the head file is missing or unreadable.
            fprintf(stderr, "Warning: Could not read HEAD for old subsection '%s'. Rebuild will treat it as empty.\n", current_subsec_name);
            old_commit_hash[0] = '\0';
        }

        // 5. Trigger a "rebuild" to the new subsection's HEAD
        printf("Switched to subsection '%s'.\n", new_sub_name);
        printf("Rebuilding working directory to match subsection HEAD...\n");
        
        int result = cortez_ipc_send("./exodus_snapshot",
                                     CORTEZ_TYPE_STRING, "rebuild",
                                     CORTEZ_TYPE_STRING, node_name,
                                     CORTEZ_TYPE_STRING, node_path,
                                     CORTEZ_TYPE_STRING, new_sub_name,      // Target subsection
                                     CORTEZ_TYPE_STRING, "LATEST_HEAD",   // arg1 (target tag)
                                     CORTEZ_TYPE_STRING, old_commit_hash, // arg2 (old commit)
                                     0);
        
        if (result == 0) {
            // 6. Write the new current subsection *after* successful rebuild
            char subsec_file_path[PATH_MAX];
            snprintf(subsec_file_path, sizeof(subsec_file_path), "%s/.log/CURRENT_SUBSECTION", node_path);
            FILE* f = fopen(subsec_file_path, "w");
            if (!f) {
                perror("CRITICAL Error: Rebuild succeeded but failed to update CURRENT_SUBSECTION file");
            } else {
                fprintf(f, "%s\n", new_sub_name);
                fclose(f);
            }
            
            printf("Rebuild complete. Working directory now matches subsection '%s'.\n", new_sub_name);
        } else {
            fprintf(stderr, "Fatal: Failed to rebuild to new subsection HEAD. Switch aborted.\n");
        }
    } else if (strcmp(argv[1], "promote") == 0) {
        if (argc != 5) {
            fprintf(stderr, "Usage: exodus promote <node_name> <subsection_name> <message>\n");
            return 1;
        }
        char* node_name = argv[2];
        char* sub_to_promote = argv[3];
        char* message = argv[4];
        char node_path[PATH_MAX];
        if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) return 1;

        if (strcmp(sub_to_promote, "master") == 0) {
            fprintf(stderr, "Error: Cannot promote 'master' onto itself.\n");
            return 1;
        }

        // --- Safety Checks ---
        char current_subsec_name[MAX_NODE_NAME_LEN];
        get_current_subsection(node_path, current_subsec_name, sizeof(current_subsec_name));
        
        if (strcmp(sub_to_promote, current_subsec_name) == 0) {
            fprintf(stderr, "Error: Cannot promote the subsection you are currently on.\n");
            fprintf(stderr, "Please switch to 'master' first: exodus switch %s master\n", node_name);
            return 1;
        }
        if (strcmp("master", current_subsec_name) != 0) {
             fprintf(stderr, "Warning: You are promoting *from* subsection '%s', not from 'master'.\n", current_subsec_name);
             fprintf(stderr, "Promotions should typically be done *while on* the 'master' (Trunk) subsection.\n");
             printf("Continue anyway? (y/N) ");
             char confirm[10];
             if (fgets(confirm, sizeof(confirm), stdin) == NULL || (confirm[0] != 'y' && confirm[0] != 'Y')) {
                printf("Promotion cancelled.\n");
                return 1;
             }
        }
        
        const char* delete_flag = "--keep"; 
        printf("Delete subsection '%s' after successful promotion? (y/N) ", sub_to_promote);
        char confirm_del[10];
        if (fgets(confirm_del, sizeof(confirm_del), stdin) != NULL && (confirm_del[0] == 'y' || confirm_del[0] == 'Y')) {
            delete_flag = "--delete";
        }

        printf("Sending 'promote' command for subsection '%s'...\n", sub_to_promote);
        

        int result = cortez_ipc_send("./exodus_snapshot",
                                     CORTEZ_TYPE_STRING, "promote",
                                     CORTEZ_TYPE_STRING, node_name,
                                     CORTEZ_TYPE_STRING, node_path,
                                     CORTEZ_TYPE_STRING, sub_to_promote, 
                                     CORTEZ_TYPE_STRING, message,  
                                     CORTEZ_TYPE_STRING, delete_flag, 
                                     0);
        if (result != 0) {
             fprintf(stderr, "Failed to start promote process.\n");
        } else {
            printf("Promotion process complete. Check logs for details.\n");
        }
    
    }else if (strcmp(argv[1], "log") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: exodus log <node_name>\n");
            return 1;
        }
        char* node_name = argv[2];
        char node_path[PATH_MAX];
        if (find_node_path_in_config(node_name, node_path, sizeof(node_path)) != 0) return 1;

        char subsection_name[MAX_NODE_NAME_LEN];
        get_current_subsection(node_path, subsection_name, sizeof(subsection_name));

        printf("Displaying commit log for subsection '%s':\n\n", subsection_name);

        int result = cortez_ipc_send("./exodus_snapshot",
                                     CORTEZ_TYPE_STRING, "log",
                                     CORTEZ_TYPE_STRING, node_name,
                                     CORTEZ_TYPE_STRING, node_path,
                                     CORTEZ_TYPE_STRING, subsection_name,
                                     0); // No other args needed
        if (result != 0) {
             fprintf(stderr, "Failed to start log process.\n");
        }
    } else if (strcmp(argv[1], "commit") == 0) {
    if (argc != 4) {
        fprintf(stderr, "Usage: exodus commit <node_name> <version_tag>\n");
        return 1;
    }
    char node_path[PATH_MAX];
    if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) {
        return 1; 
    }

    char subsection_name[MAX_NODE_NAME_LEN];
    get_current_subsection(node_path, subsection_name, sizeof(subsection_name));
    printf("Sending 'commit' for node '%s' (subsection '%s') with tag '%s'...\n", argv[2], subsection_name, argv[3]);



    int result = cortez_ipc_send("./exodus_snapshot",
                                 CORTEZ_TYPE_STRING, "commit",
                                 CORTEZ_TYPE_STRING, argv[2],     // node_name
                                 CORTEZ_TYPE_STRING, node_path,   // node_path
                                 CORTEZ_TYPE_STRING, subsection_name, // NEW ARG
                                 CORTEZ_TYPE_STRING, argv[3],     // version_tag
                                 0); 


    if (result == 0) {
        printf("Snapshot process complete. Check logs for details.\n");

        if (strcmp(subsection_name, "master") == 0) {
            printf("Updating node's current version to '%s'...\n", argv[3]);
            if (update_node_current_version(argv[2], argv[3]) != 0) {
                fprintf(stderr, "Warning: Snapshot was created, but failed to update 'current_version' in nodewatch.json.\n");
            }
        } else {
            printf("Commit created on subsection '%s'. 'current_version' in nodewatch.json not updated.\n", subsection_name);
        }


    } else {
        fprintf(stderr, "Failed to start snapshot process. Is 'exodus_snapshot' in the same directory?\n");
    }
    } else if (strcmp(argv[1], "rebuild") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Usage: exodus rebuild <node_name> <version_tag>\n");
            return 1;
        }
        char node_path[PATH_MAX];
        if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) {
            return 1;
        }

        char subsection_name[MAX_NODE_NAME_LEN];
        get_current_subsection(node_path, subsection_name, sizeof(subsection_name));
        printf("Sending 'rebuild' for node '%s' (subsection '%s') to version '%s'...\n", argv[2], subsection_name, argv[3]);
        printf("Sending 'rebuild' command for node '%s' to version '%s'...\n", argv[2], argv[3]);

        printf("WARNING: This will delete all current data in the node. Continue? (y/N) ");
        char confirm[10];
        if (fgets(confirm, sizeof(confirm), stdin) == NULL || (confirm[0] != 'y' && confirm[0] != 'Y')) {
            printf("Rebuild cancelled.\n");
            return 1;
        }
        
        int result = cortez_ipc_send("./exodus_snapshot",
                                 CORTEZ_TYPE_STRING, "rebuild",
                                 CORTEZ_TYPE_STRING, argv[2],
                                 CORTEZ_TYPE_STRING, node_path,
                                 CORTEZ_TYPE_STRING, subsection_name, // NEW ARG
                                 CORTEZ_TYPE_STRING, argv[3],
                                 0);// 0 terminates the argument list
        if (result == 0) {
            printf("Rebuild process complete. Check logs for details.\n");
        } else {
            fprintf(stderr, "Failed to start rebuild process. Is 'exodus_snapshot' in the same directory?\n");
        }
    }else if (strcmp(argv[1], "diff") == 0) {
    if (argc != 5) {
        fprintf(stderr, "Usage: exodus diff <node_name> <version_tag_1> <version_tag_2>\n");
        return 1;
    }
    char node_path[PATH_MAX];
    if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) {
        return 1;
    }

    char subsection_name[MAX_NODE_NAME_LEN];
    get_current_subsection(node_path, subsection_name, sizeof(subsection_name));
    printf("Generating diff for node '%s' (subsection '%s') between '%s' and '%s'...\n", argv[2], subsection_name, argv[3], argv[4]);
    printf("Generating diff for node '%s' between '%s' and '%s'...\n", argv[2], argv[3], argv[4]);
    int result = cortez_ipc_send("./exodus_snapshot",
                                 CORTEZ_TYPE_STRING, "diff",
                                 CORTEZ_TYPE_STRING, argv[2],
                                 CORTEZ_TYPE_STRING, node_path,
                                 CORTEZ_TYPE_STRING, subsection_name, // NEW ARG
                                 CORTEZ_TYPE_STRING, argv[3], // v1
                                 CORTEZ_TYPE_STRING, argv[4], // v2
                                 0);
    if (result != 0) {
        fprintf(stderr, "Failed to start diff process. Is 'exodus_snapshot' in the same directory?\n");
    }

    }else if (strcmp(argv[1], "checkout") == 0) {
    if (argc != 5) {
        fprintf(stderr, "Usage: exodus checkout <node_name> <version_tag> <file/path/to/restore>\n");
        return 1;
    }
    char node_path[PATH_MAX];
    if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) {
        return 1;
    }

    char subsection_name[MAX_NODE_NAME_LEN];
    get_current_subsection(node_path, subsection_name, sizeof(subsection_name));
    printf("Restoring '%s' in node '%s' (subsection '%s') to version '%s'...\n", argv[4], argv[2], subsection_name, argv[3]);
    printf("Restoring '%s' in node '%s' to version '%s'...\n", argv[4], argv[2], argv[3]);
    int result = cortez_ipc_send("./exodus_snapshot",
                                 CORTEZ_TYPE_STRING, "checkout",
                                 CORTEZ_TYPE_STRING, argv[2],
                                 CORTEZ_TYPE_STRING, node_path,
                                 CORTEZ_TYPE_STRING, subsection_name, // NEW ARG
                                 CORTEZ_TYPE_STRING, argv[3], // version
                                 CORTEZ_TYPE_STRING, argv[4], // file_path
                                 0);
    if (result == 0) {
        printf("File restore process complete. Check logs for details.\n");
    } else {
        fprintf(stderr, "Failed to start checkout process. Is 'exodus_snapshot' in the same directory?\n");
    }

    } else if (strcmp(argv[1], "history") == 0) {
        if (argc < 3 || argc > 4) {
            fprintf(stderr, "Usage: exodus history <node name> [--V]\n");
            return 1;
        }
        char node_path[PATH_MAX];
        if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) {
            return 1;
        }
        

        char subsection_name[MAX_NODE_NAME_LEN];
        get_current_subsection(node_path, subsection_name, sizeof(subsection_name));

        
        char data_path[PATH_MAX];
        const char* title;
        
        if (argc == 4 && strcmp(argv[3], "--V") == 0) {

            if (strcmp(subsection_name, "master") == 0) {
                snprintf(data_path, sizeof(data_path), "%s/.log/TRUNK.versions.json", node_path);
            } else {
                snprintf(data_path, sizeof(data_path), "%s/.log/subsections/%s.versions.json", node_path, subsection_name);
            }
            title = "Persistent Commit History (Versions)";

        } else if (argc == 3) {
            snprintf(data_path, sizeof(data_path), "%s/.log/history.json", node_path);
            title = "Recent Activity (Uncommitted Changes)";
        } else {
             fprintf(stderr, "Usage: exodus history <node name> [--V]\n");
             return 1;
        }

        FILE* f = fopen(data_path, "r");
        if (!f) {

            if (argc == 4) {
                 fprintf(stderr, "Error: Could not open data file. Does subsection '%s' have any commits?\n", subsection_name);
                 fprintf(stderr, "Path: %s\n", data_path);
            } else {
                fprintf(stderr, "Error: Could not open data file at %s.\n", data_path);
            }

            return 1;
        }
        

        printf("--- %s for Node '%s' (Subsection: '%s') ---\n", title, argv[2], subsection_name);

        
        int c;
        while ((c = fgetc(f)) != EOF) {
            putchar(c);
        }
        fclose(f);
    }else if (strcmp(argv[1], "node-status") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: exodus node-status <node name>\n");
            return 1;
        }
        char node_path[PATH_MAX];
        if (find_node_path_in_config(argv[2], node_path, sizeof(node_path)) != 0) {
            return 1;
        }
        char history_path[PATH_MAX];
        snprintf(history_path, sizeof(history_path), "%s/.log/history.json", node_path);
        FILE* f = fopen(history_path, "r");
        if (!f) {
            fprintf(stderr, "Error: Could not open history file at %s.\n", history_path);
            return 1;
        }
        
        fseek(f, 0, SEEK_END);
        long length = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buffer = malloc(length + 1);
        if (!buffer) { fclose(f); return 1; }
        fread(buffer, 1, length, f);
        buffer[length] = '\0';
        fclose(f);
        char error_buf[256];
        ctz_json_value* root = ctz_json_parse(buffer, error_buf, sizeof(error_buf));
        free(buffer);
        if (!root || ctz_json_get_type(root) != CTZ_JSON_ARRAY) {
            fprintf(stderr, "Node is clean or history.json is corrupt.\n");
            if (root) ctz_json_free(root);
            return 0;
        }
        size_t change_count = ctz_json_get_array_size(root);
        if (change_count == 0) {
            printf("Node '%s' is clean. No uncommitted changes.\n", argv[2]);
            ctz_json_free(root); // Free root here
            return 0;
        }


        FileStatusNode* status_head = NULL;

        for (size_t i = 0; i < change_count; i++) {
            ctz_json_value* item = ctz_json_get_array_element(root, i);
            ctz_json_value* event_val = ctz_json_find_object_value(item, "event");
            ctz_json_value* name_val = ctz_json_find_object_value(item, "name");
            if (!event_val || !name_val) continue;

            const char* event_str = ctz_json_get_string(event_val);
            const char* name_str = ctz_json_get_string(name_val);

            FileStatusNode* node = find_or_create_status(&status_head, name_str);
            if (!node) continue; // Out of memory

            if (strcmp(event_str, "Created") == 0) {

                if (node->state != NET_STATE_CREATED) {
                    node->state = NET_STATE_CREATED;
                    node->modify_count = 0;
                }
            } else if (strcmp(event_str, "Modified") == 0) {
                if (node->state == NET_STATE_NONE) {
                    // First change to a committed file
                    node->state = NET_STATE_MODIFIED;
                }

                if (node->state != NET_STATE_DELETED) {
                    node->modify_count++;
                }
            } else if (strcmp(event_str, "Deleted") == 0) {
                if (node->state == NET_STATE_CREATED) {
                    // Created and then deleted in this session. Net change is nothing.
                    node->state = NET_STATE_TEMP_DELETED;
                } else {
                    // Was a committed file (NONE) or a modified file.
                    node->state = NET_STATE_DELETED;
                }
                node->modify_count = 0; // Reset count on deletion
            }else if (strcmp(event_str, "Moved") == 0) {
                // A "Move" is a Delete from the 'from' path and a Move/Create at the 'to' path
                ctz_json_value* changes_val = ctz_json_find_object_value(item, "changes");
                if (!changes_val) continue; // Corrupt event
                
                ctz_json_value* from_val = ctz_json_find_object_value(changes_val, "from");
                if (!from_val) continue; // Corrupt event
                
                const char* from_path_str = ctz_json_get_string(from_val);
                if (!from_path_str) continue;

                // 1. Process the 'from' path: Mark it as deleted
                FileStatusNode* from_node = find_or_create_status(&status_head, from_path_str);
                if (from_node) {
                    if (from_node->state == NET_STATE_CREATED || from_node->state == NET_STATE_MOVED) {
                        // Was created/moved, then moved again; net change is nothing for this path
                        from_node->state = NET_STATE_TEMP_DELETED;
                    } else {
                        // Was a committed file that got moved
                        from_node->state = NET_STATE_DELETED;
                    }
                    from_node->modify_count = 0;
                }

                // 2. Process the 'to' path ('node' / 'name_str'): Mark it as moved
                node->state = NET_STATE_MOVED;
                strncpy(node->from_path, from_path_str, sizeof(node->from_path) - 1);
                node->modify_count = 0; // Reset count, this is a new "base" state
            }

        }
        
        // 2. Print the summarized results
        printf("Uncommitted changes for node '%s':\n", argv[2]);
        printf("(Changes since last commit)\n\n");
        const char* RED = "\033[0;31m";
        const char* GREEN = "\033[0;32m";
        const char* YELLOW = "\033[0;33m";
        const char* CYAN = "\033[0;36m";
        const char* RESET = "\033[0m";
        

        FileStatusNode* current = status_head;
        int changes_found = 0;

        // We iterate the list again to print, which may be in reverse order
        // but ensures all unique files are processed.
        while (current) {
            switch (current->state) {
                case NET_STATE_CREATED:
                    printf("  %s%-12s%s %s", GREEN, "[Created]", RESET, current->path);
                    if (current->modify_count > 0) {
                        printf(" (Modified %d time(s))\n", current->modify_count);
                    } else {
                        printf("\n");
                    }
                    changes_found++;
                    break;
                case NET_STATE_MODIFIED:
                    printf("  %s%-12s%s %s (Modified %d time(s))\n", YELLOW, "[Modified]", RESET, current->path, current->modify_count);
                    changes_found++;
                    break;
                case NET_STATE_DELETED:
                    printf("  %s%-12s%s %s\n", RED, "[Deleted]", RESET, current->path);
                    changes_found++;
                    break;
                case NET_STATE_MOVED:
                    printf("  %s%-12s%s %s", CYAN, "[Moved]", RESET, current->path);
                    printf(" [From]: %s", current->from_path);
                    if (current->modify_count > 0) {
                        printf(" (Modified %d time(s))\n", current->modify_count);
                    } else {
                        printf("\n");
                    }
                    changes_found++;
                    break;
                case NET_STATE_TEMP_DELETED:
                case NET_STATE_NONE:    
                default:

                    break;
            }
            current = current->next;
        }

        if (changes_found == 0) {
            printf("Node '%s' is clean. (All changes were temporary, e.g., .swp files)\n", argv[2]);
        }

        // 3. Free the linked list
        current = status_head;
        while (current) {
            FileStatusNode* next = current->next;
            free(current);
            current = next;
        }

        ctz_json_free(root);
    } else if (strcmp(argv[1], "look") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: exodus look <file/folder> [--pin <pin_name>]\n");
            return 1;
        }
        cortez_mesh_t* mesh = cortez_mesh_init("exodus_client", NULL);
        if (!mesh) {
            fprintf(stderr, "Could not connect to exodus mesh. Are daemons running?\n");
            return 1;
        }
        usleep(200000); 
        pid_t target_pid = find_query_daemon_pid();
        if (target_pid == 0) {
             fprintf(stderr, "Could not find the query daemon.\n");
             cortez_mesh_shutdown(mesh);
             return 1;
        }
        int sent_ok = 0;
        for (int i = 0; i < 5; i++) {
            cortez_write_handle_t* h = NULL;
            if (argc == 5 && strcmp(argv[3], "--pin") == 0) {
                uint32_t payload_size = sizeof(pin_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if(h) {
                    size_t part1_size;
                    pin_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->item_name, argv[2], MAX_PATH_LEN - 1);
                    strncpy(req->pin_name, argv[4], MAX_NODE_NAME_LEN - 1);
                    cortez_mesh_commit_send_zc(h, MSG_PIN_ITEM);
                    sent_ok = 1;
                }
            } else { // Handle simple look
                uint32_t payload_size = sizeof(lookup_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if(h) {
                    size_t part1_size;
                    lookup_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->item_name, argv[2], MAX_PATH_LEN - 1);
                    cortez_mesh_commit_send_zc(h, MSG_LOOKUP_ITEM);
                    sent_ok = 1;
                }
            }
            if (sent_ok) break;
            if (i < 4) {
                 printf("Query daemon not ready, retrying... (%d/5)\n", i + 1);
                 sleep(2);
            }
        }
        if (sent_ok) {
            printf("Waiting for response...\n");
            cortez_msg_t* msg = cortez_mesh_read(mesh, 10000);
            if(msg) {
                if(cortez_msg_type(msg) == MSG_OPERATION_ACK) {
                    const ack_t* ack = cortez_msg_payload(msg);
                    printf("Result: %s (%s)\n", ack->success ? "Success" : "Failure", ack->details);
                } else if (cortez_msg_type(msg) == MSG_LOOKUP_RESPONSE) {
                    const list_resp_t* resp = cortez_msg_payload(msg);
                    printf("--- Lookup Results ---\n");
                    const char* current = resp->data;
                    for (int i = 0; i < resp->item_count; i++) {
                        printf("%s", current);
                        current += strlen(current) + 1;
                    }
                    if (resp->item_count == 0 && strlen(resp->data) > 0) {
                        printf("%s", resp->data);
                    }
                }
                cortez_mesh_msg_release(mesh, msg);
            } else {
                printf("No response from daemon (timeout).\n");
            }
        } else {
             fprintf(stderr, "Failed to send message to query daemon after 5 attempts.\n");
        }
        cortez_mesh_shutdown(mesh);
    } else if (strcmp(argv[1], "unpin") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Usage: exodus unpin <pin_name>\n");
            return 1;
        }
        cortez_mesh_t* mesh = cortez_mesh_init("exodus_client", NULL);
        if (!mesh) {
            fprintf(stderr, "Could not connect to exodus mesh. Are daemons running?\n");
            return 1;
        }
        usleep(200000);
        pid_t target_pid = find_query_daemon_pid();
        if (target_pid == 0) {
            fprintf(stderr, "Could not find the query daemon.\n");
            cortez_mesh_shutdown(mesh);
            return 1;
        }
        int sent_ok = 0;
        for (int i = 0; i < 5; i++) {
            uint32_t payload_size = sizeof(unpin_req_t);
            cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
            if(h) {
                size_t part1_size;
                unpin_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                strncpy(req->pin_name, argv[2], MAX_NODE_NAME_LEN - 1);
                cortez_mesh_commit_send_zc(h, MSG_UNPIN_ITEM);
                sent_ok = 1;
                break;
            }
            if (i < 4) {
                printf("Query daemon not ready, retrying... (%d/5)\n", i + 1);
                usleep(200000);
            }
        }
        if (sent_ok) {
            printf("Waiting for response...\n");
            cortez_msg_t* msg = cortez_mesh_read(mesh, 10000);
            if(msg) {
                if(cortez_msg_type(msg) == MSG_OPERATION_ACK) {
                    const ack_t* ack = cortez_msg_payload(msg);
                    printf("Result: %s (%s)\n", ack->success ? "Success" : "Failure", ack->details);
                }
                cortez_mesh_msg_release(mesh, msg);
            } else {
                printf("No response from daemon (timeout).\n");
            }
        } else {
            fprintf(stderr, "Failed to send message to query daemon after 5 attempts.\n");
        }
        cortez_mesh_shutdown(mesh);
    }else if (strcmp(argv[1], "push") == 0) {

        if (argc != 4) {
            fprintf(stderr, "Usage: exodus push <node_name> <target_unit>\n");
            return 1;
        }

        cortez_mesh_t* mesh = cortez_mesh_init("exodus_client", NULL);

        if (!mesh) return 1;
        pid_t target_pid = find_query_daemon_pid();

        if (target_pid > 0) {

            run_push_node(mesh, target_pid, argv[2], argv[3]);

        } else {

            fprintf(stderr, "Daemon not running.\n");
        }

        cortez_mesh_shutdown(mesh);
    } else {
        cortez_mesh_t* mesh = cortez_mesh_init("exodus_client", NULL);
        if (!mesh) {
            fprintf(stderr, "Could not connect to exodus mesh. Are daemons running?\n");
            return 1;
        }
        printf("Discovering daemons on the mesh...\n");
        sleep(1);
        pid_t target_pid = find_query_daemon_pid();
        if (target_pid == 0) {
             fprintf(stderr, "Could not find the query daemon. Make sure it's running.\n");
             cortez_mesh_shutdown(mesh);
             return 1;
        }
        printf("Found query daemon with PID: %d\n", target_pid);
                if (strcmp(argv[1], "unit-list") == 0 && argc == 2) {
            run_unit_list(mesh, target_pid);
        } else if (strcmp(argv[1], "view-unit") == 0 && argc == 3) {
            run_view_unit(mesh, target_pid, argv[2]);
        } else if (strcmp(argv[1], "sync") == 0 && argc == 5) {
            run_sync_node(mesh, target_pid, argv[2], argv[3], argv[4]);
        } else if (strcmp(argv[1], "view-cache") == 0 && argc == 2) {
            int sent_ok = 0;
            for (int i = 0; i < 5; i++) {
                cortez_write_handle_t* h = cortez_mesh_begin_send_zc(mesh, target_pid, 1); // Dummy 1-byte payload
                if (h) {
                    cortez_mesh_commit_send_zc(h, MSG_SIG_REQUEST_VIEW_CACHE);
                    sent_ok = 1;
                    break;
                }
                usleep(100000);
            }
            
            if (!sent_ok) {
                fprintf(stderr, "Failed to send VIEW_CACHE request.\n");
            } else {
                printf("Waiting for response [10s]...\n");
                cortez_msg_t* msg = cortez_mesh_read(mesh, 10000);
                if (msg) {
                    if (cortez_msg_type(msg) == MSG_SIG_RESPONSE_VIEW_CACHE) {
                        const char* json_body = (const char*)cortez_msg_payload(msg);
                        printf("--- Signal Daemon Local Node Cache ---\n");
                        // The payload is the raw JSON string
                        printf("%s\n", json_body ? json_body : "(null)");
                    } else if (cortez_msg_type(msg) == MSG_OPERATION_ACK) {
                        const ack_t* ack = cortez_msg_payload(msg);
                        fprintf(stderr, "Error from daemon: %s\n", ack->details);
                    } else {
                        fprintf(stderr, "Received unexpected response type: %d\n", cortez_msg_type(msg));
                    }
                    cortez_mesh_msg_release(mesh, msg);
                } else {
                    printf("No response from daemon (timeout).\n");
                }
            }

        }else if (strcmp(argv[1], "node-man") == 0) {
        run_node_man(argc, argv);
        } else{
        int sent_ok = 0;
        int max_retries = 5;
        for (int i = 0; i < max_retries; i++) {
            cortez_write_handle_t* h = NULL;
        if (strcmp(argv[1], "upload") == 0 && argc == 3) {
                char absolute_path[PATH_MAX];
                if (realpath(argv[2], absolute_path) == NULL) {
                    perror("Error resolving file path");
                    fprintf(stderr, "Please ensure the file '%s' exists and you have permission to read it.\n", argv[2]);
                    cortez_mesh_shutdown(mesh);
                    return 1; 
                }
                uint32_t payload_size = strlen(absolute_path) + 1;
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    char* buffer = cortez_write_handle_get_part1(h, &part1_size);
                    memcpy(buffer, absolute_path, payload_size);
                    cortez_mesh_commit_send_zc(h, MSG_UPLOAD_FILE);
                    sent_ok = 1;
                }
        } else if (strcmp(argv[1], "find") == 0 && argc == 3) {
                 const char* word = argv[2];
                 uint32_t payload_size = strlen(word) + 1;
                 h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                 if (h) {
                    size_t part1_size;
                    char* buffer = cortez_write_handle_get_part1(h, &part1_size);
                    memcpy(buffer, word, payload_size);
                    cortez_mesh_commit_send_zc(h, MSG_QUERY_WORD);
                    sent_ok = 1;
                }
            } else if (strcmp(argv[1], "change") == 0 && argc == 4) {
                uint32_t payload_size = sizeof(change_word_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    change_word_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->target_word, argv[2], MAX_WORD_LEN - 1);
                    strncpy(req->new_word, argv[3], MAX_WORD_LEN - 1);
                    cortez_mesh_commit_send_zc(h, MSG_CHANGE_WORD);
                    sent_ok = 1;
                }
            }else if ((strcmp(argv[1], "wc") == 0 || strcmp(argv[1], "wl") == 0 || strcmp(argv[1], "cc") == 0) && argc == 2) {
                h = cortez_mesh_begin_send_zc(mesh, target_pid, 1); // Send dummy payload
                if (h) {
                    uint16_t msg_type = 0;
                    if (strcmp(argv[1], "wc") == 0) msg_type = MSG_WORD_COUNT;
                    if (strcmp(argv[1], "wl") == 0) msg_type = MSG_LINE_COUNT;
                    if (strcmp(argv[1], "cc") == 0) msg_type = MSG_CHAR_COUNT;
                    cortez_mesh_commit_send_zc(h, msg_type);
                    sent_ok = 1;
                }
            } else if (strcmp(argv[1], "add-node") == 0 && argc == 4) {
                uint32_t payload_size = sizeof(add_node_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    add_node_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->node_name, argv[3], MAX_NODE_NAME_LEN - 1);
                    realpath(argv[2], req->path); 
                    cortez_mesh_commit_send_zc(h, MSG_ADD_NODE);
                    sent_ok = 1;
                }
            } else if (strcmp(argv[1], "list-nodes") == 0 && argc == 2) {
                h = cortez_mesh_begin_send_zc(mesh, target_pid, 1);
                if (h) {
                    cortez_mesh_commit_send_zc(h, MSG_LIST_NODES);
                    sent_ok = 1;
                }
            } else if ((strcmp(argv[1], "view-node") == 0 || strcmp(argv[1], "activate") == 0 || strcmp(argv[1], "deactivate") == 0) && argc == 3) {
                uint32_t payload_size = sizeof(node_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                 if (h) {
                    size_t part1_size;
                    node_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->node_name, argv[2], MAX_NODE_NAME_LEN - 1);
                    uint16_t msg_type = 0;
                    if (strcmp(argv[1], "view-node") == 0) msg_type = MSG_VIEW_NODE;
                    if (strcmp(argv[1], "activate") == 0) msg_type = MSG_ACTIVATE_NODE;
                    if (strcmp(argv[1], "deactivate") == 0) msg_type = MSG_DEACTIVATE_NODE;
                    cortez_mesh_commit_send_zc(h, msg_type);
                    sent_ok = 1;
                }
            } else if (strcmp(argv[1], "remove-node") == 0 && argc == 3) {
                uint32_t payload_size = sizeof(node_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    node_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->node_name, argv[2], MAX_NODE_NAME_LEN - 1);
                    cortez_mesh_commit_send_zc(h, MSG_REMOVE_NODE);
                    sent_ok = 1;
                }
            }else if (strcmp(argv[1], "attr-node") == 0 && argc >= 4) {
                uint32_t payload_size = sizeof(attr_node_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    attr_node_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    memset(req, 0, sizeof(attr_node_req_t));
                    strncpy(req->node_name, argv[2], MAX_NODE_NAME_LEN - 1);
                    for (int i = 3; i < argc; i++) {
                        if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) {
                            req->flags |= ATTR_FLAG_AUTHOR;
                            strncpy(req->author, argv[++i], MAX_ATTR_LEN - 1);
                        } else if (strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
                            req->flags |= ATTR_FLAG_DESC;
                            strncpy(req->desc, argv[++i], MAX_ATTR_LEN - 1);
                        } else if (strcmp(argv[i], "--tag") == 0 && i + 1 < argc) {
                            req->flags |= ATTR_FLAG_TAG;
                            strncpy(req->tag, argv[++i], MAX_ATTR_LEN - 1);
                        }
                    }
                    cortez_mesh_commit_send_zc(h, MSG_ATTR_NODE);
                    sent_ok = 1;
                }
            } else if (strcmp(argv[1], "info-node") == 0 && argc == 3) {
                uint32_t payload_size = sizeof(node_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    node_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    strncpy(req->node_name, argv[2], MAX_NODE_NAME_LEN - 1);
                    cortez_mesh_commit_send_zc(h, MSG_INFO_NODE);
                    sent_ok = 1;
                }
            } else if (strcmp(argv[1], "search-attr") == 0 && argc == 4) {
                uint32_t payload_size = sizeof(search_attr_req_t);
                h = cortez_mesh_begin_send_zc(mesh, target_pid, payload_size);
                if (h) {
                    size_t part1_size;
                    search_attr_req_t* req = cortez_write_handle_get_part1(h, &part1_size);
                    int valid_option = 0;
                    if (strcmp(argv[2], "--author") == 0) {
                        req->type = SEARCH_BY_AUTHOR;
                        valid_option = 1;
                    } else if (strcmp(argv[2], "--tag") == 0) {
                        req->type = SEARCH_BY_TAG;
                        valid_option = 1;
                    }
                    if (valid_option) {
                        strncpy(req->target, argv[3], MAX_ATTR_LEN - 1);
                        cortez_mesh_commit_send_zc(h, MSG_SEARCH_ATTR);
                        sent_ok = 1;
                    } else {
                        cortez_mesh_abort_send_zc(h);
                        fprintf(stderr, "Invalid option for search-attr. Use --author or --tag.\n");
                    }
                }
            } else {
                 fprintf(stderr, "Invalid command or arguments.\n");
                 cortez_mesh_shutdown(mesh);
                 return 1;
            }
            if(sent_ok) break;
            if (i < max_retries - 1) {
                printf("Query daemon not yet visible on the mesh, retrying...\n");
                sleep(1);
            }
        }
        if (!sent_ok) {
            fprintf(stderr, "Failed to send message to query daemon after %d retries. Aborting.\n", max_retries);
            cortez_mesh_shutdown(mesh);
            return 1;
        }

        printf("Waiting for response [10s]...\n");
        cortez_msg_t* msg = cortez_mesh_read(mesh, 10000); 
        if (msg) {
            if (cortez_msg_type(msg) == MSG_QUERY_RESPONSE) {
                const query_response_t* resp = cortez_msg_payload(msg);
                printf("Result: Found '%s' %d times.\n", resp->word, resp->count);
                if (resp->num_sentences > 0) {
                    printf("\nSentences:\n");
                    const char* sentence_ptr = resp->sentences;
                    for(int i = 0; i < resp->num_sentences; i++) {
                        while (*sentence_ptr && isspace((unsigned char)*sentence_ptr)) sentence_ptr++;
                        printf("  - %s\n", sentence_ptr);
                        sentence_ptr += strlen(sentence_ptr) + 1;
                    }
                }
            } else if (cortez_msg_type(msg) == MSG_OPERATION_ACK) {
                const ack_t* ack = cortez_msg_payload(msg);
                printf("Result: %s (%s)\n", ack->success ? "Success" : "Failure", ack->details);
            }else if (cortez_msg_type(msg) == MSG_COUNT_RESPONSE) {
                const count_response_t* resp = cortez_msg_payload(msg);
                printf("Result: %ld\n", resp->count);
            }else if (cortez_msg_type(msg) == MSG_LIST_NODES_RESPONSE) {
                const list_resp_t* resp = cortez_msg_payload(msg);
                printf("--- Active Nodes (%d) ---\n", resp->item_count);
                const char* current = resp->data;
                for (int i = 0; i < resp->item_count; i++) {
                    printf("%s", current);
                    current += strlen(current) + 1;
                }
            } else if (cortez_msg_type(msg) == MSG_VIEW_NODE_RESPONSE) {
                const list_resp_t* resp = cortez_msg_payload(msg);
                printf("--- Node History (%d events) ---\n", resp->item_count);
                const char* current = resp->data;
                 for (int i = 0; i < resp->item_count; i++) {
                    printf("%s", current);
                    current += strlen(current) + 1;
                }
            }else if (cortez_msg_type(msg) == MSG_LOOKUP_RESPONSE) {
                const list_resp_t* resp = cortez_msg_payload(msg);
                printf("--- Lookup Results ---\n");
                const char* current = resp->data;
                for (int i = 0; i < resp->item_count; i++) {
                    printf("%s", current);
                    current += strlen(current) + 1;
                }
                if (resp->item_count == 0 && strlen(resp->data) > 0) {
                    printf("%s", resp->data);
                }
            }else if (cortez_msg_type(msg) == MSG_INFO_NODE_RESPONSE) {
                const info_node_resp_t* resp = cortez_msg_payload(msg);
                if (resp->success) {
                    printf("--- Info for Node ---\n");
                    printf("Author: %s\n", strlen(resp->author) > 0 ? resp->author : "[not set]");
                    printf("Tag: %s\n", strlen(resp->tag) > 0 ? resp->tag : "[not set]");
                    printf("Description: %s\n", strlen(resp->desc) > 0 ? resp->desc : "[not set]");
                    printf("Current Version: %s\n", strlen(resp->current_version) > 0 ? resp->current_version : "[not set]");
                } else {
                    printf("Result: Failure (Node not found).\n");
                }
            } else {
                printf("Received unexpected response of type %d\n", cortez_msg_type(msg));
            }
            cortez_mesh_msg_release(mesh, msg);
        } else {
            printf("No response from daemon (timeout).\n");
        }
        cortez_mesh_shutdown(mesh);
        }    
    }


    return 0;
}
