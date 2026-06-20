#define _POSIX_C_SOURCE 200809L

#include "console.h"
#include "cpu_info.h"
#include "sha256d.h"
#include "stratum.h"
#include "btcrig_version.h"

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

#define DEFAULT_POOL_URL "stratum+tls://public-pool.io:4333"
#define DEFAULT_USER "bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3"
#define DEFAULT_PASSWORD "x"
#define DEFAULT_SUGGEST_DIFFICULTY 0.001
#define DEFAULT_RETRIES -1
#define DEFAULT_RECONNECT_DELAY 2
#define MAX_RECONNECT_DELAY 60
#define DEFAULT_STATS_INTERVAL 5.0
#define DEFAULT_DONATE_LEVEL 1
#define DONATE_CYCLE_MINUTES 100
#define DONATE_USER "bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3"

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
    int enable_mining;
    double runtime_seconds;
    double stats_interval;
    int donate_level;
} app_config_t;

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static unsigned long long donation_seed(void) {
    unsigned long long seed = (unsigned long long)time(NULL);
    seed ^= (unsigned long long)(monotonic_seconds() * 1000000.0);
#if defined(_WIN32)
    seed ^= (unsigned long long)GetCurrentProcessId() << 32;
#else
    seed ^= (unsigned long long)getpid() << 32;
#endif
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed *  UINT64_C(2685821657736338717);
}

static double donation_phase_seconds(int donate_level, int donating) {
    int minutes = donating ? donate_level : DONATE_CYCLE_MINUTES - donate_level;
    return (double)minutes * 60.0;
}

static double donation_initial_user_seconds(int donate_level) {
    unsigned long long seed = donation_seed();
    double unit = (double)(seed >> 11) * (1.0 / 9007199254740992.0);
    return donation_phase_seconds(donate_level, 0) * (0.5 + unit);
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
    config->enable_mining = 1;
    config->runtime_seconds = 0.0;
    config->stats_interval = DEFAULT_STATS_INTERVAL;
    config->donate_level = DEFAULT_DONATE_LEVEL;
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

static double json_number_value_or(json_t *value, double fallback) {
    return json_is_number(value) ? json_number_value(value) : fallback;
}

static int load_config_file(app_config_t *config, const char *path, int required) {
    json_error_t error;
    json_t *root = json_load_file(path, 0, &error);
    if (root == NULL) {
        if (required) {
            fprintf(stderr, "%s[CONFIG]%s failed to load %s line=%d col=%d: %s\n",
                    C_BRIGHT_RED, C_RESET, path, error.line, error.column, error.text);
            return -1;
        }
        return 0;
    }

    printf("%s[CONFIG]%s loaded %s\n", C_CYAN, C_RESET, path);

    json_t *cpu = json_object_get(root, "cpu");
    if (json_is_object(cpu)) {
        config->enable_mining = json_bool_value(json_object_get(cpu, "enabled"), config->enable_mining);
        config->thread_count = json_int_value(json_object_get(cpu, "threads"), config->thread_count);
    }

    config->thread_count = json_int_value(json_object_get(root, "threads"), config->thread_count);
    config->retries = json_int_value(json_object_get(root, "retries"), config->retries);
    config->reconnect_delay = json_int_value(json_object_get(root, "retry-pause"), config->reconnect_delay);
    config->reconnect_delay = json_int_value(json_object_get(root, "reconnect-delay"), config->reconnect_delay);
    config->stats_interval = json_number_value_or(json_object_get(root, "print-time"), config->stats_interval);
    config->stats_interval = json_number_value_or(json_object_get(root, "stats"), config->stats_interval);
    config->runtime_seconds = json_number_value_or(json_object_get(root, "runtime"), config->runtime_seconds);
    config->donate_level = json_int_value(json_object_get(root, "donate-level"), config->donate_level);

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
    printf("  %s --cpu-info\n", argv0);
    printf("  %s --version\n", argv0);
    printf("  %s [-c config.json] [-o stratum+tls://host:port] [-u wallet.worker] [-p password] [-d difficulty]\n", argv0);
    printf("     [-t threads] [-r retries] [--runtime seconds] [--stats seconds]\n");
    printf("     [--reconnect-delay seconds] [--donate-level N] [--no-mine]\n");
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
    printf("  donate-level: %d%% (1 minute in 100 minutes)\n", DEFAULT_DONATE_LEVEL);
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
        donation_phase_seconds(1, 0) != 5940.0) {
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
        } else if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
            app_config.runtime_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            app_config.stats_interval = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--reconnect-delay") == 0 && i + 1 < argc) {
            app_config.reconnect_delay = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--donate-level") == 0 && i + 1 < argc) {
            app_config.donate_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-mine") == 0) {
            app_config.enable_mining = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (app_config.pool_count <= 0) {
        fprintf(stderr, "%s[CONFIG]%s no pools configured\n", C_BRIGHT_RED, C_RESET);
        return 1;
    }
    if (app_config.thread_count <= 0) {
        app_config.thread_count = default_thread_count();
    }
    if (app_config.reconnect_delay < 0) {
        app_config.reconnect_delay = DEFAULT_RECONNECT_DELAY;
    }
    if (app_config.stats_interval < 0.0) {
        app_config.stats_interval = 0.0;
    }
    if (app_config.donate_level < 0 || app_config.donate_level >= DONATE_CYCLE_MINUTES) {
        fprintf(stderr, "%s[CONFIG]%s donate-level must be between 0 and 99\n",
                C_BRIGHT_RED, C_RESET);
        return 2;
    }

    app_config.retries = -1;

    printf("%s[CONFIG]%s pools=%d threads=%s%d%s mine=%s retries=infinite retry-pause=%d..%d stats=%.1f runtime=%.1f donate=%d%% sha=%s%s%s\n",
           C_CYAN,
           C_RESET,
           app_config.pool_count,
           C_BRIGHT_GREEN,
           app_config.thread_count,
           C_RESET,
           app_config.enable_mining ? "yes" : "no",
           app_config.reconnect_delay,
           MAX_RECONNECT_DELAY,
           app_config.stats_interval,
           app_config.runtime_seconds,
           app_config.donate_level,
           C_BRIGHT_GREEN,
           sha256d_backend_name(sha256d_get_backend()),
           C_RESET);

    double stop_at = app_config.runtime_seconds > 0.0 ? monotonic_seconds() + app_config.runtime_seconds : 0.0;
    int last_rc = 0;
    int pool_index = 0;
    int donating = 0;
    int donation_enabled = app_config.enable_mining && app_config.donate_level > 0;
    double phase_seconds = donation_enabled ?
        donation_initial_user_seconds(app_config.donate_level) : 0.0;

    if (donation_enabled) {
        printf("%s[DONATE]%s level=%d%% address=%s pool=same-as-user\n",
               C_MAGENTA, C_RESET, app_config.donate_level, DONATE_USER);
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
            copy_string(donate_pool.user, sizeof(donate_pool.user), DONATE_USER);
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
            .enable_mining = app_config.enable_mining,
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
