#define _GNU_SOURCE

#include "console.h"
#include "cpu_info.h"
#include "sha256d.h"
#include "btcrig_version.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int id;
    int thread_count;
    int seconds;
    sha256d_backend_t backend;
    uint64_t hashes;
    uint8_t sink;
} worker_arg_t;

typedef struct {
    uint8_t header[80];
    uint32_t expected_nonce;
    size_t seen;
    int failed;
} range_self_check_t;

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void store_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

static void make_test_header(uint8_t header[80]) {
    memset(header, 0, 80);

    header[0] = 0x01;
    header[68] = 0xff;
    header[69] = 0xff;
    header[70] = 0x00;
    header[71] = 0x1d;
}

static void range_self_check_match(void *opaque, uint32_t nonce, const uint32_t hash_words[8]) {
    range_self_check_t *ctx = (range_self_check_t *)opaque;
    uint8_t full[32];
    uint8_t mid[32];

    if (ctx == NULL) {
        return;
    }
    if (nonce != ctx->expected_nonce + (uint32_t)ctx->seen) {
        ctx->failed = 1;
        return;
    }

    store_le32(&ctx->header[76], nonce);
    sha256d_80(ctx->header, full);
    sha256d_words_to_hash(hash_words, mid);
    if (memcmp(full, mid, sizeof(full)) != 0) {
        ctx->failed = 1;
        return;
    }
    ++ctx->seen;
}

static void bench_scan_match(void *opaque, uint32_t nonce, const uint32_t hash_words[8]) {
    uint8_t *sink = (uint8_t *)opaque;
    if (sink != NULL) {
        *sink ^= (uint8_t)(hash_words[0] ^ nonce);
    }
}

static int sha256d_self_check_backend(sha256d_backend_t backend) {
    const uint32_t nonces[] = {0, 1, 0x13579bdfU, 0xffffffffU};
    uint8_t header[80];
    uint8_t full[32];
    uint8_t mid[32];
    sha256_midstate_t midstate;
    uint32_t tail_words[4];
    uint32_t target_words[8];

    if (sha256d_set_backend(backend) != 0) {
        return -1;
    }
    make_test_header(header);
    sha256d_80_midstate_prepare(&midstate, header);

    for (size_t i = 0; i < sizeof(nonces) / sizeof(nonces[0]); ++i) {
        store_le32(&header[76], nonces[i]);
        sha256d_80(header, full);
        sha256d_80_midstate_hash(&midstate, &header[64], mid);
        if (memcmp(full, mid, sizeof(full)) != 0) {
            fprintf(stderr,
                    "sha256d self-check failed backend=%s nonce=%08x\n",
                    sha256d_backend_name(backend),
                    nonces[i]);
            return -1;
        }
    }

    for (int i = 0; i < 4; ++i) {
        tail_words[i] = load_be32(header + 64 + i * 4);
    }
    for (int i = 0; i < 8; ++i) {
        target_words[i] = 0xffffffffU;
    }

    range_self_check_t range_check;
    memset(&range_check, 0, sizeof(range_check));
    memcpy(range_check.header, header, sizeof(range_check.header));
    range_check.expected_nonce = 0;
    sha256d_nonce_range_func()(&midstate, tail_words, target_words, 0, 4,
                               &range_check, range_self_check_match);
    if (range_check.failed || range_check.seen != 4) {
        fprintf(stderr,
                "sha256d range self-check failed backend=%s seen=%zu\n",
                sha256d_backend_name(backend),
                range_check.seen);
        return -1;
    }

    return 0;
}

static int sha256d_self_check(void) {
    if (sha256d_self_check_backend(SHA256D_BACKEND_OPENSSL) != 0) {
        return -1;
    }
    if (sha256d_self_check_backend(SHA256D_BACKEND_FAST_C) != 0) {
        return -1;
    }
    if (sha256d_backend_available(SHA256D_BACKEND_ARM_SHA2) &&
        sha256d_self_check_backend(SHA256D_BACKEND_ARM_SHA2) != 0) {
        return -1;
    }
    if (sha256d_backend_available(SHA256D_BACKEND_X86_SHA_NI) &&
        sha256d_self_check_backend(SHA256D_BACKEND_X86_SHA_NI) != 0) {
        return -1;
    }
    return 0;
}

static void *worker_main(void *opaque) {
    worker_arg_t *arg = (worker_arg_t *)opaque;
    uint8_t header[80];
    uint32_t tail_words[4];
    uint32_t target_words[8] = {0};
    sha256_midstate_t midstate;
    uint32_t nonce = (uint32_t)arg->id * 4096U;
    uint64_t hashes = 0;
    uint8_t sink = 0;
    const double deadline = monotonic_seconds() + (double)arg->seconds;
    sha256d_nonce_range_func_t scan_nonce_range = NULL;

    (void)sha256d_set_backend(arg->backend);
    scan_nonce_range = sha256d_nonce_range_func();
    make_test_header(header);
    sha256d_80_midstate_prepare(&midstate, header);
    for (int i = 0; i < 4; ++i) {
        tail_words[i] = load_be32(header + 64 + i * 4);
    }

    while (monotonic_seconds() < deadline) {
        scan_nonce_range(&midstate, tail_words, target_words, nonce, 4096, &sink, bench_scan_match);
        sink ^= (uint8_t)nonce;
        nonce += 4096U * (uint32_t)arg->thread_count;
        hashes += 4096;
    }

    arg->hashes = hashes;
    arg->sink = sink;
    return NULL;
}

static int parse_positive_int(const char *text, int fallback) {
    if (text == NULL || *text == '\0') {
        return fallback;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 1024) {
        return fallback;
    }
    return (int)value;
}

static int configure_backend_from_env(sha256d_backend_t *backend) {
    const char *text = getenv("BTC_MINER_SHA_BACKEND");
    if (text == NULL || text[0] == '\0' || strcmp(text, "auto") == 0) {
        return 0;
    }
    if (sha256d_parse_backend(text, backend) != 0) {
        fprintf(stderr, "unknown BTC_MINER_SHA_BACKEND: %s\n", text);
        return -1;
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [-t threads] [-s seconds] [--backend openssl|fast-c|arm-sha2|x86-sha-ni] [--all] [--cpu-info] [--version]\n",
            argv0);
}

static int run_bench(int threads, int seconds, sha256d_backend_t backend) {
    if (sha256d_self_check_backend(backend) != 0) {
        return 1;
    }

    if (sha256d_set_backend(backend) != 0) {
        fprintf(stderr, "invalid backend\n");
        return 2;
    }

    pthread_t *workers = calloc((size_t)threads, sizeof(*workers));
    worker_arg_t *args = calloc((size_t)threads, sizeof(*args));
    if (workers == NULL || args == NULL) {
        fprintf(stderr, "allocation failed\n");
        free(workers);
        free(args);
        return 1;
    }

    const double start = monotonic_seconds();
    for (int i = 0; i < threads; ++i) {
        args[i].id = i;
        args[i].thread_count = threads;
        args[i].seconds = seconds;
        args[i].backend = backend;
        if (pthread_create(&workers[i], NULL, worker_main, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed for worker %d\n", i);
            threads = i;
            break;
        }
    }

    uint64_t total = 0;
    uint8_t sink = 0;
    for (int i = 0; i < threads; ++i) {
        pthread_join(workers[i], NULL);
        total += args[i].hashes;
        sink ^= args[i].sink;
    }
    const double elapsed = monotonic_seconds() - start;
    const double rate = elapsed > 0.0 ? (double)total / elapsed : 0.0;

    printf("%s[BENCH]%s threads=%d seconds=%d elapsed=%.3f\n",
           C_BRIGHT_CYAN, C_RESET, threads, seconds, elapsed);
    printf("%s[BENCH]%s backend=%s affinity=off\n",
           C_BRIGHT_CYAN, C_RESET, sha256d_backend_name(backend));
    printf("%s[BENCH]%s sha256d_midstate_hashes=%" PRIu64 " rate=%s%.3f MH/s%s sink=%02x\n",
           C_BRIGHT_CYAN, C_RESET, total, C_BRIGHT_GREEN, rate / 1000000.0, C_RESET, sink);
    printf("%s[BENCH]%s note=midstate saves the constant first 64-byte header block\n",
           C_GRAY, C_RESET);

    free(workers);
    free(args);
    return 0;
}

int main(int argc, char **argv) {
    int threads = cpu_info_recommended_threads();
    int seconds = 5;
    int run_all = 0;
    sha256d_backend_t backend = SHA256D_BACKEND_OPENSSL;

    console_init();

    if (threads <= 0) {
        threads = 1;
    }

    if (sha256d_self_check() != 0) {
        return 1;
    }
    backend = sha256d_auto_backend();
    if (configure_backend_from_env(&backend) != 0) {
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            threads = parse_positive_int(argv[++i], threads);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seconds = parse_positive_int(argv[++i], seconds);
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            if (sha256d_parse_backend(argv[++i], &backend) != 0) {
                fprintf(stderr, "unknown backend: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--all") == 0) {
            run_all = 1;
        } else if (strcmp(argv[i], "--cpu-info") == 0) {
            cpu_info_t info;
            cpu_info_detect(&info);
            cpu_info_print(&info);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s bench %s\n", BTCRIG_NAME, BTCRIG_VERSION_TAG);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (run_all) {
        int rc = run_bench(threads, seconds, SHA256D_BACKEND_OPENSSL);
        if (rc != 0) {
            return rc;
        }
        rc = run_bench(threads, seconds, SHA256D_BACKEND_FAST_C);
        if (rc != 0) {
            return rc;
        }
        if (sha256d_backend_available(SHA256D_BACKEND_ARM_SHA2)) {
            rc = run_bench(threads, seconds, SHA256D_BACKEND_ARM_SHA2);
            if (rc != 0) {
                return rc;
            }
        }
        if (sha256d_backend_available(SHA256D_BACKEND_X86_SHA_NI)) {
            return run_bench(threads, seconds, SHA256D_BACKEND_X86_SHA_NI);
        }
        return 0;
    }

    return run_bench(threads, seconds, backend);
}
