#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>

#define SNAPSHOT_BASE "/nf-tree/snapshots"
#define AUTO_DIR "/nf-tree/snapshots/auto"
#define MANUAL_DIR "/nf-tree/snapshots/manual"
#define ROOT_SOURCE "/"
#define HOME_SOURCE "/home"

#define CMD_BUFFER_SIZE 4096

typedef struct {
    int returncode;
    char stdout_data[8192];
} CmdResult;

typedef struct {
    char source[512];
    char subvol_path[512];
} BtrfsPathInfo;

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

void decode_mount_field(const char* input, char* output, size_t output_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (input[in_pos] != '\0' && out_pos + 1 < output_size) {
        if (input[in_pos] == '\\' &&
            input[in_pos + 1] != '\0' &&
            input[in_pos + 2] != '\0' &&
            input[in_pos + 3] != '\0' &&
            isdigit((unsigned char)input[in_pos + 1]) &&
            isdigit((unsigned char)input[in_pos + 2]) &&
            isdigit((unsigned char)input[in_pos + 3])) {
            char octal[4];
            octal[0] = input[in_pos + 1];
            octal[1] = input[in_pos + 2];
            octal[2] = input[in_pos + 3];
            octal[3] = '\0';
            output[out_pos++] = (char)strtol(octal, NULL, 8);
            in_pos += 4;
            continue;
        }
        output[out_pos++] = input[in_pos++];
    }
    output[out_pos] = '\0';
}

bool path_is_prefix(const char* base, const char* path) {
    size_t base_len = strlen(base);
    if (strcmp(base, "/") == 0) {
        return path[0] == '/';
    }
    if (strncmp(base, path, base_len) != 0) {
        return false;
    }
    return path[base_len] == '\0' || path[base_len] == '/';
}

bool resolve_btrfs_path(const char* target_path, BtrfsPathInfo* info) {
    FILE* mountinfo = fopen("/proc/self/mountinfo", "r");
    if (!mountinfo) {
        return false;
    }
    char line[4096];
    char best_mount_point[512] = "";
    char best_root[512] = "";
    char best_source[512] = "";
    size_t best_len = 0;
    while (fgets(line, sizeof(line), mountinfo) != NULL) {
        char* separator = strstr(line, " - ");
        if (!separator) {
            continue;
        }
        *separator = '\0';
        char* post = separator + 3;
        char root_raw[512];
        char mount_point_raw[512];
        char fs_type[64];
        char source_raw[512];
        if (sscanf(line, "%*s %*s %*s %511s %511s", root_raw, mount_point_raw) != 2) {
            continue;
        }
        if (sscanf(post, "%63s %511s", fs_type, source_raw) != 2) {
            continue;
        }
        if (strcmp(fs_type, "btrfs") != 0) {
            continue;
        }
        char mount_point[512];
        char root[512];
        char source[512];
        decode_mount_field(mount_point_raw, mount_point, sizeof(mount_point));
        decode_mount_field(root_raw, root, sizeof(root));
        decode_mount_field(source_raw, source, sizeof(source));
        if (!path_is_prefix(mount_point, target_path)) {
            continue;
        }
        size_t mount_len = strlen(mount_point);
        if (mount_len >= best_len) {
            best_len = mount_len;
            snprintf(best_mount_point, sizeof(best_mount_point), "%s", mount_point);
            snprintf(best_root, sizeof(best_root), "%s", root);
            snprintf(best_source, sizeof(best_source), "%s", source);
        }
    }
    fclose(mountinfo);
    if (best_len == 0) {
        return false;
    }
    const char* relative = "";
    if (strcmp(best_mount_point, "/") == 0) {
        relative = target_path[0] == '/' ? target_path + 1 : target_path;
    } else if (strncmp(target_path, best_mount_point, best_len) == 0) {
        relative = target_path + best_len;
        if (relative[0] == '/') {
            relative++;
        }
    }
    const char* root_relative = best_root;
    if (root_relative[0] == '/') {
        root_relative++;
    }
    if (root_relative[0] != '\0' && relative[0] != '\0') {
        snprintf(info->subvol_path, sizeof(info->subvol_path), "%s/%s", root_relative, relative);
    } else if (root_relative[0] != '\0') {
        snprintf(info->subvol_path, sizeof(info->subvol_path), "%s", root_relative);
    } else {
        snprintf(info->subvol_path, sizeof(info->subvol_path), "%s", relative);
    }
    snprintf(info->source, sizeof(info->source), "%s", best_source);
    return true;
}

bool ensure_dir(const char* path) {
    char temp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(temp)) {
        return false;
    }
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    if (temp[len - 1] == '/' && len > 1) {
        temp[len - 1] = '\0';
    }
    for (char* p = temp + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }
    if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

void get_parent_path(const char* path, char* parent, size_t parent_size) {
    strncpy(parent, path, parent_size - 1);
    parent[parent_size - 1] = '\0';
    char* slash = strrchr(parent, '/');
    if (!slash) {
        snprintf(parent, parent_size, ".");
        return;
    }
    if (slash == parent) {
        slash[1] = '\0';
        return;
    }
    *slash = '\0';
}

void setup_dirs() {
    mkdir("/nf-tree", 0755);
    mkdir(SNAPSHOT_BASE, 0755);
    mkdir(AUTO_DIR, 0755);
    mkdir(MANUAL_DIR, 0755);
}

void update_grub() {
    system("update-grub > /dev/null 2>&1");
}

void toggle_swap(bool active) {
    if (!active) {
        system("swapoff -a > /dev/null 2>&1");
    } else {
        if (access("/dev/zram0", F_OK) == 0) {
            system("mkswap /dev/zram0 > /dev/null 2>&1");
            system("swapon /dev/zram0 -p 100 > /dev/null 2>&1");
        } else {
            system("swapon -a > /dev/null 2>&1");
        }
    }
}

bool create_snapshot(const char* source, const char* prefix, const char* type, const char* name) {
    setup_dirs();
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", tm);
    char filename[512];
    snprintf(filename, sizeof(filename), "%s-%s-%s-%s", prefix, type, name, timestamp);
    char target_path[1024];
    snprintf(target_path, sizeof(target_path), "%s/%s", (strcmp(type, "auto") == 0) ? AUTO_DIR : MANUAL_DIR, filename);
    bool is_root = (strcmp(source, ROOT_SOURCE) == 0);
    if (is_root) toggle_swap(false);
    char cmd[CMD_BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot %s %s", source, target_path);
    CmdResult res = run_cmd(cmd);
    if (is_root) toggle_swap(true);
    if (res.returncode == 0) {
        printf("Created (%s): %s\n", type, filename);
        return true;
    }
    return false;
}

void list_group(const char* base_path) {
    DIR* d = opendir(base_path);
    if (!d) return;
    struct dirent* dir;
    char* root_entries[256];
    char* home_entries[256];
    int r_count = 0, h_count = 0;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        if (strncmp(dir->d_name, "root-", 5) == 0) {
            root_entries[r_count++] = strdup(dir->d_name);
        } else if (strncmp(dir->d_name, "home-", 5) == 0) {
            home_entries[h_count++] = strdup(dir->d_name);
        }
    }
    printf("  [Root Snapshots]\n");
    if (r_count == 0) printf("    (none)\n");
    for (int i = 0; i < r_count; i++) {
        printf("    %s\n", root_entries[i]);
        free(root_entries[i]);
    }
    printf("  [Home Snapshots]\n");
    if (h_count == 0) printf("    (none)\n");
    for (int i = 0; i < h_count; i++) {
        printf("    %s\n", home_entries[i]);
        free(home_entries[i]);
    }
    closedir(d);
}

void list_snapshots() {
    printf("\n=== MANUAL STORAGE ===\n");
    list_group(MANUAL_DIR);
    printf("\n=== AUTO STORAGE ===\n");
    list_group(AUTO_DIR);
    printf("\n");
}

bool rollback_snapshot(const char* name) {
    char snap_path[1024];
    snprintf(snap_path, sizeof(snap_path), "%s/%s", MANUAL_DIR, name);
    if (access(snap_path, F_OK) != 0) {
        snprintf(snap_path, sizeof(snap_path), "%s/%s", AUTO_DIR, name);
        if (access(snap_path, F_OK) != 0) {
            printf("Error: Snapshot %s not found.\n", name);
            return false;
        }
    }
    const char* target_path = NULL;
    if (strncmp(name, "root-", 5) == 0) {
        target_path = ROOT_SOURCE;
    } else if (strncmp(name, "home-", 5) == 0) {
        target_path = HOME_SOURCE;
    } else {
        printf("Error: Prefix must be root- or home-\n");
        return false;
    }
    BtrfsPathInfo path_info;
    if (!resolve_btrfs_path(target_path, &path_info)) {
        printf("Error: Could not resolve the Btrfs source for %s.\n", target_path);
        return false;
    }
    if (path_info.subvol_path[0] == '\0') {
        printf("Error: Rollback of the top-level Btrfs volume is not supported.\n");
        return false;
    }
    if (!ensure_dir("/mnt/nf-tree-pool")) {
        printf("Error: Could not prepare the temporary mount point.\n");
        return false;
    }
    char cmd[CMD_BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "mount -t btrfs -o subvolid=5 %s /mnt/nf-tree-pool", path_info.source);
    if (run_cmd(cmd).returncode != 0) {
        printf("Error: Could not mount top-level Btrfs volume.\n");
        return false;
    }
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm);
    char live_path[1024];
    char backup_path[1024];
    char parent_path[1024];
    snprintf(live_path, sizeof(live_path), "/mnt/nf-tree-pool/%s", path_info.subvol_path);
    snprintf(backup_path, sizeof(backup_path), "/mnt/nf-tree-pool/%s_old_%s", path_info.subvol_path, timestamp);
    get_parent_path(live_path, parent_path, sizeof(parent_path));
    if (!ensure_dir(parent_path)) {
        run_cmd("umount -l /mnt/nf-tree-pool");
        printf("Error: Could not prepare the subvolume path.\n");
        return false;
    }
    snprintf(cmd, sizeof(cmd), "mv %s %s", live_path, backup_path);
    CmdResult move_res = run_cmd(cmd);
    if (move_res.returncode != 0) {
        run_cmd("umount -l /mnt/nf-tree-pool");
        printf("Error: Could not preserve the current subvolume: %s\n", move_res.stdout_data);
        return false;
    }
    snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot %s %s", snap_path, live_path);
    CmdResult res = run_cmd(cmd);
    if (res.returncode == 0) {
        run_cmd("umount -l /mnt/nf-tree-pool");
        printf("SUCCESS: Rollback complete.\n");
        printf("Old subvolume saved as %s_old_%s\n", path_info.subvol_path, timestamp);
        printf("REBOOT REQUIRED to apply changes.\n");
        return true;
    }
    snprintf(cmd, sizeof(cmd), "mv %s %s", backup_path, live_path);
    CmdResult restore_res = run_cmd(cmd);
    run_cmd("umount -l /mnt/nf-tree-pool");
    if (restore_res.returncode != 0) {
        printf("Rollback failed: %s\n", res.stdout_data);
        printf("Additional error while restoring the old subvolume: %s\n", restore_res.stdout_data);
        return false;
    }
    printf("Rollback failed: %s\n", res.stdout_data);
    printf("The old subvolume was restored.\n");
    return false;
}

void print_usage(const char* prog_name) {
    printf("Usage: %s <command> [arguments]\n\n", prog_name);
    printf("Commands:\n");
    printf("  create <name>      Create root/home snapshots (auto if name is 'apt' or 'apt-*')\n");
    printf("  rollback <name>    Rollback to a specific snapshot\n");
    printf("  list               List all available snapshots\n");
    printf("  autodel            Cleanup auto snapshots, keeping the latest 6\n");
}

bool is_auto_snapshot(const char* name) {
    const char* p = name;
    while ((p = strstr(p, "apt")) != NULL) {
        bool prev_ok = (p == name || !isalnum(*(p - 1)));
        bool next_ok = (*(p + 3) == '\0' || !isalnum(*(p + 3)));
        if (prev_ok && next_ok) return true;
        p++;
    }
    return false;
}

void cleanup_snapshots(const char* dir_path) {
    DIR* d = opendir(dir_path);
    if (!d) return;
    struct dirent* dir;
    char* root_entries[256];
    char* home_entries[256];
    int r_count = 0, h_count = 0;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        if (strncmp(dir->d_name, "root-", 5) == 0) {
            root_entries[r_count++] = strdup(dir->d_name);
        } else if (strncmp(dir->d_name, "home-", 5) == 0) {
            home_entries[h_count++] = strdup(dir->d_name);
        }
    }
    closedir(d);
    if (r_count > 6) {
        for (int i = 0; i < r_count - 6; i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir_path, root_entries[i]);
            remove(path);
        }
    }
    if (h_count > 6) {
        for (int i = 0; i < h_count - 6; i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir_path, home_entries[i]);
            remove(path);
        }
    }
    for (int i = 0; i < r_count; i++) free(root_entries[i]);
    for (int i = 0; i < h_count; i++) free(home_entries[i]);
}

void autodel_snapshots() {
    cleanup_snapshots(AUTO_DIR);
    cleanup_snapshots(MANUAL_DIR);
}

int main(int argc, char* argv[]) {
    if (geteuid() != 0) {
        printf("Error: Root privileges required.\n");
        return 1;
    }
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    char* command = argv[1];
    if (strcmp(command, "create") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        bool is_auto = is_auto_snapshot(argv[2]);
        const char* type = is_auto ? "auto" : "manual";
        create_snapshot(ROOT_SOURCE, "root", type, argv[2]);
        create_snapshot(HOME_SOURCE, "home", type, argv[2]);
        update_grub();
        if (is_auto) {
            cleanup_snapshots(AUTO_DIR);
            cleanup_snapshots(MANUAL_DIR);
        }
    } else if (strcmp(command, "rollback") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        if (rollback_snapshot(argv[2])) update_grub();
    } else if (strcmp(command, "list") == 0) {
        list_snapshots();
    } else if (strcmp(command, "autodel") == 0) {
        autodel_snapshots();
    } else {
        printf("Error: Unknown command '%s'\n\n", command);
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
