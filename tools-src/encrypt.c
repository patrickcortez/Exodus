/* tools/encrypt.c
 *
 * Professional-grade file encryption using Aethel-128 in CBC mode with HMAC-SHA256.
 * This is an Encrypt-then-MAC authenticated encryption scheme.
 *
 * Usage: encrypt <key_file> <input_plaintext_file> <output_ciphertext_file>
 * The key file must be 48 bytes (16 for Aethel, 32 for HMAC).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// --- Constants ---
#define AETHEL_BLOCK_SIZE 16
#define AETHEL_KEY_SIZE 16
#define NUM_ROUNDS 10
#define HMAC_KEY_SIZE 32
#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

// --- Aethel Cipher Implementation (AES-Inspired) ---
typedef uint8_t state_t[4][4];

static const uint8_t s_box[256] = {
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
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};
static const uint8_t Rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi_bit_set = (a & 0x80);
        a <<= 1;
        if (hi_bit_set) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

static void key_expansion(const uint8_t* key, uint8_t round_keys[NUM_ROUNDS + 1][AETHEL_BLOCK_SIZE]) {
    uint8_t temp[4];
    int key_words = AETHEL_KEY_SIZE / 4;
    uint8_t* w = (uint8_t*)round_keys;
    memcpy(w, key, AETHEL_KEY_SIZE);

    for (int i = key_words; i < (4 * (NUM_ROUNDS + 1)); i++) {
        memcpy(temp, &w[(i - 1) * 4], 4);
        if (i % key_words == 0) {
            uint8_t t = temp[0]; temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t; // RotWord
            for (int j = 0; j < 4; j++) temp[j] = s_box[temp[j]]; // SubWord
            temp[0] ^= Rcon[(i / key_words) - 1];
        }
        for (int j = 0; j < 4; j++) w[i * 4 + j] = w[(i - key_words) * 4 + j] ^ temp[j];
    }
}

static void add_round_key(state_t* state, const uint8_t* round_key) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) (*state)[i][j] ^= round_key[i * 4 + j];
}
static void sub_bytes(state_t* state) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) (*state)[i][j] = s_box[(*state)[i][j]];
}
static void shift_rows(state_t* state) {
    uint8_t temp;
    temp = (*state)[1][0]; (*state)[1][0] = (*state)[1][1]; (*state)[1][1] = (*state)[1][2]; (*state)[1][2] = (*state)[1][3]; (*state)[1][3] = temp;
    temp = (*state)[2][0]; (*state)[2][0] = (*state)[2][2]; (*state)[2][2] = temp; temp = (*state)[2][1]; (*state)[2][1] = (*state)[2][3]; (*state)[2][3] = temp;
    temp = (*state)[3][0]; (*state)[3][0] = (*state)[3][3]; (*state)[3][3] = (*state)[3][2]; (*state)[3][2] = (*state)[3][1]; (*state)[3][1] = temp;
}
static void mix_columns(state_t* state) {
    uint8_t t[4];
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) t[i] = (*state)[i][j];
        (*state)[0][j] = gmul(t[0], 2) ^ gmul(t[1], 3) ^ t[2] ^ t[3];
        (*state)[1][j] = t[0] ^ gmul(t[1], 2) ^ gmul(t[2], 3) ^ t[3];
        (*state)[2][j] = t[0] ^ t[1] ^ gmul(t[2], 2) ^ gmul(t[3], 3);
        (*state)[3][j] = gmul(t[0], 3) ^ t[1] ^ t[2] ^ gmul(t[3], 2);
    }
}

static void aethel_encrypt_block(uint8_t* block, const uint8_t round_keys[NUM_ROUNDS + 1][AETHEL_BLOCK_SIZE]) {
    state_t state;
    memcpy(state, block, AETHEL_BLOCK_SIZE);
    add_round_key(&state, round_keys[0]);
    for (int i = 1; i < NUM_ROUNDS; i++) {
        sub_bytes(&state);
        shift_rows(&state);
        mix_columns(&state);
        add_round_key(&state, round_keys[i]);
    }
    sub_bytes(&state);
    shift_rows(&state);
    add_round_key(&state, round_keys[NUM_ROUNDS]);
    memcpy(block, state, AETHEL_BLOCK_SIZE);
}

// --- SHA-256 and HMAC Implementation ---
typedef struct { uint8_t data[64]; uint32_t datalen; uint64_t bitlen; uint32_t state[8]; } SHA256_CTX;
static const uint32_t K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROTR(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define Ch(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))
#define EP1(x) (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))
#define SIG0(x) (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
	uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
	for (i = 0, j = 0; i < 16; ++i, j += 4) m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
	for ( ; i < 64; ++i) m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
	for (i = 0; i < 64; ++i) {
		t1 = h + EP1(e) + Ch(e,f,g) + K[i] + m[i];
		t2 = EP0(a) + Maj(a,b,c);
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}
static void sha256_init(SHA256_CTX *ctx) {
	ctx->datalen = 0; ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85; ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c; ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}
static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
	for (size_t i = 0; i < len; ++i) {
		ctx->data[ctx->datalen] = data[i]; ctx->datalen++;
		if (ctx->datalen == 64) {
			sha256_transform(ctx, ctx->data);
			ctx->bitlen += 512; ctx->datalen = 0;
		}
	}
}
static void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
	uint32_t i = ctx->datalen;
	ctx->data[i++] = 0x80;
	if (ctx->datalen < 56) { memset(ctx->data + i, 0, 56 - i); }
	else { memset(ctx->data + i, 0, 64 - i); sha256_transform(ctx, ctx->data); memset(ctx->data, 0, 56); }
	ctx->bitlen += ctx->datalen * 8;
	ctx->data[63] = ctx->bitlen; ctx->data[62] = ctx->bitlen >> 8; ctx->data[61] = ctx->bitlen >> 16; ctx->data[60] = ctx->bitlen >> 24;
	ctx->data[59] = ctx->bitlen >> 32; ctx->data[58] = ctx->bitlen >> 40; ctx->data[57] = ctx->bitlen >> 48; ctx->data[56] = ctx->bitlen >> 56;
	sha256_transform(ctx, ctx->data);
	for (i = 0; i < 4; ++i) {
		hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xff; hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xff;
		hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xff; hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
		hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xff; hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
		hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xff; hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
	}
}

static void hmac_sha256(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t* out) {
    uint8_t k[SHA256_BLOCK_SIZE];
    uint8_t temp[SHA256_DIGEST_SIZE];
    SHA256_CTX ctx;

    if (keylen > SHA256_BLOCK_SIZE) {
        sha256_init(&ctx); sha256_update(&ctx, key, keylen); sha256_final(&ctx, k);
        memset(k + SHA256_DIGEST_SIZE, 0, SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    } else {
        memcpy(k, key, keylen);
        memset(k + keylen, 0, SHA256_BLOCK_SIZE - keylen);
    }

    uint8_t o_key_pad[SHA256_BLOCK_SIZE];
    uint8_t i_key_pad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        o_key_pad[i] = k[i] ^ 0x5c;
        i_key_pad[i] = k[i] ^ 0x36;
    }

    sha256_init(&ctx); sha256_update(&ctx, i_key_pad, SHA256_BLOCK_SIZE); sha256_update(&ctx, data, datalen); sha256_final(&ctx, temp);
    sha256_init(&ctx); sha256_update(&ctx, o_key_pad, SHA256_BLOCK_SIZE); sha256_update(&ctx, temp, SHA256_DIGEST_SIZE); sha256_final(&ctx, out);
}

// --- Main Application Logic ---
int main(int argc, char *argv[]) {
    if (argc != 4) { fprintf(stderr, "Usage: %s <key_file> <input_file> <output_file>\n", argv[0]); return 1; }

    // 1. Read and split the combined key file
    FILE *key_f = fopen(argv[1], "rb");
    if (!key_f) { perror("Could not open key file"); return 1; }
    
    uint8_t aethel_key[AETHEL_KEY_SIZE];
    uint8_t hmac_key[HMAC_KEY_SIZE];
    if (fread(aethel_key, 1, AETHEL_KEY_SIZE, key_f) != AETHEL_KEY_SIZE) {
        fprintf(stderr, "Key file too short for Aethel key.\n"); fclose(key_f); return 1;
    }
    if (fread(hmac_key, 1, HMAC_KEY_SIZE, key_f) != HMAC_KEY_SIZE) {
        fprintf(stderr, "Key file too short for HMAC key.\n"); fclose(key_f); return 1;
    }
    fclose(key_f);

    FILE *in_f = fopen(argv[2], "rb");
    if (!in_f) { perror("Could not open input file"); return 1; }
    
    FILE *out_f = fopen(argv[3], "wb");
    if (!out_f) { perror("Could not open output file"); return 1; }

    // 2. Generate random IV and expand Aethel key
    uint8_t iv[AETHEL_BLOCK_SIZE];
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (!urandom || fread(iv, 1, sizeof(iv), urandom) != sizeof(iv)) {
        fprintf(stderr, "Failed to generate random IV. Ensure /dev/urandom is available.\n");
        if(urandom) fclose(urandom); return 1;
    }
    fclose(urandom);
    
    uint8_t round_keys[NUM_ROUNDS + 1][AETHEL_BLOCK_SIZE];
    key_expansion(aethel_key, round_keys);

    // 3. Encrypt in CBC mode and buffer the IV and ciphertext for HMAC
    fseek(in_f, 0, SEEK_END);
    long pt_size = ftell(in_f);
    fseek(in_f, 0, SEEK_SET);
    long ct_size = (pt_size / AETHEL_BLOCK_SIZE + 1) * AETHEL_BLOCK_SIZE;
    uint8_t* mac_buffer = malloc(AETHEL_BLOCK_SIZE + ct_size);
    if (!mac_buffer) { fprintf(stderr, "Failed to allocate memory for MAC buffer.\n"); return 1; }

    memcpy(mac_buffer, iv, AETHEL_BLOCK_SIZE);
    
    uint8_t plaintext_block[AETHEL_BLOCK_SIZE];
    uint8_t ciphertext_block[AETHEL_BLOCK_SIZE];
    uint8_t prev_cipher_block[AETHEL_BLOCK_SIZE];
    memcpy(prev_cipher_block, iv, AETHEL_BLOCK_SIZE);

    size_t bytes_read, total_written = 0;
    while ((bytes_read = fread(plaintext_block, 1, AETHEL_BLOCK_SIZE, in_f))) {
        for (int i = 0; i < AETHEL_BLOCK_SIZE; i++) ciphertext_block[i] = plaintext_block[i] ^ prev_cipher_block[i];
        
        if (bytes_read < AETHEL_BLOCK_SIZE) {
            uint8_t padding_val = AETHEL_BLOCK_SIZE - bytes_read;
            for(size_t i = bytes_read; i < AETHEL_BLOCK_SIZE; i++) ciphertext_block[i] = padding_val ^ prev_cipher_block[i];
        }

        aethel_encrypt_block(ciphertext_block, round_keys);
        memcpy(mac_buffer + AETHEL_BLOCK_SIZE + total_written, ciphertext_block, AETHEL_BLOCK_SIZE);
        total_written += AETHEL_BLOCK_SIZE;
        memcpy(prev_cipher_block, ciphertext_block, AETHEL_BLOCK_SIZE);
    }
    if (pt_size % AETHEL_BLOCK_SIZE == 0) { // Add full padding block if necessary
        for (int i = 0; i < AETHEL_BLOCK_SIZE; i++) ciphertext_block[i] = AETHEL_BLOCK_SIZE ^ prev_cipher_block[i];
        aethel_encrypt_block(ciphertext_block, round_keys);
        memcpy(mac_buffer + AETHEL_BLOCK_SIZE + total_written, ciphertext_block, AETHEL_BLOCK_SIZE);
        total_written += AETHEL_BLOCK_SIZE;
    }

    // 4. Calculate HMAC and write final file
    uint8_t hmac_tag[SHA256_DIGEST_SIZE];
    hmac_sha256(hmac_key, HMAC_KEY_SIZE, mac_buffer, AETHEL_BLOCK_SIZE + total_written, hmac_tag);
    
    fwrite(mac_buffer, 1, AETHEL_BLOCK_SIZE + total_written, out_f);
    fwrite(hmac_tag, 1, SHA256_DIGEST_SIZE, out_f);

    free(mac_buffer);
    fclose(in_f);
    fclose(out_f);
    printf("Encryption complete.\n");
    return 0;
}