#define _POSIX_C_SOURCE 200809L

#include "cpu_info.h"

#include "console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#endif

static int sysconf_online_cpus(void) {
#if defined(_WIN32)
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        count = info.dwNumberOfProcessors;
    }
    return count > 0 && count <= CPU_INFO_MAX_CPUS ? (int)count : 1;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 && count <= CPU_INFO_MAX_CPUS ? (int)count : 1;
#endif
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void set_fallback(cpu_info_t *info) {
    memset(info, 0, sizeof(*info));
    info->logical_count = sysconf_online_cpus();
    info->physical_count = info->logical_count;
    info->has_smt = 0;
    info->recommended_threads = info->logical_count;
#if defined(_WIN32)
    copy_text(info->source, sizeof(info->source), "windows");
#else
    copy_text(info->source, sizeof(info->source), "sysconf");
#endif
}

#if defined(__linux__)
static int parse_cpu_dir_name(const char *name) {
    if (name[0] != 'c' || name[1] != 'p' || name[2] != 'u' || !isdigit((unsigned char)name[3])) {
        return -1;
    }

    int id = 0;
    for (const char *p = name + 3; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        id = id * 10 + (*p - '0');
        if (id >= CPU_INFO_MAX_CPUS) {
            return -1;
        }
    }
    return id;
}

static int read_long_file(const char *path, long *value) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    char buf[64];
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    char *end = NULL;
    long parsed = strtol(buf, &end, 10);
    if (end == buf) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int read_text_file(const char *path, char *out, size_t out_size) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    if (fgets(out, out_size, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ' || out[len - 1] == '\t')) {
        out[--len] = '\0';
    }
    return 0;
}

static int cpu_is_online(int id) {
    char path[128];
    long online = 1;

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", id);
    if (read_long_file(path, &online) != 0) {
        return 1;
    }

    return online != 0;
}

static void read_cpu_entry(int id, cpu_info_entry_t *entry) {
    char path[160];
    long value = 0;

    memset(entry, 0, sizeof(*entry));
    entry->id = id;
    entry->package_id = 0;
    entry->core_id = id;
    entry->max_freq_khz = 0;
    entry->capacity = 0;

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", id);
    if (read_long_file(path, &value) == 0) {
        entry->package_id = (int)value;
    }

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", id);
    if (read_long_file(path, &value) == 0) {
        entry->core_id = (int)value;
    }

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", id);
    (void)read_text_file(path, entry->thread_siblings, sizeof(entry->thread_siblings));

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", id);
    if (read_long_file(path, &value) == 0) {
        entry->max_freq_khz = value;
    }

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", id);
    if (read_long_file(path, &value) == 0) {
        entry->capacity = value;
    }
}

static int same_physical_core(const cpu_info_entry_t *a, const cpu_info_entry_t *b) {
    return a->package_id == b->package_id && a->core_id == b->core_id;
}

static int sibling_list_implies_smt(const char *text) {
    return text != NULL && (strchr(text, ',') != NULL || strchr(text, '-') != NULL);
}

static int same_affinity_group(const cpu_info_entry_t *a, const cpu_info_entry_t *b) {
    if (same_physical_core(a, b)) {
        return 1;
    }

    return a->thread_siblings[0] != '\0' &&
           b->thread_siblings[0] != '\0' &&
           strcmp(a->thread_siblings, b->thread_siblings) == 0;
}

static int compare_cpu_entries(const void *left, const void *right) {
    const cpu_info_entry_t *a = (const cpu_info_entry_t *)left;
    const cpu_info_entry_t *b = (const cpu_info_entry_t *)right;
    return a->id - b->id;
}

static void finalize_sysfs_info(cpu_info_t *info) {
    int physical = 0;
    int smt = 0;

    for (int i = 0; i < info->entry_count; ++i) {
        int seen = 0;
        for (int j = 0; j < i; ++j) {
            if (same_affinity_group(&info->entries[i], &info->entries[j])) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            ++physical;
        }
        if (sibling_list_implies_smt(info->entries[i].thread_siblings)) {
            smt = 1;
        }
    }

    info->logical_count = info->entry_count;
    info->physical_count = physical > 0 ? physical : info->logical_count;
    if (info->logical_count > info->physical_count) {
        smt = 1;
    }
    info->has_smt = smt;
    info->recommended_threads = info->logical_count;
    if (info->recommended_threads <= 0) {
        info->recommended_threads = 1;
    }
}

static int detect_sysfs(cpu_info_t *info) {
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (dir == NULL) {
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && info->entry_count < CPU_INFO_MAX_CPUS) {
        int id = parse_cpu_dir_name(ent->d_name);
        if (id < 0 || !cpu_is_online(id)) {
            continue;
        }
        read_cpu_entry(id, &info->entries[info->entry_count]);
        ++info->entry_count;
    }
    closedir(dir);

    if (info->entry_count <= 0) {
        return -1;
    }

    qsort(info->entries, (size_t)info->entry_count, sizeof(info->entries[0]), compare_cpu_entries);
    finalize_sysfs_info(info);
#if defined(__ANDROID__)
    copy_text(info->source, sizeof(info->source), "android-sysfs");
#else
    copy_text(info->source, sizeof(info->source), "linux-sysfs");
#endif
    return 0;
}
#endif

void cpu_info_detect(cpu_info_t *info) {
    if (info == NULL) {
        return;
    }

    set_fallback(info);
#if defined(__linux__)
    (void)detect_sysfs(info);
#endif
}

int cpu_info_recommended_threads(void) {
    cpu_info_t info;
    cpu_info_detect(&info);
    return info.recommended_threads > 0 ? info.recommended_threads : 1;
}

static int append_cpu_id(int *out, int *count, int max_count, int id) {
    if (id < 0 || out == NULL || count == NULL || *count >= max_count) {
        return 0;
    }

    for (int i = 0; i < *count; ++i) {
        if (out[i] == id) {
            return 0;
        }
    }

    out[(*count)++] = id;
    return 1;
}

static int append_sysconf_order(int *out, int max_count) {
    int online = sysconf_online_cpus();
    int count = 0;

    for (int id = 0; id < online && count < max_count; ++id) {
        append_cpu_id(out, &count, max_count, id);
    }

    return count;
}

static int append_entry_order(const cpu_info_t *info, int *out, int max_count) {
    int count = 0;

    for (int i = 0; i < info->entry_count && count < max_count; ++i) {
        append_cpu_id(out, &count, max_count, info->entries[i].id);
    }

    return count;
}

int cpu_info_affinity_plan(const cpu_info_t *info, int *out, int max_count) {
    if (out == NULL || max_count <= 0) {
        return 0;
    }

    if (info == NULL || info->entry_count <= 0) {
        return append_sysconf_order(out, max_count);
    }

    return append_entry_order(info, out, max_count);
}

void cpu_info_print(const cpu_info_t *info) {
    if (info == NULL) {
        return;
    }

    int affinity[CPU_INFO_MAX_CPUS];
    int affinity_count = cpu_info_affinity_plan(info, affinity, CPU_INFO_MAX_CPUS);

    printf("%s[CPU]%s source=%s logical=%d physical=%d smt=%s recommended=%d\n",
           C_BRIGHT_CYAN,
           C_RESET,
           info->source,
           info->logical_count,
           info->physical_count,
           info->has_smt ? "yes" : "no",
           info->recommended_threads);
    if (affinity_count > 0) {
        printf("%s[CPU]%s cpu-order=", C_BRIGHT_CYAN, C_RESET);
        for (int i = 0; i < affinity_count; ++i) {
            printf("%s%d", i == 0 ? "" : ",", affinity[i]);
        }
        printf("\n");
    }

    for (int i = 0; i < info->entry_count; ++i) {
        const cpu_info_entry_t *entry = &info->entries[i];
        printf("%s[CPU]%s cpu%d package=%d core=%d",
               C_CYAN,
               C_RESET,
               entry->id,
               entry->package_id,
               entry->core_id);
        if (entry->thread_siblings[0] != '\0') {
            printf(" siblings=%s", entry->thread_siblings);
        }
        if (entry->max_freq_khz > 0) {
            printf(" max=%ldMHz", entry->max_freq_khz / 1000);
        }
        if (entry->capacity > 0) {
            printf(" capacity=%ld", entry->capacity);
        }
        printf("\n");
    }
}
