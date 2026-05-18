#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ftw.h>
#include <errno.h>
#include <time.h>

#define SERVICE_FILE "/etc/systemd/system/nsmd.service"
#define STATE_DIR "/nsm/daemon"
#define STATE_FILE "/nsm/daemon/state"
#define CMD_BUFFER_SIZE 4096
#define NOTIFICATION_MESSAGE "The Automatic snapshot was been canceled due to prevent OOM Killer,close some apps to create the snapshot"

typedef struct {
    int returncode;
    char stdout_data[8192];
} CmdResult;

typedef struct {
    uint64_t root_hash;
    uint64_t home_hash;
} DaemonState;

typedef struct {
    const char* base_path;
    uint64_t hash;
} TreeScanState;

static TreeScanState* active_scan = NULL;

CmdResult run_cmd(const char* command) {
    CmdResult res = {-1, ""};
    char buffer[256];
    char full_cmd[CMD_BUFFER_SIZE];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", command);
    FILE* pipe = popen(full_cmd, "r");
    if (!pipe) return res;
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        strncat(res.stdout_data, buffer, sizeof(res.stdout_data) - strlen(res.stdout_data) - 1);
    }
    int status = pclose(pipe);
    if (WIFEXITED(status)) {
        res.returncode = WEXITSTATUS(status);
    }
    return res;
}

bool ensure_dir(const char* path) {
    char temp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(temp)) return false;
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    if (temp[len - 1] == '/' && len > 1) temp[len - 1] = '\0';
    for (char* p = temp + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(temp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(temp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

bool get_self_path(char* output, size_t output_size) {
    ssize_t size = readlink("/proc/self/exe", output, output_size - 1);
    if (size < 0 || (size_t)size >= output_size) return false;
    output[size] = '\0';
    return true;
}

bool build_sibling_binary_path(const char* self_path, const char* binary_name, char* output, size_t output_size) {
    snprintf(output, output_size, "%s", self_path);
    char* slash = strrchr(output, '/');
    if (!slash) return false;
    slash[1] = '\0';
    if (strlen(output) + strlen(binary_name) + 1 > output_size) return false;
    strcat(output, binary_name);
    return true;
}

bool has_systemd(void) {
    return access("/run/systemd/system", F_OK) == 0;
}

uint64_t mix_hash(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

void hash_string(TreeScanState* state, const char* text) {
    while (*text != '\0') {
        state->hash = mix_hash(state->hash, (unsigned char)*text);
        text++;
    }
}

bool path_is_exact_or_child(const char* base, const char* path) {
    size_t base_len = strlen(base);
    if (strncmp(base, path, base_len) != 0) return false;
    return path[base_len] == '\0' || path[base_len] == '/';
}

bool should_skip_root_path(const char* path) {
    const char* skip_list[] = {
        "/proc",
        "/sys",
        "/dev",
        "/run",
        "/tmp",
        "/var/tmp",
        "/var/cache",
        "/var/log",
        "/nsm",
        "/nf-tree"
        "/mnt",
        "/media",
        "/lost+found",
        "/home",
        "/swapfile"
    };
    size_t count = sizeof(skip_list) / sizeof(skip_list[0]);
    for (size_t i = 0; i < count; i++) {
        if (path_is_exact_or_child(skip_list[i], path)) return true;
    }
    return false;
}

int scan_callback(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf) {
    (void)ftwbuf;
    if (!active_scan) return 0;
    if (strcmp(active_scan->base_path, "/") == 0 && should_skip_root_path(fpath)) {
        if (typeflag == FTW_D) return FTW_SKIP_SUBTREE;
        return 0;
    }
    if (!sb) return 0;
    hash_string(active_scan, fpath);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_mode);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_uid);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_gid);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_size);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_mtim.tv_sec);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_mtim.tv_nsec);
    active_scan->hash = mix_hash(active_scan->hash, (uint64_t)sb->st_ino);
    return 0;
}

bool compute_tree_hash(const char* path, uint64_t* hash_value) {
    if (access(path, F_OK) != 0) {
        *hash_value = 0;
        return true;
    }
    TreeScanState state = {path, 1469598103934665603ULL};
    active_scan = &state;
    int result = nftw(path, scan_callback, 64, FTW_PHYS | FTW_MOUNT | FTW_ACTIONRETVAL);
    active_scan = NULL;
    if (result != 0) return false;
    *hash_value = state.hash;
    return true;
}

bool load_state(DaemonState* state) {
    FILE* file = fopen(STATE_FILE, "r");
    if (!file) return false;
    unsigned long long root_hash = 0;
    unsigned long long home_hash = 0;
    int read_items = fscanf(file, "%llu %llu", &root_hash, &home_hash);
    fclose(file);
    if (read_items != 2) return false;
    state->root_hash = (uint64_t)root_hash;
    state->home_hash = (uint64_t)home_hash;
    return true;
}

bool save_state(const DaemonState* state) {
    if (!ensure_dir(STATE_DIR)) return false;
    FILE* file = fopen(STATE_FILE, "w");
    if (!file) return false;
    fprintf(file, "%llu %llu\n", (unsigned long long)state->root_hash, (unsigned long long)state->home_hash);
    fclose(file);
    return true;
}

bool read_memory_state(double* free_ram_percent, double* swap_usage_percent) {
    FILE* file = fopen("/proc/meminfo", "r");
    if (!file) return false;
    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    unsigned long long mem_free = 0;
    unsigned long long swap_total = 0;
    unsigned long long swap_free = 0;
    char key[64];
    unsigned long long value = 0;
    char unit[32];
    while (fscanf(file, "%63s %llu %31s", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0) mem_total = value;
        else if (strcmp(key, "MemAvailable:") == 0) mem_available = value;
        else if (strcmp(key, "MemFree:") == 0) mem_free = value;
        else if (strcmp(key, "SwapTotal:") == 0) swap_total = value;
        else if (strcmp(key, "SwapFree:") == 0) swap_free = value;
    }
    fclose(file);
    if (mem_available == 0) mem_available = mem_free;
    if (mem_total == 0) return false;
    *free_ram_percent = ((double)mem_available * 100.0) / (double)mem_total;
    if (swap_total == 0) {
        *swap_usage_percent = 0.0;
    } else {
        *swap_usage_percent = ((double)(swap_total - swap_free) * 100.0) / (double)swap_total;
    }
    return true;
}

bool system_resources_ok(void) {
#ifdef sim
    return false;
#else
    double free_ram_percent = 0.0;
    double swap_usage_percent = 0.0;
    if (!read_memory_state(&free_ram_percent, &swap_usage_percent)) return false;
    return free_ram_percent > 10.0 && swap_usage_percent < 20.0;
#endif
}

void send_notification(const char* message) {
    char cmd[CMD_BUFFER_SIZE];
    fprintf(stderr, "%s\n", message);
    snprintf(cmd, sizeof(cmd), "wall '%s' > /dev/null 2>&1", message);
    run_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "logger -t nsmd '%s' > /dev/null 2>&1", message);
    run_cmd(cmd);
}

bool create_daemon_snapshot(const char* nf_tree_path) {
    char cmd[CMD_BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "%s create nsmd", nf_tree_path);
    CmdResult result = run_cmd(cmd);
    return result.returncode == 0;
}

bool ensure_nf_tree_runtime(const char* nf_tree_path) {
    char cmd[CMD_BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "%s list > /dev/null", nf_tree_path);
    return run_cmd(cmd).returncode == 0;
}

bool get_current_state(DaemonState* state) {
    if (!compute_tree_hash("/", &state->root_hash)) return false;
    if (!compute_tree_hash("/home", &state->home_hash)) return false;
    return true;
}

bool write_service_file(const char* self_path, const char* nf_tree_path) {
    FILE* file = fopen(SERVICE_FILE, "w");
    if (!file) return false;
    fprintf(file,
            "[Unit]\n"
            "Description=nsm automatic snapshot daemon\n"
            "After=local-fs.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=%s --run-daemon --nsm %s\n"
            "Restart=always\n"
            "RestartSec=30\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n",
            self_path,
            nf_tree_path);
    fclose(file);
    return true;
}

int start_service(void) {
    if (!has_systemd()) {
        printf("This Script is not compartible with OpenRC and other init,please use systemd or modify this script by its source code.\n");
        return 1;
    }
    if (geteuid() != 0) {
        printf("Error: Root privileges required.\n");
        return 1;
    }
    char self_path[1024];
    char nf_tree_path[1024];
    if (!get_self_path(self_path, sizeof(self_path))) {
        printf("Error: Could not resolve executable path.\n");
        return 1;
    }
    if (!build_sibling_binary_path(self_path, "nsm", nf_tree_path, sizeof(nf_tree_path))) {
        printf("Error: Could not resolve nsm path.\n");
        return 1;
    }
    if (access(nf_tree_path, X_OK) != 0) {
        printf("Error: Could not find executable nsm at %s\n", nf_tree_path);
        return 1;
    }
    if (!write_service_file(self_path, nf_tree_path)) {
        printf("Error: Could not write %s\n", SERVICE_FILE);
        return 1;
    }
    CmdResult reload = run_cmd("systemctl daemon-reload");
    if (reload.returncode != 0) {
        printf("%s", reload.stdout_data);
        return 1;
    }
    CmdResult enable = run_cmd("systemctl enable --now nsmd.service");
    if (enable.returncode != 0) {
        printf("%s", enable.stdout_data);
        return 1;
    }
    printf("nsmd.service started.\n");
    return 0;
}

int stop_service(void) {
    if (!has_systemd()) {
        printf("This Script is not compartible with OpenRC and other init,please use systemd or modify this script by its source code.\n");
        return 1;
    }
    if (geteuid() != 0) {
        printf("Error: Root privileges required.\n");
        return 1;
    }
    CmdResult disable = run_cmd("systemctl disable --now nsmd.service");
    if (disable.returncode != 0) {
        printf("%s", disable.stdout_data);
        return 1;
    }
    CmdResult reload = run_cmd("systemctl daemon-reload");
    if (reload.returncode != 0) {
        printf("%s", reload.stdout_data);
        return 1;
    }
    printf("nsmd.service stopped.\n");
    return 0;
}

int run_daemon_loop(const char* nf_tree_path) {
    if (geteuid() != 0) return 1;
    if (!ensure_nf_tree_runtime(nf_tree_path)) return 1;
    if (!ensure_dir(STATE_DIR)) return 1;
    chdir("/");
    nice(15);
    DaemonState previous_state;
    if (!load_state(&previous_state)) {
        if (!get_current_state(&previous_state)) return 1;
        if (!save_state(&previous_state)) return 1;
    }
    while (1) {
        sleep(3600);
        DaemonState current_state;
        if (!get_current_state(&current_state)) continue;
        bool changed = current_state.root_hash != previous_state.root_hash || current_state.home_hash != previous_state.home_hash;
        if (!changed) continue;
        if (!system_resources_ok()) {
            send_notification(NOTIFICATION_MESSAGE);
            continue;
        }
        if (!create_daemon_snapshot(nf_tree_path)) continue;
        previous_state = current_state;
        save_state(&previous_state);
    }
}

void print_usage(const char* program_name) {
    printf("Usage: %s <1|0>\n", program_name);
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--run-daemon") == 0) {
        const char* nf_tree_path = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--nsm") == 0 && i + 1 < argc) {
                nf_tree_path = argv[i + 1];
                i++;
            }
        }
        char self_path[1024];
        char sibling_path[1024];
        if (!nf_tree_path) {
            if (!get_self_path(self_path, sizeof(self_path))) return 1;
            if (!build_sibling_binary_path(self_path, "nsm", sibling_path, sizeof(sibling_path))) return 1;
            nf_tree_path = sibling_path;
        }
        return run_daemon_loop(nf_tree_path);
    }
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "1") == 0) return start_service();
    if (strcmp(argv[1], "0") == 0) return stop_service();
    print_usage(argv[0]);
    return 1;
}
