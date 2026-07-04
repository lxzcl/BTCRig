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

#if defined(BTC_MINER_CUDA)
static int parse_cuda_kernel_variant(const char *value, int fallback) {
    if (value == NULL || value[0] == '\0' || strcmp(value, "standard") == 0) {
        return MINER_CUDA_KERNEL_STANDARD;
    }
    if (strcmp(value, "dual") == 0 || strcmp(value, "dual-nonce") == 0 || strcmp(value, "dual_nonce") == 0) {
        return MINER_CUDA_KERNEL_DUAL;
    }
    if (strcmp(value, "fixed-npt1") == 0 || strcmp(value, "fixed_npt1") == 0) {
        return MINER_CUDA_KERNEL_FIXED_NPT1;
    }
    if (strcmp(value, "fixed-npt2") == 0 || strcmp(value, "fixed_npt2") == 0) {
        return MINER_CUDA_KERNEL_FIXED_NPT2;
    }
    if (strcmp(value, "fixed-npt4") == 0 || strcmp(value, "fixed_npt4") == 0) {
        return MINER_CUDA_KERNEL_FIXED_NPT4;
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
            "       [--all] [--cpu-info] [--cuda-info] [--cuda] [--cuda-autotune] [--cuda-self-test]\n"
            "       [--cuda-device N] [--cuda-batch N] [--cuda-block N] [--cuda-npt N]\n"
            "       [--cuda-kernel standard|dual|fixed-npt1|fixed-npt2|fixed-npt4]\n"
            "       [--version]\n",
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
typedef struct {
    uint64_t hashes;
    double elapsed;
    double rate;
    uint8_t sink;
    char device_name[128];
    int compute_major;
    int compute_minor;
    int driver_version;
    uint32_t batch_size;
    uint32_t threads_per_block;
    uint32_t nonces_per_thread;
    int kernel_variant;
} cuda_bench_result_t;

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

static int run_cuda_measure(int seconds,
                            const miner_cuda_config_t *config,
                            cuda_bench_result_t *result) {
    char error[512];
    error[0] = '\0';

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

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->hashes = total;
        result->elapsed = elapsed;
        result->rate = rate;
        result->sink = sink;
        snprintf(result->device_name, sizeof(result->device_name), "%s", cuda_miner_device_name(miner));
        result->compute_major = cuda_miner_compute_major(miner);
        result->compute_minor = cuda_miner_compute_minor(miner);
        result->driver_version = cuda_miner_driver_version(miner);
        result->batch_size = cuda_miner_batch_size(miner);
        result->threads_per_block = cuda_miner_threads_per_block(miner);
        result->nonces_per_thread = cuda_miner_nonces_per_thread(miner);
        result->kernel_variant = cuda_miner_kernel_variant(miner);
    }

    cuda_miner_destroy(miner);
    return 0;
}

static int run_cuda_bench(int seconds, const miner_cuda_config_t *config) {
    char error[512];
    cuda_bench_result_t result;
    error[0] = '\0';

    if (cuda_miner_self_test(config, NULL, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s[CUDA]%s self-test failed: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    if (run_cuda_measure(seconds, config, &result) != 0) {
        return 1;
    }

    printf("%s[BENCH]%s backend=cuda seconds=%d elapsed=%.3f\n",
           C_BRIGHT_CYAN, C_RESET, seconds, result.elapsed);
    printf("%s[BENCH]%s cuda_device=%s compute=sm_%d%d driver=%d kernel=%s batch=%u block=%u npt=%u\n",
           C_BRIGHT_CYAN,
           C_RESET,
           result.device_name,
           result.compute_major,
           result.compute_minor,
           result.driver_version,
           cuda_miner_kernel_variant_name(result.kernel_variant),
           result.batch_size,
           result.threads_per_block,
           result.nonces_per_thread);
    printf("%s[BENCH]%s sha256d_midstate_hashes=%" PRIu64 " rate=%s%.3f MH/s%s sink=%02x\n",
           C_BRIGHT_CYAN,
           C_RESET,
           result.hashes,
           C_BRIGHT_GREEN,
           result.rate / 1000000.0,
           C_RESET,
           result.sink);
    printf("%s[BENCH]%s note=CUDA uses the NVIDIA driver API and embedded PTX\n",
           C_GRAY, C_RESET);

    return 0;
}

static int value_seen_u32(const uint32_t *values, int count, uint32_t value) {
    for (int i = 0; i < count; ++i) {
        if (values[i] == value) {
            return 1;
        }
    }
    return 0;
}

static uint32_t cuda_autotune_block_value(uint32_t value) {
    if (value == 0U) {
        value = MINER_CUDA_DEFAULT_THREADS_PER_BLOCK;
    }
    if (value < 32U) {
        value = 32U;
    }
    if (value > 1024U) {
        value = 1024U;
    }
    return (value + 31U) & ~31U;
}

static uint32_t cuda_autotune_npt_value(uint32_t value) {
    if (value == 0U) {
        value = MINER_CUDA_DEFAULT_NONCES_PER_THREAD;
    }
    if (value < 1U) {
        value = 1U;
    }
    if (value > 16U) {
        value = 16U;
    }
    return value;
}

static uint32_t cuda_autotune_batch_value(uint32_t value) {
    if (value == 0U) {
        value = MINER_CUDA_DEFAULT_BATCH_SIZE;
    }
    return value < 1024U ? 1024U : value;
}

static uint32_t cuda_autotune_kernel_forced_npt(int variant) {
    switch (variant) {
    case MINER_CUDA_KERNEL_FIXED_NPT1:
        return 1U;
    case MINER_CUDA_KERNEL_FIXED_NPT2:
        return 2U;
    case MINER_CUDA_KERNEL_FIXED_NPT4:
        return 4U;
    default:
        return 0U;
    }
}

static int cuda_autotune_hashrate_is_better(double candidate, double best) {
    const double threshold = 1.02;

    if (best <= 0.0) {
        return 1;
    }
    return candidate > best * threshold;
}

static int cuda_autotune_batch_is_better(double candidate, double best) {
    const double threshold = 1.01;

    if (best <= 0.0) {
        return 1;
    }
    return candidate > best * threshold;
}

static void print_cuda_autotune_result(const char *label,
                                       const miner_cuda_config_t *config,
                                       const cuda_bench_result_t *result) {
    printf("%s[AUTOTUNE]%s %s device=%d kernel=%s batch=%u block=%u npt=%u rate=%s%.3f MH/s%s elapsed=%.3f hashes=%" PRIu64 "\n",
           C_CYAN,
           C_RESET,
           label,
           config->device,
           cuda_miner_kernel_variant_name(result->kernel_variant),
           result->batch_size,
           result->threads_per_block,
           result->nonces_per_thread,
           C_BRIGHT_GREEN,
           result->rate / 1000000.0,
           C_RESET,
           result->elapsed,
           result->hashes);
}

static int run_cuda_autotune(int seconds, const miner_cuda_config_t *config) {
    miner_cuda_config_t base;
    miner_cuda_config_t best;
    cuda_bench_result_t best_result;
    char error[512];
    int best_ok = 0;

    miner_cuda_config_defaults(&base);
    if (config != NULL) {
        base = *config;
    }
    base.batch_size = cuda_autotune_batch_value(base.batch_size);
    base.threads_per_block = cuda_autotune_block_value(base.threads_per_block);
    base.nonces_per_thread = cuda_autotune_npt_value(base.nonces_per_thread);
    if (base.kernel_variant < MINER_CUDA_KERNEL_STANDARD ||
        base.kernel_variant > MINER_CUDA_KERNEL_LAST) {
        base.kernel_variant = MINER_CUDA_DEFAULT_KERNEL_VARIANT;
    }
    uint32_t forced_npt = cuda_autotune_kernel_forced_npt(base.kernel_variant);
    if (forced_npt != 0U) {
        base.nonces_per_thread = forced_npt;
    }

    error[0] = '\0';
    if (cuda_miner_self_test(&base, NULL, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s[CUDA]%s self-test failed: %s\n",
                C_BRIGHT_RED, C_RESET, error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    printf("%s[AUTOTUNE]%s cuda seconds=%d device=%d\n",
           C_BRIGHT_CYAN,
           C_RESET,
           seconds,
           base.device);
    printf("%s[AUTOTUNE]%s selection thresholds: block/npt=2%% batch=1%%\n",
           C_GRAY,
           C_RESET);
    printf("%s[AUTOTUNE]%s warming device=%d kernel=%s batch=%u block=%u npt=%u\n",
           C_CYAN,
           C_RESET,
           base.device,
           cuda_miner_kernel_variant_name(base.kernel_variant),
           base.batch_size,
           base.threads_per_block,
           base.nonces_per_thread);
    (void)run_cuda_measure(seconds, &base, NULL);

    const uint32_t block_candidates_raw[] = {
        base.threads_per_block,
        MINER_CUDA_DEFAULT_THREADS_PER_BLOCK,
        128U,
        256U,
        512U,
    };
    const uint32_t kernel_candidates_raw[] = {
        (uint32_t)base.kernel_variant,
        MINER_CUDA_KERNEL_STANDARD,
        MINER_CUDA_KERNEL_DUAL,
        MINER_CUDA_KERNEL_FIXED_NPT1,
        MINER_CUDA_KERNEL_FIXED_NPT2,
        MINER_CUDA_KERNEL_FIXED_NPT4,
    };
    const uint32_t npt_candidates_raw[] = {
        base.nonces_per_thread,
        MINER_CUDA_DEFAULT_NONCES_PER_THREAD,
        1U,
        2U,
        4U,
    };
    const uint32_t batch_candidates_raw[] = {
        base.batch_size,
        MINER_CUDA_DEFAULT_BATCH_SIZE,
        1048576U,
        2097152U,
        4194304U,
        8388608U,
        16777216U,
    };
    uint32_t block_candidates[sizeof(block_candidates_raw) / sizeof(block_candidates_raw[0])];
    uint32_t npt_candidates[sizeof(npt_candidates_raw) / sizeof(npt_candidates_raw[0])];
    uint32_t batch_candidates[sizeof(batch_candidates_raw) / sizeof(batch_candidates_raw[0])];
    uint32_t kernel_candidates[sizeof(kernel_candidates_raw) / sizeof(kernel_candidates_raw[0])];
    int block_count = 0;
    int npt_count = 0;
    int batch_count = 0;
    int kernel_count = 0;

    for (size_t i = 0; i < sizeof(block_candidates_raw) / sizeof(block_candidates_raw[0]); ++i) {
        uint32_t value = cuda_autotune_block_value(block_candidates_raw[i]);
        if (!value_seen_u32(block_candidates, block_count, value)) {
            block_candidates[block_count++] = value;
        }
    }
    for (size_t i = 0; i < sizeof(npt_candidates_raw) / sizeof(npt_candidates_raw[0]); ++i) {
        uint32_t value = cuda_autotune_npt_value(npt_candidates_raw[i]);
        if (!value_seen_u32(npt_candidates, npt_count, value)) {
            npt_candidates[npt_count++] = value;
        }
    }
    for (size_t i = 0; i < sizeof(batch_candidates_raw) / sizeof(batch_candidates_raw[0]); ++i) {
        uint32_t value = cuda_autotune_batch_value(batch_candidates_raw[i]);
        if (!value_seen_u32(batch_candidates, batch_count, value)) {
            batch_candidates[batch_count++] = value;
        }
    }
    for (size_t i = 0; i < sizeof(kernel_candidates_raw) / sizeof(kernel_candidates_raw[0]); ++i) {
        uint32_t value = kernel_candidates_raw[i];
        if (value > MINER_CUDA_KERNEL_LAST) {
            continue;
        }
        if (!value_seen_u32(kernel_candidates, kernel_count, value)) {
            kernel_candidates[kernel_count++] = value;
        }
    }

    best = base;
    memset(&best_result, 0, sizeof(best_result));

    printf("%s[AUTOTUNE]%s tuning CUDA kernel/block/npt\n", C_BRIGHT_CYAN, C_RESET);
    for (int ki = 0; ki < kernel_count; ++ki) {
        for (int bi = 0; bi < block_count; ++bi) {
            for (int ni = 0; ni < npt_count; ++ni) {
                miner_cuda_config_t candidate = base;
                cuda_bench_result_t result;

                candidate.kernel_variant = (int)kernel_candidates[ki];
                uint32_t forced_npt = cuda_autotune_kernel_forced_npt(candidate.kernel_variant);
                if (forced_npt != 0U && npt_candidates[ni] != forced_npt) {
                    continue;
                }
                candidate.threads_per_block = block_candidates[bi];
                candidate.nonces_per_thread = forced_npt != 0U ? forced_npt : npt_candidates[ni];
                printf("%s[AUTOTUNE]%s testing device=%d kernel=%s batch=%u block=%u npt=%u\n",
                       C_CYAN,
                       C_RESET,
                       candidate.device,
                       cuda_miner_kernel_variant_name(candidate.kernel_variant),
                       candidate.batch_size,
                       candidate.threads_per_block,
                       candidate.nonces_per_thread);
                if (run_cuda_measure(seconds, &candidate, &result) != 0) {
                    printf("%s[AUTOTUNE]%s unavailable device=%d kernel=%s batch=%u block=%u npt=%u\n",
                           C_YELLOW,
                           C_RESET,
                           candidate.device,
                           cuda_miner_kernel_variant_name(candidate.kernel_variant),
                           candidate.batch_size,
                           candidate.threads_per_block,
                           candidate.nonces_per_thread);
                    continue;
                }
                print_cuda_autotune_result("result", &candidate, &result);
                if (!best_ok || cuda_autotune_hashrate_is_better(result.rate, best_result.rate)) {
                    best_ok = 1;
                    best = candidate;
                    best_result = result;
                }
            }
        }
    }

    if (!best_ok) {
        fprintf(stderr, "%s[AUTOTUNE]%s no working CUDA parameter set found\n", C_BRIGHT_RED, C_RESET);
        return 1;
    }

    printf("%s[AUTOTUNE]%s tuning CUDA batch-size\n", C_BRIGHT_CYAN, C_RESET);
    for (int bi = 0; bi < batch_count; ++bi) {
        miner_cuda_config_t candidate = best;
        cuda_bench_result_t result;

        candidate.batch_size = batch_candidates[bi];
        printf("%s[AUTOTUNE]%s testing device=%d kernel=%s batch=%u block=%u npt=%u\n",
               C_CYAN,
               C_RESET,
               candidate.device,
               cuda_miner_kernel_variant_name(candidate.kernel_variant),
               candidate.batch_size,
               candidate.threads_per_block,
               candidate.nonces_per_thread);
        if (run_cuda_measure(seconds, &candidate, &result) != 0) {
            printf("%s[AUTOTUNE]%s unavailable device=%d kernel=%s batch=%u block=%u npt=%u\n",
                   C_YELLOW,
                   C_RESET,
                   candidate.device,
                   cuda_miner_kernel_variant_name(candidate.kernel_variant),
                   candidate.batch_size,
                   candidate.threads_per_block,
                   candidate.nonces_per_thread);
            continue;
        }
        print_cuda_autotune_result("result", &candidate, &result);
        if (cuda_autotune_batch_is_better(result.rate, best_result.rate)) {
            best = candidate;
            best_result = result;
        }
    }

    printf("%s[AUTOTUNE]%s selected device=%d kernel=%s batch=%u block=%u npt=%u rate=%s%.3f MH/s%s\n",
           C_BRIGHT_GREEN,
           C_RESET,
           best.device,
           cuda_miner_kernel_variant_name(best.kernel_variant),
           best.batch_size,
           best.threads_per_block,
           best.nonces_per_thread,
           C_BRIGHT_GREEN,
           best_result.rate / 1000000.0,
           C_RESET);
    printf("%s[AUTOTUNE]%s command: --cuda --cuda-device %d --cuda-kernel %s --cuda-batch %u --cuda-block %u --cuda-npt %u -s %d\n",
           C_BRIGHT_CYAN,
           C_RESET,
           best.device,
           cuda_miner_kernel_variant_name(best.kernel_variant),
           best.batch_size,
           best.threads_per_block,
           best.nonces_per_thread,
           seconds);
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
#if defined(BTC_MINER_CUDA)
    int run_cuda = 0;
    int run_cuda_info = 0;
    int run_cuda_autotune_flag = 0;
    int run_cuda_self_test = 0;
#endif
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
        } else if (strcmp(argv[i], "--cuda-autotune") == 0) {
            run_cuda_autotune_flag = 1;
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
        } else if (strcmp(argv[i], "--cuda-kernel") == 0 && i + 1 < argc) {
            int parsed = parse_cuda_kernel_variant(argv[++i], -1);
            if (parsed < 0) {
                fprintf(stderr, "invalid --cuda-kernel, use standard, dual, fixed-npt1, fixed-npt2, or fixed-npt4\n");
                return 2;
            }
            cuda_config.kernel_variant = parsed;
#else
        } else if (strcmp(argv[i], "--cuda-info") == 0 ||
                   strcmp(argv[i], "--cuda") == 0 ||
                   strcmp(argv[i], "--cuda-autotune") == 0 ||
                   strcmp(argv[i], "--cuda-self-test") == 0 ||
                   strcmp(argv[i], "--cuda-device") == 0 ||
                   strcmp(argv[i], "--cuda-batch") == 0 ||
                   strcmp(argv[i], "--cuda-block") == 0 ||
                   strcmp(argv[i], "--cuda-npt") == 0 ||
                   strcmp(argv[i], "--cuda-kernel") == 0) {
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
    if (run_cuda_autotune_flag) {
        return run_cuda_autotune(seconds, &cuda_config);
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
