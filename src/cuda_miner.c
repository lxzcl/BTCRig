#include "cuda_miner.h"
#include "cuda_sha256d_ptx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef FARPROC cuda_symbol_t;
#else
#include <dlfcn.h>
typedef void *cuda_symbol_t;
#endif

#define CUDA_SUCCESS 0

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef unsigned long long CUdeviceptr;

typedef struct {
#if defined(_WIN32)
    HMODULE library;
#else
    void *library;
#endif
    int attempted;
    int loaded;
    CUresult (*cuInit)(unsigned int flags);
    CUresult (*cuDriverGetVersion)(int *driver_version);
    CUresult (*cuDeviceGetCount)(int *count);
    CUresult (*cuDeviceGet)(CUdevice *device, int ordinal);
    CUresult (*cuDeviceGetName)(char *name, int len, CUdevice device);
    CUresult (*cuDeviceComputeCapability)(int *major, int *minor, CUdevice device);
    CUresult (*cuDeviceTotalMem_v2)(size_t *bytes, CUdevice device);
    CUresult (*cuCtxCreate_v2)(CUcontext *context, unsigned int flags, CUdevice device);
    CUresult (*cuCtxDestroy_v2)(CUcontext context);
    CUresult (*cuCtxSetCurrent)(CUcontext context);
    CUresult (*cuModuleLoadData)(CUmodule *module, const void *image);
    CUresult (*cuModuleUnload)(CUmodule module);
    CUresult (*cuModuleGetFunction)(CUfunction *function, CUmodule module, const char *name);
    CUresult (*cuMemAlloc_v2)(CUdeviceptr *ptr, size_t bytes);
    CUresult (*cuMemFree_v2)(CUdeviceptr ptr);
    CUresult (*cuMemsetD32_v2)(CUdeviceptr dst, unsigned int value, size_t count);
    CUresult (*cuMemcpyDtoH_v2)(void *dst, CUdeviceptr src, size_t bytes);
    CUresult (*cuMemcpyHtoD_v2)(CUdeviceptr dst, const void *src, size_t bytes);
    CUresult (*cuLaunchKernel)(CUfunction function,
                               unsigned int grid_x,
                               unsigned int grid_y,
                               unsigned int grid_z,
                               unsigned int block_x,
                               unsigned int block_y,
                               unsigned int block_z,
                               unsigned int shared_mem_bytes,
                               void *stream,
                               void **kernel_params,
                               void **extra);
    CUresult (*cuCtxSynchronize)(void);
    CUresult (*cuGetErrorString)(CUresult error, const char **text);
} cuda_driver_t;

struct cuda_miner {
    CUdevice device;
    CUcontext context;
    CUmodule module;
    CUfunction kernel;
    CUdeviceptr count_buf;
    CUdeviceptr matches_buf;
    uint32_t *matches;
    uint32_t batch_size;
    uint32_t threads_per_block;
    uint32_t nonces_per_thread;
    uint32_t max_results;
    int driver_version;
    int compute_major;
    int compute_minor;
    char device_name[128];
};

static cuda_driver_t g_cuda;

static void set_error(char *error, size_t error_size, const char *message) {
    if (error == NULL || error_size == 0) {
        return;
    }
    if (message == NULL) {
        message = "CUDA error";
    }
    snprintf(error, error_size, "%s", message);
}

static const char *cuda_error_string(CUresult rc) {
    const char *text = NULL;
    if (g_cuda.loaded && g_cuda.cuGetErrorString != NULL &&
        g_cuda.cuGetErrorString(rc, &text) == CUDA_SUCCESS &&
        text != NULL) {
        return text;
    }
    return "CUDA driver call failed";
}

static void set_cuda_error(char *error, size_t error_size, const char *message, CUresult rc) {
    if (error == NULL || error_size == 0) {
        return;
    }
    snprintf(error, error_size, "%s: %s (%d)", message, cuda_error_string(rc), (int)rc);
}

#if defined(_WIN32)
static cuda_symbol_t load_symbol(const char *name) {
    return g_cuda.library != NULL ? GetProcAddress(g_cuda.library, name) : NULL;
}
#else
static cuda_symbol_t load_symbol(const char *name) {
    return g_cuda.library != NULL ? dlsym(g_cuda.library, name) : NULL;
}
#endif

#define LOAD_CUDA_SYMBOL(name) do { \
    cuda_symbol_t symbol__ = load_symbol(#name); \
    if (symbol__ == NULL) { \
        set_error(error, error_size, "CUDA driver is missing required symbol " #name); \
        return -1; \
    } \
    memcpy(&g_cuda.name, &symbol__, sizeof(g_cuda.name)); \
} while (0)

static int cuda_driver_load(char *error, size_t error_size) {
    if (g_cuda.loaded) {
        if (error != NULL && error_size > 0) {
            error[0] = '\0';
        }
        return 0;
    }
    if (g_cuda.attempted) {
        set_error(error, error_size, "CUDA driver is not available");
        return -1;
    }
    g_cuda.attempted = 1;

#if defined(_WIN32)
    g_cuda.library = LoadLibraryA("nvcuda.dll");
#else
    g_cuda.library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_cuda.library == NULL) {
        g_cuda.library = dlopen("libcuda.so", RTLD_NOW | RTLD_LOCAL);
    }
#endif
    if (g_cuda.library == NULL) {
        set_error(error, error_size, "CUDA driver library not found");
        return -1;
    }

    LOAD_CUDA_SYMBOL(cuInit);
    LOAD_CUDA_SYMBOL(cuDriverGetVersion);
    LOAD_CUDA_SYMBOL(cuDeviceGetCount);
    LOAD_CUDA_SYMBOL(cuDeviceGet);
    LOAD_CUDA_SYMBOL(cuDeviceGetName);
    LOAD_CUDA_SYMBOL(cuDeviceComputeCapability);
    LOAD_CUDA_SYMBOL(cuDeviceTotalMem_v2);
    LOAD_CUDA_SYMBOL(cuCtxCreate_v2);
    LOAD_CUDA_SYMBOL(cuCtxDestroy_v2);
    LOAD_CUDA_SYMBOL(cuCtxSetCurrent);
    LOAD_CUDA_SYMBOL(cuModuleLoadData);
    LOAD_CUDA_SYMBOL(cuModuleUnload);
    LOAD_CUDA_SYMBOL(cuModuleGetFunction);
    LOAD_CUDA_SYMBOL(cuMemAlloc_v2);
    LOAD_CUDA_SYMBOL(cuMemFree_v2);
    LOAD_CUDA_SYMBOL(cuMemsetD32_v2);
    LOAD_CUDA_SYMBOL(cuMemcpyDtoH_v2);
    LOAD_CUDA_SYMBOL(cuMemcpyHtoD_v2);
    LOAD_CUDA_SYMBOL(cuLaunchKernel);
    LOAD_CUDA_SYMBOL(cuCtxSynchronize);
    LOAD_CUDA_SYMBOL(cuGetErrorString);

    CUresult rc = g_cuda.cuInit(0);
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "CUDA driver initialization failed", rc);
        return -1;
    }

    g_cuda.loaded = 1;
    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    return 0;
}

#undef LOAD_CUDA_SYMBOL

static uint32_t config_u32_or(uint32_t value, uint32_t fallback) {
    return value != 0 ? value : fallback;
}

static uint32_t clamp_threads_per_block(uint32_t value) {
    value = config_u32_or(value, MINER_CUDA_DEFAULT_THREADS_PER_BLOCK);
    if (value < 32U) {
        return 32U;
    }
    if (value > 1024U) {
        return 1024U;
    }
    return (value + 31U) & ~31U;
}

static uint32_t clamp_nonces_per_thread(uint32_t value) {
    value = config_u32_or(value, MINER_CUDA_DEFAULT_NONCES_PER_THREAD);
    if (value < 1U) {
        return 1U;
    }
    if (value > 16U) {
        return 16U;
    }
    return value;
}

void miner_cuda_config_defaults(miner_cuda_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    config->device = MINER_CUDA_DEFAULT_DEVICE;
    config->batch_size = MINER_CUDA_DEFAULT_BATCH_SIZE;
    config->threads_per_block = MINER_CUDA_DEFAULT_THREADS_PER_BLOCK;
    config->nonces_per_thread = MINER_CUDA_DEFAULT_NONCES_PER_THREAD;
    config->max_results = MINER_CUDA_DEFAULT_MAX_RESULTS;
}

int cuda_miner_driver_available(char *error, size_t error_size) {
    return cuda_driver_load(error, error_size);
}

int cuda_miner_device_count(char *error, size_t error_size) {
    if (cuda_driver_load(error, error_size) != 0) {
        return -1;
    }
    int count = 0;
    CUresult rc = g_cuda.cuDeviceGetCount(&count);
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to enumerate CUDA devices", rc);
        return -1;
    }
    return count;
}

int cuda_miner_print_devices(void) {
    char error[256];
    int count = cuda_miner_device_count(error, sizeof(error));
    if (count < 0) {
        fprintf(stderr, "[CUDA] unavailable: %s\n", error);
        return 1;
    }

    int driver_version = 0;
    (void)g_cuda.cuDriverGetVersion(&driver_version);
    printf("[CUDA] driver=%d devices=%d\n", driver_version, count);
    for (int i = 0; i < count; ++i) {
        CUdevice device;
        char name[128] = {0};
        int major = 0;
        int minor = 0;
        size_t memory = 0;
        if (g_cuda.cuDeviceGet(&device, i) != CUDA_SUCCESS) {
            continue;
        }
        (void)g_cuda.cuDeviceGetName(name, (int)sizeof(name), device);
        (void)g_cuda.cuDeviceComputeCapability(&major, &minor, device);
        (void)g_cuda.cuDeviceTotalMem_v2(&memory, device);
        printf("[CUDA] #%d device=%s compute=sm_%d%d memory=%zu MiB\n",
               i,
               name[0] != '\0' ? name : "unknown",
               major,
               minor,
               memory / (size_t)(1024 * 1024));
    }
    return 0;
}

cuda_miner_t *cuda_miner_create(const miner_cuda_config_t *config,
                                char *error,
                                size_t error_size) {
    miner_cuda_config_t effective;
    miner_cuda_config_defaults(&effective);
    if (config != NULL) {
        effective = *config;
    }
    effective.batch_size = config_u32_or(effective.batch_size, MINER_CUDA_DEFAULT_BATCH_SIZE);
    effective.threads_per_block = clamp_threads_per_block(effective.threads_per_block);
    effective.nonces_per_thread = clamp_nonces_per_thread(effective.nonces_per_thread);
    effective.max_results = config_u32_or(effective.max_results, MINER_CUDA_DEFAULT_MAX_RESULTS);

    if (cuda_driver_load(error, error_size) != 0) {
        return NULL;
    }

    int count = 0;
    CUresult rc = g_cuda.cuDeviceGetCount(&count);
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to enumerate CUDA devices", rc);
        return NULL;
    }
    if (count <= 0) {
        set_error(error, error_size, "no CUDA devices found");
        return NULL;
    }
    if (effective.device < 0 || effective.device >= count) {
        set_error(error, error_size, "configured CUDA device index is out of range");
        return NULL;
    }

    cuda_miner_t *miner = calloc(1, sizeof(*miner));
    if (miner == NULL) {
        set_error(error, error_size, "CUDA miner allocation failed");
        return NULL;
    }

    miner->batch_size = effective.batch_size;
    miner->threads_per_block = effective.threads_per_block;
    miner->nonces_per_thread = effective.nonces_per_thread;
    miner->max_results = effective.max_results;

    rc = g_cuda.cuDriverGetVersion(&miner->driver_version);
    if (rc != CUDA_SUCCESS) {
        miner->driver_version = 0;
    }
    rc = g_cuda.cuDeviceGet(&miner->device, effective.device);
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to select CUDA device", rc);
        cuda_miner_destroy(miner);
        return NULL;
    }
    (void)g_cuda.cuDeviceGetName(miner->device_name, (int)sizeof(miner->device_name), miner->device);
    (void)g_cuda.cuDeviceComputeCapability(&miner->compute_major, &miner->compute_minor, miner->device);

    rc = g_cuda.cuCtxCreate_v2(&miner->context, 0, miner->device);
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to create CUDA context", rc);
        cuda_miner_destroy(miner);
        return NULL;
    }

    rc = g_cuda.cuModuleLoadData(&miner->module, k_btcrig_cuda_sha256d_ptx);
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to load CUDA SHA256d PTX", rc);
        cuda_miner_destroy(miner);
        return NULL;
    }

    rc = g_cuda.cuModuleGetFunction(&miner->kernel, miner->module, "btcrig_cuda_scan_nonce_range");
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to find CUDA SHA256d kernel", rc);
        cuda_miner_destroy(miner);
        return NULL;
    }

    rc = g_cuda.cuMemAlloc_v2(&miner->count_buf, sizeof(uint32_t));
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to allocate CUDA result count buffer", rc);
        cuda_miner_destroy(miner);
        return NULL;
    }
    rc = g_cuda.cuMemAlloc_v2(&miner->matches_buf, (size_t)miner->max_results * 9U * sizeof(uint32_t));
    if (rc != CUDA_SUCCESS) {
        set_cuda_error(error, error_size, "failed to allocate CUDA matches buffer", rc);
        cuda_miner_destroy(miner);
        return NULL;
    }
    miner->matches = calloc((size_t)miner->max_results * 9U, sizeof(*miner->matches));
    if (miner->matches == NULL) {
        set_error(error, error_size, "CUDA host result allocation failed");
        cuda_miner_destroy(miner);
        return NULL;
    }

    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    return miner;
}

void cuda_miner_destroy(cuda_miner_t *miner) {
    if (miner == NULL) {
        return;
    }
    if (g_cuda.loaded && miner->context != NULL) {
        (void)g_cuda.cuCtxSetCurrent(miner->context);
    }
    if (g_cuda.loaded && miner->matches_buf != 0) {
        (void)g_cuda.cuMemFree_v2(miner->matches_buf);
    }
    if (g_cuda.loaded && miner->count_buf != 0) {
        (void)g_cuda.cuMemFree_v2(miner->count_buf);
    }
    if (g_cuda.loaded && miner->module != NULL) {
        (void)g_cuda.cuModuleUnload(miner->module);
    }
    if (g_cuda.loaded && miner->context != NULL) {
        (void)g_cuda.cuCtxDestroy_v2(miner->context);
    }
    free(miner->matches);
    free(miner);
}

uint32_t cuda_miner_batch_size(const cuda_miner_t *miner) {
    return miner != NULL ? miner->batch_size : MINER_CUDA_DEFAULT_BATCH_SIZE;
}

uint32_t cuda_miner_threads_per_block(const cuda_miner_t *miner) {
    return miner != NULL ? miner->threads_per_block : MINER_CUDA_DEFAULT_THREADS_PER_BLOCK;
}

uint32_t cuda_miner_nonces_per_thread(const cuda_miner_t *miner) {
    return miner != NULL ? miner->nonces_per_thread : MINER_CUDA_DEFAULT_NONCES_PER_THREAD;
}

const char *cuda_miner_device_name(const cuda_miner_t *miner) {
    return miner != NULL ? miner->device_name : "unavailable";
}

int cuda_miner_driver_version(const cuda_miner_t *miner) {
    return miner != NULL ? miner->driver_version : 0;
}

int cuda_miner_compute_major(const cuda_miner_t *miner) {
    return miner != NULL ? miner->compute_major : 0;
}

int cuda_miner_compute_minor(const cuda_miner_t *miner) {
    return miner != NULL ? miner->compute_minor : 0;
}

static void store_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
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

static void make_self_test_header(uint8_t header[80]) {
    memset(header, 0, 80);
    header[0] = 0x01;
    header[68] = 0xff;
    header[69] = 0xff;
    header[70] = 0x00;
    header[71] = 0x1d;
}

typedef struct {
    uint8_t header[80];
    uint32_t start_nonce;
    uint32_t nonce_count;
    uint32_t seen_mask;
    int failed;
} cuda_self_test_context_t;

static void cuda_self_test_match(void *opaque, uint32_t nonce, const uint32_t hash_words[8]) {
    cuda_self_test_context_t *ctx = (cuda_self_test_context_t *)opaque;
    uint8_t cpu_hash[32];
    uint8_t cuda_hash[32];

    if (ctx == NULL || hash_words == NULL) {
        return;
    }
    if (nonce < ctx->start_nonce || nonce >= ctx->start_nonce + ctx->nonce_count) {
        ctx->failed = 1;
        return;
    }

    uint32_t offset = nonce - ctx->start_nonce;
    uint32_t bit = 1U << offset;
    if ((ctx->seen_mask & bit) != 0) {
        ctx->failed = 1;
        return;
    }
    ctx->seen_mask |= bit;

    store_le32(&ctx->header[76], nonce);
    sha256d_80(ctx->header, cpu_hash);
    sha256d_words_to_hash(hash_words, cuda_hash);
    if (memcmp(cpu_hash, cuda_hash, sizeof(cpu_hash)) != 0) {
        ctx->failed = 1;
    }
}

int cuda_miner_self_test(const miner_cuda_config_t *config,
                         cuda_self_test_result_t *result,
                         char *error,
                         size_t error_size) {
    const uint32_t nonce_count = 16U;
    miner_cuda_config_t test_config;
    uint8_t header[80];
    sha256_midstate_t midstate;
    uint32_t tail_words[4];
    uint32_t target_words[8];
    cuda_self_test_context_t context;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    miner_cuda_config_defaults(&test_config);
    if (config != NULL) {
        test_config = *config;
    }
    test_config.batch_size = 1024U;
    test_config.max_results = 32U;

    char create_error[512];
    create_error[0] = '\0';
    cuda_miner_t *miner = cuda_miner_create(&test_config, create_error, sizeof(create_error));
    if (miner == NULL) {
        set_error(error, error_size, create_error[0] != '\0' ? create_error : "CUDA self-test setup failed");
        return -1;
    }

    if (result != NULL) {
        snprintf(result->device_name, sizeof(result->device_name), "%s", cuda_miner_device_name(miner));
        result->compute_major = cuda_miner_compute_major(miner);
        result->compute_minor = cuda_miner_compute_minor(miner);
        result->driver_version = cuda_miner_driver_version(miner);
        result->checked_nonces = nonce_count;
    }

    make_self_test_header(header);
    sha256d_80_midstate_prepare(&midstate, header);
    for (int i = 0; i < 4; ++i) {
        tail_words[i] = load_be32(header + 64 + i * 4);
    }
    for (int i = 0; i < 8; ++i) {
        target_words[i] = UINT32_MAX;
    }

    memset(&context, 0, sizeof(context));
    memcpy(context.header, header, sizeof(context.header));
    context.start_nonce = 0x13579b00U;
    context.nonce_count = nonce_count;

    if (cuda_miner_scan(miner,
                        &midstate,
                        tail_words,
                        target_words,
                        context.start_nonce,
                        nonce_count,
                        &context,
                        cuda_self_test_match) != 0) {
        cuda_miner_destroy(miner);
        set_error(error, error_size, "CUDA self-test scan failed");
        return -1;
    }

    cuda_miner_destroy(miner);

    uint32_t expected_mask = (1U << nonce_count) - 1U;
    if (context.failed || context.seen_mask != expected_mask) {
        set_error(error, error_size, "CUDA self-test hash verification failed");
        return -1;
    }

    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    return 0;
}

int cuda_miner_scan(cuda_miner_t *miner,
                    const sha256_midstate_t *state,
                    const uint32_t tail_words[4],
                    const uint32_t target_words[8],
                    uint32_t start_nonce,
                    uint32_t nonce_count,
                    void *opaque,
                    sha256d_scan_match_func_t on_match) {
    if (miner == NULL || state == NULL || tail_words == NULL || target_words == NULL) {
        return -1;
    }
    if (nonce_count == 0) {
        return 0;
    }
    if (g_cuda.cuCtxSetCurrent(miner->context) != CUDA_SUCCESS) {
        return -1;
    }

    CUresult rc = g_cuda.cuMemsetD32_v2(miner->count_buf, 0, 1);
    if (rc != CUDA_SUCCESS) {
        return -1;
    }

    uint32_t fast0 = state->fast_state[0];
    uint32_t fast1 = state->fast_state[1];
    uint32_t fast2 = state->fast_state[2];
    uint32_t fast3 = state->fast_state[3];
    uint32_t fast4 = state->fast_state[4];
    uint32_t fast5 = state->fast_state[5];
    uint32_t fast6 = state->fast_state[6];
    uint32_t fast7 = state->fast_state[7];
    uint32_t target0 = target_words[0];
    uint32_t target1 = target_words[1];
    uint32_t target2 = target_words[2];
    uint32_t target3 = target_words[3];
    uint32_t target4 = target_words[4];
    uint32_t target5 = target_words[5];
    uint32_t target6 = target_words[6];
    uint32_t target7 = target_words[7];
    uint32_t tail0 = tail_words[0];
    uint32_t tail1 = tail_words[1];
    uint32_t tail2 = tail_words[2];
    uint32_t npt = miner->nonces_per_thread;
    uint32_t max_results = miner->max_results;
    CUdeviceptr count_buf = miner->count_buf;
    CUdeviceptr matches_buf = miner->matches_buf;

    void *args[] = {
        &fast0, &fast1, &fast2, &fast3,
        &fast4, &fast5, &fast6, &fast7,
        &target0, &target1, &target2, &target3,
        &target4, &target5, &target6, &target7,
        &tail0, &tail1, &tail2,
        &start_nonce, &nonce_count,
        &npt,
        &max_results,
        &count_buf,
        &matches_buf,
    };

    uint64_t work_items = ((uint64_t)nonce_count + npt - 1U) / npt;
    uint64_t grid = (work_items + miner->threads_per_block - 1U) / miner->threads_per_block;
    if (grid == 0 || grid > UINT32_MAX) {
        return -1;
    }

    rc = g_cuda.cuLaunchKernel(miner->kernel,
                               (unsigned int)grid,
                               1,
                               1,
                               miner->threads_per_block,
                               1,
                               1,
                               0,
                               NULL,
                               args,
                               NULL);
    if (rc != CUDA_SUCCESS) {
        return -1;
    }
    rc = g_cuda.cuCtxSynchronize();
    if (rc != CUDA_SUCCESS) {
        return -1;
    }

    uint32_t count = 0;
    rc = g_cuda.cuMemcpyDtoH_v2(&count, miner->count_buf, sizeof(count));
    if (rc != CUDA_SUCCESS) {
        return -1;
    }

    if (count == 0 || on_match == NULL) {
        return 0;
    }

    uint32_t limited = count < miner->max_results ? count : miner->max_results;
    rc = g_cuda.cuMemcpyDtoH_v2(miner->matches,
                                miner->matches_buf,
                                (size_t)limited * 9U * sizeof(uint32_t));
    if (rc != CUDA_SUCCESS) {
        return -1;
    }

    for (uint32_t i = 0; i < limited; ++i) {
        uint32_t *item = &miner->matches[i * 9U];
        on_match(opaque, item[0], &item[1]);
    }
    return 0;
}
