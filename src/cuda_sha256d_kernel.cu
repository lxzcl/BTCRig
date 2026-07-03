typedef unsigned int u32;

#define CUDA_DEVICE_INLINE __attribute__((device)) __attribute__((always_inline)) static inline

CUDA_DEVICE_INLINE u32 cuda_tid_x(void) {
    u32 v;
    asm volatile("mov.u32 %0, %%tid.x;" : "=r"(v));
    return v;
}

CUDA_DEVICE_INLINE u32 cuda_ctaid_x(void) {
    u32 v;
    asm volatile("mov.u32 %0, %%ctaid.x;" : "=r"(v));
    return v;
}

CUDA_DEVICE_INLINE u32 cuda_ntid_x(void) {
    u32 v;
    asm volatile("mov.u32 %0, %%ntid.x;" : "=r"(v));
    return v;
}

CUDA_DEVICE_INLINE u32 cuda_atomic_add_u32(u32 *p, u32 v) {
    u32 old;
    asm volatile("atom.global.add.u32 %0, [%1], %2;" : "=r"(old) : "l"(p), "r"(v));
    return old;
}

CUDA_DEVICE_INLINE u32 rotr32(u32 x, u32 n) {
    return (x >> n) | (x << (32U - n));
}

CUDA_DEVICE_INLINE u32 bswap32(u32 v) {
    return ((v & 0x000000ffU) << 24) |
           ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) |
           ((v & 0xff000000U) >> 24);
}

#define SS0(x) (rotr32((x), 7) ^ rotr32((x), 18) ^ ((x) >> 3))
#define SS1(x) (rotr32((x), 17) ^ rotr32((x), 19) ^ ((x) >> 10))
#define BS0(x) (rotr32((x), 2) ^ rotr32((x), 13) ^ rotr32((x), 22))
#define BS1(x) (rotr32((x), 6) ^ rotr32((x), 11) ^ rotr32((x), 25))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define MSG(w0,w1,w9,w14) ((w0) += SS0(w1) + (w9) + SS1(w14))
#define ROUND(a,b,c,d,e,f,g,h,w,k) do { \
    u32 t1 = (h) + BS1(e) + CH(e,f,g) + (k) + (w); \
    u32 t2 = BS0(a) + MAJ(a,b,c); \
    (d) += t1; \
    (h) = t1 + t2; \
} while (0)

CUDA_DEVICE_INLINE int meets_target_words(u32 h0, u32 h1, u32 h2, u32 h3,
                                          u32 h4, u32 h5, u32 h6, u32 h7,
                                          u32 t0, u32 t1, u32 t2, u32 t3,
                                          u32 t4, u32 t5, u32 t6, u32 t7) {
    u32 h = bswap32(h7); if (h < t7) return 1; if (h > t7) return 0;
    h = bswap32(h6); if (h < t6) return 1; if (h > t6) return 0;
    h = bswap32(h5); if (h < t5) return 1; if (h > t5) return 0;
    h = bswap32(h4); if (h < t4) return 1; if (h > t4) return 0;
    h = bswap32(h3); if (h < t3) return 1; if (h > t3) return 0;
    h = bswap32(h2); if (h < t2) return 1; if (h > t2) return 0;
    h = bswap32(h1); if (h < t1) return 1; if (h > t1) return 0;
    h = bswap32(h0); if (h < t0) return 1; if (h > t0) return 0;
    return 1;
}

CUDA_DEVICE_INLINE void store_match_words(u32 nonce,
                                          u32 h0, u32 h1, u32 h2, u32 h3,
                                          u32 h4, u32 h5, u32 h6, u32 h7,
                                          u32 max_results,
                                          u32 *result_count,
                                          u32 *matches) {
    u32 idx = cuda_atomic_add_u32(result_count, 1U);
    if (idx < max_results) {
        u32 base = idx * 9U;
        matches[base] = nonce;
        matches[base + 1U] = h0;
        matches[base + 2U] = h1;
        matches[base + 3U] = h2;
        matches[base + 4U] = h3;
        matches[base + 5U] = h4;
        matches[base + 6U] = h5;
        matches[base + 7U] = h6;
        matches[base + 8U] = h7;
    }
}

#define SHA256_COMPRESS(s0,s1,s2,s3,s4,s5,s6,s7,w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15) do { \
    u32 a=(s0), b=(s1), c=(s2), d=(s3), e=(s4), f=(s5), g=(s6), h=(s7); \
    ROUND(a,b,c,d,e,f,g,h,w0,0x428a2f98U); \
    ROUND(h,a,b,c,d,e,f,g,w1,0x71374491U); \
    ROUND(g,h,a,b,c,d,e,f,w2,0xb5c0fbcfU); \
    ROUND(f,g,h,a,b,c,d,e,w3,0xe9b5dba5U); \
    ROUND(e,f,g,h,a,b,c,d,w4,0x3956c25bU); \
    ROUND(d,e,f,g,h,a,b,c,w5,0x59f111f1U); \
    ROUND(c,d,e,f,g,h,a,b,w6,0x923f82a4U); \
    ROUND(b,c,d,e,f,g,h,a,w7,0xab1c5ed5U); \
    ROUND(a,b,c,d,e,f,g,h,w8,0xd807aa98U); \
    ROUND(h,a,b,c,d,e,f,g,w9,0x12835b01U); \
    ROUND(g,h,a,b,c,d,e,f,w10,0x243185beU); \
    ROUND(f,g,h,a,b,c,d,e,w11,0x550c7dc3U); \
    ROUND(e,f,g,h,a,b,c,d,w12,0x72be5d74U); \
    ROUND(d,e,f,g,h,a,b,c,w13,0x80deb1feU); \
    ROUND(c,d,e,f,g,h,a,b,w14,0x9bdc06a7U); \
    ROUND(b,c,d,e,f,g,h,a,w15,0xc19bf174U); \
    ROUND(a,b,c,d,e,f,g,h,MSG(w0, w1, w9, w14),0xe49b69c1U); \
    ROUND(h,a,b,c,d,e,f,g,MSG(w1, w2, w10, w15),0xefbe4786U); \
    ROUND(g,h,a,b,c,d,e,f,MSG(w2, w3, w11, w0),0x0fc19dc6U); \
    ROUND(f,g,h,a,b,c,d,e,MSG(w3, w4, w12, w1),0x240ca1ccU); \
    ROUND(e,f,g,h,a,b,c,d,MSG(w4, w5, w13, w2),0x2de92c6fU); \
    ROUND(d,e,f,g,h,a,b,c,MSG(w5, w6, w14, w3),0x4a7484aaU); \
    ROUND(c,d,e,f,g,h,a,b,MSG(w6, w7, w15, w4),0x5cb0a9dcU); \
    ROUND(b,c,d,e,f,g,h,a,MSG(w7, w8, w0, w5),0x76f988daU); \
    ROUND(a,b,c,d,e,f,g,h,MSG(w8, w9, w1, w6),0x983e5152U); \
    ROUND(h,a,b,c,d,e,f,g,MSG(w9, w10, w2, w7),0xa831c66dU); \
    ROUND(g,h,a,b,c,d,e,f,MSG(w10, w11, w3, w8),0xb00327c8U); \
    ROUND(f,g,h,a,b,c,d,e,MSG(w11, w12, w4, w9),0xbf597fc7U); \
    ROUND(e,f,g,h,a,b,c,d,MSG(w12, w13, w5, w10),0xc6e00bf3U); \
    ROUND(d,e,f,g,h,a,b,c,MSG(w13, w14, w6, w11),0xd5a79147U); \
    ROUND(c,d,e,f,g,h,a,b,MSG(w14, w15, w7, w12),0x06ca6351U); \
    ROUND(b,c,d,e,f,g,h,a,MSG(w15, w0, w8, w13),0x14292967U); \
    ROUND(a,b,c,d,e,f,g,h,MSG(w0, w1, w9, w14),0x27b70a85U); \
    ROUND(h,a,b,c,d,e,f,g,MSG(w1, w2, w10, w15),0x2e1b2138U); \
    ROUND(g,h,a,b,c,d,e,f,MSG(w2, w3, w11, w0),0x4d2c6dfcU); \
    ROUND(f,g,h,a,b,c,d,e,MSG(w3, w4, w12, w1),0x53380d13U); \
    ROUND(e,f,g,h,a,b,c,d,MSG(w4, w5, w13, w2),0x650a7354U); \
    ROUND(d,e,f,g,h,a,b,c,MSG(w5, w6, w14, w3),0x766a0abbU); \
    ROUND(c,d,e,f,g,h,a,b,MSG(w6, w7, w15, w4),0x81c2c92eU); \
    ROUND(b,c,d,e,f,g,h,a,MSG(w7, w8, w0, w5),0x92722c85U); \
    ROUND(a,b,c,d,e,f,g,h,MSG(w8, w9, w1, w6),0xa2bfe8a1U); \
    ROUND(h,a,b,c,d,e,f,g,MSG(w9, w10, w2, w7),0xa81a664bU); \
    ROUND(g,h,a,b,c,d,e,f,MSG(w10, w11, w3, w8),0xc24b8b70U); \
    ROUND(f,g,h,a,b,c,d,e,MSG(w11, w12, w4, w9),0xc76c51a3U); \
    ROUND(e,f,g,h,a,b,c,d,MSG(w12, w13, w5, w10),0xd192e819U); \
    ROUND(d,e,f,g,h,a,b,c,MSG(w13, w14, w6, w11),0xd6990624U); \
    ROUND(c,d,e,f,g,h,a,b,MSG(w14, w15, w7, w12),0xf40e3585U); \
    ROUND(b,c,d,e,f,g,h,a,MSG(w15, w0, w8, w13),0x106aa070U); \
    ROUND(a,b,c,d,e,f,g,h,MSG(w0, w1, w9, w14),0x19a4c116U); \
    ROUND(h,a,b,c,d,e,f,g,MSG(w1, w2, w10, w15),0x1e376c08U); \
    ROUND(g,h,a,b,c,d,e,f,MSG(w2, w3, w11, w0),0x2748774cU); \
    ROUND(f,g,h,a,b,c,d,e,MSG(w3, w4, w12, w1),0x34b0bcb5U); \
    ROUND(e,f,g,h,a,b,c,d,MSG(w4, w5, w13, w2),0x391c0cb3U); \
    ROUND(d,e,f,g,h,a,b,c,MSG(w5, w6, w14, w3),0x4ed8aa4aU); \
    ROUND(c,d,e,f,g,h,a,b,MSG(w6, w7, w15, w4),0x5b9cca4fU); \
    ROUND(b,c,d,e,f,g,h,a,MSG(w7, w8, w0, w5),0x682e6ff3U); \
    ROUND(a,b,c,d,e,f,g,h,MSG(w8, w9, w1, w6),0x748f82eeU); \
    ROUND(h,a,b,c,d,e,f,g,MSG(w9, w10, w2, w7),0x78a5636fU); \
    ROUND(g,h,a,b,c,d,e,f,MSG(w10, w11, w3, w8),0x84c87814U); \
    ROUND(f,g,h,a,b,c,d,e,MSG(w11, w12, w4, w9),0x8cc70208U); \
    ROUND(e,f,g,h,a,b,c,d,MSG(w12, w13, w5, w10),0x90befffaU); \
    ROUND(d,e,f,g,h,a,b,c,MSG(w13, w14, w6, w11),0xa4506cebU); \
    ROUND(c,d,e,f,g,h,a,b,MSG(w14, w15, w7, w12),0xbef9a3f7U); \
    ROUND(b,c,d,e,f,g,h,a,MSG(w15, w0, w8, w13),0xc67178f2U); \
    (s0)+=a; (s1)+=b; (s2)+=c; (s3)+=d; (s4)+=e; (s5)+=f; (s6)+=g; (s7)+=h; \
} while (0)

CUDA_DEVICE_INLINE void scan_one_nonce(u32 fast0, u32 fast1, u32 fast2, u32 fast3,
                                       u32 fast4, u32 fast5, u32 fast6, u32 fast7,
                                       u32 target0, u32 target1, u32 target2, u32 target3,
                                       u32 target4, u32 target5, u32 target6, u32 target7,
                                       u32 tail0, u32 tail1, u32 tail2, u32 nonce,
                                       u32 max_results,
                                       u32 *result_count,
                                       u32 *matches) {
    u32 s0=fast0, s1=fast1, s2=fast2, s3=fast3;
    u32 s4=fast4, s5=fast5, s6=fast6, s7=fast7;
    u32 w0=tail0, w1=tail1, w2=tail2, w3=bswap32(nonce);
    u32 w4=0x80000000U, w5=0U, w6=0U, w7=0U;
    u32 w8=0U, w9=0U, w10=0U, w11=0U;
    u32 w12=0U, w13=0U, w14=0U, w15=640U;

    SHA256_COMPRESS(s0,s1,s2,s3,s4,s5,s6,s7,
                    w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15);

    u32 h0=0x6a09e667U, h1=0xbb67ae85U, h2=0x3c6ef372U, h3=0xa54ff53aU;
    u32 h4=0x510e527fU, h5=0x9b05688cU, h6=0x1f83d9abU, h7=0x5be0cd19U;
    w0=s0; w1=s1; w2=s2; w3=s3; w4=s4; w5=s5; w6=s6; w7=s7;
    w8=0x80000000U; w9=0U; w10=0U; w11=0U;
    w12=0U; w13=0U; w14=0U; w15=256U;

    SHA256_COMPRESS(h0,h1,h2,h3,h4,h5,h6,h7,
                    w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15);

    if (meets_target_words(h0,h1,h2,h3,h4,h5,h6,h7,
                           target0,target1,target2,target3,target4,target5,target6,target7)) {
        store_match_words(nonce,h0,h1,h2,h3,h4,h5,h6,h7,max_results,result_count,matches);
    }
}

extern "C" __attribute__((global)) void btcrig_cuda_scan_nonce_range(
    u32 fast0, u32 fast1, u32 fast2, u32 fast3,
    u32 fast4, u32 fast5, u32 fast6, u32 fast7,
    u32 target0, u32 target1, u32 target2, u32 target3,
    u32 target4, u32 target5, u32 target6, u32 target7,
    u32 tail0, u32 tail1, u32 tail2,
    u32 start_nonce, u32 nonce_count,
    u32 nonces_per_thread,
    u32 max_results,
    u32 *result_count,
    u32 *matches) {
    u32 gid = cuda_ctaid_x() * cuda_ntid_x() + cuda_tid_x();
    u32 base_nonce = gid * nonces_per_thread;
    if (base_nonce >= nonce_count) {
        return;
    }
    u32 limit = nonces_per_thread;
    if (base_nonce + limit > nonce_count) {
        limit = nonce_count - base_nonce;
    }
    for (u32 item = 0U; item < limit; ++item) {
        scan_one_nonce(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                       target0, target1, target2, target3, target4, target5, target6, target7,
                       tail0, tail1, tail2, start_nonce + base_nonce + item,
                       max_results, result_count, matches);
    }
}

#undef SHA256_COMPRESS
#undef ROUND
#undef MSG
#undef MAJ
#undef CH
#undef BS1
#undef BS0
#undef SS1
#undef SS0
#undef CUDA_DEVICE_INLINE
