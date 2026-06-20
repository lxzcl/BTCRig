#include "sha256d.h"

#include <immintrin.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define BTC_X86_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define BTC_X86_ALWAYS_INLINE static inline
#endif

static const uint32_t k_sha256_initial_state[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

static const uint32_t k_sha256_round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

BTC_X86_ALWAYS_INLINE __m128i make_u32x4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return _mm_set_epi32(d, c, b, a);
}

BTC_X86_ALWAYS_INLINE uint32_t bswap32(uint32_t v) {
#if defined(__GNUC__)
    return __builtin_bswap32(v);
#else
    return ((v & 0x000000ffU) << 24) |
           ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) |
           ((v & 0xff000000U) >> 24);
#endif
}

BTC_X86_ALWAYS_INLINE int hash_words_meet_target(const uint32_t hash_words[8], const uint32_t target_words[8]) {
    for (int i = 7; i >= 0; --i) {
        uint32_t h = bswap32(hash_words[i]);
        uint32_t t = target_words[i];
        if (h < t) {
            return 1;
        }
        if (h > t) {
            return 0;
        }
    }
    return 1;
}

BTC_X86_ALWAYS_INLINE __m128i make_k(int offset) {
    return _mm_set_epi32(
        k_sha256_round_constants[offset + 3],
        k_sha256_round_constants[offset + 2],
        k_sha256_round_constants[offset + 1],
        k_sha256_round_constants[offset]);
}

BTC_X86_ALWAYS_INLINE void sha256_x86_load_state_vec(const uint32_t state[8],
                                                     __m128i *state0,
                                                     __m128i *state1) {
    __m128i tmp = _mm_loadu_si128((const __m128i *)&state[0]);
    *state1 = _mm_loadu_si128((const __m128i *)&state[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xb1);
    *state1 = _mm_shuffle_epi32(*state1, 0x1b);
    *state0 = _mm_alignr_epi8(tmp, *state1, 8);
    *state1 = _mm_blend_epi16(*state1, tmp, 0xf0);
}

BTC_X86_ALWAYS_INLINE void sha256_x86_store_state_vec(uint32_t state[8],
                                                      __m128i state0,
                                                      __m128i state1) {
    __m128i tmp = _mm_shuffle_epi32(state0, 0x1b);
    state1 = _mm_shuffle_epi32(state1, 0xb1);
    state0 = _mm_blend_epi16(tmp, state1, 0xf0);
    state1 = _mm_alignr_epi8(state1, tmp, 8);

    _mm_storeu_si128((__m128i *)&state[0], state0);
    _mm_storeu_si128((__m128i *)&state[4], state1);
}

BTC_X86_ALWAYS_INLINE void sha256_x86_compress_vec(uint32_t state[8],
                                                   __m128i msg0,
                                                   __m128i msg1,
                                                   __m128i msg2,
                                                   __m128i msg3) {
    __m128i state0;
    __m128i state1;
    __m128i msg;
    __m128i tmp;
    __m128i abef_save;
    __m128i cdgh_save;

    sha256_x86_load_state_vec(state, &state0, &state1);

    abef_save = state0;
    cdgh_save = state1;

#define BTC_SHA256_X86_ROUND4_MSG(msg_var, offset) do { \
        msg = _mm_add_epi32((msg_var), make_k((offset))); \
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg); \
        msg = _mm_shuffle_epi32(msg, 0x0e); \
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg); \
    } while (0)

#define BTC_SHA256_X86_MSG2(dst, prev, last) do { \
        tmp = _mm_alignr_epi8((last), (prev), 4); \
        (dst) = _mm_add_epi32((dst), tmp); \
        (dst) = _mm_sha256msg2_epu32((dst), (last)); \
    } while (0)

    BTC_SHA256_X86_ROUND4_MSG(msg0, 0);
    BTC_SHA256_X86_ROUND4_MSG(msg1, 4);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);
    BTC_SHA256_X86_ROUND4_MSG(msg2, 8);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);
    BTC_SHA256_X86_ROUND4_MSG(msg3, 12);
    BTC_SHA256_X86_MSG2(msg0, msg2, msg3);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    BTC_SHA256_X86_ROUND4_MSG(msg0, 16);
    BTC_SHA256_X86_MSG2(msg1, msg3, msg0);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);
    BTC_SHA256_X86_ROUND4_MSG(msg1, 20);
    BTC_SHA256_X86_MSG2(msg2, msg0, msg1);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);
    BTC_SHA256_X86_ROUND4_MSG(msg2, 24);
    BTC_SHA256_X86_MSG2(msg3, msg1, msg2);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);
    BTC_SHA256_X86_ROUND4_MSG(msg3, 28);
    BTC_SHA256_X86_MSG2(msg0, msg2, msg3);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    BTC_SHA256_X86_ROUND4_MSG(msg0, 32);
    BTC_SHA256_X86_MSG2(msg1, msg3, msg0);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);
    BTC_SHA256_X86_ROUND4_MSG(msg1, 36);
    BTC_SHA256_X86_MSG2(msg2, msg0, msg1);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);
    BTC_SHA256_X86_ROUND4_MSG(msg2, 40);
    BTC_SHA256_X86_MSG2(msg3, msg1, msg2);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);
    BTC_SHA256_X86_ROUND4_MSG(msg3, 44);
    BTC_SHA256_X86_MSG2(msg0, msg2, msg3);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    BTC_SHA256_X86_ROUND4_MSG(msg0, 48);
    BTC_SHA256_X86_MSG2(msg1, msg3, msg0);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);
    BTC_SHA256_X86_ROUND4_MSG(msg1, 52);
    BTC_SHA256_X86_MSG2(msg2, msg0, msg1);
    BTC_SHA256_X86_ROUND4_MSG(msg2, 56);
    BTC_SHA256_X86_MSG2(msg3, msg1, msg2);
    BTC_SHA256_X86_ROUND4_MSG(msg3, 60);

#undef BTC_SHA256_X86_ROUND4_MSG
#undef BTC_SHA256_X86_MSG2

    state0 = _mm_add_epi32(state0, abef_save);
    state1 = _mm_add_epi32(state1, cdgh_save);

    sha256_x86_store_state_vec(state, state0, state1);
}

BTC_X86_ALWAYS_INLINE void sha256_x86_compress_vec2(uint32_t state_a[8],
                                                    uint32_t state_b[8],
                                                    __m128i a_msg0,
                                                    __m128i a_msg1,
                                                    __m128i a_msg2,
                                                    __m128i a_msg3,
                                                    __m128i b_msg0,
                                                    __m128i b_msg1,
                                                    __m128i b_msg2,
                                                    __m128i b_msg3) {
    __m128i a_state0;
    __m128i a_state1;
    __m128i b_state0;
    __m128i b_state1;
    __m128i a_msg;
    __m128i b_msg;
    __m128i a_tmp;
    __m128i b_tmp;

    sha256_x86_load_state_vec(state_a, &a_state0, &a_state1);
    sha256_x86_load_state_vec(state_b, &b_state0, &b_state1);

    const __m128i a_abef_save = a_state0;
    const __m128i a_cdgh_save = a_state1;
    const __m128i b_abef_save = b_state0;
    const __m128i b_cdgh_save = b_state1;

#define BTC_SHA256_X86_ROUND4_MSG2(a_msg_var, b_msg_var, offset) do { \
        const __m128i k__ = make_k((offset)); \
        a_msg = _mm_add_epi32((a_msg_var), k__); \
        b_msg = _mm_add_epi32((b_msg_var), k__); \
        a_state1 = _mm_sha256rnds2_epu32(a_state1, a_state0, a_msg); \
        b_state1 = _mm_sha256rnds2_epu32(b_state1, b_state0, b_msg); \
        a_msg = _mm_shuffle_epi32(a_msg, 0x0e); \
        b_msg = _mm_shuffle_epi32(b_msg, 0x0e); \
        a_state0 = _mm_sha256rnds2_epu32(a_state0, a_state1, a_msg); \
        b_state0 = _mm_sha256rnds2_epu32(b_state0, b_state1, b_msg); \
    } while (0)

#define BTC_SHA256_X86_MSG2_PAIR(a_dst, b_dst, a_prev, b_prev, a_last, b_last) do { \
        a_tmp = _mm_alignr_epi8((a_last), (a_prev), 4); \
        b_tmp = _mm_alignr_epi8((b_last), (b_prev), 4); \
        (a_dst) = _mm_add_epi32((a_dst), a_tmp); \
        (b_dst) = _mm_add_epi32((b_dst), b_tmp); \
        (a_dst) = _mm_sha256msg2_epu32((a_dst), (a_last)); \
        (b_dst) = _mm_sha256msg2_epu32((b_dst), (b_last)); \
    } while (0)

    BTC_SHA256_X86_ROUND4_MSG2(a_msg0, b_msg0, 0);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg1, b_msg1, 4);
    a_msg0 = _mm_sha256msg1_epu32(a_msg0, a_msg1);
    b_msg0 = _mm_sha256msg1_epu32(b_msg0, b_msg1);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg2, b_msg2, 8);
    a_msg1 = _mm_sha256msg1_epu32(a_msg1, a_msg2);
    b_msg1 = _mm_sha256msg1_epu32(b_msg1, b_msg2);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg3, b_msg3, 12);
    BTC_SHA256_X86_MSG2_PAIR(a_msg0, b_msg0, a_msg2, b_msg2, a_msg3, b_msg3);
    a_msg2 = _mm_sha256msg1_epu32(a_msg2, a_msg3);
    b_msg2 = _mm_sha256msg1_epu32(b_msg2, b_msg3);

    BTC_SHA256_X86_ROUND4_MSG2(a_msg0, b_msg0, 16);
    BTC_SHA256_X86_MSG2_PAIR(a_msg1, b_msg1, a_msg3, b_msg3, a_msg0, b_msg0);
    a_msg3 = _mm_sha256msg1_epu32(a_msg3, a_msg0);
    b_msg3 = _mm_sha256msg1_epu32(b_msg3, b_msg0);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg1, b_msg1, 20);
    BTC_SHA256_X86_MSG2_PAIR(a_msg2, b_msg2, a_msg0, b_msg0, a_msg1, b_msg1);
    a_msg0 = _mm_sha256msg1_epu32(a_msg0, a_msg1);
    b_msg0 = _mm_sha256msg1_epu32(b_msg0, b_msg1);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg2, b_msg2, 24);
    BTC_SHA256_X86_MSG2_PAIR(a_msg3, b_msg3, a_msg1, b_msg1, a_msg2, b_msg2);
    a_msg1 = _mm_sha256msg1_epu32(a_msg1, a_msg2);
    b_msg1 = _mm_sha256msg1_epu32(b_msg1, b_msg2);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg3, b_msg3, 28);
    BTC_SHA256_X86_MSG2_PAIR(a_msg0, b_msg0, a_msg2, b_msg2, a_msg3, b_msg3);
    a_msg2 = _mm_sha256msg1_epu32(a_msg2, a_msg3);
    b_msg2 = _mm_sha256msg1_epu32(b_msg2, b_msg3);

    BTC_SHA256_X86_ROUND4_MSG2(a_msg0, b_msg0, 32);
    BTC_SHA256_X86_MSG2_PAIR(a_msg1, b_msg1, a_msg3, b_msg3, a_msg0, b_msg0);
    a_msg3 = _mm_sha256msg1_epu32(a_msg3, a_msg0);
    b_msg3 = _mm_sha256msg1_epu32(b_msg3, b_msg0);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg1, b_msg1, 36);
    BTC_SHA256_X86_MSG2_PAIR(a_msg2, b_msg2, a_msg0, b_msg0, a_msg1, b_msg1);
    a_msg0 = _mm_sha256msg1_epu32(a_msg0, a_msg1);
    b_msg0 = _mm_sha256msg1_epu32(b_msg0, b_msg1);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg2, b_msg2, 40);
    BTC_SHA256_X86_MSG2_PAIR(a_msg3, b_msg3, a_msg1, b_msg1, a_msg2, b_msg2);
    a_msg1 = _mm_sha256msg1_epu32(a_msg1, a_msg2);
    b_msg1 = _mm_sha256msg1_epu32(b_msg1, b_msg2);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg3, b_msg3, 44);
    BTC_SHA256_X86_MSG2_PAIR(a_msg0, b_msg0, a_msg2, b_msg2, a_msg3, b_msg3);
    a_msg2 = _mm_sha256msg1_epu32(a_msg2, a_msg3);
    b_msg2 = _mm_sha256msg1_epu32(b_msg2, b_msg3);

    BTC_SHA256_X86_ROUND4_MSG2(a_msg0, b_msg0, 48);
    BTC_SHA256_X86_MSG2_PAIR(a_msg1, b_msg1, a_msg3, b_msg3, a_msg0, b_msg0);
    a_msg3 = _mm_sha256msg1_epu32(a_msg3, a_msg0);
    b_msg3 = _mm_sha256msg1_epu32(b_msg3, b_msg0);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg1, b_msg1, 52);
    BTC_SHA256_X86_MSG2_PAIR(a_msg2, b_msg2, a_msg0, b_msg0, a_msg1, b_msg1);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg2, b_msg2, 56);
    BTC_SHA256_X86_MSG2_PAIR(a_msg3, b_msg3, a_msg1, b_msg1, a_msg2, b_msg2);
    BTC_SHA256_X86_ROUND4_MSG2(a_msg3, b_msg3, 60);

    a_state0 = _mm_add_epi32(a_state0, a_abef_save);
    a_state1 = _mm_add_epi32(a_state1, a_cdgh_save);
    b_state0 = _mm_add_epi32(b_state0, b_abef_save);
    b_state1 = _mm_add_epi32(b_state1, b_cdgh_save);

    sha256_x86_store_state_vec(state_a, a_state0, a_state1);
    sha256_x86_store_state_vec(state_b, b_state0, b_state1);

#undef BTC_SHA256_X86_MSG2_PAIR
#undef BTC_SHA256_X86_ROUND4_MSG2
}

int sha256d_x86_sha_ni_available(void) {
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("sha") &&
           __builtin_cpu_supports("ssse3") &&
           __builtin_cpu_supports("sse4.1");
#else
    return 0;
#endif
}

BTC_X86_ALWAYS_INLINE void sha256d_hash_tail_words_x86_raw(const sha256_midstate_t *state,
                                                           const uint32_t tail_words[4],
                                                           uint32_t out_words[8]) {
    uint32_t first_state[8];
    uint32_t second_state[8];

    memcpy(first_state, state->fast_state, sizeof(first_state));

    sha256_x86_compress_vec(
        first_state,
        make_u32x4(tail_words[0], tail_words[1], tail_words[2], tail_words[3]),
        make_u32x4(0x80000000U, 0, 0, 0),
        _mm_setzero_si128(),
        make_u32x4(0, 0, 0, 80U * 8U));

    memcpy(second_state, k_sha256_initial_state, sizeof(second_state));
    sha256_x86_compress_vec(
        second_state,
        make_u32x4(first_state[0], first_state[1], first_state[2], first_state[3]),
        make_u32x4(first_state[4], first_state[5], first_state[6], first_state[7]),
        make_u32x4(0x80000000U, 0, 0, 0),
        make_u32x4(0, 0, 0, 32U * 8U));

    for (int i = 0; i < 8; ++i) {
        out_words[i] = second_state[i];
    }
}

BTC_X86_ALWAYS_INLINE void sha256d_hash_nonce_pair_x86_raw(const sha256_midstate_t *state,
                                                           uint32_t tail0,
                                                           uint32_t tail1,
                                                           uint32_t tail2,
                                                           uint32_t nonce_a,
                                                           uint32_t nonce_b,
                                                           uint32_t out_a[8],
                                                           uint32_t out_b[8]) {
    uint32_t first_a[8];
    uint32_t first_b[8];
    uint32_t second_a[8];
    uint32_t second_b[8];

    memcpy(first_a, state->fast_state, sizeof(first_a));
    memcpy(first_b, state->fast_state, sizeof(first_b));

    sha256_x86_compress_vec2(
        first_a,
        first_b,
        make_u32x4(tail0, tail1, tail2, bswap32(nonce_a)),
        make_u32x4(0x80000000U, 0, 0, 0),
        _mm_setzero_si128(),
        make_u32x4(0, 0, 0, 80U * 8U),
        make_u32x4(tail0, tail1, tail2, bswap32(nonce_b)),
        make_u32x4(0x80000000U, 0, 0, 0),
        _mm_setzero_si128(),
        make_u32x4(0, 0, 0, 80U * 8U));

    memcpy(second_a, k_sha256_initial_state, sizeof(second_a));
    memcpy(second_b, k_sha256_initial_state, sizeof(second_b));

    sha256_x86_compress_vec2(
        second_a,
        second_b,
        make_u32x4(first_a[0], first_a[1], first_a[2], first_a[3]),
        make_u32x4(first_a[4], first_a[5], first_a[6], first_a[7]),
        make_u32x4(0x80000000U, 0, 0, 0),
        make_u32x4(0, 0, 0, 32U * 8U),
        make_u32x4(first_b[0], first_b[1], first_b[2], first_b[3]),
        make_u32x4(first_b[4], first_b[5], first_b[6], first_b[7]),
        make_u32x4(0x80000000U, 0, 0, 0),
        make_u32x4(0, 0, 0, 32U * 8U));

    for (int i = 0; i < 8; ++i) {
        out_a[i] = second_a[i];
        out_b[i] = second_b[i];
    }
}

void sha256d_80_midstate_hash_tail_words_x86_sha_ni(const sha256_midstate_t *state,
                                                    const uint32_t tail_words[4],
                                                    uint32_t out_words[8]) {
    sha256d_hash_tail_words_x86_raw(state, tail_words, out_words);
}

void sha256d_scan_nonce_range_x86_sha_ni(const sha256_midstate_t *state,
                                         const uint32_t base_tail_words[4],
                                         const uint32_t target_words[8],
                                         uint32_t start_nonce,
                                         uint32_t nonce_count,
                                         void *opaque,
                                         sha256d_scan_match_func_t on_match) {
    uint32_t tail_words[4];
    uint32_t hash_words_a[8];
    uint32_t hash_words_b[8];
    uint32_t i = 0;

    tail_words[0] = base_tail_words[0];
    tail_words[1] = base_tail_words[1];
    tail_words[2] = base_tail_words[2];

    const uint32_t paired_count = nonce_count & ~1U;
    for (; i < paired_count; i += 2) {
        uint32_t nonce_a = start_nonce + i;
        uint32_t nonce_b = nonce_a + 1U;

        sha256d_hash_nonce_pair_x86_raw(
            state,
            tail_words[0],
            tail_words[1],
            tail_words[2],
            nonce_a,
            nonce_b,
            hash_words_a,
            hash_words_b);

        if (hash_words_meet_target(hash_words_a, target_words) && on_match != NULL) {
            on_match(opaque, nonce_a, hash_words_a);
        }
        if (hash_words_meet_target(hash_words_b, target_words) && on_match != NULL) {
            on_match(opaque, nonce_b, hash_words_b);
        }
    }

    if (i < nonce_count) {
        uint32_t nonce = start_nonce + i;
        tail_words[3] = bswap32(nonce);
        sha256d_hash_tail_words_x86_raw(state, tail_words, hash_words_a);
        if (hash_words_meet_target(hash_words_a, target_words) && on_match != NULL) {
            on_match(opaque, nonce, hash_words_a);
        }
    }
}

#undef BTC_X86_ALWAYS_INLINE
