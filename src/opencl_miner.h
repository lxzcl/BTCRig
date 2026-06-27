#ifndef BTCRIG_OPENCL_MINER_H
#define BTCRIG_OPENCL_MINER_H

#include "miner.h"
#include "sha256d.h"

#include <stddef.h>
#include <stdint.h>

typedef struct opencl_miner opencl_miner_t;

opencl_miner_t *opencl_miner_create(const miner_opencl_config_t *config,
                                    char *error,
                                    size_t error_size);
void opencl_miner_destroy(opencl_miner_t *miner);
uint32_t opencl_miner_batch_size(const opencl_miner_t *miner);
const char *opencl_miner_device_name(const opencl_miner_t *miner);
const char *opencl_miner_device_version(const opencl_miner_t *miner);

int opencl_miner_scan(opencl_miner_t *miner,
                      const sha256_midstate_t *state,
                      const uint32_t tail_words[4],
                      const uint32_t target_words[8],
                      uint32_t start_nonce,
                      uint32_t nonce_count,
                      void *opaque,
                      sha256d_scan_match_func_t on_match);

#endif
