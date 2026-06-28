#define _POSIX_C_SOURCE 200809L

#include "console.h"
#include "cpu_info.h"
#include "donation.h"
#include "sha256d.h"
#include "stratum.h"
#include "btcrig_version.h"
#if defined(BTC_MINER_OPENCL)
#include "opencl_miner.h"
#endif

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define BTC_ACCESS(path, mode) _access((path), (mode))
#define BTC_R_OK 4
#else
#include <unistd.h>
#define BTC_ACCESS(path, mode) access((path), (mode))
#define BTC_R_OK R_OK
#endif

#define MAX_POOLS 8

#define DEFAULT_POOL_URL "stratum+tls://public-pool.io:14333"
#define DEFAULT_USER "bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3"
#define DEFAULT_PASSWORD "x"
#define DEFAULT_SUGGEST_DIFFICULTY 0.001
#define DEFAULT_RETRIES -1
#define DEFAULT_RECONNECT_DELAY 2
#define MAX_RECONNECT_DELAY 60
#define DEFAULT_STATS_INTERVAL 5.0
#define DEFAULT_AUTOTUNE_SECONDS 1.5
#define AUTOTUNE_MAX_RESULTS 64

static char stdout_buffer[1024 * 1024];
static char stderr_buffer[64 * 1024];

typedef struct {
    char url[256];
    char user[256];
    char pass[128];
    double difficulty;
} pool_config_t;

typedef struct {
    pool_config_t pools[MAX_POOLS];
    int pool_count;
    int retries;
    int reconnect_delay;
    int thread_count;
    int cpu_enabled;
    int enable_mining;
    int autosave;
    int autotune_enabled;
    int cpu_autotune_done;
    int gpu_autotune_done;
    miner_opencl_config_t opencl;
    double runtime_seconds;
    double stats_interval;
    double autotune_seconds;
    int donate_level;
} app_config_t;

typedef struct {
    char name[96];
    int cpu_threads;
    miner_opencl_config_t opencl;
    double hashrate;
    int ok;
} autotune_result_t;

static int run_opencl_self_test(const miner_opencl_config_t *config) {
#if defined(BTC_MINER_OPENCL)
    miner_opencl_device_config_t devices[MINER_OPENCL_MAX_DEVICES];
    char error[2048];
    error[0] = '\0';
    int count = opencl_miner_resolve_devices(config, devices, MINER_OPENCL_MAX_DEVICES, error, sizeof(error));
    if (count <= 0) {
        fprintf(stderr,
                "%s[OPENCL]%s self-test failed: %s\n",
                C_BRIGHT_RED,
                C_RESET,
                error[0] != '\0' ? error : "unknown error");
        return 1;
    }

    int failed = 0;
    for (int i = 0; i < count; ++i) {
        miner_opencl_config_t device_config;
        miner_opencl_config_defaults(&device_config);
        if (config != NULL) {
            device_config = *config;
        }
        device_config.enabled = 1;
        device_config.all_devices = 0;
        device_config.device_count = 0;
        device_config.platform = devices[i].platform;
        device_config.device = devices[i].device;
        device_config.batch_size = devices[i].batch_size;
        device_config.local_work_size = devices[i].local_work_size;
        device_config.nonces_per_work_item = devices[i].nonces_per_work_item;
        device_config.max_results = devices[i].max_results;
        device_config.kernel_variant = devices[i].kernel_variant;

        opencl_self_test_result_t result;
        error[0] = '\0';
        if (opencl_miner_self_test(&device_config, &result, error, sizeof(error)) != 0) {
            fprintf(stderr,
                    "%s[OPENCL]%s #%d platform=%d device=%d self-test failed: %s\n",
                    C_BRIGHT_RED,
                    C_RESET,
                    i,
                    devices[i].platform,
                    devices[i].device,
                    error[0] != '\0' ? error : "unknown error");
            failed = 1;
            continue;
        }
        printf("%s[OPENCL]%s #%d platform=%d device=%d self-test ok backend=%s device=%s%s%s version=%s nonces=%u\n",
               C_BRIGHT_GREEN,
               C_RESET,
               i,
               devices[i].platform,
               devices[i].device,
               result.backend,
               C_BRIGHT_CYAN,
               result.device_name,
               C_RESET,
               result.device_version,
               result.checked_nonces);
    }
    return failed ? 1 : 0;
#else
    (void)config;
    fprintf(stderr,
            "%s[OPENCL]%s self-test unavailable: this build was compiled without OpenCL support\n",
            C_BRIGHT_RED,
            C_RESET);
    return 1;
#endif
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

#if defined(_WIN32)
static DWORD WINAPI stdio_flush_thread(LPVOID ignored);
#endif

static void configure_stdio(void) {
#if defined(_WIN32)
    setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer));
    setvbuf(stderr, stderr_buffer, _IOFBF, sizeof(stderr_buffer));
    HANDLE thread = CreateThread(NULL, 0, stdio_flush_thread, NULL, 0, NULL);
    if (thread != NULL) {
        CloseHandle(thread);
    }
#else
    setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer));
    setvbuf(stderr, stderr_buffer, _IOLBF, sizeof(stderr_buffer));
#endif
}

static int configure_sha_backend_from_env(void) {
    const char *text = getenv("BTC_MINER_SHA_BACKEND");
    sha256d_backend_t backend;

    if (text == NULL || text[0] == '\0' || strcmp(text, "auto") == 0) {
        return sha256d_set_backend(sha256d_auto_backend());
    }
    if (sha256d_parse_backend(text, &backend) != 0) {
        fprintf(stderr, "%s[CONFIG]%s invalid BTC_MINER_SHA_BACKEND=%s\n", C_BRIGHT_RED, C_RESET, text);
        return -1;
    }
    if (sha256d_set_backend(backend) != 0) {
        fprintf(stderr, "%s[CONFIG]%s failed to set SHA backend=%s\n", C_BRIGHT_RED, C_RESET, text);
        return -1;
    }
    return 0;
}

static void print_sha_backend_summary(void) {
    static const sha256d_backend_t backends[] = {
        SHA256D_BACKEND_X86_SHA_NI,
        SHA256D_BACKEND_ARM_SHA2,
        SHA256D_BACKEND_OPENSSL,
        SHA256D_BACKEND_FAST_C,
    };
    const char *requested = getenv("BTC_MINER_SHA_BACKEND");
    const char *mode = requested == NULL || requested[0] == '\0' || strcmp(requested, "auto") == 0 ?
        "auto" : "manual";
    sha256d_backend_t selected = sha256d_get_backend();
    int first = 1;

    printf("%s[SHA]%s available=", C_CYAN, C_RESET);
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); ++i) {
        sha256d_backend_t backend = backends[i];
        if (!sha256d_backend_available(backend)) {
            continue;
        }
        printf("%s%s%s%s",
               first ? "" : ",",
               backend == selected ? C_BRIGHT_GREEN : C_GRAY,
               sha256d_backend_name(backend),
               C_RESET);
        first = 0;
    }
    printf("\n");
    printf("%s[SHA]%s selected=%s%s%s mode=%s\n",
           C_CYAN,
           C_RESET,
           C_BRIGHT_GREEN,
           sha256d_backend_name(selected),
           C_RESET,
           mode);
}

#if defined(_WIN32)
static DWORD WINAPI stdio_flush_thread(LPVOID ignored) {
    (void)ignored;
    for (;;) {
        fflush(stdout);
        fflush(stderr);
        Sleep(30);
    }
    return 0;
}
#endif

static void sleep_seconds(int seconds) {
    if (seconds <= 0) {
        return;
    }
#if defined(_WIN32)
    Sleep((DWORD)seconds * 1000U);
#else
    sleep((unsigned int)seconds);
#endif
}

static void sleep_milliseconds(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static int retry_delay_seconds(int base_delay, unsigned long attempt) {
    int delay = base_delay;
    if (delay <= 0) {
        delay = 1;
    }
    if (delay > MAX_RECONNECT_DELAY) {
        return MAX_RECONNECT_DELAY;
    }

    for (unsigned long i = 1; i < attempt && delay < MAX_RECONNECT_DELAY; ++i) {
        if (delay > MAX_RECONNECT_DELAY / 2) {
            delay = MAX_RECONNECT_DELAY;
        } else {
            delay *= 2;
        }
    }
    return delay > MAX_RECONNECT_DELAY ? MAX_RECONNECT_DELAY : delay;
}

static int default_thread_count(void) {
    return cpu_info_recommended_threads();
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static char ascii_tolower_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int string_equals_ci(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        if (ascii_tolower_char(*a) != ascii_tolower_char(*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static const char *opencl_kernel_variant_name(int variant) {
    switch (variant) {
    case MINER_OPENCL_KERNEL_COMPACT:
        return "compact";
    case MINER_OPENCL_KERNEL_UNROLLED:
        return "unrolled";
    default:
        return "auto";
    }
}

static int parse_opencl_kernel_variant(const char *value, int fallback) {
    if (value == NULL || value[0] == '\0' || string_equals_ci(value, "auto")) {
        return MINER_OPENCL_KERNEL_AUTO;
    }
    if (string_equals_ci(value, "compact")) {
        return MINER_OPENCL_KERNEL_COMPACT;
    }
    if (string_equals_ci(value, "unrolled")) {
        return MINER_OPENCL_KERNEL_UNROLLED;
    }
    return fallback;
}

static void app_config_set_defaults(app_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->pool_count = 1;
    copy_string(config->pools[0].url, sizeof(config->pools[0].url), DEFAULT_POOL_URL);
    copy_string(config->pools[0].user, sizeof(config->pools[0].user), DEFAULT_USER);
    copy_string(config->pools[0].pass, sizeof(config->pools[0].pass), DEFAULT_PASSWORD);
    config->pools[0].difficulty = DEFAULT_SUGGEST_DIFFICULTY;
    config->retries = DEFAULT_RETRIES;
    config->reconnect_delay = DEFAULT_RECONNECT_DELAY;
    config->thread_count = 0;
    config->cpu_enabled = 1;
    config->enable_mining = 1;
    config->autosave = 1;
    config->autotune_enabled = 1;
    config->cpu_autotune_done = 0;
    config->gpu_autotune_done = 0;
    miner_opencl_config_defaults(&config->opencl);
    config->runtime_seconds = 0.0;
    config->stats_interval = DEFAULT_STATS_INTERVAL;
    config->autotune_seconds = DEFAULT_AUTOTUNE_SECONDS;
    config->donate_level = DONATION_DEFAULT_LEVEL;
}

static int json_bool_value(json_t *value, int fallback) {
    if (json_is_boolean(value)) {
        return json_is_true(value) ? 1 : 0;
    }
    return fallback;
}

static int json_int_value(json_t *value, int fallback) {
    return json_is_integer(value) ? (int)json_integer_value(value) : fallback;
}

static uint32_t json_u32_value(json_t *value, uint32_t fallback) {
    if (!json_is_integer(value)) {
        return fallback;
    }
    json_int_t parsed = json_integer_value(value);
    if (parsed < 0 || parsed > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)parsed;
}

static double json_number_value_or(json_t *value, double fallback) {
    return json_is_number(value) ? json_number_value(value) : fallback;
}

static json_t *json_load_file_allow_bom(const char *path, json_error_t *error) {
    json_t *root = json_load_file(path, 0, error);
    if (root != NULL) {
        return root;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long file_size = ftell(fp);
    if (file_size < 3 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    char *buffer = malloc((size_t)file_size);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }
    size_t read_size = fread(buffer, 1, (size_t)file_size, fp);
    fclose(fp);
    if (read_size != (size_t)file_size) {
        free(buffer);
        return NULL;
    }

    const unsigned char *bytes = (const unsigned char *)buffer;
    if (read_size < 3 || bytes[0] != 0xefU || bytes[1] != 0xbbU || bytes[2] != 0xbfU) {
        free(buffer);
        return NULL;
    }

    json_error_t bom_error;
    root = json_loadb(buffer + 3, read_size - 3, 0, &bom_error);
    free(buffer);
    if (root == NULL && error != NULL) {
        *error = bom_error;
    }
    return root;
}

static void app_config_set_donate_level(app_config_t *config, int level, const char *source) {
    if (donation_level_valid(level)) {
        config->donate_level = level;
        return;
    }

    printf("%s[DONATE]%s ignored %s donate-level=%d; compiled minimum is %d%%\n",
           C_YELLOW,
           C_RESET,
           source,
           level,
           DONATION_MINIMUM_LEVEL);
}

static int load_config_file(app_config_t *config, const char *path, int required) {
    json_error_t error;
    json_t *root = json_load_file_allow_bom(path, &error);
    if (root == NULL) {
        if (required) {
            fprintf(stderr, "%s[CONFIG]%s failed to load %s line=%d col=%d: %s\n",
                    C_BRIGHT_RED, C_RESET, path, error.line, error.column, error.text);
            return -1;
        }
        return 0;
    }

    printf("%s[CONFIG]%s loaded %s\n", C_CYAN, C_RESET, path);

    config->autosave = json_bool_value(json_object_get(root, "autosave"), config->autosave);

    json_t *autotune = json_object_get(root, "autotune");
    if (json_is_object(autotune)) {
        config->autotune_enabled = json_bool_value(json_object_get(autotune, "enabled"), config->autotune_enabled);
        config->cpu_autotune_done = json_bool_value(json_object_get(autotune, "self-test"), config->cpu_autotune_done);
        config->cpu_autotune_done = json_bool_value(json_object_get(autotune, "self_test"), config->cpu_autotune_done);
        config->cpu_autotune_done = json_bool_value(json_object_get(autotune, "done"), config->cpu_autotune_done);
        config->cpu_autotune_done = json_bool_value(json_object_get(autotune, "completed"), config->cpu_autotune_done);
        config->cpu_autotune_done = json_bool_value(json_object_get(autotune, "cpu-self-test"), config->cpu_autotune_done);
        config->cpu_autotune_done = json_bool_value(json_object_get(autotune, "cpu_self_test"), config->cpu_autotune_done);
        config->gpu_autotune_done = json_bool_value(json_object_get(autotune, "gpu-self-test"), config->gpu_autotune_done);
        config->gpu_autotune_done = json_bool_value(json_object_get(autotune, "gpu_self_test"), config->gpu_autotune_done);
        config->gpu_autotune_done = json_bool_value(json_object_get(autotune, "opencl-self-test"), config->gpu_autotune_done);
        config->gpu_autotune_done = json_bool_value(json_object_get(autotune, "opencl_self_test"), config->gpu_autotune_done);
        config->autotune_seconds = json_number_value_or(json_object_get(autotune, "seconds"), config->autotune_seconds);
    }

    json_t *cpu = json_object_get(root, "cpu");
    if (json_is_object(cpu)) {
        config->cpu_enabled = json_bool_value(json_object_get(cpu, "enabled"), config->cpu_enabled);
        config->thread_count = json_int_value(json_object_get(cpu, "threads"), config->thread_count);
    }

    json_t *opencl = json_object_get(root, "opencl");
    if (json_is_object(opencl)) {
        config->opencl.enabled = json_bool_value(json_object_get(opencl, "enabled"), config->opencl.enabled);
        config->opencl.all_devices = json_bool_value(json_object_get(opencl, "all-devices"), config->opencl.all_devices);
        config->opencl.all_devices = json_bool_value(json_object_get(opencl, "all_devices"), config->opencl.all_devices);
        config->opencl.platform = json_int_value(json_object_get(opencl, "platform"), config->opencl.platform);
        config->opencl.device = json_int_value(json_object_get(opencl, "device"), config->opencl.device);
        config->opencl.batch_size = json_u32_value(json_object_get(opencl, "batch-size"), config->opencl.batch_size);
        config->opencl.batch_size = json_u32_value(json_object_get(opencl, "batch_size"), config->opencl.batch_size);
        config->opencl.local_work_size = json_u32_value(json_object_get(opencl, "local-work-size"), config->opencl.local_work_size);
        config->opencl.local_work_size = json_u32_value(json_object_get(opencl, "local_work_size"), config->opencl.local_work_size);
        config->opencl.nonces_per_work_item = json_u32_value(json_object_get(opencl, "nonces-per-work-item"), config->opencl.nonces_per_work_item);
        config->opencl.nonces_per_work_item = json_u32_value(json_object_get(opencl, "nonces_per_work_item"), config->opencl.nonces_per_work_item);
        config->opencl.nonces_per_work_item = json_u32_value(json_object_get(opencl, "npi"), config->opencl.nonces_per_work_item);
        config->opencl.max_results = json_u32_value(json_object_get(opencl, "max-results"), config->opencl.max_results);
        config->opencl.max_results = json_u32_value(json_object_get(opencl, "max_results"), config->opencl.max_results);
        config->opencl.kernel_variant = parse_opencl_kernel_variant(json_string_value(json_object_get(opencl, "kernel")),
                                                                    config->opencl.kernel_variant);
        config->opencl.kernel_variant = parse_opencl_kernel_variant(json_string_value(json_object_get(opencl, "kernel-variant")),
                                                                    config->opencl.kernel_variant);
        config->opencl.kernel_variant = parse_opencl_kernel_variant(json_string_value(json_object_get(opencl, "kernel_variant")),
                                                                    config->opencl.kernel_variant);

        json_t *devices = json_object_get(opencl, "devices");
        if (json_is_array(devices)) {
            config->opencl.device_count = 0;
            config->opencl.all_devices = 0;
            size_t index;
            json_t *device;
            json_array_foreach(devices, index, device) {
                if (!json_is_object(device) || config->opencl.device_count >= MINER_OPENCL_MAX_DEVICES) {
                    continue;
                }
                miner_opencl_device_config_t *dst = &config->opencl.devices[config->opencl.device_count];
                memset(dst, 0, sizeof(*dst));
                dst->platform = json_int_value(json_object_get(device, "platform"), config->opencl.platform);
                dst->device = json_int_value(json_object_get(device, "device"), config->opencl.device);
                dst->batch_size = json_u32_value(json_object_get(device, "batch-size"), config->opencl.batch_size);
                dst->batch_size = json_u32_value(json_object_get(device, "batch_size"), dst->batch_size);
                dst->local_work_size = json_u32_value(json_object_get(device, "local-work-size"), config->opencl.local_work_size);
                dst->local_work_size = json_u32_value(json_object_get(device, "local_work_size"), dst->local_work_size);
                dst->nonces_per_work_item = json_u32_value(json_object_get(device, "nonces-per-work-item"), config->opencl.nonces_per_work_item);
                dst->nonces_per_work_item = json_u32_value(json_object_get(device, "nonces_per_work_item"), dst->nonces_per_work_item);
                dst->nonces_per_work_item = json_u32_value(json_object_get(device, "npi"), dst->nonces_per_work_item);
                dst->max_results = json_u32_value(json_object_get(device, "max-results"), config->opencl.max_results);
                dst->max_results = json_u32_value(json_object_get(device, "max_results"), dst->max_results);
                dst->kernel_variant = parse_opencl_kernel_variant(json_string_value(json_object_get(device, "kernel")),
                                                                  config->opencl.kernel_variant);
                dst->kernel_variant = parse_opencl_kernel_variant(json_string_value(json_object_get(device, "kernel-variant")),
                                                                  dst->kernel_variant);
                dst->kernel_variant = parse_opencl_kernel_variant(json_string_value(json_object_get(device, "kernel_variant")),
                                                                  dst->kernel_variant);
                ++config->opencl.device_count;
            }
        }
    }

    config->thread_count = json_int_value(json_object_get(root, "threads"), config->thread_count);
    config->retries = json_int_value(json_object_get(root, "retries"), config->retries);
    config->reconnect_delay = json_int_value(json_object_get(root, "retry-pause"), config->reconnect_delay);
    config->reconnect_delay = json_int_value(json_object_get(root, "reconnect-delay"), config->reconnect_delay);
    config->stats_interval = json_number_value_or(json_object_get(root, "print-time"), config->stats_interval);
    config->stats_interval = json_number_value_or(json_object_get(root, "stats"), config->stats_interval);
    config->runtime_seconds = json_number_value_or(json_object_get(root, "runtime"), config->runtime_seconds);
    json_t *donate_level = json_object_get(root, "donate-level");
    if (json_is_integer(donate_level)) {
        app_config_set_donate_level(config, (int)json_integer_value(donate_level), "config");
    }

    json_t *pools = json_object_get(root, "pools");
    if (json_is_array(pools)) {
        int count = 0;
        size_t index;
        json_t *pool;
        json_array_foreach(pools, index, pool) {
            if (!json_is_object(pool) || count >= MAX_POOLS) {
                continue;
            }

            const char *url = json_string_value(json_object_get(pool, "url"));
            if (url == NULL || url[0] == '\0') {
                continue;
            }

            pool_config_t *dst = &config->pools[count];
            memset(dst, 0, sizeof(*dst));
            copy_string(dst->url, sizeof(dst->url), url);
            copy_string(dst->user, sizeof(dst->user),
                        json_string_value(json_object_get(pool, "user")));
            copy_string(dst->pass, sizeof(dst->pass),
                        json_string_value(json_object_get(pool, "pass")));
            if (dst->user[0] == '\0') {
                copy_string(dst->user, sizeof(dst->user), DEFAULT_USER);
            }
            if (dst->pass[0] == '\0') {
                copy_string(dst->pass, sizeof(dst->pass), DEFAULT_PASSWORD);
            }
            dst->difficulty = json_number_value_or(json_object_get(pool, "diff"), DEFAULT_SUGGEST_DIFFICULTY);
            dst->difficulty = json_number_value_or(json_object_get(pool, "difficulty"), dst->difficulty);
            ++count;
        }

        if (count > 0) {
            config->pool_count = count;
        }
    }

    json_decref(root);
    return 0;
}

static void autotune_make_job(miner_job_t *job) {
    memset(job, 0, sizeof(*job));
    copy_string(job->job_id, sizeof(job->job_id), "autotune");
    copy_string(job->extranonce2, sizeof(job->extranonce2), "00000000");
    copy_string(job->ntime, sizeof(job->ntime), "00000000");
    job->header[0] = 0x01;
    job->header[68] = 0xff;
    job->header[69] = 0xff;
    job->header[70] = 0x00;
    job->header[71] = 0x1d;
    memset(job->target, 0, sizeof(job->target));
}

static void autotune_disable_opencl(miner_opencl_config_t *config) {
    miner_opencl_config_defaults(config);
    config->enabled = 0;
}

#if defined(BTC_MINER_OPENCL)
static void autotune_all_gpu_config(const miner_opencl_config_t *base, miner_opencl_config_t *out) {
    miner_opencl_config_defaults(out);
    if (base != NULL) {
        *out = *base;
    }
    out->enabled = 1;
    out->all_devices = 1;
    out->device_count = 0;
}

static void autotune_device_list_config(const miner_opencl_config_t *base,
                                        const miner_opencl_device_config_t *devices,
                                        int count,
                                        miner_opencl_config_t *out) {
    miner_opencl_config_defaults(out);
    if (base != NULL) {
        *out = *base;
    }
    out->enabled = count > 0 ? 1 : 0;
    out->all_devices = 0;
    out->device_count = 0;
    for (int i = 0; i < count && i < MINER_OPENCL_MAX_DEVICES; ++i) {
        out->devices[out->device_count++] = devices[i];
    }
    if (out->device_count > 0) {
        out->platform = out->devices[0].platform;
        out->device = out->devices[0].device;
    }
}
#endif

static double autotune_run_mode(int cpu_threads,
                                const miner_opencl_config_t *opencl,
                                double seconds,
                                int *ok) {
    if (ok != NULL) {
        *ok = 0;
    }
    if (seconds < 0.25) {
        seconds = 0.25;
    }

    miner_t *miner = miner_create_with_options(cpu_threads, opencl);
    if (miner == NULL) {
        return 0.0;
    }
    if (miner_start(miner) != 0) {
        miner_destroy(miner);
        return 0.0;
    }
    if (opencl != NULL && opencl->enabled && miner_thread_count(miner) <= cpu_threads) {
        miner_destroy(miner);
        return 0.0;
    }

    miner_job_t job;
    autotune_make_job(&job);
    miner_set_job(miner, &job);

    double started_at = monotonic_seconds();
    double stop_at = started_at + seconds;
    uint64_t start_hashes = miner_hashes(miner);
    while (monotonic_seconds() < stop_at) {
        sleep_milliseconds(50);
    }
    double ended_at = monotonic_seconds();
    uint64_t end_hashes = miner_hashes(miner);
    miner_destroy(miner);

    double elapsed = ended_at - started_at;
    if (elapsed <= 0.0 || end_hashes <= start_hashes) {
        return 0.0;
    }
    if (ok != NULL) {
        *ok = 1;
    }
    return (double)(end_hashes - start_hashes) / elapsed;
}

#if defined(BTC_MINER_OPENCL)
static int value_seen_u32(const uint32_t *values, int count, uint32_t value) {
    for (int i = 0; i < count; ++i) {
        if (values[i] == value) {
            return 1;
        }
    }
    return 0;
}

static int autotune_hashrate_is_better(double candidate, double best) {
    const double threshold = 1.02;

    if (best <= 0.0) {
        return 1;
    }
    return candidate > best * threshold;
}

static void autotune_opencl_device_params(const miner_opencl_config_t *base,
                                          miner_opencl_device_config_t *device,
                                          int device_index,
                                          double seconds) {
    if (base == NULL || device == NULL) {
        return;
    }

    const uint32_t local_candidates_raw[] = {
        device->local_work_size,
        base->local_work_size,
        64U,
        128U,
        256U,
        512U,
    };
    const uint32_t npi_candidates_raw[] = {
        device->nonces_per_work_item,
        base->nonces_per_work_item,
        1U,
        2U,
        4U,
    };
    const uint32_t batch_candidates_raw[] = {
        device->batch_size,
        base->batch_size,
        524288U,
        1048576U,
        2097152U,
        4194304U,
    };
    const int kernel_candidates_raw[] = {
        device->kernel_variant,
        base->kernel_variant,
        MINER_OPENCL_KERNEL_UNROLLED,
        MINER_OPENCL_KERNEL_COMPACT,
    };
    uint32_t local_candidates[sizeof(local_candidates_raw) / sizeof(local_candidates_raw[0])];
    uint32_t npi_candidates[sizeof(npi_candidates_raw) / sizeof(npi_candidates_raw[0])];
    uint32_t batch_candidates[sizeof(batch_candidates_raw) / sizeof(batch_candidates_raw[0])];
    uint32_t kernel_candidates[sizeof(kernel_candidates_raw) / sizeof(kernel_candidates_raw[0])];
    int local_count = 0;
    int npi_count = 0;
    int batch_count = 0;
    int kernel_count = 0;

    for (size_t i = 0; i < sizeof(local_candidates_raw) / sizeof(local_candidates_raw[0]); ++i) {
        uint32_t value = local_candidates_raw[i];
        if (!value_seen_u32(local_candidates, local_count, value)) {
            local_candidates[local_count++] = value;
        }
    }
    for (size_t i = 0; i < sizeof(npi_candidates_raw) / sizeof(npi_candidates_raw[0]); ++i) {
        uint32_t value = npi_candidates_raw[i] == 0 ? 1U : npi_candidates_raw[i];
        if (!value_seen_u32(npi_candidates, npi_count, value)) {
            npi_candidates[npi_count++] = value;
        }
    }
    for (size_t i = 0; i < sizeof(batch_candidates_raw) / sizeof(batch_candidates_raw[0]); ++i) {
        uint32_t value = batch_candidates_raw[i] < 1024U ? 1024U : batch_candidates_raw[i];
        if (!value_seen_u32(batch_candidates, batch_count, value)) {
            batch_candidates[batch_count++] = value;
        }
    }
    for (size_t i = 0; i < sizeof(kernel_candidates_raw) / sizeof(kernel_candidates_raw[0]); ++i) {
        uint32_t value = (uint32_t)(kernel_candidates_raw[i] == MINER_OPENCL_KERNEL_COMPACT ?
            MINER_OPENCL_KERNEL_COMPACT : MINER_OPENCL_KERNEL_UNROLLED);
        if (!value_seen_u32(kernel_candidates, kernel_count, value)) {
            kernel_candidates[kernel_count++] = value;
        }
    }

    miner_opencl_device_config_t best_device = *device;
    double best_hashrate = 0.0;
    int best_ok = 0;

    printf("%s[AUTOTUNE]%s tuning gpu%d OpenCL kernel/local-work-size/npi\n",
           C_CYAN,
           C_RESET,
           device_index);

    for (int ki = 0; ki < kernel_count; ++ki) {
        for (int li = 0; li < local_count; ++li) {
            for (int ni = 0; ni < npi_count; ++ni) {
            miner_opencl_device_config_t candidate = *device;
            miner_opencl_config_t opencl;
            int ok = 0;

            candidate.kernel_variant = (int)kernel_candidates[ki];
            candidate.local_work_size = local_candidates[li];
            candidate.nonces_per_work_item = npi_candidates[ni];
            candidate.batch_size = best_device.batch_size;
            autotune_device_list_config(base, &candidate, 1, &opencl);

            printf("%s[AUTOTUNE]%s testing gpu%d kernel=%s batch=%u local=%u npi=%u\n",
                   C_CYAN,
                   C_RESET,
                   device_index,
                   opencl_kernel_variant_name(candidate.kernel_variant),
                   candidate.batch_size,
                   candidate.local_work_size,
                   candidate.nonces_per_work_item);
            double hashrate = autotune_run_mode(0, &opencl, seconds, &ok);
            if (ok) {
                printf("%s[AUTOTUNE]%s gpu%d kernel=%s batch=%u local=%u npi=%u hashrate=%.3f MH/s\n",
                       C_CYAN,
                       C_RESET,
                       device_index,
                       opencl_kernel_variant_name(candidate.kernel_variant),
                       candidate.batch_size,
                       candidate.local_work_size,
                       candidate.nonces_per_work_item,
                       hashrate / 1000000.0);
            } else {
                printf("%s[AUTOTUNE]%s gpu%d kernel=%s batch=%u local=%u npi=%u unavailable\n",
                       C_YELLOW,
                       C_RESET,
                       device_index,
                       opencl_kernel_variant_name(candidate.kernel_variant),
                       candidate.batch_size,
                       candidate.local_work_size,
                       candidate.nonces_per_work_item);
            }
            if (ok && (!best_ok || autotune_hashrate_is_better(hashrate, best_hashrate))) {
                best_ok = 1;
                best_hashrate = hashrate;
                best_device = candidate;
            }
        }
        }
    }

    if (best_ok) {
        *device = best_device;
        printf("%s[AUTOTUNE]%s gpu%d selected kernel=%s batch=%u local=%u npi=%u hashrate=%.3f MH/s\n",
               C_BRIGHT_GREEN,
               C_RESET,
               device_index,
               opencl_kernel_variant_name(device->kernel_variant),
               device->batch_size,
               device->local_work_size,
               device->nonces_per_work_item,
               best_hashrate / 1000000.0);
    }

    if (!best_ok) {
        return;
    }

    printf("%s[AUTOTUNE]%s tuning gpu%d OpenCL batch-size\n",
           C_CYAN,
           C_RESET,
           device_index);

    for (int bi = 0; bi < batch_count; ++bi) {
        miner_opencl_device_config_t candidate = *device;
        miner_opencl_config_t opencl;
        int ok = 0;

        candidate.batch_size = batch_candidates[bi];
        autotune_device_list_config(base, &candidate, 1, &opencl);

        printf("%s[AUTOTUNE]%s testing gpu%d kernel=%s batch=%u local=%u npi=%u\n",
               C_CYAN,
               C_RESET,
               device_index,
               opencl_kernel_variant_name(candidate.kernel_variant),
               candidate.batch_size,
               candidate.local_work_size,
               candidate.nonces_per_work_item);
        double hashrate = autotune_run_mode(0, &opencl, seconds, &ok);
        if (ok) {
            printf("%s[AUTOTUNE]%s gpu%d kernel=%s batch=%u local=%u npi=%u hashrate=%.3f MH/s\n",
                   C_CYAN,
                   C_RESET,
                   device_index,
                   opencl_kernel_variant_name(candidate.kernel_variant),
                   candidate.batch_size,
                   candidate.local_work_size,
                   candidate.nonces_per_work_item,
                   hashrate / 1000000.0);
        } else {
            printf("%s[AUTOTUNE]%s gpu%d kernel=%s batch=%u local=%u npi=%u unavailable\n",
                   C_YELLOW,
                   C_RESET,
                   device_index,
                   opencl_kernel_variant_name(candidate.kernel_variant),
                   candidate.batch_size,
                   candidate.local_work_size,
                   candidate.nonces_per_work_item);
        }
        if (ok && autotune_hashrate_is_better(hashrate, best_hashrate)) {
            best_hashrate = hashrate;
            best_device = candidate;
            *device = best_device;
        }
    }

    printf("%s[AUTOTUNE]%s gpu%d final kernel=%s batch=%u local=%u npi=%u hashrate=%.3f MH/s\n",
           C_BRIGHT_GREEN,
           C_RESET,
           device_index,
           opencl_kernel_variant_name(device->kernel_variant),
           device->batch_size,
           device->local_work_size,
           device->nonces_per_work_item,
           best_hashrate / 1000000.0);
}
#endif

static const char *autotune_opencl_mode_name(const miner_opencl_config_t *opencl) {
    if (opencl == NULL || !opencl->enabled) {
        return "off";
    }
    if (opencl->device_count > 0) {
        return "devices";
    }
    return opencl->all_devices ? "all-gpu" : "single";
}

static int autotune_append_result(autotune_result_t *results,
                                  int *count,
                                  const char *name,
                                  int cpu_threads,
                                  const miner_opencl_config_t *opencl,
                                  double seconds) {
    if (results == NULL || count == NULL || *count >= AUTOTUNE_MAX_RESULTS) {
        return -1;
    }

    autotune_result_t *result = &results[*count];
    memset(result, 0, sizeof(*result));
    copy_string(result->name, sizeof(result->name), name);
    result->cpu_threads = cpu_threads;
    if (opencl != NULL) {
        result->opencl = *opencl;
    } else {
        autotune_disable_opencl(&result->opencl);
    }

    printf("%s[AUTOTUNE]%s testing mode=%s%s%s cpu-threads=%d opencl=%s\n",
           C_CYAN,
           C_RESET,
           C_BRIGHT_YELLOW,
           result->name,
           C_RESET,
           cpu_threads,
           autotune_opencl_mode_name(&result->opencl));
    result->hashrate = autotune_run_mode(cpu_threads, &result->opencl, seconds, &result->ok);
    if (result->ok) {
        printf("%s[AUTOTUNE]%s mode=%s%s%s hashrate=%.3f MH/s\n",
               C_CYAN,
               C_RESET,
               C_BRIGHT_GREEN,
               result->name,
               C_RESET,
               result->hashrate / 1000000.0);
    } else {
        printf("%s[AUTOTUNE]%s mode=%s%s%s unavailable\n",
               C_YELLOW,
               C_RESET,
               C_BRIGHT_YELLOW,
               result->name,
               C_RESET);
    }
    ++(*count);
    return 0;
}

static json_t *json_object_get_or_create(json_t *parent, const char *key) {
    json_t *value = json_object_get(parent, key);
    if (json_is_object(value)) {
        return value;
    }

    value = json_object();
    if (value == NULL) {
        return NULL;
    }
    if (json_object_set_new(parent, key, value) != 0) {
        json_decref(value);
        return NULL;
    }
    return value;
}

static int save_autotune_config(const char *path,
                                const app_config_t *config,
                                const autotune_result_t *results,
                                int result_count,
                                const autotune_result_t *best) {
    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "%s[AUTOTUNE]%s no config path available; result was not saved\n", C_YELLOW, C_RESET);
        return -1;
    }

    json_error_t error;
    json_t *root = json_load_file_allow_bom(path, &error);
    if (root != NULL && !json_is_object(root)) {
        json_decref(root);
        root = NULL;
    }
    if (root == NULL) {
        root = json_object();
        if (root == NULL) {
            return -1;
        }
    }

    json_object_set_new(root, "autosave", config->autosave ? json_true() : json_false());

    json_t *cpu = json_object_get_or_create(root, "cpu");
    json_t *opencl = json_object_get_or_create(root, "opencl");
    json_t *autotune = json_object_get_or_create(root, "autotune");
    if (cpu == NULL || opencl == NULL || autotune == NULL) {
        json_decref(root);
        return -1;
    }

    json_object_set_new(cpu, "enabled", config->cpu_enabled ? json_true() : json_false());
    json_object_set_new(cpu, "threads", json_integer(config->thread_count));

    json_object_set_new(opencl, "enabled", config->opencl.enabled ? json_true() : json_false());
    json_object_set_new(opencl, "all-devices", config->opencl.all_devices ? json_true() : json_false());
    json_object_set_new(opencl, "platform", json_integer(config->opencl.platform));
    json_object_set_new(opencl, "device", json_integer(config->opencl.device));
    json_object_set_new(opencl, "batch-size", json_integer((json_int_t)config->opencl.batch_size));
    json_object_set_new(opencl, "local-work-size", json_integer((json_int_t)config->opencl.local_work_size));
    json_object_set_new(opencl, "nonces-per-work-item", json_integer((json_int_t)config->opencl.nonces_per_work_item));
    json_object_set_new(opencl, "max-results", json_integer((json_int_t)config->opencl.max_results));
    json_object_set_new(opencl, "kernel", json_string(opencl_kernel_variant_name(config->opencl.kernel_variant)));
    if (config->opencl.enabled && config->opencl.device_count > 0) {
        json_t *devices = json_array();
        if (devices == NULL) {
            json_decref(root);
            return -1;
        }
        for (int i = 0; i < config->opencl.device_count; ++i) {
            const miner_opencl_device_config_t *device = &config->opencl.devices[i];
            json_t *item = json_object();
            if (item == NULL) {
                json_decref(devices);
                json_decref(root);
                return -1;
            }
            json_object_set_new(item, "platform", json_integer(device->platform));
            json_object_set_new(item, "device", json_integer(device->device));
            json_object_set_new(item, "batch-size", json_integer((json_int_t)device->batch_size));
            json_object_set_new(item, "local-work-size", json_integer((json_int_t)device->local_work_size));
            json_object_set_new(item, "nonces-per-work-item", json_integer((json_int_t)device->nonces_per_work_item));
            json_object_set_new(item, "max-results", json_integer((json_int_t)device->max_results));
            json_object_set_new(item, "kernel", json_string(opencl_kernel_variant_name(device->kernel_variant)));
            json_array_append_new(devices, item);
        }
        json_object_set_new(opencl, "devices", devices);
    } else {
        json_object_del(opencl, "devices");
    }

    json_object_set_new(autotune, "enabled", config->autotune_enabled ? json_true() : json_false());
    json_object_set_new(autotune, "cpu-self-test", config->cpu_autotune_done ? json_true() : json_false());
    json_object_set_new(autotune, "gpu-self-test", config->gpu_autotune_done ? json_true() : json_false());
    json_object_del(autotune, "self-test");
    json_object_del(autotune, "self_test");
    json_object_del(autotune, "done");
    json_object_del(autotune, "completed");
    json_object_set_new(autotune, "seconds", json_real(config->autotune_seconds));
    if (best != NULL) {
        json_object_set_new(autotune, "selected", json_string(best->name));
        json_object_set_new(autotune, "selected-hashrate", json_real(best->hashrate));
    }

    json_t *array = json_array();
    if (array == NULL) {
        json_decref(root);
        return -1;
    }
    for (int i = 0; i < result_count; ++i) {
        const autotune_result_t *result = &results[i];
        json_t *item = json_object();
        if (item == NULL) {
            json_decref(array);
            json_decref(root);
            return -1;
        }
        json_object_set_new(item, "mode", json_string(result->name));
        json_object_set_new(item, "ok", result->ok ? json_true() : json_false());
        json_object_set_new(item, "hashrate", json_real(result->hashrate));
        json_object_set_new(item, "cpu-threads", json_integer(result->cpu_threads));
        json_object_set_new(item, "opencl", json_string(autotune_opencl_mode_name(&result->opencl)));
        if (result->opencl.device_count > 0) {
            json_t *devices = json_array();
            if (devices != NULL) {
                for (int j = 0; j < result->opencl.device_count; ++j) {
                    const miner_opencl_device_config_t *device = &result->opencl.devices[j];
                    json_t *device_item = json_object();
                    if (device_item == NULL) {
                        continue;
                    }
                    json_object_set_new(device_item, "platform", json_integer(device->platform));
                    json_object_set_new(device_item, "device", json_integer(device->device));
                    json_object_set_new(device_item, "batch-size", json_integer((json_int_t)device->batch_size));
                    json_object_set_new(device_item, "local-work-size", json_integer((json_int_t)device->local_work_size));
                    json_object_set_new(device_item, "nonces-per-work-item", json_integer((json_int_t)device->nonces_per_work_item));
                    json_object_set_new(device_item, "kernel", json_string(opencl_kernel_variant_name(device->kernel_variant)));
                    json_array_append_new(devices, device_item);
                }
                json_object_set_new(item, "devices", devices);
            }
        }
        json_array_append_new(array, item);
    }
    json_object_set_new(autotune, "results", array);

    int rc = json_dump_file(root, path, JSON_INDENT(2));
    json_decref(root);
    if (rc != 0) {
        fprintf(stderr, "%s[AUTOTUNE]%s failed to save %s\n", C_BRIGHT_RED, C_RESET, path);
        return -1;
    }

    printf("%s[AUTOTUNE]%s saved result to %s%s%s\n", C_CYAN, C_RESET, C_BRIGHT_CYAN, path, C_RESET);
    return 0;
}

static int run_autotune(app_config_t *config, const char *config_path) {
    autotune_result_t results[AUTOTUNE_MAX_RESULTS];
    int result_count = 0;
    miner_opencl_config_t cpu_only_opencl;

    int full_threads = config->thread_count > 0 ? config->thread_count : default_thread_count();
    if (full_threads < 0) {
        full_threads = 0;
    }
    double seconds = config->autotune_seconds > 0.0 ? config->autotune_seconds : DEFAULT_AUTOTUNE_SECONDS;

    int tune_opencl = config->opencl.enabled;

    printf("%s[AUTOTUNE]%s first-run benchmark seconds=%.1f strategy=%s\n",
           C_CYAN,
           C_RESET,
           seconds,
           tune_opencl ? "cpu,gpu-all,cpu+gpu-all,single-gpu,drop-one" : "cpu-only");

    autotune_disable_opencl(&cpu_only_opencl);
    if (full_threads > 0) {
        autotune_append_result(results, &result_count, "cpu", full_threads, &cpu_only_opencl, seconds);
    }

#if defined(BTC_MINER_OPENCL)
    if (!tune_opencl) {
        printf("%s[AUTOTUNE]%s OpenCL skipped: disabled in config; set opencl.enabled=true or use --opencl to benchmark GPU modes\n",
               C_YELLOW,
               C_RESET);
    } else {
        miner_opencl_config_t all_gpu_opencl;
        miner_opencl_device_config_t resolved[MINER_OPENCL_MAX_DEVICES];
        int resolved_count = 0;
        int half_threads = full_threads > 2 ? full_threads / 2 : (full_threads > 1 ? 1 : full_threads);
        char error[2048];
        error[0] = '\0';

        autotune_all_gpu_config(&config->opencl, &all_gpu_opencl);
        resolved_count = opencl_miner_resolve_devices(&all_gpu_opencl,
                                                      resolved,
                                                      MINER_OPENCL_MAX_DEVICES,
                                                      error,
                                                      sizeof(error));
        if (resolved_count <= 0) {
            printf("%s[AUTOTUNE]%s OpenCL skipped: %s\n",
                   C_YELLOW,
                   C_RESET,
                   error[0] != '\0' ? error : "no OpenCL GPU devices found");
        } else {
            for (int i = 0; i < resolved_count; ++i) {
                autotune_opencl_device_params(&config->opencl, &resolved[i], i, seconds);
            }

            autotune_device_list_config(&config->opencl, resolved, resolved_count, &all_gpu_opencl);

            autotune_append_result(results, &result_count, "all-gpu", 0, &all_gpu_opencl, seconds);
            if (full_threads > 0) {
                autotune_append_result(results, &result_count, "cpu+all-gpu", full_threads, &all_gpu_opencl, seconds);
                if (half_threads > 0 && half_threads != full_threads) {
                    autotune_append_result(results, &result_count, "half-cpu+all-gpu", half_threads, &all_gpu_opencl, seconds);
                }
            }

            for (int i = 0; i < resolved_count; ++i) {
                miner_opencl_config_t device_opencl;
                char name[96];
                autotune_device_list_config(&config->opencl, &resolved[i], 1, &device_opencl);
                snprintf(name, sizeof(name), "gpu%d", i);
                autotune_append_result(results, &result_count, name, 0, &device_opencl, seconds);
                if (full_threads > 0) {
                    snprintf(name, sizeof(name), "cpu+gpu%d", i);
                    autotune_append_result(results, &result_count, name, full_threads, &device_opencl, seconds);
                }
            }

            if (resolved_count > 2) {
                for (int skip = 0; skip < resolved_count; ++skip) {
                    miner_opencl_device_config_t subset[MINER_OPENCL_MAX_DEVICES];
                    int subset_count = 0;
                    char name[96];
                    for (int i = 0; i < resolved_count; ++i) {
                        if (i != skip) {
                            subset[subset_count++] = resolved[i];
                        }
                    }
                    miner_opencl_config_t subset_opencl;
                    autotune_device_list_config(&config->opencl, subset, subset_count, &subset_opencl);
                    snprintf(name, sizeof(name), "all-gpu-minus-gpu%d", skip);
                    autotune_append_result(results, &result_count, name, 0, &subset_opencl, seconds);
                    if (full_threads > 0) {
                        snprintf(name, sizeof(name), "cpu+all-gpu-minus-gpu%d", skip);
                        autotune_append_result(results, &result_count, name, full_threads, &subset_opencl, seconds);
                    }
                }
            }
        }
    }
#else
    printf("%s[AUTOTUNE]%s OpenCL skipped: this build was compiled without OpenCL support\n", C_YELLOW, C_RESET);
#endif

    const autotune_result_t *best = NULL;
    int cpu_autotune_ok = 0;
    int gpu_autotune_ok = 0;
    for (int i = 0; i < result_count; ++i) {
        if (!results[i].ok) {
            continue;
        }
        if (results[i].cpu_threads > 0) {
            cpu_autotune_ok = 1;
        }
        if (results[i].opencl.enabled) {
            gpu_autotune_ok = 1;
        }
        if (best == NULL || results[i].hashrate > best->hashrate) {
            best = &results[i];
        }
    }
    if (best == NULL) {
        fprintf(stderr, "%s[AUTOTUNE]%s no working mining mode found\n", C_BRIGHT_RED, C_RESET);
        return -1;
    }

    config->cpu_enabled = best->cpu_threads > 0 ? 1 : 0;
    config->thread_count = best->cpu_threads;
    config->opencl = best->opencl;
    if (cpu_autotune_ok) {
        config->cpu_autotune_done = 1;
    }
    if (gpu_autotune_ok) {
        config->gpu_autotune_done = 1;
    }

    printf("%s[AUTOTUNE]%s selected mode=%s%s%s hashrate=%.3f MH/s cpu-threads=%d opencl=%s\n",
           C_BRIGHT_GREEN,
           C_RESET,
           C_BRIGHT_GREEN,
           best->name,
           C_RESET,
           best->hashrate / 1000000.0,
           config->thread_count,
           autotune_opencl_mode_name(&config->opencl));

    if (config->autosave) {
        save_autotune_config(config_path, config, results, result_count, best);
    } else {
        printf("%s[AUTOTUNE]%s autosave=false; result kept for this run only\n", C_YELLOW, C_RESET);
    }
    return 0;
}

enum {
    APP_AUTOTUNE_REQUIRED_CPU = 1 << 0,
    APP_AUTOTUNE_REQUIRED_GPU = 1 << 1
};

static int app_config_autotune_required(const app_config_t *config) {
    if (config == NULL || !config->autotune_enabled || !config->enable_mining) {
        return 0;
    }
    int required = 0;
    if (config->cpu_enabled && config->thread_count > 0 && !config->cpu_autotune_done) {
        required |= APP_AUTOTUNE_REQUIRED_CPU;
    }
    if (config->opencl.enabled && !config->gpu_autotune_done) {
        required |= APP_AUTOTUNE_REQUIRED_GPU;
    }
    return required;
}

static const char *find_config_arg(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static void usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s --self-test\n", argv0);
    printf("  %s --opencl-self-test [--opencl-platform N] [--opencl-device N]\n", argv0);
    printf("  %s --cpu-info\n", argv0);
    printf("  %s --version\n", argv0);
    printf("  %s [-c config.json] [-o stratum+tls://host:port] [-u wallet.worker] [-p password] [-d difficulty]\n", argv0);
    printf("     [-t threads] [-r retries] [--runtime seconds] [--stats seconds]\n");
    printf("     [--reconnect-delay seconds] [--donate-level N] [--no-mine]\n");
    printf("     [--no-cpu] [--opencl] [--opencl-all] [--opencl-platform N] [--opencl-device N]\n");
    printf("     [--opencl-batch N] [--opencl-local N] [--opencl-npi N] [--opencl-kernel auto|compact|unrolled]\n");
    printf("     [--autotune] [--no-autotune] [--autotune-seconds N]\n");
    printf("\nDefaults:\n");
    printf("  version: %s\n", BTCRIG_VERSION_TAG);
    printf("  agent: %s\n", BTCRIG_USER_AGENT);
    printf("  pool: %s\n", DEFAULT_POOL_URL);
    printf("  user: %s\n", DEFAULT_USER);
    printf("  pass: %s\n", DEFAULT_PASSWORD);
    printf("  diff: %.6f\n", DEFAULT_SUGGEST_DIFFICULTY);
    printf("  retries: infinite\n");
    printf("  reconnect-delay: %d..%d seconds\n", DEFAULT_RECONNECT_DELAY, MAX_RECONNECT_DELAY);
    printf("  stats: %.1f seconds\n", DEFAULT_STATS_INTERVAL);
    printf("  threads: auto (%d recommended)\n", default_thread_count());
    printf("  opencl: manual enable uses all OpenCL GPU devices unless a device is selected; autotune may select it\n");
    printf("  opencl compat10: OpenCL 1.0/1.1 compatible, requires global int32 atomics on 1.0 devices\n");
    printf("  opencl kernel: auto prefers unrolled and can fall back to compact; autotune tests compact and unrolled\n");
    printf("  autotune: enabled by default; GPU modes are benchmarked only when OpenCL is enabled\n");
    printf("  donate-level: %d%% (%s)\n",
           DONATION_DEFAULT_LEVEL,
           DONATION_DEFAULT_LEVEL == 0 ? "disabled" : "minutes per 100 minutes");
    printf("\nNotes:\n");
    printf("  TLS and plain TCP are supported: stratum+tls://host:port or stratum+tcp://host:port.\n");
    printf("  stratum+tls:// verifies trusted certificates first, then accepts self-signed TLS if needed.\n");
    printf("  Use stratum+tls-insecure://host:port to skip certificate verification immediately.\n");
    printf("  Network reconnects are always infinite; -r is accepted for compatibility.\n");
    printf("  Set BTC_MINER_VERBOSE_SHARES=0 to hide per-share submit and accepted logs.\n");
    printf("  Use --runtime 0 for no time limit.\n");
}

static int run_self_test(void) {
    stratum_state_t state;
    const char *lines[] = {
        "{\"id\":1,\"result\":[[[\"mining.set_difficulty\",\"1\"],[\"mining.notify\",\"2\"]],\"abcd1234\",4],\"error\":null}",
        "{\"id\":2,\"result\":true,\"error\":null}",
        "{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[0.00015]}",
        "{\"id\":null,\"method\":\"mining.notify\",\"params\":[\"job1\",\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\",\"coinb1\",\"coinb2\",[],\"20000000\",\"170fffff\",\"665ee001\",true]}",
        NULL,
    };

    if (donation_phase_seconds(1, 1) != 60.0 ||
        donation_phase_seconds(1, 0) != 5940.0 ||
        !donation_level_valid(DONATION_DEFAULT_LEVEL) ||
        donation_level_valid(DONATION_MINIMUM_LEVEL - 1)) {
        fprintf(stderr, "donation schedule self-test failed\n");
        return 1;
    }

    stratum_state_init(&state);
    for (int i = 0; lines[i] != NULL; ++i) {
        if (stratum_process_line(&state, lines[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

static int handle_early_command(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", BTCRIG_NAME, BTCRIG_VERSION_TAG);
            return 0;
        }
        if (strcmp(argv[i], "--cpu-info") == 0) {
            cpu_info_t info;
            cpu_info_detect(&info);
            cpu_info_print(&info);
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    app_config_t app_config;
    const char *config_path = NULL;
    int config_required = 0;
    int early_rc = 0;
    int opencl_self_test = 0;
    int worker_override = 0;
    int force_autotune = 0;

    configure_stdio();
    if (configure_sha_backend_from_env() != 0) {
        return 2;
    }

    early_rc = handle_early_command(argc, argv);
    if (early_rc >= 0) {
        return early_rc;
    }

    app_config_set_defaults(&app_config);
    config_path = find_config_arg(argc, argv);
    if (config_path != NULL) {
        config_required = 1;
    } else if (BTC_ACCESS("config.json", BTC_R_OK) == 0) {
        config_path = "config.json";
    }

    if (config_path != NULL && load_config_file(&app_config, config_path, config_required) != 0) {
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", BTCRIG_NAME, BTCRIG_VERSION_TAG);
            return 0;
        } else if (strcmp(argv[i], "--cpu-info") == 0) {
            cpu_info_t info;
            cpu_info_detect(&info);
            cpu_info_print(&info);
            return 0;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            ++i;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--url") == 0) && i + 1 < argc) {
            copy_string(app_config.pools[0].url, sizeof(app_config.pools[0].url), argv[++i]);
            if (app_config.pool_count <= 0) {
                app_config.pool_count = 1;
            }
        } else if ((strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0) && i + 1 < argc) {
            copy_string(app_config.pools[0].user, sizeof(app_config.pools[0].user), argv[++i]);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pass") == 0) && i + 1 < argc) {
            copy_string(app_config.pools[0].pass, sizeof(app_config.pools[0].pass), argv[++i]);
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--suggest-diff") == 0) && i + 1 < argc) {
            app_config.pools[0].difficulty = strtod(argv[++i], NULL);
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--retries") == 0) && i + 1 < argc) {
            app_config.retries = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) && i + 1 < argc) {
            app_config.thread_count = atoi(argv[++i]);
            app_config.cpu_enabled = 1;
            worker_override = 1;
        } else if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
            app_config.runtime_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            app_config.stats_interval = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--reconnect-delay") == 0 && i + 1 < argc) {
            app_config.reconnect_delay = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--donate-level") == 0 && i + 1 < argc) {
            app_config_set_donate_level(&app_config, atoi(argv[++i]), "command-line");
        } else if (strcmp(argv[i], "--no-mine") == 0) {
            app_config.enable_mining = 0;
        } else if (strcmp(argv[i], "--no-cpu") == 0) {
            app_config.cpu_enabled = 0;
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl") == 0) {
            app_config.opencl.enabled = 1;
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl-all") == 0) {
            app_config.opencl.enabled = 1;
            app_config.opencl.all_devices = 1;
            app_config.opencl.device_count = 0;
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl-self-test") == 0) {
            opencl_self_test = 1;
            app_config.opencl.enabled = 1;
        } else if (strcmp(argv[i], "--opencl-platform") == 0 && i + 1 < argc) {
            app_config.opencl.platform = atoi(argv[++i]);
            app_config.opencl.all_devices = 0;
            app_config.opencl.device_count = 0;
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl-device") == 0 && i + 1 < argc) {
            app_config.opencl.device = atoi(argv[++i]);
            app_config.opencl.all_devices = 0;
            app_config.opencl.device_count = 0;
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl-batch") == 0 && i + 1 < argc) {
            app_config.opencl.batch_size = (uint32_t)strtoul(argv[++i], NULL, 10);
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl-local") == 0 && i + 1 < argc) {
            app_config.opencl.local_work_size = (uint32_t)strtoul(argv[++i], NULL, 10);
            worker_override = 1;
        } else if ((strcmp(argv[i], "--opencl-npi") == 0 || strcmp(argv[i], "--opencl-nonces-per-work-item") == 0) && i + 1 < argc) {
            app_config.opencl.nonces_per_work_item = (uint32_t)strtoul(argv[++i], NULL, 10);
            worker_override = 1;
        } else if (strcmp(argv[i], "--opencl-kernel") == 0 && i + 1 < argc) {
            int parsed = parse_opencl_kernel_variant(argv[++i], -1);
            if (parsed < 0) {
                fprintf(stderr, "%s[CONFIG]%s invalid --opencl-kernel, use auto, compact, or unrolled\n",
                        C_BRIGHT_RED,
                        C_RESET);
                return 2;
            }
            app_config.opencl.kernel_variant = parsed;
            worker_override = 1;
        } else if (strcmp(argv[i], "--autotune") == 0) {
            app_config.autotune_enabled = 1;
            app_config.cpu_autotune_done = 0;
            app_config.gpu_autotune_done = 0;
            force_autotune = 1;
        } else if (strcmp(argv[i], "--no-autotune") == 0) {
            app_config.autotune_enabled = 0;
        } else if (strcmp(argv[i], "--autotune-seconds") == 0 && i + 1 < argc) {
            app_config.autotune_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (opencl_self_test) {
        return run_opencl_self_test(&app_config.opencl);
    }

    if (app_config.pool_count <= 0) {
        fprintf(stderr, "%s[CONFIG]%s no pools configured\n", C_BRIGHT_RED, C_RESET);
        return 1;
    }
    if (app_config.cpu_enabled && app_config.thread_count <= 0) {
        app_config.thread_count = default_thread_count();
    }
    if (!app_config.cpu_enabled) {
        app_config.thread_count = 0;
    }
    if (app_config.reconnect_delay < 0) {
        app_config.reconnect_delay = DEFAULT_RECONNECT_DELAY;
    }
    if (app_config.stats_interval < 0.0) {
        app_config.stats_interval = 0.0;
    }
    if (!donation_level_valid(app_config.donate_level)) {
        fprintf(stderr, "%s[CONFIG]%s default donate-level must be between %d and 99\n",
                C_BRIGHT_RED, C_RESET, DONATION_MINIMUM_LEVEL);
        return 2;
    }
    if (!(app_config.autotune_seconds >= 0.25)) {
        app_config.autotune_seconds = 0.25;
    }

    int autotune_required = app_config_autotune_required(&app_config);
    if (autotune_required != 0) {
        if (worker_override && !force_autotune && (autotune_required & APP_AUTOTUNE_REQUIRED_GPU) == 0) {
            printf("%s[AUTOTUNE]%s skipped because worker options were set on the command line; use --autotune to force\n",
                   C_YELLOW,
                   C_RESET);
        } else if (run_autotune(&app_config, config_path) != 0) {
            return 1;
        }
    }

    app_config.retries = -1;

    print_sha_backend_summary();

    int worker_enabled = app_config.cpu_enabled || app_config.opencl.enabled;
    int mining_enabled = app_config.enable_mining && worker_enabled;

    const char *opencl_mode = !app_config.opencl.enabled ? "off" :
        (app_config.opencl.device_count > 0 ? "devices" :
            (app_config.opencl.all_devices ? "all-gpu" : "single"));

    printf("%s[CONFIG]%s pools=%d cpu=%s threads=%s%d%s opencl=%s mode=%s mine=%s retries=infinite retry-pause=%d..%d stats=%.1f runtime=%.1f donate=%d%%\n",
           C_CYAN,
           C_RESET,
           app_config.pool_count,
           app_config.cpu_enabled ? "on" : "off",
           C_BRIGHT_GREEN,
           app_config.thread_count,
           C_RESET,
           app_config.opencl.enabled ? "on" : "off",
           opencl_mode,
           mining_enabled ? "yes" : "no",
           app_config.reconnect_delay,
           MAX_RECONNECT_DELAY,
           app_config.stats_interval,
           app_config.runtime_seconds,
           app_config.donate_level);

    double stop_at = app_config.runtime_seconds > 0.0 ? monotonic_seconds() + app_config.runtime_seconds : 0.0;
    int last_rc = 0;
    int pool_index = 0;
    int donating = 0;
    int donation_enabled = mining_enabled && app_config.donate_level > 0;
    double phase_seconds = donation_enabled ?
        donation_initial_user_seconds(app_config.donate_level) : 0.0;

    if (donation_enabled) {
        printf("%s[DONATE]%s level=%d%% address=%s pool=same-as-user\n",
               C_MAGENTA, C_RESET, app_config.donate_level, DONATION_USER);
        printf("%s[DONATE]%s first round scheduled after %.1f minutes of active user mining\n",
               C_MAGENTA, C_RESET, phase_seconds / 60.0);
    }

    unsigned long attempt = 0;
    for (;;) {
        if (stop_at > 0.0 && monotonic_seconds() >= stop_at) {
            printf("%s[RUN]%s runtime limit reached before reconnect\n", C_GRAY, C_RESET);
            break;
        }

        if (attempt > 0) {
            printf("%s[RETRY]%s attempt %lu/infinite\n", C_YELLOW, C_RESET, attempt + 1);
        }

        pool_config_t donate_pool;
        pool_config_t *pool = &app_config.pools[pool_index];
        if (donating) {
            donate_pool = *pool;
            copy_string(donate_pool.user, sizeof(donate_pool.user), DONATION_USER);
            pool = &donate_pool;
        }

        printf("%s[POOLCFG]%s mode=%s index=%d url=%s%s%s user=%s diff=%.6f\n",
               C_CYAN,
               C_RESET,
               donating ? "donate" : "user",
               pool_index,
               C_BRIGHT_CYAN,
               pool->url,
               C_RESET,
               pool->user,
               pool->difficulty);

        stratum_client_config_t config = {
            .thread_count = app_config.thread_count,
            .enable_mining = mining_enabled,
            .opencl = app_config.opencl,
            .stats_interval = app_config.stats_interval,
            .stop_at = stop_at,
            .session_seconds = phase_seconds,
            .session_label = donating ? "donate" : "user",
        };
        last_rc = stratum_run_client(pool->url, pool->user, pool->pass, pool->difficulty, &config);

        if (stop_at > 0.0 && monotonic_seconds() >= stop_at) {
            break;
        }

        if (donation_enabled && last_rc == 0) {
            if (donating) {
                donating = 0;
                phase_seconds = donation_phase_seconds(app_config.donate_level, 0);
                printf("%s[DONATE]%s finished; returning to user mining for %.0f minutes\n",
                       C_MAGENTA, C_RESET, phase_seconds / 60.0);
            } else {
                donating = 1;
                phase_seconds = donation_phase_seconds(app_config.donate_level, 1);
                printf("%s[DONATE]%s starting %d%% donation round\n",
                       C_MAGENTA, C_RESET, app_config.donate_level);
            }
            attempt = 0;
            continue;
        }

        if (donating && last_rc != 0) {
            donating = 0;
            phase_seconds = donation_phase_seconds(app_config.donate_level, 0);
            printf("%s[DONATE]%s connection failed; returning to user mining\n",
                   C_YELLOW, C_RESET);
        } else if (app_config.pool_count > 1) {
            pool_index = (pool_index + 1) % app_config.pool_count;
        }

        {
            int delay = retry_delay_seconds(app_config.reconnect_delay, attempt + 1);
            if (stop_at > 0.0) {
                double remaining = stop_at - monotonic_seconds();
                if (remaining <= 0.0) {
                    break;
                }
                if (remaining < 1.0) {
                    delay = 1;
                }
                if ((double)delay > remaining) {
                    delay = (int)remaining;
                    if (delay <= 0) {
                        delay = 1;
                    }
                }
            }
            if (delay > 0) {
                printf("%s[RETRY]%s waiting %d seconds before reconnect\n", C_YELLOW, C_RESET, delay);
                sleep_seconds(delay);
            }
        }
        ++attempt;
    }

    return last_rc;
}
