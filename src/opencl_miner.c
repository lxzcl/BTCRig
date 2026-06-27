#include "opencl_miner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 100
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
    cl_mem count_buf;
    cl_mem matches_buf;
    uint32_t batch_size;
    uint32_t local_work_size;
    uint32_t max_results;
    uint32_t *matches;
    char device_name[128];
    char device_version[128];
    char backend_name[32];
};

static const char *k_opencl_kernel =
"#ifdef cl_khr_global_int32_base_atomics\n"
"#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics : enable\n"
"#endif\n"
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
"}\n";

static const char *k_opencl_kernel_tail =
"inline int meets_target(const u32 hash[8], u32 t0, u32 t1, u32 t2, u32 t3, u32 t4, u32 t5, u32 t6, u32 t7) {\n"
"  u32 h = bswap32(hash[7]); if (h < t7) return 1; if (h > t7) return 0;\n"
"  h = bswap32(hash[6]); if (h < t6) return 1; if (h > t6) return 0;\n"
"  h = bswap32(hash[5]); if (h < t5) return 1; if (h > t5) return 0;\n"
"  h = bswap32(hash[4]); if (h < t4) return 1; if (h > t4) return 0;\n"
"  h = bswap32(hash[3]); if (h < t3) return 1; if (h > t3) return 0;\n"
"  h = bswap32(hash[2]); if (h < t2) return 1; if (h > t2) return 0;\n"
"  h = bswap32(hash[1]); if (h < t1) return 1; if (h > t1) return 0;\n"
"  h = bswap32(hash[0]); if (h < t0) return 1; if (h > t0) return 0;\n"
"  return 1;\n"
"}\n"
"__kernel void scan_nonce_range(u32 fast0, u32 fast1, u32 fast2, u32 fast3,\n"
"                               u32 fast4, u32 fast5, u32 fast6, u32 fast7,\n"
"                               u32 target0, u32 target1, u32 target2, u32 target3,\n"
"                               u32 target4, u32 target5, u32 target6, u32 target7,\n"
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
"  st[0] = fast0; st[1] = fast1; st[2] = fast2; st[3] = fast3;\n"
"  st[4] = fast4; st[5] = fast5; st[6] = fast6; st[7] = fast7;\n"
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
"  if (meets_target(out, target0, target1, target2, target3, target4, target5, target6, target7)) {\n"
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

static int parse_opencl_version(const char *text, int *major, int *minor) {
    int parsed_major = 0;
    int parsed_minor = 0;
    if (text == NULL) {
        return -1;
    }
    if (sscanf(text, "OpenCL %d.%d", &parsed_major, &parsed_minor) != 2) {
        return -1;
    }
    if (major != NULL) {
        *major = parsed_major;
    }
    if (minor != NULL) {
        *minor = parsed_minor;
    }
    return 0;
}

static int opencl_version_at_least(const char *text, int major, int minor) {
    int parsed_major = 0;
    int parsed_minor = 0;
    if (parse_opencl_version(text, &parsed_major, &parsed_minor) != 0) {
        return 0;
    }
    return parsed_major > major || (parsed_major == major && parsed_minor >= minor);
}

static int extension_list_has(const char *extensions, const char *needle) {
    size_t needle_len = strlen(needle);
    const char *p = extensions;

    if (extensions == NULL || needle == NULL || needle_len == 0) {
        return 0;
    }

    while ((p = strstr(p, needle)) != NULL) {
        int left_ok = p == extensions || p[-1] == ' ';
        char right = p[needle_len];
        int right_ok = right == '\0' || right == ' ';
        if (left_ok && right_ok) {
            return 1;
        }
        p += needle_len;
    }
    return 0;
}

static int device_has_extension(cl_device_id device, const char *needle) {
    size_t size = 0;
    if (clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, 0, NULL, &size) != CL_SUCCESS || size == 0) {
        return 0;
    }

    char *extensions = malloc(size);
    if (extensions == NULL) {
        return 0;
    }
    if (clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, size, extensions, NULL) != CL_SUCCESS) {
        free(extensions);
        return 0;
    }

    int found = extension_list_has(extensions, needle);
    free(extensions);
    return found;
}

static int validate_compat10_device(cl_device_id device,
                                    const char *device_version,
                                    char *error,
                                    size_t error_size) {
    if (!opencl_version_at_least(device_version, 1, 0)) {
        set_error(error, error_size, "selected OpenCL device did not report a usable OpenCL 1.x version", CL_SUCCESS);
        return -1;
    }

    if (!opencl_version_at_least(device_version, 1, 1) &&
        !device_has_extension(device, "cl_khr_global_int32_base_atomics")) {
        set_error(error,
                  error_size,
                  "compat10 backend requires OpenCL 1.1+ or cl_khr_global_int32_base_atomics",
                  CL_SUCCESS);
        return -1;
    }

    return 0;
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

static void opencl_device_config_apply_defaults(miner_opencl_device_config_t *device,
                                                const miner_opencl_config_t *config) {
    if (device == NULL || config == NULL) {
        return;
    }
    if (device->batch_size == 0) {
        device->batch_size = config_u32_or(config->batch_size, OPENCL_DEFAULT_BATCH_SIZE);
    }
    if (device->max_results == 0) {
        device->max_results = config_u32_or(config->max_results, OPENCL_DEFAULT_MAX_RESULTS);
    }
    if (device->local_work_size == 0) {
        device->local_work_size = config->local_work_size;
    }
}

int opencl_miner_resolve_devices(const miner_opencl_config_t *config,
                                 miner_opencl_device_config_t *devices_out,
                                 int max_devices,
                                 char *error,
                                 size_t error_size) {
    if (config == NULL || !config->enabled || devices_out == NULL || max_devices <= 0) {
        return 0;
    }

    if (config->device_count > 0) {
        int count = config->device_count < max_devices ? config->device_count : max_devices;
        for (int i = 0; i < count; ++i) {
            devices_out[i] = config->devices[i];
            opencl_device_config_apply_defaults(&devices_out[i], config);
        }
        return count;
    }

    if (!config->all_devices) {
        devices_out[0].platform = config->platform;
        devices_out[0].device = config->device;
        devices_out[0].batch_size = config_u32_or(config->batch_size, OPENCL_DEFAULT_BATCH_SIZE);
        devices_out[0].local_work_size = config->local_work_size;
        devices_out[0].max_results = config_u32_or(config->max_results, OPENCL_DEFAULT_MAX_RESULTS);
        return 1;
    }

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

    int found = 0;
    for (cl_uint p = 0; p < platform_count && found < max_devices; ++p) {
        cl_uint device_count = 0;
        rc = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_ALL, 0, NULL, &device_count);
        if (rc != CL_SUCCESS || device_count == 0) {
            continue;
        }

        cl_device_id *devices = calloc(device_count, sizeof(*devices));
        if (devices == NULL) {
            free(platforms);
            set_error(error, error_size, "OpenCL device allocation failed", CL_SUCCESS);
            return -1;
        }

        rc = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_ALL, device_count, devices, NULL);
        if (rc != CL_SUCCESS) {
            free(devices);
            continue;
        }

        for (cl_uint d = 0; d < device_count && found < max_devices; ++d) {
            cl_device_type type = 0;
            if (clGetDeviceInfo(devices[d], CL_DEVICE_TYPE, sizeof(type), &type, NULL) != CL_SUCCESS) {
                continue;
            }
            if ((type & CL_DEVICE_TYPE_GPU) == 0) {
                continue;
            }

            devices_out[found].platform = (int)p;
            devices_out[found].device = (int)d;
            devices_out[found].batch_size = config_u32_or(config->batch_size, OPENCL_DEFAULT_BATCH_SIZE);
            devices_out[found].local_work_size = config->local_work_size;
            devices_out[found].max_results = config_u32_or(config->max_results, OPENCL_DEFAULT_MAX_RESULTS);
            ++found;
        }

        free(devices);
    }

    free(platforms);
    if (found == 0) {
        set_error(error, error_size, "no OpenCL GPU devices found", CL_SUCCESS);
        return -1;
    }
    return found;
}

static int build_program(opencl_miner_t *miner, char *error, size_t error_size) {
    cl_int rc = CL_SUCCESS;
    const char *sources[] = {k_opencl_kernel, k_opencl_kernel_tail};
    size_t lengths[] = {strlen(k_opencl_kernel), strlen(k_opencl_kernel_tail)};

    miner->program = clCreateProgramWithSource(miner->context, 2, sources, lengths, &rc);
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
    snprintf(miner->backend_name, sizeof(miner->backend_name), "compat10");
    if (miner->device_name[0] == '\0') {
        snprintf(miner->device_name, sizeof(miner->device_name), "unknown");
    }
    if (miner->device_version[0] == '\0') {
        snprintf(miner->device_version, sizeof(miner->device_version), "unknown");
    }
    if (validate_compat10_device(miner->device, miner->device_version, error, error_size) != 0) {
        opencl_miner_destroy(miner);
        return NULL;
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

const char *opencl_miner_backend_name(const opencl_miner_t *miner) {
    return miner != NULL ? miner->backend_name : "unavailable";
}

const char *opencl_miner_device_name(const opencl_miner_t *miner) {
    return miner != NULL ? miner->device_name : "unavailable";
}

const char *opencl_miner_device_version(const opencl_miner_t *miner) {
    return miner != NULL ? miner->device_version : "unavailable";
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
} opencl_self_test_context_t;

static void opencl_self_test_match(void *opaque, uint32_t nonce, const uint32_t hash_words[8]) {
    opencl_self_test_context_t *ctx = (opencl_self_test_context_t *)opaque;
    uint8_t cpu_hash[32];
    uint8_t opencl_hash[32];

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
    sha256d_words_to_hash(hash_words, opencl_hash);
    if (memcmp(cpu_hash, opencl_hash, sizeof(cpu_hash)) != 0) {
        ctx->failed = 1;
    }
}

int opencl_miner_self_test(const miner_opencl_config_t *config,
                           opencl_self_test_result_t *result,
                           char *error,
                           size_t error_size) {
    const uint32_t nonce_count = 16U;
    uint8_t header[80];
    sha256_midstate_t midstate;
    uint32_t tail_words[4];
    uint32_t target_words[8];
    miner_opencl_config_t test_config;
    opencl_self_test_context_t context;

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    if (config != NULL) {
        test_config = *config;
    } else {
        miner_opencl_config_defaults(&test_config);
    }
    test_config.batch_size = 1024U;
    test_config.max_results = 32U;

    char create_error[2048];
    create_error[0] = '\0';
    opencl_miner_t *miner = opencl_miner_create(&test_config, create_error, sizeof(create_error));
    if (miner == NULL) {
        set_error(error, error_size, create_error[0] != '\0' ? create_error : "OpenCL self-test setup failed", CL_SUCCESS);
        return -1;
    }

    if (result != NULL) {
        snprintf(result->backend, sizeof(result->backend), "%s", opencl_miner_backend_name(miner));
        snprintf(result->device_name, sizeof(result->device_name), "%s", opencl_miner_device_name(miner));
        snprintf(result->device_version, sizeof(result->device_version), "%s", opencl_miner_device_version(miner));
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

    if (opencl_miner_scan(miner,
                          &midstate,
                          tail_words,
                          target_words,
                          context.start_nonce,
                          nonce_count,
                          &context,
                          opencl_self_test_match) != 0) {
        opencl_miner_destroy(miner);
        set_error(error, error_size, "OpenCL self-test scan failed", CL_SUCCESS);
        return -1;
    }

    opencl_miner_destroy(miner);

    uint32_t expected_mask = (1U << nonce_count) - 1U;
    if (context.failed || context.seen_mask != expected_mask) {
        set_error(error, error_size, "OpenCL self-test hash verification failed", CL_SUCCESS);
        return -1;
    }

    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    return 0;
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

    uint32_t zero = 0;
    cl_int rc = clEnqueueWriteBuffer(miner->queue,
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
    for (int i = 0; i < 8; ++i) {
        rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &state->fast_state[i]);
    }
    for (int i = 0; i < 8; ++i) {
        rc |= clSetKernelArg(miner->kernel, arg++, sizeof(uint32_t), &target_words[i]);
    }
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
