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

CUDA_DEVICE_INLINE u32 ch_lop3(u32 x, u32 y, u32 z) {
    u32 out;
    asm volatile("lop3.b32 %0, %1, %2, %3, 0xca;" : "=r"(out) : "r"(x), "r"(y), "r"(z));
    return out;
}

CUDA_DEVICE_INLINE u32 maj_lop3(u32 x, u32 y, u32 z) {
    u32 out;
    asm volatile("lop3.b32 %0, %1, %2, %3, 0xe8;" : "=r"(out) : "r"(x), "r"(y), "r"(z));
    return out;
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

#define ROUND_PAIR(a,b,c,d,e,f,g,h,w,aa,bb,cc,dd,ee,ff,gg,hh,ww,k) do { \
    ROUND(a,b,c,d,e,f,g,h,w,k); \
    ROUND(aa,bb,cc,dd,ee,ff,gg,hh,ww,k); \
} while (0)

#define SHA256_COMPRESS_DUAL(s0,s1,s2,s3,s4,s5,s6,s7,w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15, \
                             q0,q1,q2,q3,q4,q5,q6,q7,x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,x12,x13,x14,x15) do { \
    u32 a=(s0), b=(s1), c=(s2), d=(s3), e=(s4), f=(s5), g=(s6), h=(s7); \
    u32 aa=(q0), bb=(q1), cc=(q2), dd=(q3), ee=(q4), ff=(q5), gg=(q6), hh=(q7); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,w0,aa,bb,cc,dd,ee,ff,gg,hh,x0,0x428a2f98U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,w1,hh,aa,bb,cc,dd,ee,ff,gg,x1,0x71374491U); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,w2,gg,hh,aa,bb,cc,dd,ee,ff,x2,0xb5c0fbcfU); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,w3,ff,gg,hh,aa,bb,cc,dd,ee,x3,0xe9b5dba5U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,w4,ee,ff,gg,hh,aa,bb,cc,dd,x4,0x3956c25bU); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,w5,dd,ee,ff,gg,hh,aa,bb,cc,x5,0x59f111f1U); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,w6,cc,dd,ee,ff,gg,hh,aa,bb,x6,0x923f82a4U); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,w7,bb,cc,dd,ee,ff,gg,hh,aa,x7,0xab1c5ed5U); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,w8,aa,bb,cc,dd,ee,ff,gg,hh,x8,0xd807aa98U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,w9,hh,aa,bb,cc,dd,ee,ff,gg,x9,0x12835b01U); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,w10,gg,hh,aa,bb,cc,dd,ee,ff,x10,0x243185beU); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,w11,ff,gg,hh,aa,bb,cc,dd,ee,x11,0x550c7dc3U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,w12,ee,ff,gg,hh,aa,bb,cc,dd,x12,0x72be5d74U); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,w13,dd,ee,ff,gg,hh,aa,bb,cc,x13,0x80deb1feU); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,w14,cc,dd,ee,ff,gg,hh,aa,bb,x14,0x9bdc06a7U); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,w15,bb,cc,dd,ee,ff,gg,hh,aa,x15,0xc19bf174U); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,MSG(w0, w1, w9, w14),aa,bb,cc,dd,ee,ff,gg,hh,MSG(x0, x1, x9, x14),0xe49b69c1U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,MSG(w1, w2, w10, w15),hh,aa,bb,cc,dd,ee,ff,gg,MSG(x1, x2, x10, x15),0xefbe4786U); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,MSG(w2, w3, w11, w0),gg,hh,aa,bb,cc,dd,ee,ff,MSG(x2, x3, x11, x0),0x0fc19dc6U); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,MSG(w3, w4, w12, w1),ff,gg,hh,aa,bb,cc,dd,ee,MSG(x3, x4, x12, x1),0x240ca1ccU); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,MSG(w4, w5, w13, w2),ee,ff,gg,hh,aa,bb,cc,dd,MSG(x4, x5, x13, x2),0x2de92c6fU); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,MSG(w5, w6, w14, w3),dd,ee,ff,gg,hh,aa,bb,cc,MSG(x5, x6, x14, x3),0x4a7484aaU); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,MSG(w6, w7, w15, w4),cc,dd,ee,ff,gg,hh,aa,bb,MSG(x6, x7, x15, x4),0x5cb0a9dcU); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,MSG(w7, w8, w0, w5),bb,cc,dd,ee,ff,gg,hh,aa,MSG(x7, x8, x0, x5),0x76f988daU); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,MSG(w8, w9, w1, w6),aa,bb,cc,dd,ee,ff,gg,hh,MSG(x8, x9, x1, x6),0x983e5152U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,MSG(w9, w10, w2, w7),hh,aa,bb,cc,dd,ee,ff,gg,MSG(x9, x10, x2, x7),0xa831c66dU); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,MSG(w10, w11, w3, w8),gg,hh,aa,bb,cc,dd,ee,ff,MSG(x10, x11, x3, x8),0xb00327c8U); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,MSG(w11, w12, w4, w9),ff,gg,hh,aa,bb,cc,dd,ee,MSG(x11, x12, x4, x9),0xbf597fc7U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,MSG(w12, w13, w5, w10),ee,ff,gg,hh,aa,bb,cc,dd,MSG(x12, x13, x5, x10),0xc6e00bf3U); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,MSG(w13, w14, w6, w11),dd,ee,ff,gg,hh,aa,bb,cc,MSG(x13, x14, x6, x11),0xd5a79147U); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,MSG(w14, w15, w7, w12),cc,dd,ee,ff,gg,hh,aa,bb,MSG(x14, x15, x7, x12),0x06ca6351U); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,MSG(w15, w0, w8, w13),bb,cc,dd,ee,ff,gg,hh,aa,MSG(x15, x0, x8, x13),0x14292967U); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,MSG(w0, w1, w9, w14),aa,bb,cc,dd,ee,ff,gg,hh,MSG(x0, x1, x9, x14),0x27b70a85U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,MSG(w1, w2, w10, w15),hh,aa,bb,cc,dd,ee,ff,gg,MSG(x1, x2, x10, x15),0x2e1b2138U); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,MSG(w2, w3, w11, w0),gg,hh,aa,bb,cc,dd,ee,ff,MSG(x2, x3, x11, x0),0x4d2c6dfcU); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,MSG(w3, w4, w12, w1),ff,gg,hh,aa,bb,cc,dd,ee,MSG(x3, x4, x12, x1),0x53380d13U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,MSG(w4, w5, w13, w2),ee,ff,gg,hh,aa,bb,cc,dd,MSG(x4, x5, x13, x2),0x650a7354U); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,MSG(w5, w6, w14, w3),dd,ee,ff,gg,hh,aa,bb,cc,MSG(x5, x6, x14, x3),0x766a0abbU); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,MSG(w6, w7, w15, w4),cc,dd,ee,ff,gg,hh,aa,bb,MSG(x6, x7, x15, x4),0x81c2c92eU); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,MSG(w7, w8, w0, w5),bb,cc,dd,ee,ff,gg,hh,aa,MSG(x7, x8, x0, x5),0x92722c85U); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,MSG(w8, w9, w1, w6),aa,bb,cc,dd,ee,ff,gg,hh,MSG(x8, x9, x1, x6),0xa2bfe8a1U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,MSG(w9, w10, w2, w7),hh,aa,bb,cc,dd,ee,ff,gg,MSG(x9, x10, x2, x7),0xa81a664bU); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,MSG(w10, w11, w3, w8),gg,hh,aa,bb,cc,dd,ee,ff,MSG(x10, x11, x3, x8),0xc24b8b70U); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,MSG(w11, w12, w4, w9),ff,gg,hh,aa,bb,cc,dd,ee,MSG(x11, x12, x4, x9),0xc76c51a3U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,MSG(w12, w13, w5, w10),ee,ff,gg,hh,aa,bb,cc,dd,MSG(x12, x13, x5, x10),0xd192e819U); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,MSG(w13, w14, w6, w11),dd,ee,ff,gg,hh,aa,bb,cc,MSG(x13, x14, x6, x11),0xd6990624U); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,MSG(w14, w15, w7, w12),cc,dd,ee,ff,gg,hh,aa,bb,MSG(x14, x15, x7, x12),0xf40e3585U); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,MSG(w15, w0, w8, w13),bb,cc,dd,ee,ff,gg,hh,aa,MSG(x15, x0, x8, x13),0x106aa070U); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,MSG(w0, w1, w9, w14),aa,bb,cc,dd,ee,ff,gg,hh,MSG(x0, x1, x9, x14),0x19a4c116U); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,MSG(w1, w2, w10, w15),hh,aa,bb,cc,dd,ee,ff,gg,MSG(x1, x2, x10, x15),0x1e376c08U); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,MSG(w2, w3, w11, w0),gg,hh,aa,bb,cc,dd,ee,ff,MSG(x2, x3, x11, x0),0x2748774cU); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,MSG(w3, w4, w12, w1),ff,gg,hh,aa,bb,cc,dd,ee,MSG(x3, x4, x12, x1),0x34b0bcb5U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,MSG(w4, w5, w13, w2),ee,ff,gg,hh,aa,bb,cc,dd,MSG(x4, x5, x13, x2),0x391c0cb3U); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,MSG(w5, w6, w14, w3),dd,ee,ff,gg,hh,aa,bb,cc,MSG(x5, x6, x14, x3),0x4ed8aa4aU); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,MSG(w6, w7, w15, w4),cc,dd,ee,ff,gg,hh,aa,bb,MSG(x6, x7, x15, x4),0x5b9cca4fU); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,MSG(w7, w8, w0, w5),bb,cc,dd,ee,ff,gg,hh,aa,MSG(x7, x8, x0, x5),0x682e6ff3U); \
    ROUND_PAIR(a,b,c,d,e,f,g,h,MSG(w8, w9, w1, w6),aa,bb,cc,dd,ee,ff,gg,hh,MSG(x8, x9, x1, x6),0x748f82eeU); \
    ROUND_PAIR(h,a,b,c,d,e,f,g,MSG(w9, w10, w2, w7),hh,aa,bb,cc,dd,ee,ff,gg,MSG(x9, x10, x2, x7),0x78a5636fU); \
    ROUND_PAIR(g,h,a,b,c,d,e,f,MSG(w10, w11, w3, w8),gg,hh,aa,bb,cc,dd,ee,ff,MSG(x10, x11, x3, x8),0x84c87814U); \
    ROUND_PAIR(f,g,h,a,b,c,d,e,MSG(w11, w12, w4, w9),ff,gg,hh,aa,bb,cc,dd,ee,MSG(x11, x12, x4, x9),0x8cc70208U); \
    ROUND_PAIR(e,f,g,h,a,b,c,d,MSG(w12, w13, w5, w10),ee,ff,gg,hh,aa,bb,cc,dd,MSG(x12, x13, x5, x10),0x90befffaU); \
    ROUND_PAIR(d,e,f,g,h,a,b,c,MSG(w13, w14, w6, w11),dd,ee,ff,gg,hh,aa,bb,cc,MSG(x13, x14, x6, x11),0xa4506cebU); \
    ROUND_PAIR(c,d,e,f,g,h,a,b,MSG(w14, w15, w7, w12),cc,dd,ee,ff,gg,hh,aa,bb,MSG(x14, x15, x7, x12),0xbef9a3f7U); \
    ROUND_PAIR(b,c,d,e,f,g,h,a,MSG(w15, w0, w8, w13),bb,cc,dd,ee,ff,gg,hh,aa,MSG(x15, x0, x8, x13),0xc67178f2U); \
    (s0)+=a; (s1)+=b; (s2)+=c; (s3)+=d; (s4)+=e; (s5)+=f; (s6)+=g; (s7)+=h; \
    (q0)+=aa; (q1)+=bb; (q2)+=cc; (q3)+=dd; (q4)+=ee; (q5)+=ff; (q6)+=gg; (q7)+=hh; \
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

CUDA_DEVICE_INLINE void scan_two_nonces(u32 fast0, u32 fast1, u32 fast2, u32 fast3,
                                        u32 fast4, u32 fast5, u32 fast6, u32 fast7,
                                        u32 target0, u32 target1, u32 target2, u32 target3,
                                        u32 target4, u32 target5, u32 target6, u32 target7,
                                        u32 tail0, u32 tail1, u32 tail2,
                                        u32 nonce0, u32 nonce1,
                                        u32 max_results,
                                        u32 *result_count,
                                        u32 *matches) {
    u32 s0=fast0, s1=fast1, s2=fast2, s3=fast3;
    u32 s4=fast4, s5=fast5, s6=fast6, s7=fast7;
    u32 q0=fast0, q1=fast1, q2=fast2, q3=fast3;
    u32 q4=fast4, q5=fast5, q6=fast6, q7=fast7;
    u32 w0=tail0, w1=tail1, w2=tail2, w3=bswap32(nonce0);
    u32 w4=0x80000000U, w5=0U, w6=0U, w7=0U;
    u32 w8=0U, w9=0U, w10=0U, w11=0U;
    u32 w12=0U, w13=0U, w14=0U, w15=640U;
    u32 x0=tail0, x1=tail1, x2=tail2, x3=bswap32(nonce1);
    u32 x4=0x80000000U, x5=0U, x6=0U, x7=0U;
    u32 x8=0U, x9=0U, x10=0U, x11=0U;
    u32 x12=0U, x13=0U, x14=0U, x15=640U;

    SHA256_COMPRESS_DUAL(s0,s1,s2,s3,s4,s5,s6,s7,
                         w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,
                         q0,q1,q2,q3,q4,q5,q6,q7,
                         x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,x12,x13,x14,x15);

    u32 h0=0x6a09e667U, h1=0xbb67ae85U, h2=0x3c6ef372U, h3=0xa54ff53aU;
    u32 h4=0x510e527fU, h5=0x9b05688cU, h6=0x1f83d9abU, h7=0x5be0cd19U;
    u32 y0=0x6a09e667U, y1=0xbb67ae85U, y2=0x3c6ef372U, y3=0xa54ff53aU;
    u32 y4=0x510e527fU, y5=0x9b05688cU, y6=0x1f83d9abU, y7=0x5be0cd19U;
    w0=s0; w1=s1; w2=s2; w3=s3; w4=s4; w5=s5; w6=s6; w7=s7;
    w8=0x80000000U; w9=0U; w10=0U; w11=0U;
    w12=0U; w13=0U; w14=0U; w15=256U;
    x0=q0; x1=q1; x2=q2; x3=q3; x4=q4; x5=q5; x6=q6; x7=q7;
    x8=0x80000000U; x9=0U; x10=0U; x11=0U;
    x12=0U; x13=0U; x14=0U; x15=256U;

    SHA256_COMPRESS_DUAL(h0,h1,h2,h3,h4,h5,h6,h7,
                         w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,
                         y0,y1,y2,y3,y4,y5,y6,y7,
                         x0,x1,x2,x3,x4,x5,x6,x7,x8,x9,x10,x11,x12,x13,x14,x15);

    if (meets_target_words(h0,h1,h2,h3,h4,h5,h6,h7,
                           target0,target1,target2,target3,target4,target5,target6,target7)) {
        store_match_words(nonce0,h0,h1,h2,h3,h4,h5,h6,h7,max_results,result_count,matches);
    }
    if (meets_target_words(y0,y1,y2,y3,y4,y5,y6,y7,
                           target0,target1,target2,target3,target4,target5,target6,target7)) {
        store_match_words(nonce1,y0,y1,y2,y3,y4,y5,y6,y7,max_results,result_count,matches);
    }
}

#undef CH
#undef MAJ
#define CH(x,y,z) ch_lop3((x), (y), (z))
#define MAJ(x,y,z) maj_lop3((x), (y), (z))

CUDA_DEVICE_INLINE void scan_one_nonce_lop3(u32 fast0, u32 fast1, u32 fast2, u32 fast3,
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

#undef CH
#undef MAJ
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

extern "C" __attribute__((global)) void btcrig_cuda_scan_nonce_range_dual(
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
    u32 item = 0U;
    for (; item + 1U < limit; item += 2U) {
        scan_two_nonces(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                        target0, target1, target2, target3, target4, target5, target6, target7,
                        tail0, tail1, tail2,
                        start_nonce + base_nonce + item,
                        start_nonce + base_nonce + item + 1U,
                        max_results, result_count, matches);
    }
    if (item < limit) {
        scan_one_nonce(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                       target0, target1, target2, target3, target4, target5, target6, target7,
                       tail0, tail1, tail2, start_nonce + base_nonce + item,
                       max_results, result_count, matches);
    }
}

extern "C" __attribute__((global)) void btcrig_cuda_scan_nonce_range_fixed_npt1(
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
    (void)nonces_per_thread;
    u32 gid = cuda_ctaid_x() * cuda_ntid_x() + cuda_tid_x();
    if (gid >= nonce_count) {
        return;
    }
    scan_one_nonce(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                   target0, target1, target2, target3, target4, target5, target6, target7,
                   tail0, tail1, tail2, start_nonce + gid,
                   max_results, result_count, matches);
}

extern "C" __attribute__((global)) void btcrig_cuda_scan_nonce_range_fixed_npt2(
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
    (void)nonces_per_thread;
    u32 gid = cuda_ctaid_x() * cuda_ntid_x() + cuda_tid_x();
    u32 base_nonce = gid * 2U;
    if (base_nonce >= nonce_count) {
        return;
    }
    u32 limit = nonce_count - base_nonce;
    if (limit > 2U) {
        limit = 2U;
    }
    #pragma clang loop unroll(disable)
    for (u32 item = 0U; item < limit; ++item) {
        scan_one_nonce(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                       target0, target1, target2, target3, target4, target5, target6, target7,
                       tail0, tail1, tail2, start_nonce + base_nonce + item,
                       max_results, result_count, matches);
    }
}

extern "C" __attribute__((global)) void btcrig_cuda_scan_nonce_range_fixed_npt4(
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
    (void)nonces_per_thread;
    u32 gid = cuda_ctaid_x() * cuda_ntid_x() + cuda_tid_x();
    u32 base_nonce = gid * 4U;
    if (base_nonce >= nonce_count) {
        return;
    }
    u32 limit = nonce_count - base_nonce;
    if (limit > 4U) {
        limit = 4U;
    }
    #pragma clang loop unroll(disable)
    for (u32 item = 0U; item < limit; ++item) {
        scan_one_nonce(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                       target0, target1, target2, target3, target4, target5, target6, target7,
                       tail0, tail1, tail2, start_nonce + base_nonce + item,
                       max_results, result_count, matches);
    }
}

extern "C" __attribute__((global)) void btcrig_cuda_scan_nonce_range_lop3(
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
        scan_one_nonce_lop3(fast0, fast1, fast2, fast3, fast4, fast5, fast6, fast7,
                            target0, target1, target2, target3, target4, target5, target6, target7,
                            tail0, tail1, tail2, start_nonce + base_nonce + item,
                            max_results, result_count, matches);
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

#undef SHA256_COMPRESS_DUAL
#undef ROUND_PAIR
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
