#include "opencl_miner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif

#if defined(__APPLE__)
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#define OPENCL_DEFAULT_BATCH_SIZE 1048576U
#define OPENCL_DEFAULT_MAX_RESULTS 256U

struct opencl_miner {
    cl_platform_id platform;
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;
    cl_mem state_buf;
    cl_mem target_buf;
    cl_mem count_buf;
    cl_mem matches_buf;
    uint32_t batch_size;
    uint32_t local_work_size;
    uint32_t max_results;
    uint32_t *matches;
    char device_name[128];
    char device_version[128];
};

static const char *k_opencl_kernel =
"#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics : enable\n"
"typedef unsigned int u32;\n"
"__constant u32 K[64] = {\n"
"0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,\n"
"0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,\n"
"0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,\n"
"0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,\n"
"0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,\n"
"0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,\n"
"0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,\n"
"0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};\n"
"inline u32 rotr32(u32 x, u32 n) { return (x >> n) | (x << (32U - n)); }\n"
"inline u32 bswap32(u32 v) { return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) | ((v & 0x00ff0000U) >> 8) | ((v & 0xff000000U) >> 24); }\n"
"inline void compress(u32 st[8], u32 w[64]) {\n"
"  for (int i = 16; i < 64; ++i) {\n"
"    u32 s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);\n"
"    u32 s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);\n"
"    w[i] = w[i-16] + s0 + w[i-7] + s1;\n"
"  }\n"
"  u32 a=st[0], b=st[1], c=st[2], d=st[3], e=st[4], f=st[5], g=st[6], h=st[7];\n"
"  for (int i = 0; i < 64; ++i) {\n"
"    u32 S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);\n"
"    u32 ch = (e & f) ^ (~e & g);\n"
"    u32 t1 = h + S1 + ch + K[i] + w[i];\n"
"    u32 S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);\n"
"    u32 maj = (a & b) ^ (a & c) ^ (b & c);\n"
"    u32 t2 = S0 + maj;\n"
"    h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;\n"
"  }\n"
"  st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d; st[4]+=e; st[5]+=f; st[6]+=g; st[7]+=h;\n"
"}\n"
"inline int meets_target(const u32 hash[8], __global const u32 *target) {\n"
"  for (int i = 7; i >= 0; --i) {\n"
"    u32 h = bswap32(hash[i]);\n"
"    u32 t = target[i];\n"
"    if (h < t) return 1;\n"
"    if (h > t) return 0;\n"
"  }\n"
"  return 1;\n"
"}\n"
"__kernel void scan_nonce_range(__global const u32 *fast_state,\n"
"                               __global const u32 *target,\n"
"                               u32 tail0, u32 tail1, u32 tail2,\n"
"                               u32 start_nonce, u32 nonce_count,\n"
"                               u32 max_results,\n"
"                               __global volatile u32 *result_count,\n"
"                               __global u32 *matches) {\n"
"  u32 gid = (u32)get_global_id(0);\n"
"  if (gid >= nonce_count) return;\n"
"  u32 nonce = start_nonce + gid;\n"
"  u32 st[8];\n"
"  u32 w[64];\n"
"  for (int i = 0; i < 8; ++i) st[i] = fast_state[i];\n"
"  w[0] = tail0; w[1] = tail1; w[2] = tail2; w[3] = bswap32(nonce); w[4] = 0x80000000U;\n"
"  for (int i = 5; i < 15; ++i) w[i] = 0U;\n"
"  w[15] = 640U;\n"
"  compress(st, w);\n"
"  u32 out[8] = {0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};\n"
"  for (int i = 0; i < 8; ++i) w[i] = st[i];\n"
"  w[8] = 0x80000000U;\n"
"  for (int i = 9; i < 15; ++i) w[i] = 0U;\n"
"  w[15] = 256U;\n"
"  compress(out, w);\n"
"  if (meets_target(out, target)) {\n"
"    u32 idx = atomic_inc(result_count);\n"
"    if (idx < max_results) {\n"
"      u32 base = idx * 9U;\n"
"      matches[base] = nonce;\n"
"      for (int i = 0; i < 8; ++i) matches[base + 1U + (u32)i] = out[i];\n"
"    }\n"
"  }\n"
"}\n";

static void set_error(char *error, size_t error_size, const char *message, cl_int code) {
    if (error == NULL || error_size == 0) {
        return;
    }
    if (code == CL_SUCCESS) {
        snprintf(error, error_size, "%s", message);
    } else {
        snprintf(error, error_size, "%s (OpenCL error %d)", message, (int)code);
    }
}

static uint32_t config_u32_or(uint32_t value, uint32_t fallback) {
    return value == 0 ? fallback : value;
}

static int select_platform_device(const miner_opencl_config_t *config,
                                  cl_platform_id *platform_out,
                                  cl_device_id *device_out,
                                  char *error,
                                  size_t error_size) {
    cl_uint platform_count = 0;
    cl_int rc = clGetPlatformIDs(0, NULL, &platform_count);
    if (rc != CL_SUCCESS || platform_count == 0) {
        set_error(error, error_size, "no OpenCL platforms found", rc);
        return -1;
    }

    cl_platform_id *platforms = calloc(platform_count, sizeof(*platforms));
    if (platforms == NULL) {
        set_error(error, error_size, "OpenCL platform allocation failed", CL_SUCCESS);
        return -1;
    }

    rc = clGetPlatformIDs(platform_count, platforms, NULL);
    if (rc != CL_SUCCESS) {
        free(platforms);
        set_error(error, error_size, "failed to enumerate OpenCL platforms", rc);
        return -1;
    }

    int platform_index = config != NULL ? config->platform : 0;
    if (platform_index < 0 || (cl_uint)platform_index >= platform_count) {
        free(platforms);
        set_error(error, error_size, "configured OpenCL platform index is out of range", CL_SUCCESS);
        return -1;
    }

    cl_platform_id platform = platforms[platform_index];
    free(platforms);

    cl_uint device_count = 0;
    rc = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, NULL, &device_count);
    if (rc != CL_SUCCESS || device_count == 0) {
        set_error(error, error_size, "no OpenCL devices found on selected platform", rc);
        return -1;
    }

    cl_device_id *devices = calloc(device_count, sizeof(*devices));
    if (devices == NULL) {
        set_error(error, error_size, "OpenCL device allocation failed", CL_SUCCESS);
        return -1;
    }

    rc = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices, NULL);
    if (rc != CL_SUCCESS) {
        free(devices);
        set_error(error, error_size, "failed to enumerate OpenCL devices", rc);
        return -1;
    }

    int device_index = config != NULL ? config->device : 0;
    if (device_index < 0 || (cl_uint)device_index >= device_count) {
        free(devices);
        set_error(error, error_size, "configured OpenCL device index is out of range", CL_SUCCESS);
        return -1;
    }

    *platform_out = platform;
    *device_out = devices[device_index];
    free(devices);
    return 0;
}

static int build_program(opencl_miner_t *miner, char *error, size_t error_size) {
    cl_int rc = CL_SUCCESS;
    const char *sources[] = {k_opencl_kernel};
    size_t lengths[] = {strlen(k_opencl_kernel)};

    miner->program = clCreateProgramWithSource(miner->context, 1, sources, lengths, &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL program", rc);
        return -1;
    }

    rc = clBuildProgram(miner->program, 1, &miner->device, NULL, NULL, NULL);
    if (rc != CL_SUCCESS) {
        char log[2048];
        size_t log_size = 0;
        log[0] = '\0';
        (void)clGetProgramBuildInfo(miner->program,
                                    miner->device,
                                    CL_PROGRAM_BUILD_LOG,
                                    sizeof(log) - 1,
                                    log,
                                    &log_size);
        if (log_size >= sizeof(log)) {
            log_size = sizeof(log) - 1;
        }
        log[log_size] = '\0';
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "failed to build OpenCL kernel: %.1800s", log);
        }
        return -1;
    }

    miner->kernel = clCreateKernel(miner->program, "scan_nonce_range", &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL kernel", rc);
        return -1;
    }

    return 0;
}

opencl_miner_t *opencl_miner_create(const miner_opencl_config_t *config,
                                    char *error,
                                    size_t error_size) {
    cl_int rc = CL_SUCCESS;
    opencl_miner_t *miner = calloc(1, sizeof(*miner));
    if (miner == NULL) {
        set_error(error, error_size, "OpenCL miner allocation failed", CL_SUCCESS);
        return NULL;
    }

    miner->batch_size = config != NULL ? config_u32_or(config->batch_size, OPENCL_DEFAULT_BATCH_SIZE) : OPENCL_DEFAULT_BATCH_SIZE;
    miner->local_work_size = config != NULL ? config->local_work_size : 0;
    miner->max_results = config != NULL ? config_u32_or(config->max_results, OPENCL_DEFAULT_MAX_RESULTS) : OPENCL_DEFAULT_MAX_RESULTS;
    if (miner->batch_size < 1024U) {
        miner->batch_size = 1024U;
    }
    if (miner->max_results < 1U) {
        miner->max_results = 1U;
    }

    if (select_platform_device(config, &miner->platform, &miner->device, error, error_size) != 0) {
        opencl_miner_destroy(miner);
        return NULL;
    }

    (void)clGetDeviceInfo(miner->device, CL_DEVICE_NAME, sizeof(miner->device_name), miner->device_name, NULL);
    (void)clGetDeviceInfo(miner->device, CL_DEVICE_VERSION, sizeof(miner->device_version), miner->device_version, NULL);
    if (miner->device_name[0] == '\0') {
        snprintf(miner->device_name, sizeof(miner->device_name), "unknown");
    }
    if (miner->device_version[0] == '\0') {
        snprintf(miner->device_version, sizeof(miner->device_version), "unknown");
    }

    size_t max_work_group = 0;
    (void)clGetDeviceInfo(miner->device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(max_work_group), &max_work_group, NULL);
    if (miner->local_work_size == 0) {
        miner->local_work_size = max_work_group >= 256 ? 256U : 0U;
    } else if (max_work_group > 0 && miner->local_work_size > max_work_group) {
        miner->local_work_size = (uint32_t)max_work_group;
    }

    miner->context = clCreateContext(NULL, 1, &miner->device, NULL, NULL, &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL context", rc);
        opencl_miner_destroy(miner);
        return NULL;
    }

    miner->queue = clCreateCommandQueue(miner->context, miner->device, 0, &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL command queue", rc);
        opencl_miner_destroy(miner);
        return NULL;
    }

    if (build_program(miner, error, error_size) != 0) {
        opencl_miner_destroy(miner);
        return NULL;
    }

    miner->state_buf = clCreateBuffer(miner->context, CL_MEM_READ_ONLY, sizeof(uint32_t) * 8U, NULL, &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL state buffer", rc);
        opencl_miner_destroy(miner);
        return NULL;
    }
    miner->target_buf = clCreateBuffer(miner->context, CL_MEM_READ_ONLY, sizeof(uint32_t) * 8U, NULL, &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL target buffer", rc);
        opencl_miner_destroy(miner);
        return NULL;
    }
    miner->count_buf = clCreateBuffer(miner->context, CL_MEM_READ_WRITE, sizeof(uint32_t), NULL, &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL result count buffer", rc);
        opencl_miner_destroy(miner);
        return NULL;
    }
    miner->matches_buf = clCreateBuffer(miner->context,
                                        CL_MEM_WRITE_ONLY,
                                        sizeof(uint32_t) * 9U * miner->max_results,
                                        NULL,
                                        &rc);
    if (rc != CL_SUCCESS) {
        set_error(error, error_size, "failed to create OpenCL matches buffer", rc);
        opencl_miner_destroy(miner);
        return NULL;
    }

    miner->matches = calloc((size_t)miner->max_results * 9U, sizeof(*miner->matches));
    if (miner->matches == NULL) {
        set_error(error, error_size, "OpenCL host result allocation failed", CL_SUCCESS);
        opencl_miner_destroy(miner);
        return NULL;
    }

    return miner;
}

void opencl_miner_destroy(opencl_miner_t *miner) {
    if (miner == NULL) {
        return;
    }
    free(miner->matches);
    if (miner->matches_buf != NULL) {
        clReleaseMemObject(miner->matches_buf);
    }
    if (miner->count_buf != NULL) {
        clReleaseMemObject(miner->count_buf);
    }
    if (miner->target_buf != NULL) {
        clReleaseMemObject(miner->target_buf);
    }
    if (miner->state_buf != NULL) {
        clReleaseMemObject(miner->state_buf);
    }
    if (miner->kernel != NULL) {
        clReleaseKernel(miner->kernel);
    }
    if (miner->program != NULL) {
        clReleaseProgram(miner->program);
    }
    if (miner->queue != NULL) {
        clReleaseCommandQueue(miner->queue);
    }
    if (miner->context != NULL) {
        clReleaseContext(miner->context);
    }
    free(miner);
}

uint32_t opencl_miner_batch_size(const opencl_miner_t *miner) {
    return miner != NULL ? miner->batch_size : OPENCL_DEFAULT_BATCH_SIZE;
}

const char *opencl_miner_device_name(const opencl_miner_t *miner) {
    return miner != NULL ? miner->device_name : "unavailable";
}

const char *opencl_miner_device_version(const opencl_miner_t *miner) {
    return miner != NULL ? miner->device_version : "unavailable";
}

int opencl_miner_scan(opencl_miner_t *miner,
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

    cl_int rc = clEnqueueWriteBuffer(miner->queue,
                                     miner->state_buf,
                                     CL_FALSE,
                                     0,
                                     sizeof(uint32_t) * 8U,
                                     state->fast_state,
                                     0,
                                     NULL,
                                     NULL);
    if (rc != CL_SUCCESS) {
        return -1;
    }
    rc = clEnqueueWriteBuffer(miner->queue,
                              miner->target_buf,
                              CL_FALSE,
                              0,
                              sizeof(uint32_t) * 8U,
                              target_words,
                              0,
                              NULL,
                              NULL);
    if (rc != CL_SUCCESS) {
        return -1;
    }

    uint32_t zero = 0;
    rc = clEnqueueWriteBuffer(miner->queue,
                              miner->count_buf,
                              CL_FALSE,
                              0,
                              sizeof(zero),
                              &zero,
                              0,
                              NULL,
                              NULL);
    if (rc != CL_SUCCESS) {
        return -1;
    }

    int arg = 0;
    rc  = clSetKernelArg(miner->kernel, arg++, sizeof(miner->state_buf), &miner->state_buf);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(miner->target_buf), &miner->target_buf);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &tail_words[0]);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &tail_words[1]);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &tail_words[2]);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &start_nonce);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &nonce_count);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &miner->max_results);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(miner->count_buf), &miner->count_buf);
    rc |= clSetKernelArg(miner->kernel, arg++, sizeof(miner->matches_buf), &miner->matches_buf);
    if (rc != CL_SUCCESS) {
        return -1;
    }

    size_t global = nonce_count;
    size_t local = miner->local_work_size;
    if (local > 0) {
        size_t rem = global % local;
        if (rem != 0) {
            global += local - rem;
        }
    }
    rc = clEnqueueNDRangeKernel(miner->queue,
                                miner->kernel,
                                1,
                                NULL,
                                &global,
                                local > 0 ? &local : NULL,
                                0,
                                NULL,
                                NULL);
    if (rc != CL_SUCCESS) {
        return -1;
    }
    rc = clFinish(miner->queue);
    if (rc != CL_SUCCESS) {
        return -1;
    }

    uint32_t count = 0;
    rc = clEnqueueReadBuffer(miner->queue,
                             miner->count_buf,
                             CL_TRUE,
                             0,
                             sizeof(count),
                             &count,
                             0,
                             NULL,
                             NULL);
    if (rc != CL_SUCCESS) {
        return -1;
    }

    if (count == 0 || on_match == NULL) {
        return 0;
    }

    uint32_t stored = count > miner->max_results ? miner->max_results : count;
    rc = clEnqueueReadBuffer(miner->queue,
                             miner->matches_buf,
                             CL_TRUE,
                             0,
                             sizeof(uint32_t) * 9U * stored,
                             miner->matches,
                             0,
                             NULL,
                             NULL);
    if (rc != CL_SUCCESS) {
        return -1;
    }

    for (uint32_t i = 0; i < stored; ++i) {
        uint32_t *entry = miner->matches + (size_t)i * 9U;
        on_match(opaque, entry[0], entry + 1);
    }

    return 0;
}
