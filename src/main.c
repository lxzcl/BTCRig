#define _GNU_SOURCE

#include "console.h"
#include "cpu_info.h"
#include "sha256d.h"
#include "btcrig_version.h"
#if defined(BTC_MINER_OPENCL)
#include "opencl_miner.h"
#endif
#if defined(BTC_MINER_CUDA)
#include "cuda_miner.h"
#endif

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
    const uint32_t range_check_count = 5;
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
    sha256d_nonce_range_func()(&midstate, tail_words, target_words, 0, range_check_count,
                               &range_check, range_self_check_match);
    if (range_check.failed || range_check.seen != range_check_count) {
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

static int parse_nonnegative_int(const char *text, int fallback) {
    if (text == NULL || *text == '\0') {
        return fallback;
    }

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 1024) {
        return fallback;
    }
    return (int)value;
}

static uint32_t parse_positive_u32(const char *text, uint32_t fallback) {
    if (text == NULL || *text == '\0') {
        return fallback;
    }

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)value;
}

#if defined(BTC_MINER_OPENCL)
static int parse_opencl_backend_variant(const char *value, int fallback) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "auto") == 0) {
        return MINER_OPENCL_BACKEND_AUTO;
    }
    if (strcmp(value, "compat10") == 0 || strcmp(value, "compat") == 0) {
        return MINER_OPENCL_BACKEND_COMPAT10;
    }
    if (strcmp(value, "modern") == 0) {
        return MINER_OPENCL_BACKEND_MODERN;
    }
    return fallback;
}

static int parse_opencl_kernel_variant(const char *value, int fallback) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "auto") == 0) {
        return MINER_OPENCL_KERNEL_AUTO;
    }
    if (strcmp(value, "compact") == 0) {
        return MINER_OPENCL_KERNEL_COMPACT;
    }
    if (strcmp(value, "unrolled") == 0) {
        return MINER_OPENCL_KERNEL_UNROLLED;
    }
    if (strcmp(value, "fixed-npi1") == 0 || strcmp(value, "fixed_npi1") == 0) {
        return MINER_OPENCL_KERNEL_FIXED_NPI1;
    }
    if (strcmp(value, "fixed-npi2") == 0 || strcmp(value, "fixed_npi2") == 0) {
        return MINER_OPENCL_KERNEL_FIXED_NPI2;
    }
    if (strcmp(value, "fixed-npi4") == 0 || strcmp(value, "fixed_npi4") == 0) {
        return MINER_OPENCL_KERNEL_FIXED_NPI4;
    }
    if (strcmp(value, "register-heavy") == 0 || strcmp(value, "register_heavy") == 0) {
        return MINER_OPENCL_KERNEL_REGISTER_HEAVY;
    }
    return fallback;
}
#endif

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
            "Usage: %s [-t threads] [-s seconds] [--backend openssl|fast-c|arm-sha2|x86-sha-ni]\n"
            "       [--opencl] [--opencl-self-test] [--opencl-platform N] [--opencl-device N]\n"
            "       [--opencl-batch N] [--opencl-local N] [--opencl-npi N]\n"
            "       [--opencl-backend auto|compat10|modern] [--opencl-kernel auto|compact|unrolled|fixed-npi1|fixed-npi2|fixed-npi4|register-heavy]\n"
            "       [--all] [--cpu-info] [--cuda-info] [--cuda] [--cuda-self-test]\n"
            "       [--cuda-device N] [--cuda-batch N] [--cuda-block N] [--cuda-npt N] [--version]\n",
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

#if defined(BTC_MINER_OPENCL)
static int run_opencl_self_test_command(const miner_opencl_config_t *config) {
    opencl_self_test_result_t result;
    char error[2048];
    error[0] = '\0';
    if (opencl_miner_self_test(config, &result, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s[OPENCL]%s self-test failed: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }
    printf("%s[OPENCL]%s self-test ok backend=%s device=%s version=%s nonces=%u\n",
           C_BRIGHT_GREEN,
           C_RESET,
           result.backend,
           result.device_name,
           result.device_version,
           result.checked_nonces);
    return 0;
}

static int run_opencl_bench(int seconds, const miner_opencl_config_t *config) {
    char error[2048];
    error[0] = '\0';

    if (opencl_miner_self_test(config, NULL, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s[OPENCL]%s self-test failed: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    opencl_miner_t *miner = opencl_miner_create(config, error, sizeof(error));
    if (miner == NULL) {
        fprintf(stderr, "%s[OPENCL]%s unavailable: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    uint8_t header[80];
    uint32_t tail_words[4];
    uint32_t target_words[8] = {0};
    sha256_midstate_t midstate;
    make_test_header(header);
    sha256d_80_midstate_prepare(&midstate, header);
    for (int i = 0; i < 4; ++i) {
        tail_words[i] = load_be32(header + 64 + i * 4);
    }

    uint32_t nonce = 0;
    uint64_t total = 0;
    uint8_t sink = 0;
    const double start = monotonic_seconds();
    const double deadline = start + (double)seconds;
    const uint32_t batch_size = opencl_miner_batch_size(miner);
    while (monotonic_seconds() < deadline) {
        if (opencl_miner_scan(miner,
                              &midstate,
                              tail_words,
                              target_words,
                              nonce,
                              batch_size,
                              &sink,
                              bench_scan_match) != 0) {
            fprintf(stderr, "%s[OPENCL]%s scan failed\n", C_BRIGHT_RED, C_RESET);
            opencl_miner_destroy(miner);
            return 1;
        }
        nonce += batch_size;
        total += batch_size;
    }
    const double elapsed = monotonic_seconds() - start;
    const double rate = elapsed > 0.0 ? (double)total / elapsed : 0.0;

    printf("%s[BENCH]%s backend=opencl seconds=%d elapsed=%.3f\n",
           C_BRIGHT_CYAN, C_RESET, seconds, elapsed);
    printf("%s[BENCH]%s opencl_backend=%s device=%s version=%s batch=%u local=%u npi=%u\n",
           C_BRIGHT_CYAN,
           C_RESET,
           opencl_miner_backend_name(miner),
           opencl_miner_device_name(miner),
           opencl_miner_device_version(miner),
           opencl_miner_batch_size(miner),
           opencl_miner_local_work_size(miner),
           opencl_miner_nonces_per_work_item(miner));
    printf("%s[BENCH]%s sha256d_midstate_hashes=%" PRIu64 " rate=%s%.3f MH/s%s sink=%02x\n",
           C_BRIGHT_CYAN, C_RESET, total, C_BRIGHT_GREEN, rate / 1000000.0, C_RESET, sink);
    printf("%s[BENCH]%s note=OpenCL uses the same midstate scan contract as CPU and CUDA\n",
           C_GRAY, C_RESET);

    opencl_miner_destroy(miner);
    return 0;
}
#endif

#if defined(BTC_MINER_CUDA)
static int run_cuda_self_test_command(const miner_cuda_config_t *config) {
    cuda_self_test_result_t result;
    char error[512];
    error[0] = '\0';
    if (cuda_miner_self_test(config, &result, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s[CUDA]%s self-test failed: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }
    printf("%s[CUDA]%s self-test ok device=%s compute=sm_%d%d driver=%d nonces=%u\n",
           C_BRIGHT_GREEN,
           C_RESET,
           result.device_name,
           result.compute_major,
           result.compute_minor,
           result.driver_version,
           result.checked_nonces);
    return 0;
}

static int run_cuda_bench(int seconds, const miner_cuda_config_t *config) {
    char error[512];
    error[0] = '\0';

    if (cuda_miner_self_test(config, NULL, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s[CUDA]%s self-test failed: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    cuda_miner_t *miner = cuda_miner_create(config, error, sizeof(error));
    if (miner == NULL) {
        fprintf(stderr, "%s[CUDA]%s unavailable: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    uint8_t header[80];
    uint32_t tail_words[4];
    uint32_t target_words[8] = {0};
    sha256_midstate_t midstate;
    make_test_header(header);
    sha256d_80_midstate_prepare(&midstate, header);
    for (int i = 0; i < 4; ++i) {
        tail_words[i] = load_be32(header + 64 + i * 4);
    }

    uint32_t nonce = 0;
    uint64_t total = 0;
    uint8_t sink = 0;
    const double start = monotonic_seconds();
    const double deadline = start + (double)seconds;
    const uint32_t batch_size = cuda_miner_batch_size(miner);
    while (monotonic_seconds() < deadline) {
        if (cuda_miner_scan(miner,
                            &midstate,
                            tail_words,
                            target_words,
                            nonce,
                            batch_size,
                            &sink,
                            bench_scan_match) != 0) {
            fprintf(stderr, "%s[CUDA]%s scan failed\n", C_BRIGHT_RED, C_RESET);
            cuda_miner_destroy(miner);
            return 1;
        }
        nonce += batch_size;
        total += batch_size;
    }
    const double elapsed = monotonic_seconds() - start;
    const double rate = elapsed > 0.0 ? (double)total / elapsed : 0.0;

    printf("%s[BENCH]%s backend=cuda seconds=%d elapsed=%.3f\n",
           C_BRIGHT_CYAN, C_RESET, seconds, elapsed);
    printf("%s[BENCH]%s cuda_device=%s compute=sm_%d%d driver=%d batch=%u block=%u npt=%u\n",
           C_BRIGHT_CYAN,
           C_RESET,
           cuda_miner_device_name(miner),
           cuda_miner_compute_major(miner),
           cuda_miner_compute_minor(miner),
           cuda_miner_driver_version(miner),
           cuda_miner_batch_size(miner),
           cuda_miner_threads_per_block(miner),
           cuda_miner_nonces_per_thread(miner));
    printf("%s[BENCH]%s sha256d_midstate_hashes=%" PRIu64 " rate=%s%.3f MH/s%s sink=%02x\n",
           C_BRIGHT_CYAN, C_RESET, total, C_BRIGHT_GREEN, rate / 1000000.0, C_RESET, sink);
    printf("%s[BENCH]%s note=CUDA uses the NVIDIA driver API and embedded PTX\n",
           C_GRAY, C_RESET);

    cuda_miner_destroy(miner);
    return 0;
}
#endif

int main(int argc, char **argv) {
    int threads = cpu_info_recommended_threads();
    int seconds = 5;
    int run_all = 0;
#if defined(BTC_MINER_OPENCL)
    int run_opencl = 0;
    int run_opencl_self_test = 0;
#endif
    int run_cuda = 0;
    int run_cuda_info = 0;
    int run_cuda_self_test = 0;
    sha256d_backend_t backend = SHA256D_BACKEND_OPENSSL;
#if defined(BTC_MINER_OPENCL)
    miner_opencl_config_t opencl_config;
    miner_opencl_config_defaults(&opencl_config);
#endif
#if defined(BTC_MINER_CUDA)
    miner_cuda_config_t cuda_config;
    miner_cuda_config_defaults(&cuda_config);
#endif

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
#if defined(BTC_MINER_OPENCL)
        } else if (strcmp(argv[i], "--opencl") == 0) {
            run_opencl = 1;
            opencl_config.enabled = 1;
        } else if (strcmp(argv[i], "--opencl-self-test") == 0) {
            run_opencl_self_test = 1;
            opencl_config.enabled = 1;
        } else if (strcmp(argv[i], "--opencl-platform") == 0 && i + 1 < argc) {
            opencl_config.platform = parse_nonnegative_int(argv[++i], opencl_config.platform);
            opencl_config.all_devices = 0;
            opencl_config.device_count = 0;
        } else if (strcmp(argv[i], "--opencl-device") == 0 && i + 1 < argc) {
            opencl_config.device = parse_nonnegative_int(argv[++i], opencl_config.device);
            opencl_config.all_devices = 0;
            opencl_config.device_count = 0;
        } else if (strcmp(argv[i], "--opencl-batch") == 0 && i + 1 < argc) {
            opencl_config.batch_size = parse_positive_u32(argv[++i], opencl_config.batch_size);
        } else if (strcmp(argv[i], "--opencl-local") == 0 && i + 1 < argc) {
            opencl_config.local_work_size = parse_positive_u32(argv[++i], opencl_config.local_work_size);
        } else if ((strcmp(argv[i], "--opencl-npi") == 0 ||
                    strcmp(argv[i], "--opencl-nonces-per-work-item") == 0) && i + 1 < argc) {
            opencl_config.nonces_per_work_item = parse_positive_u32(argv[++i], opencl_config.nonces_per_work_item);
        } else if (strcmp(argv[i], "--opencl-backend") == 0 && i + 1 < argc) {
            int parsed = parse_opencl_backend_variant(argv[++i], -1);
            if (parsed < 0) {
                fprintf(stderr, "invalid --opencl-backend, use auto, compat10, or modern\n");
                return 2;
            }
            opencl_config.backend_variant = parsed;
        } else if (strcmp(argv[i], "--opencl-kernel") == 0 && i + 1 < argc) {
            int parsed = parse_opencl_kernel_variant(argv[++i], -1);
            if (parsed < 0) {
                fprintf(stderr,
                        "invalid --opencl-kernel, use auto, compact, unrolled, fixed-npi1, fixed-npi2, fixed-npi4, or register-heavy\n");
                return 2;
            }
            opencl_config.kernel_variant = parsed;
#else
        } else if (strcmp(argv[i], "--opencl") == 0 ||
                   strcmp(argv[i], "--opencl-self-test") == 0 ||
                   strcmp(argv[i], "--opencl-platform") == 0 ||
                   strcmp(argv[i], "--opencl-device") == 0 ||
                   strcmp(argv[i], "--opencl-batch") == 0 ||
                   strcmp(argv[i], "--opencl-local") == 0 ||
                   strcmp(argv[i], "--opencl-npi") == 0 ||
                   strcmp(argv[i], "--opencl-nonces-per-work-item") == 0 ||
                   strcmp(argv[i], "--opencl-backend") == 0 ||
                   strcmp(argv[i], "--opencl-kernel") == 0) {
            fprintf(stderr, "OpenCL support was not compiled into this build\n");
            return 2;
#endif
#if defined(BTC_MINER_CUDA)
        } else if (strcmp(argv[i], "--cuda-info") == 0) {
            run_cuda_info = 1;
        } else if (strcmp(argv[i], "--cuda") == 0) {
            run_cuda = 1;
        } else if (strcmp(argv[i], "--cuda-self-test") == 0) {
            run_cuda_self_test = 1;
        } else if (strcmp(argv[i], "--cuda-device") == 0 && i + 1 < argc) {
            cuda_config.device = parse_nonnegative_int(argv[++i], cuda_config.device);
        } else if (strcmp(argv[i], "--cuda-batch") == 0 && i + 1 < argc) {
            cuda_config.batch_size = parse_positive_u32(argv[++i], cuda_config.batch_size);
        } else if (strcmp(argv[i], "--cuda-block") == 0 && i + 1 < argc) {
            cuda_config.threads_per_block = parse_positive_u32(argv[++i], cuda_config.threads_per_block);
        } else if (strcmp(argv[i], "--cuda-npt") == 0 && i + 1 < argc) {
            cuda_config.nonces_per_thread = parse_positive_u32(argv[++i], cuda_config.nonces_per_thread);
#else
        } else if (strcmp(argv[i], "--cuda-info") == 0 ||
                   strcmp(argv[i], "--cuda") == 0 ||
                   strcmp(argv[i], "--cuda-self-test") == 0 ||
                   strcmp(argv[i], "--cuda-device") == 0 ||
                   strcmp(argv[i], "--cuda-batch") == 0 ||
                   strcmp(argv[i], "--cuda-block") == 0 ||
                   strcmp(argv[i], "--cuda-npt") == 0) {
            fprintf(stderr, "CUDA support was not compiled into this build\n");
            return 2;
#endif
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

#if defined(BTC_MINER_OPENCL)
    if (run_opencl_self_test) {
        return run_opencl_self_test_command(&opencl_config);
    }
    if (run_opencl) {
        return run_opencl_bench(seconds, &opencl_config);
    }
#endif

#if defined(BTC_MINER_CUDA)
    if (run_cuda_info) {
        return cuda_miner_print_devices();
    }
    if (run_cuda_self_test) {
        return run_cuda_self_test_command(&cuda_config);
    }
    if (run_cuda) {
        return run_cuda_bench(seconds, &cuda_config);
    }
#endif

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
            rc = run_bench(threads, seconds, SHA256D_BACKEND_X86_SHA_NI);
            if (rc != 0) {
                return rc;
            }
        }
#if defined(BTC_MINER_CUDA)
        char cuda_error[256];
        cuda_error[0] = '\0';
        if (cuda_miner_driver_available(cuda_error, sizeof(cuda_error)) == 0 &&
            cuda_miner_device_count(cuda_error, sizeof(cuda_error)) > 0) {
            rc = run_cuda_bench(seconds, &cuda_config);
            if (rc != 0) {
                return rc;
            }
        }
#endif
        return 0;
    }

    return run_bench(threads, seconds, backend);
}
