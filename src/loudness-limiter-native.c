#include <pulse/pulseaudio.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <errno.h>

#define VIRTUAL_SINK "loudness_limiter_native"
#define SHM_NAME "/loudness-limiter-limit"

typedef struct {
    pa_mainloop *mainloop;
    pa_context *context;
    char *hardware_sink;
    char *original_sink;
    uint32_t module_index;
    float limit_db;
    float default_limit_db;
    float shared_limit;
    int shm_fd;
    volatile float *shared_limit_ptr;
    pthread_t stdin_thread;
    bool stdin_thread_started;
    struct termios original_terminal;
    bool terminal_changed;
    char *extracted_plugin;
    int watchdog_write_fd;
    int stale_unload_pending;
} state;

extern const unsigned char _binary_loudness_limiter_ladspa_so_start[];
extern const unsigned char _binary_loudness_limiter_ladspa_so_end[];

static volatile sig_atomic_t stopping;
static void on_signal(int sig) { (void)sig; stopping = 1; }

static void emergency_sink_cb(pa_context *c,
                              const pa_sink_info *info,
                              int eol, void *userdata) {
    (void)userdata;
    if (eol || !info) return;
    if (strstr(info->name, "loudness_limiter") ||
        strstr(info->name, "loudness-limiter")) return;

    pa_cvolume volume;
    pa_cvolume_mute(&volume, 2);
    pa_context_set_sink_volume_by_name(c, info->name, &volume, NULL, NULL);
    pa_context_set_sink_mute_by_name(c, info->name, 1, NULL, NULL);
}

static void emergency_module_cb(pa_context *c,
                                const pa_module_info *info,
                                int eol, void *userdata) {
    (void)userdata;
    if (eol || !info) return;
    if (info->name && strcmp(info->name, "module-ladspa-sink") == 0 &&
        info->argument && strstr(info->argument, "sink_name=loudness_limiter_native"))
        pa_context_unload_module(c, info->index, NULL, NULL);
}

static void emergency_context_cb(pa_context *c, void *userdata) {
    pa_mainloop *mainloop = userdata;
    pa_context_state_t st = pa_context_get_state(c);
    if (st == PA_CONTEXT_READY) {
        pa_context_get_sink_info_list(c, emergency_sink_cb, NULL);
        pa_context_get_module_info_list(c, emergency_module_cb, NULL);
    } else if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
        pa_mainloop_quit(mainloop, 1);
    }
}

static void emergency_cleanup(void) {
    pa_mainloop *mainloop = pa_mainloop_new();
    if (!mainloop) _exit(1);

    pa_context *context = pa_context_new(pa_mainloop_get_api(mainloop),
                                         "Loudness Limiter Watchdog");
    if (!context) {
        pa_mainloop_free(mainloop);
        _exit(1);
    }

    pa_context_set_state_callback(context, emergency_context_cb, mainloop);
    pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL);

    int retval = 0;
    for (int n = 0; n < 4000; ++n)
        pa_mainloop_iterate(mainloop, 0, &retval);

    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
    _exit(0);
}

static pid_t start_watchdog(state *s) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        s->watchdog_write_fd = -1;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        s->watchdog_write_fd = -1;
        return -1;
    }

    if (pid == 0) {
        close(pipefd[1]);
        char byte;
        if (read(pipefd[0], &byte, 1) <= 0)
            emergency_cleanup();
        close(pipefd[0]);
        _exit(0);
    }

    close(pipefd[0]);
    s->watchdog_write_fd = pipefd[1];
    return pid;
}

static void stop_watchdog(state *s) {
    if (s->watchdog_write_fd >= 0) {
        close(s->watchdog_write_fd);
        s->watchdog_write_fd = -1;
    }
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void remove_extracted_plugin(state *s) {
    if (s->extracted_plugin) {
        unlink(s->extracted_plugin);
        free(s->extracted_plugin); s->extracted_plugin = NULL;
    }
}

static int extract_embedded_plugin(state *s) {
    char executable[4096];
    ssize_t executable_len = readlink("/proc/self/exe", executable,
                                      sizeof(executable) - 1);
    char candidates[2][8192];
    int candidate_count = 0;

    if (executable_len > 0) {
        executable[executable_len] = '\0';
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            snprintf(candidates[candidate_count++], sizeof(candidates[0]),
                     "%s/.loudness-limiter-ladspa.so", executable);
        }
    }

    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(candidates[candidate_count++], sizeof(candidates[0]),
                 "%s/.cache/loudness-limiter-ladspa.so", home);

    const unsigned char *start = _binary_loudness_limiter_ladspa_so_start;
    size_t size = (size_t)(_binary_loudness_limiter_ladspa_so_end - start);

    for (int c = 0; c < candidate_count; ++c) {
        char *slash = strrchr(candidates[c], '/');
        if (slash) {
            *slash = '\0';
            if (mkdir(candidates[c], 0700) != 0 && errno != EEXIST) {
                *slash = '/';
                continue;
            }
            *slash = '/';
        }

        int fd = open(candidates[c], O_WRONLY | O_CREAT | O_TRUNC, 0700);
        if (fd < 0) continue;

        size_t written = 0;
        int failed = 0;
        while (written < size) {
            ssize_t n = write(fd, start + written, size - written);
            if (n <= 0) { failed = 1; break; }
            written += (size_t)n;
        }
        fsync(fd);
        close(fd);

        if (!failed) {
            s->extracted_plugin = strdup(candidates[c]);
            return s->extracted_plugin ? 0 : -1;
        }
        unlink(candidates[c]);
    }
    return -1;
}

static void restore_terminal(state *s) {
    if (s->terminal_changed) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &s->original_terminal);
        s->terminal_changed = false;
    }
}

static void *stdin_thread_main(void *userdata) {
    state *s = userdata;
    char ch;
    while (!stopping && read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == 'q') {
            stopping = 1;
            printf("\r正在退出并恢复原设备...\n");
            fflush(stdout);
        } else if (ch == '+' || ch == '=') {
            s->limit_db = clampf(s->limit_db + 1.0f, -100.0f, 0.0f);
            s->shared_limit = s->limit_db;
            if (s->shared_limit_ptr) *s->shared_limit_ptr = s->limit_db;
            printf("\r限制幅度：%.1f dBFS\n", s->limit_db); fflush(stdout);
        } else if (ch == '-' || ch == '_') {
            s->limit_db = clampf(s->limit_db - 1.0f, -100.0f, 0.0f);
            s->shared_limit = s->limit_db;
            if (s->shared_limit_ptr) *s->shared_limit_ptr = s->limit_db;
            printf("\r限制幅度：%.1f dBFS\n", s->limit_db); fflush(stdout);
        } else if (ch == '0') {
            s->limit_db = s->default_limit_db;
            s->shared_limit = s->limit_db;
            if (s->shared_limit_ptr) *s->shared_limit_ptr = s->limit_db;
            printf("\r限制幅度已重置：%.1f dBFS\n", s->limit_db); fflush(stdout);
        } else if (ch == 'h' || ch == '?') {
            printf("\r+ 提高限制；- 降低限制；0 重置为默认值；q/Ctrl+C 退出\n"); fflush(stdout);
        }
    }
    return NULL;
}

static void enable_terminal_controls(state *s) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &s->original_terminal) != 0) return;
    struct termios raw = s->original_terminal;
    raw.c_lflag &= (tcflag_t)~ICANON;
    raw.c_lflag &= (tcflag_t)~ECHO;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
        s->terminal_changed = true;
    if (pthread_create(&s->stdin_thread, NULL, stdin_thread_main, s) == 0)
        s->stdin_thread_started = true;
}

static void move_sink_inputs_to_hardware(pa_context *c,
                                          const pa_sink_input_info *i,
                                          int eol, void *userdata) {
    state *s = userdata;
    if (eol || !i) return;
    pa_context_move_sink_input_by_name(c, i->index, s->hardware_sink, NULL, NULL);
}

static void move_sink_inputs_to_limiter(pa_context *c,
                                         const pa_sink_input_info *i,
                                         int eol, void *userdata) {
    (void)userdata;
    if (eol || !i) return;
    pa_context_move_sink_input_by_name(c, i->index, VIRTUAL_SINK, NULL, NULL);
}

static void mute_hardware_sink(state *s) {
    if (!s->context || !s->hardware_sink) return;

    pa_context_set_sink_mute_by_name(s->context, s->hardware_sink, 1,
                                     NULL, NULL);

    pa_cvolume volume;
    pa_cvolume_mute(&volume, 2);
    pa_context_set_sink_volume_by_name(s->context, s->hardware_sink,
                                       &volume, NULL, NULL);
}

static void cleanup(state *s) {
    if (!s->context) return;

    mute_hardware_sink(s);

    if (s->original_sink)
        pa_context_set_default_sink(s->context, s->original_sink, NULL, NULL);
    pa_context_get_sink_input_info_list(s->context, move_sink_inputs_to_hardware, s);
    for (int n = 0; n < 1000; ++n)
        pa_mainloop_iterate(s->mainloop, 0, NULL);
    if (s->module_index != PA_INVALID_INDEX)
        pa_context_unload_module(s->context, s->module_index, NULL, NULL);
    for (int n = 0; n < 1000; ++n)
        pa_mainloop_iterate(s->mainloop, 0, NULL);
    remove_extracted_plugin(s);
}

static void module_loaded(pa_context *c, uint32_t index, void *userdata) {
    state *s = userdata;
    if (index == PA_INVALID_INDEX) {
        fprintf(stderr, "无法加载原生 LADSPA sink 模块。请确认插件已安装并重启音频服务。\n");
        stopping = 1;
        return;
    }
    s->module_index = index;
    pa_context_set_default_sink(c, VIRTUAL_SINK, NULL, NULL);
    pa_operation *op = pa_context_get_sink_input_info_list(c, move_sink_inputs_to_limiter, s);
    if (op) pa_operation_unref(op);
    printf("原生响度处理已启动：硬件输出=%s，硬上限=%.1f dBFS\n",
           s->hardware_sink, s->limit_db);
    fflush(stdout);
}

static void load_native_sink(state *s);

static void stale_module_unloaded(pa_context *c, int success, void *userdata) {
    (void)c;
    state *s = userdata;
    (void)success;
    if (s->stale_unload_pending > 0)
        s->stale_unload_pending--;
    if (s->stale_unload_pending == 0)
        load_native_sink(s);
}

static void stale_module_info_cb(pa_context *c,
                                 const pa_module_info *i,
                                 int eol, void *userdata) {
    state *s = userdata;
    if (eol) {
        if (s->stale_unload_pending == 0)
            load_native_sink(s);
        return;
    }
    if (!i) return;

    if (i->name && strcmp(i->name, "module-ladspa-sink") == 0 &&
        i->argument && strstr(i->argument, "sink_name=" VIRTUAL_SINK) != NULL) {
        s->stale_unload_pending++;
        pa_context_unload_module(c, i->index, stale_module_unloaded, s);
    }
}

static void scan_stale_modules(state *s) {
    s->stale_unload_pending = 0;
    pa_context_get_module_info_list(s->context,
                                    stale_module_info_cb, s);
}

static void enable_hardware_audio(state *s) {
    if (!s->context || !s->hardware_sink) return;

    pa_context_set_sink_mute_by_name(s->context, s->hardware_sink, 0,
                                     NULL, NULL);
    pa_cvolume volume;
    pa_cvolume_set(&volume, 2, PA_VOLUME_NORM);
    pa_context_set_sink_volume_by_name(s->context, s->hardware_sink,
                                       &volume, NULL, NULL);
}

static void load_native_sink(state *s) {
    if (!s->context || !s->hardware_sink || !s->extracted_plugin) return;

    enable_hardware_audio(s);

    char args[8192];
    snprintf(args, sizeof(args),
             "sink_name=%s sink_master=%s plugin=%s label=loudness_limiter_native "
             "control=-18,6,%.1f",
             VIRTUAL_SINK, s->hardware_sink,
             s->extracted_plugin, s->limit_db);
    pa_context_load_module(s->context, "module-ladspa-sink", args,
                           module_loaded, s);
}

static void hardware_sink_selected(pa_context *c,
                                   const pa_sink_info *info,
                                   int eol, void *userdata) {
    (void)c;
    state *s = userdata;
    if (eol) {
        if (!s->hardware_sink) {
            fprintf(stderr, "无法找到真实硬件输出设备。\n");
            stopping = 1;
        }
        return;
    }
    if (!info || s->hardware_sink) return;

    if (strstr(info->name, "loudness_limiter") == NULL &&
        strstr(info->name, "loudness-limiter") == NULL) {
        s->hardware_sink = strdup(info->name);
        enable_hardware_audio(s);
        free(s->original_sink);
        s->original_sink = strdup(info->name);

        scan_stale_modules(s);
    }
}

static void server_info(pa_context *c, const pa_server_info *info, void *userdata) {
    state *s = userdata;
    if (!info || !info->default_sink_name) {
        stopping = 1;
        return;
    }
    if (extract_embedded_plugin(s) != 0) {
        fprintf(stderr, "无法写出内嵌 LADSPA 插件。\n");
        stopping = 1;
        return;
    }

    free(s->original_sink);
    s->original_sink = strdup(info->default_sink_name);

    if (strstr(info->default_sink_name, "loudness_limiter") != NULL ||
        strstr(info->default_sink_name, "loudness-limiter") != NULL) {
        pa_context_get_sink_info_list(c, hardware_sink_selected, s);
        return;
    }

    if (!s->hardware_sink)
        s->hardware_sink = strdup(info->default_sink_name);

    enable_hardware_audio(s);
    scan_stale_modules(s);
}

static void context_state(pa_context *c, void *userdata) {
    state *s = userdata;
    pa_context_state_t st = pa_context_get_state(c);
    if (st == PA_CONTEXT_READY)
        pa_context_get_server_info(c, server_info, s);
    else if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
        if (!stopping) fprintf(stderr, "音频服务连接中断。\n");
        stopping = 1;
    }
}

static int mkdir_one(const char *path);

static int config_path(char *path, size_t size) {
    char executable[4096];
    ssize_t executable_len = readlink("/proc/self/exe", executable,
                                      sizeof(executable) - 1);
    if (executable_len <= 0) return -1;
    executable[executable_len] = '\0';

    char *slash = strrchr(executable, '/');
    if (!slash) return -1;
    *slash = '\0';

    if (strlen(executable) >= size - 26) return -1;
    snprintf(path, size, "%s/loudness-limiter.conf", executable);
    return 0;
}

static float load_default_limit(void) {
    char path[2048];
    if (config_path(path, sizeof(path)) != 0) return -32.0f;

    FILE *file = fopen(path, "r");
    if (!file) return -32.0f;

    char line[128];
    while (fgets(line, sizeof(line), file)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (strcmp(line, "limit_db") == 0) {
            char *end = NULL;
            float value = strtof(eq, &end);
            if (end != eq)
                value = clampf(value, -100.0f, 0.0f);
            else
                value = -32.0f;
            fclose(file);
            return value;
        }
    }
    fclose(file);
    return -32.0f;
}

static int set_default_limit(float value) {
    char path[2048];
    if (config_path(path, sizeof(path)) != 0) return -1;
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    fprintf(file, "limit_db=%.1f\n", clampf(value, -100.0f, 0.0f));
    fclose(file);
    return 0;
}

static int reset_default_limit(void) {
    char path[2048];
    if (config_path(path, sizeof(path)) != 0) return -1;
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    return -1;
}

static int mkdir_one(const char *path) {
    if (mkdir(path, 0700) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
}

static int autostart_path(char *path, size_t size) {
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char base[4096];

    if (home && *home)
        snprintf(base, sizeof(base), "%s/.config", home);
    else if (config && *config)
        snprintf(base, sizeof(base), "%s", config);
    else
        return -1;

    if (mkdir_one(base) != 0) return -1;
    size_t base_len = strlen(base);
    if (base_len >= 4096 - 11) return -1;
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/autostart", base);
    if (mkdir_one(dir) != 0) return -1;

    size_t dir_len = strlen(dir);
    if (dir_len >= size - 26) return -1;
    snprintf(path, size, "%s/loudness-limiter.desktop", dir);
    return 0;
}

static int autostart_enable(void) {
    char path[2048];
    if (autostart_path(path, sizeof(path)) != 0) return -1;

    FILE *file = fopen(path, "w");
    if (!file) return -1;
    char executable[1024];
    ssize_t executable_len = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (executable_len <= 0)
        snprintf(executable, sizeof(executable), "/usr/local/bin/loudness-limiter");
    else
        executable[executable_len] = '\0';

    fprintf(file,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Linux Loudness Limiter\n"
            "Comment=Start native system loudness limiting\n"
            "Exec=%s\n"
            "Icon=audio-volume-high\n"
            "Terminal=true\n"
            "X-GNOME-Autostart-enabled=true\n"
            "X-KDE-autostart-after=panel\n",
            executable);
    fclose(file);
    printf("已启用开机自启动：%s\n", path);
    return 0;
}

static int autostart_disable(void) {
    char path[2048];
    if (autostart_path(path, sizeof(path)) != 0) return -1;
    if (unlink(path) == 0)
        printf("已禁用开机自启动：%s\n", path);
    else if (errno == ENOENT)
        printf("开机自启动已处于禁用状态\n");
    else
        return -1;
    return 0;
}

static int autostart_status(void) {
    char path[2048];
    if (autostart_path(path, sizeof(path)) != 0) return -1;
    if (access(path, R_OK) == 0)
        printf("开机自启动：已启用（%s）\n", path);
    else
        printf("开机自启动：已禁用\n");
    return 0;
}

static void usage(const char *name) {
    printf("用法：%s [选项]\n", name);
    printf("  -l, --limit DB        单次运行绝对上限（范围 0 到 -100 dBFS）\n");
    printf("      --enable-autostart 启用开机自启动\n");
    printf("      --disable-autostart 禁用开机自启动\n");
    printf("      --autostart-status 查看开机自启动状态\n");
    printf("      --set-default-limit DB 保存默认限制幅度（0 到 -100 dBFS）\n");
    printf("      --get-default-limit 查询保存的默认限制幅度\n");
    printf("      --reset-default-limit 恢复默认限制幅度为 -32 dBFS\n");
    printf("  -s, --sink NAME       指定硬件 sink 名称\n");
    printf("  -h, --help            显示帮助\n");
    printf("运行中：+ 提高上限；- 降低上限；0 重置为保存的默认值；q 或 Ctrl+C 退出\n");
}

int main(int argc, char **argv) {
    state s;
    memset(&s, 0, sizeof(s));
    s.default_limit_db = load_default_limit();
    s.limit_db = s.default_limit_db;
    s.shared_limit = s.limit_db;
    s.module_index = PA_INVALID_INDEX;
    s.watchdog_write_fd = -1;

    static const struct option options[] = {
        {"limit", required_argument, NULL, 'l'},
        {"sink", required_argument, NULL, 's'},
        {"enable-autostart", no_argument, NULL, 1000},
        {"disable-autostart", no_argument, NULL, 1001},
        {"autostart-status", no_argument, NULL, 1002},
        {"set-default-limit", required_argument, NULL, 1003},
        {"get-default-limit", no_argument, NULL, 1004},
        {"reset-default-limit", no_argument, NULL, 1005},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "l:s:h", options, NULL)) != -1) {
        switch (opt) {
            case 'l': s.limit_db = clampf(strtof(optarg, NULL), -100.0f, 0.0f); break;
            case 's': s.hardware_sink = strdup(optarg); break;
            case 1000: {
                int result = autostart_enable();
                free(s.hardware_sink); free(s.original_sink);
                return result == 0 ? 0 : 1;
            }
            case 1001: {
                int result = autostart_disable();
                free(s.hardware_sink); free(s.original_sink);
                return result == 0 ? 0 : 1;
            }
            case 1002: {
                int result = autostart_status();
                free(s.hardware_sink); free(s.original_sink);
                return result == 0 ? 0 : 1;
            }
            case 1003: {
                char *end = NULL;
                float value = strtof(optarg, &end);
                if (end == optarg || *end != '\0' || value < -100.0f || value > 0.0f) {
                    fprintf(stderr, "默认限制幅度必须为 0 到 -100 dBFS\n");
                    free(s.hardware_sink); free(s.original_sink);
                    return 1;
                }
                int result = set_default_limit(value);
                if (result == 0) printf("已保存默认限制幅度：%.1f dBFS\n", value);
                free(s.hardware_sink); free(s.original_sink);
                return result == 0 ? 0 : 1;
            }
            case 1004: {
                float value = load_default_limit();
                printf("默认限制幅度：%.1f dBFS\n", value);
                free(s.hardware_sink); free(s.original_sink);
                return 0;
            }
            case 1005: {
                int result = reset_default_limit();
                if (result == 0) printf("已恢复默认限制幅度：-32.0 dBFS\n");
                free(s.hardware_sink); free(s.original_sink);
                return result == 0 ? 0 : 1;
            }
            case 'h': usage(argv[0]); free(s.hardware_sink); free(s.original_sink); return 0;
            default: usage(argv[0]); free(s.hardware_sink); free(s.original_sink); return 1;
        }
    }

    s.shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (s.shm_fd >= 0) {
        ftruncate(s.shm_fd, sizeof(float));
        s.shared_limit_ptr = mmap(NULL, sizeof(float), PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s.shm_fd, 0);
        if (s.shared_limit_ptr == MAP_FAILED) s.shared_limit_ptr = NULL;
        else *s.shared_limit_ptr = s.limit_db;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGQUIT, on_signal);
    signal(SIGABRT, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);

    start_watchdog(&s);

    s.mainloop = pa_mainloop_new();
    if (!s.mainloop) { fprintf(stderr, "无法创建音频事件循环。\n"); return 1; }

    enable_terminal_controls(&s);
    printf("限制幅度：%.1f dBFS（+ 提高，- 降低，0 重置为默认值，q/Ctrl+C 退出）\n", s.limit_db);
    fflush(stdout);

    s.context = pa_context_new(pa_mainloop_get_api(s.mainloop), "Native Loudness Limiter");
    pa_context_set_state_callback(s.context, context_state, &s);
    if (pa_context_connect(s.context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        fprintf(stderr, "无法连接 PulseAudio/PipeWire：%s\n", pa_strerror(pa_context_errno(s.context)));
        restore_terminal(&s);
        stop_watchdog(&s);
        remove_extracted_plugin(&s);
        return 1;
    }

    int retval = 0;
    while (!stopping) {
        if (pa_mainloop_iterate(s.mainloop, 0, &retval) < 0) break;
        usleep(5000);
    }

    cleanup(&s);
    restore_terminal(&s);
    for (int n = 0; n < 100; ++n)
        pa_mainloop_iterate(s.mainloop, 0, &retval);

    pa_context_disconnect(s.context);
    pa_context_unref(s.context);
    pa_mainloop_free(s.mainloop);
    stop_watchdog(&s);
    remove_extracted_plugin(&s);

    if (s.shared_limit_ptr) munmap((void *)s.shared_limit_ptr, sizeof(float));
    if (s.shm_fd >= 0) close(s.shm_fd);
    shm_unlink(SHM_NAME);
    free(s.hardware_sink);
    free(s.original_sink);
    return retval;
}
