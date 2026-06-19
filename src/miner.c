#define _GNU_SOURCE

#include "miner.h"

#include "console.h"
#include "sha256d.h"

#include <errno.h>
#include <math.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MINER_BATCH_SIZE 262144U
#define SHARE_QUEUE_SIZE 256

static void miner_sleep_ms(unsigned int ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000U);
#endif
}

typedef struct {
    miner_job_t public_job;
    sha256_midstate_t midstate;
    uint32_t tail_words[4];
    uint32_t target_words[8];
    int valid;
} active_job_t;

struct miner {
    pthread_mutex_t lock;
    pthread_t *threads;
    int thread_count;
    int started;
    int stop;
    int paused;

    active_job_t job;
    uint64_t next_nonce;
    int nonce_exhausted;
    uint64_t hashes;
    uint64_t *thread_hashes;

    miner_share_t shares[SHARE_QUEUE_SIZE];
    int share_head;
    int share_tail;
    int share_count;
};

typedef struct {
    miner_t *miner;
    int id;
} worker_arg_t;

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if ((hex_len & 1U) != 0 || hex_len / 2 != out_len) {
        return -1;
    }

    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int hex_to_alloc(const char *hex, uint8_t **out, size_t *out_len) {
    size_t hex_len = strlen(hex);
    if ((hex_len & 1U) != 0) {
        return -1;
    }

    size_t len = hex_len / 2;
    uint8_t *buf = malloc(len == 0 ? 1 : len);
    if (buf == NULL) {
        return -1;
    }

    if (hex_to_bytes(hex, buf, len) != 0) {
        free(buf);
        return -1;
    }

    *out = buf;
    *out_len = len;
    return 0;
}

static int copy_checked(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) {
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

static void sha256d_data(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint8_t first[SHA256_DIGEST_LENGTH];
    SHA256(data, len, first);
    SHA256(first, SHA256_DIGEST_LENGTH, out);
}

static uint32_t load_le32(const uint8_t *p) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
#else
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
#endif
}

static uint32_t load_be32(const uint8_t *p) {
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return __builtin_bswap32(v);
#else
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
#endif
}

static uint32_t bswap32(uint32_t v) {
#if defined(__GNUC__)
    return __builtin_bswap32(v);
#else
    return ((v & 0x000000ffU) << 24) |
           ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) |
           ((v & 0xff000000U) >> 24);
#endif
}

static int hex_u32_to_le(const char *hex, uint8_t out[4]) {
    uint8_t tmp[4];
    if (hex_to_bytes(hex, tmp, sizeof(tmp)) != 0) {
        return -1;
    }
    out[0] = tmp[3];
    out[1] = tmp[2];
    out[2] = tmp[1];
    out[3] = tmp[0];
    return 0;
}

static int prevhash_to_header_bytes(const char *hex, uint8_t out[32]) {
    if (strlen(hex) != 64) {
        return -1;
    }

    for (int i = 0; i < 32; i += 4) {
        uint8_t tmp[4];
        char chunk[9];
        memcpy(chunk, hex + i * 2, 8);
        chunk[8] = '\0';
        if (hex_to_bytes(chunk, tmp, sizeof(tmp)) != 0) {
            return -1;
        }
        out[i + 0] = tmp[3];
        out[i + 1] = tmp[2];
        out[i + 2] = tmp[1];
        out[i + 3] = tmp[0];
    }
    return 0;
}

void miner_target_from_difficulty(double difficulty, uint8_t target[32]) {
    uint8_t be_target[32];
    memset(be_target, 0, sizeof(be_target));
    memset(target, 0, 32);

    if (difficulty <= 0.0) {
        difficulty = 1.0;
    }

    double mant = 65535.0 / difficulty;
    int exp = 29;
    while (mant >= 0x800000 && exp < 32) {
        mant /= 256.0;
        ++exp;
    }
    while (mant > 0.0 && mant < 0x8000 && exp > 3) {
        mant *= 256.0;
        --exp;
    }

    uint32_t m = (uint32_t)mant;
    int idx = 32 - exp;
    if (idx >= 0 && idx + 2 < 32) {
        be_target[idx] = (uint8_t)(m >> 16);
        be_target[idx + 1] = (uint8_t)(m >> 8);
        be_target[idx + 2] = (uint8_t)m;
        for (int i = 0; i < 32; ++i) {
            target[i] = be_target[31 - i];
        }
    } else if (idx < 0) {
        memset(target, 0xff, 32);
    }
}

int miner_hash_meets_target(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 31; i >= 0; --i) {
        if (hash[i] < target[i]) {
            return 1;
        }
        if (hash[i] > target[i]) {
            return 0;
        }
    }
    return 1;
}

static int hash_words_meet_target(const uint32_t hash_words[8], const uint32_t target_words[8]) {
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

double miner_hash_difficulty(const uint8_t hash[32]) {
    static const double diff1 =
        26959535291011309493156476344723991336010898738574164086137773096960.0;
    double value = 0.0;
    for (int i = 31; i >= 0; --i) {
        value = value * 256.0 + hash[i];
    }
    if (value <= 0.0) {
        return INFINITY;
    }
    return diff1 / value;
}

void miner_format_extranonce2(char *out, size_t out_size, int extranonce2_size, uint64_t value) {
    int chars = extranonce2_size * 2;
    if (chars <= 0 || (size_t)chars + 1 > out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    snprintf(out, out_size, "%0*llx", chars, (unsigned long long)value);
}

int miner_build_job(miner_job_t *out,
                    const char *job_id,
                    const char *prevhash,
                    const char *coinb1,
                    const char *coinb2,
                    const char **merkle,
                    int merkle_count,
                    const char *version,
                    const char *nbits,
                    const char *ntime,
                    const char *extranonce1,
                    const char *extranonce2,
                    double difficulty) {
    char *coinbase_hex = NULL;
    uint8_t *coinbase = NULL;
    size_t coinbase_len = 0;
    uint8_t merkle_root[32];
    int rc = -1;

    memset(out, 0, sizeof(*out));
    if (copy_checked(out->job_id, sizeof(out->job_id), job_id) != 0 ||
        copy_checked(out->extranonce2, sizeof(out->extranonce2), extranonce2) != 0 ||
        copy_checked(out->ntime, sizeof(out->ntime), ntime) != 0) {
        return -1;
    }

    size_t coinbase_hex_len = strlen(coinb1) + strlen(extranonce1) + strlen(extranonce2) + strlen(coinb2);
    coinbase_hex = malloc(coinbase_hex_len + 1);
    if (coinbase_hex == NULL) {
        return -1;
    }
    snprintf(coinbase_hex, coinbase_hex_len + 1, "%s%s%s%s", coinb1, extranonce1, extranonce2, coinb2);

    if (hex_to_alloc(coinbase_hex, &coinbase, &coinbase_len) != 0) {
        goto done;
    }
    sha256d_data(coinbase, coinbase_len, merkle_root);

    for (int i = 0; i < merkle_count; ++i) {
        uint8_t branch[32];
        uint8_t combined[64];
        if (hex_to_bytes(merkle[i], branch, sizeof(branch)) != 0) {
            goto done;
        }
        memcpy(combined, merkle_root, 32);
        memcpy(combined + 32, branch, 32);
        sha256d_data(combined, sizeof(combined), merkle_root);
    }

    uint8_t *p = out->header;
    if (hex_u32_to_le(version, p) != 0) {
        goto done;
    }
    p += 4;
    if (prevhash_to_header_bytes(prevhash, p) != 0) {
        goto done;
    }
    p += 32;
    memcpy(p, merkle_root, 32);
    p += 32;
    if (hex_u32_to_le(ntime, p) != 0) {
        goto done;
    }
    p += 4;
    if (hex_u32_to_le(nbits, p) != 0) {
        goto done;
    }
    p += 4;
    memset(p, 0, 4);

    miner_target_from_difficulty(difficulty, out->target);
    rc = 0;

done:
    free(coinbase);
    free(coinbase_hex);
    return rc;
}

static void queue_share_locked(miner_t *miner, const miner_job_t *job, uint32_t nonce, const uint8_t hash[32]) {
    if (miner->share_count >= SHARE_QUEUE_SIZE) {
        return;
    }

    miner_share_t *share = &miner->shares[miner->share_tail];
    memset(share, 0, sizeof(*share));
    share->seq = job->seq;
    share->nonce = nonce;
    share->difficulty = miner_hash_difficulty(hash);
    memcpy(share->hash, hash, 32);
    copy_checked(share->job_id, sizeof(share->job_id), job->job_id);
    copy_checked(share->extranonce2, sizeof(share->extranonce2), job->extranonce2);
    copy_checked(share->ntime, sizeof(share->ntime), job->ntime);

    miner->share_tail = (miner->share_tail + 1) % SHARE_QUEUE_SIZE;
    ++miner->share_count;
}

static int copy_job_and_nonce_range(miner_t *miner,
                                    active_job_t *job,
                                    uint32_t *start_nonce,
                                    uint32_t *count) {
    int ok = 0;
    pthread_mutex_lock(&miner->lock);
    if (miner->job.valid) {
        if (miner->next_nonce > UINT32_MAX) {
            miner->job.valid = 0;
            miner->nonce_exhausted = 1;
        } else {
            uint64_t remaining = (uint64_t)UINT32_MAX - miner->next_nonce + 1U;
            uint32_t range = remaining < MINER_BATCH_SIZE ? (uint32_t)remaining : MINER_BATCH_SIZE;
            *job = miner->job;
            *start_nonce = (uint32_t)miner->next_nonce;
            *count = range;
            miner->next_nonce += range;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&miner->lock);
    return ok;
}

static void *worker_main(void *opaque) {
    worker_arg_t *arg = (worker_arg_t *)opaque;
    miner_t *miner = arg->miner;
    int id = arg->id;
    uint8_t hash[32];
    uint32_t tail_words[4];
    uint32_t hash_words[8];
    sha256d_tail_words_func_t hash_tail_words = sha256d_tail_words_func();

    free(arg);

    for (;;) {
        pthread_mutex_lock(&miner->lock);
        int stop = miner->stop;
        int paused = miner->paused;
        pthread_mutex_unlock(&miner->lock);
        if (stop) {
            break;
        }
        if (paused) {
            miner_sleep_ms(100);
            continue;
        }

        active_job_t job;
        uint32_t start_nonce = 0;
        uint32_t nonce_count = 0;
        if (!copy_job_and_nonce_range(miner, &job, &start_nonce, &nonce_count)) {
            miner_sleep_ms(100);
            continue;
        }

        memcpy(tail_words, job.tail_words, sizeof(tail_words));
        uint64_t local_hashes = 0;
        for (uint32_t i = 0; i < nonce_count; ++i) {
            uint32_t nonce = start_nonce + i;
            tail_words[3] = bswap32(nonce);
            hash_tail_words(&job.midstate, tail_words, hash_words);
            ++local_hashes;

            if (hash_words_meet_target(hash_words, job.target_words)) {
                sha256d_words_to_hash(hash_words, hash);
                pthread_mutex_lock(&miner->lock);
                if (miner->job.valid && miner->job.public_job.seq == job.public_job.seq) {
                    queue_share_locked(miner, &job.public_job, nonce, hash);
                }
                pthread_mutex_unlock(&miner->lock);
            }
        }

        pthread_mutex_lock(&miner->lock);
        miner->hashes += local_hashes;
        if (id >= 0 && id < miner->thread_count) {
            miner->thread_hashes[id] += local_hashes;
        }
        pthread_mutex_unlock(&miner->lock);
    }

    return NULL;
}

miner_t *miner_create(int thread_count) {
    if (thread_count <= 0) {
        thread_count = 1;
    }

    miner_t *miner = calloc(1, sizeof(*miner));
    if (miner == NULL) {
        return NULL;
    }

    miner->threads = calloc((size_t)thread_count, sizeof(*miner->threads));
    miner->thread_hashes = calloc((size_t)thread_count, sizeof(*miner->thread_hashes));
    if (miner->threads == NULL || miner->thread_hashes == NULL) {
        free(miner->thread_hashes);
        free(miner->threads);
        free(miner);
        return NULL;
    }

    pthread_mutex_init(&miner->lock, NULL);
    miner->thread_count = thread_count;
    miner->next_nonce = 0;
    return miner;
}

void miner_destroy(miner_t *miner) {
    if (miner == NULL) {
        return;
    }
    miner_stop(miner);
    pthread_mutex_destroy(&miner->lock);
    free(miner->thread_hashes);
    free(miner->threads);
    free(miner);
}

int miner_start(miner_t *miner) {
    if (miner == NULL || miner->started) {
        return 0;
    }

    for (int i = 0; i < miner->thread_count; ++i) {
        worker_arg_t *arg = malloc(sizeof(*arg));
        if (arg == NULL) {
            return -1;
        }
        arg->miner = miner;
        arg->id = i;
        if (pthread_create(&miner->threads[i], NULL, worker_main, arg) != 0) {
            free(arg);
            return -1;
        }
    }

    miner->started = 1;
    printf("%s[MINER]%s started threads=%s%d%s affinity=%soff%s\n",
           C_MAGENTA, C_RESET, C_BRIGHT_GREEN, miner->thread_count, C_RESET, C_GRAY, C_RESET);
    return 0;
}

void miner_stop(miner_t *miner) {
    if (miner == NULL || !miner->started) {
        return;
    }

    pthread_mutex_lock(&miner->lock);
    miner->stop = 1;
    pthread_mutex_unlock(&miner->lock);

    for (int i = 0; i < miner->thread_count; ++i) {
        pthread_join(miner->threads[i], NULL);
    }
    miner->started = 0;
}

void miner_set_job(miner_t *miner, const miner_job_t *job) {
    if (miner == NULL || job == NULL) {
        return;
    }

    active_job_t active;
    memset(&active, 0, sizeof(active));
    active.public_job = *job;
    active.valid = 1;
    sha256d_80_midstate_prepare(&active.midstate, active.public_job.header);
    for (int i = 0; i < 4; ++i) {
        active.tail_words[i] = load_be32(active.public_job.header + 64 + i * 4);
    }
    for (int i = 0; i < 8; ++i) {
        active.target_words[i] = load_le32(active.public_job.target + i * 4);
    }

    pthread_mutex_lock(&miner->lock);
    active.public_job.seq = miner->job.public_job.seq + 1;
    miner->job = active;
    miner->next_nonce = 0;
    miner->nonce_exhausted = 0;
    miner->share_head = 0;
    miner->share_tail = 0;
    miner->share_count = 0;
    pthread_mutex_unlock(&miner->lock);

    printf("%s[MINER]%s job=%s%s%s seq=%llu en2=%s\n",
           C_MAGENTA,
           C_RESET,
           C_BRIGHT_YELLOW,
           active.public_job.job_id,
           C_RESET,
           (unsigned long long)active.public_job.seq,
           active.public_job.extranonce2);
}

void miner_set_paused(miner_t *miner, int paused) {
    if (miner == NULL) {
        return;
    }

    pthread_mutex_lock(&miner->lock);
    miner->paused = paused ? 1 : 0;
    pthread_mutex_unlock(&miner->lock);
}

int miner_is_paused(miner_t *miner) {
    if (miner == NULL) {
        return 0;
    }

    pthread_mutex_lock(&miner->lock);
    int paused = miner->paused;
    pthread_mutex_unlock(&miner->lock);
    return paused;
}

int miner_take_nonce_exhausted(miner_t *miner) {
    if (miner == NULL) {
        return 0;
    }

    pthread_mutex_lock(&miner->lock);
    int exhausted = miner->nonce_exhausted;
    miner->nonce_exhausted = 0;
    pthread_mutex_unlock(&miner->lock);
    return exhausted;
}

int miner_pop_share(miner_t *miner, miner_share_t *share) {
    int found = 0;
    pthread_mutex_lock(&miner->lock);
    if (miner->share_count > 0) {
        *share = miner->shares[miner->share_head];
        miner->share_head = (miner->share_head + 1) % SHARE_QUEUE_SIZE;
        --miner->share_count;
        found = 1;
    }
    pthread_mutex_unlock(&miner->lock);
    return found;
}

uint64_t miner_hashes(miner_t *miner) {
    uint64_t hashes;
    pthread_mutex_lock(&miner->lock);
    hashes = miner->hashes;
    pthread_mutex_unlock(&miner->lock);
    return hashes;
}

int miner_thread_count(miner_t *miner) {
    if (miner == NULL) {
        return 0;
    }

    pthread_mutex_lock(&miner->lock);
    int count = miner->thread_count;
    pthread_mutex_unlock(&miner->lock);
    return count;
}

int miner_snapshot_thread_hashes(miner_t *miner, uint64_t *out, int max_count) {
    if (miner == NULL || out == NULL || max_count <= 0) {
        return 0;
    }

    pthread_mutex_lock(&miner->lock);
    int count = miner->thread_count;
    if (count > max_count) {
        count = max_count;
    }
    for (int i = 0; i < count; ++i) {
        out[i] = miner->thread_hashes[i];
    }
    pthread_mutex_unlock(&miner->lock);

    return count;
}
