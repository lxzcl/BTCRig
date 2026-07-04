#ifndef BTCRIG_CUDA_MINER_H
#define BTCRIG_CUDA_MINER_H

#include "sha256d.h"

#include <stddef.h>
#include <stdint.h>

typedef struct cuda_miner cuda_miner_t;

#ifndef BTCRIG_MINER_CUDA_CONFIG_DEFINED
#define BTCRIG_MINER_CUDA_CONFIG_DEFINED
#define MINER_CUDA_DEFAULT_DEVICE 0
#define MINER_CUDA_DEFAULT_BATCH_SIZE 4194304U
#define MINER_CUDA_DEFAULT_THREADS_PER_BLOCK 256U
#define MINER_CUDA_DEFAULT_NONCES_PER_THREAD 1U
#define MINER_CUDA_DEFAULT_MAX_RESULTS 256U
#define MINER_CUDA_KERNEL_STANDARD 0
#define MINER_CUDA_KERNEL_DUAL 1
#define MINER_CUDA_KERNEL_FIXED_NPT1 2
#define MINER_CUDA_KERNEL_FIXED_NPT2 3
#define MINER_CUDA_KERNEL_FIXED_NPT4 4
#define MINER_CUDA_KERNEL_LAST MINER_CUDA_KERNEL_FIXED_NPT4
#define MINER_CUDA_DEFAULT_KERNEL_VARIANT MINER_CUDA_KERNEL_STANDARD

typedef struct {
    int enabled;
    int device;
    uint32_t batch_size;
    uint32_t threads_per_block;
    uint32_t nonces_per_thread;
    uint32_t max_results;
    int kernel_variant;
} miner_cuda_config_t;
#endif

typedef struct {
    char device_name[128];
    int compute_major;
    int compute_minor;
    int driver_version;
    uint32_t checked_nonces;
} cuda_self_test_result_t;

void miner_cuda_config_defaults(miner_cuda_config_t *config);
int cuda_miner_driver_available(char *error, size_t error_size);
int cuda_miner_device_count(char *error, size_t error_size);
int cuda_miner_print_devices(void);

cuda_miner_t *cuda_miner_create(const miner_cuda_config_t *config,
                                char *error,
                                size_t error_size);
void cuda_miner_destroy(cuda_miner_t *miner);
uint32_t cuda_miner_batch_size(const cuda_miner_t *miner);
uint32_t cuda_miner_threads_per_block(const cuda_miner_t *miner);
uint32_t cuda_miner_nonces_per_thread(const cuda_miner_t *miner);
int cuda_miner_kernel_variant(const cuda_miner_t *miner);
const char *cuda_miner_kernel_variant_name(int variant);
const char *cuda_miner_device_name(const cuda_miner_t *miner);
int cuda_miner_driver_version(const cuda_miner_t *miner);
int cuda_miner_compute_major(const cuda_miner_t *miner);
int cuda_miner_compute_minor(const cuda_miner_t *miner);

int cuda_miner_self_test(const miner_cuda_config_t *config,
                         cuda_self_test_result_t *result,
                         char *error,
                         size_t error_size);

int cuda_miner_scan(cuda_miner_t *miner,
                    const sha256_midstate_t *state,
                    const uint32_t tail_words[4],
                    const uint32_t target_words[8],
                    uint32_t start_nonce,
                    uint32_t nonce_count,
                    void *opaque,
                    sha256d_scan_match_func_t on_match);

#endif
