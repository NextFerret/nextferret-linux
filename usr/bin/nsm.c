#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#define SNAPSHOT_BASE    "/nsm/snapshots"
#define AUTO_DIR         "/nsm/snapshots/auto"
#define DAEMON_AUTO_DIR  "/nsm/snapshots/auto/daemon"
#define MANUAL_DIR       "/nsm/snapshots/manual"
#define ROOT_SOURCE      "/"
#define HOME_SOURCE      "/home"
#define BTRFS_TEMP_MOUNT "/mnt/nsm-pool"
#define CMD_BUF          4096
#define STR_BUF          512
#define PATH_BUF         1024

typedef struct {
    int  rc;
    char out[8192];
} CmdResult;

typedef struct {
    char source[STR_BUF];
    char subvol[STR_BUF];
} BtrfsMount;

static CmdResult run_cmd(const char *cmd)
{
    CmdResult r = {-1, ""};
    char full[CMD_BUF];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE *p = popen(full, "r");
    if (!p) return r;
    char buf[256];
    while (fgets(buf, sizeof(buf), p))
        strncat(r.out, buf, sizeof(r.out) - strlen(r.out) - 1);
    int st = pclose(p);
    r.rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

static void decode_octal_field(const char *in, char *out, size_t sz)
{
    size_t i = 0, j = 0;
    while (in[i] && j + 1 < sz) {
        if (in[i] == '\\' && isdigit((unsigned char)in[i+1])
                          && isdigit((unsigned char)in[i+2])
                          && isdigit((unsigned char)in[i+3])) {
            char oct[4] = { in[i+1], in[i+2], in[i+3], '\0' };
            out[j++] = (char)strtol(oct, NULL, 8);
            i += 4;
        } else {
            out[j++] = in[i++];
        }
    }
    out[j] = '\0';
}

static bool path_has_prefix(const char *base, const char *path)
{
    size_t n = strlen(base);
    if (strcmp(base, "/") == 0) return path[0] == '/';
    if (strncmp(base, path, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

static bool ensure_dir(const char *path)
{
    char tmp[PATH_BUF];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return false;
    if (tmp[len - 1] == '/' && len > 1) tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static void parent_path(const char *path, char *out, size_t sz)
{
    snprintf(out, sz, "%s", path);
    char *sl = strrchr(out, '/');
    if (!sl) { snprintf(out, sz, "."); return; }
    if (sl == out) { sl[1] = '\0'; return; }
    *sl = '\0';
}

static bool resolve_btrfs_mount(const char *target, BtrfsMount *m)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return false;

    char line[4096];
    char best_mp[STR_BUF] = "";
    char best_root[STR_BUF] = "";
    char best_src[STR_BUF] = "";
    size_t best_len = 0;

    while (fgets(line, sizeof(line), f)) {
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        *sep = '\0';
        char *post = sep + 3;

        char root_raw[STR_BUF], mp_raw[STR_BUF], fstype[64], src_raw[STR_BUF];
        if (sscanf(line, "%*s %*s %*s %511s %511s", root_raw, mp_raw) != 2) continue;
        if (sscanf(post, "%63s %511s", fstype, src_raw) != 2) continue;
        if (strcmp(fstype, "btrfs") != 0) continue;

        char mp[STR_BUF], root[STR_BUF], src[STR_BUF];
        decode_octal_field(mp_raw, mp, sizeof(mp));
        decode_octal_field(root_raw, root, sizeof(root));
        decode_octal_field(src_raw, src, sizeof(src));

        if (!path_has_prefix(mp, target)) continue;
        size_t mp_len = strlen(mp);
        if (mp_len >= best_len) {
            best_len = mp_len;
            snprintf(best_mp,   sizeof(best_mp),   "%s", mp);
            snprintf(best_root, sizeof(best_root), "%s", root);
            snprintf(best_src,  sizeof(best_src),  "%s", src);
        }
    }
    fclose(f);
    if (best_len == 0) return false;

    const char *rel = (strcmp(best_mp, "/") == 0)
        ? (target[0] == '/' ? target + 1 : target)
        : (target + best_len + (target[best_len] == '/' ? 1 : 0));

    const char *root_rel = best_root[0] == '/' ? best_root + 1 : best_root;

    if (*root_rel && *rel)
        snprintf(m->subvol, sizeof(m->subvol), "%s/%s", root_rel, rel);
    else if (*root_rel)
        snprintf(m->subvol, sizeof(m->subvol), "%s", root_rel);
    else
        snprintf(m->subvol, sizeof(m->subvol), "%s", rel);

    snprintf(m->source, sizeof(m->source), "%s", best_src);
    return true;
}

static void shell(const char *cmd)
{
    int r = system(cmd);
    (void)r;
}

static void toggle_swap(bool on)
{
    if (!on) { shell("swapoff -a > /dev/null 2>&1"); return; }
    if (access("/dev/zram0", F_OK) == 0) {
        shell("mkswap /dev/zram0 > /dev/null 2>&1");
        shell("swapon /dev/zram0 -p 100 > /dev/null 2>&1");
    } else {
        shell("swapon -a > /dev/null 2>&1");
    }
}

static void update_grub(void)
{
    shell("update-grub > /dev/null 2>&1");
}

static void setup_dirs(void)
{
    mkdir("/nsm", 0755);
    mkdir(SNAPSHOT_BASE, 0755);
    mkdir(AUTO_DIR, 0755);
    mkdir(DAEMON_AUTO_DIR, 0755);
    mkdir(MANUAL_DIR, 0755);
}

static bool is_auto_name(const char *name)
{
    if (strcmp(name, "nftreedaemon") == 0) return true;
    for (const char *p = name; (p = strstr(p, "apt")) != NULL; p++) {
        bool pre = (p == name || !isalnum((unsigned char)p[-1]));
        bool post = (p[3] == '\0' || !isalnum((unsigned char)p[3]));
        if (pre && post) return true;
    }
    return false;
}

static const char *snapshot_dir(const char *name, const char *type)
{
    if (strcmp(name, "nftreedaemon") == 0) return DAEMON_AUTO_DIR;
    return strcmp(type, "auto") == 0 ? AUTO_DIR : MANUAL_DIR;
}

static bool create_snapshot(const char *src, const char *prefix,
                             const char *type, const char *name)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", tm);

    char filename[STR_BUF];
    snprintf(filename, sizeof(filename), "%s-%s-%s-%s", prefix, type, name, ts);

    char dest[PATH_BUF];
    snprintf(dest, sizeof(dest), "%s/%s", snapshot_dir(name, type), filename);

    bool is_root = strcmp(src, ROOT_SOURCE) == 0;
    if (is_root) toggle_swap(false);

    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot %s %s", src, dest);
    CmdResult r = run_cmd(cmd);

    if (is_root) toggle_swap(true);

    if (r.rc == 0) {
        printf("Created (%s): %s\n", type, filename);
        return true;
    }
    printf("%s\n", r.out);
    return false;
}

static bool mount_btrfs_toplevel(const char *src, const char *mp)
{
    if (!ensure_dir(mp)) return false;
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd), "mount -t btrfs -o subvolid=5 %s %s", src, mp);
    return run_cmd(cmd).rc == 0;
}

static bool rollback_root(const char *snap_path)
{
    BtrfsMount bm;
    if (!resolve_btrfs_mount(ROOT_SOURCE, &bm) || bm.subvol[0] == '\0') {
        printf("Error: Cannot resolve btrfs mount for %s.\n", ROOT_SOURCE);
        return false;
    }
    if (!mount_btrfs_toplevel(bm.source, BTRFS_TEMP_MOUNT)) {
        printf("Error: Cannot mount top-level btrfs volume.\n");
        return false;
    }

    char live[PATH_BUF], tmp[PATH_BUF + 16], cmd[CMD_BUF];
    snprintf(live, sizeof(live), "%s/%s", BTRFS_TEMP_MOUNT, bm.subvol);
    snprintf(tmp,  sizeof(tmp),  "%s_temp_del", live);

    if (access(tmp, F_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "btrfs subvolume delete %s", tmp);
        run_cmd(cmd);
    }

    snprintf(cmd, sizeof(cmd), "mv %s %s", live, tmp);
    CmdResult mv = run_cmd(cmd);
    if (mv.rc != 0) {
        run_cmd("umount -l " BTRFS_TEMP_MOUNT);
        printf("Error: Cannot move current root subvolume: %s\n", mv.out);
        return false;
    }

    snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot %s %s", snap_path, live);
    CmdResult snap = run_cmd(cmd);

    if (snap.rc == 0) {
        snprintf(cmd, sizeof(cmd), "btrfs subvolume delete %s", tmp);
        run_cmd(cmd);
        run_cmd("umount -l " BTRFS_TEMP_MOUNT);
        printf("SUCCESS: Root rollback complete. REBOOT REQUIRED.\n");
        return true;
    }

    snprintf(cmd, sizeof(cmd), "mv %s %s", tmp, live);
    CmdResult restore = run_cmd(cmd);
    run_cmd("umount -l " BTRFS_TEMP_MOUNT);

    if (restore.rc != 0)
        printf("Rollback failed: %s\nFailed to restore original: %s\n", snap.out, restore.out);
    else
        printf("Rollback failed: %s\nOriginal root subvolume restored.\n", snap.out);

    return false;
}

static bool rollback_home(const char *snap_path)
{
    BtrfsMount bm;
    if (!resolve_btrfs_mount(HOME_SOURCE, &bm) || bm.subvol[0] == '\0') {
        printf("Error: Cannot resolve btrfs mount for %s.\n", HOME_SOURCE);
        return false;
    }
    if (!mount_btrfs_toplevel(bm.source, BTRFS_TEMP_MOUNT)) {
        printf("Error: Cannot mount top-level btrfs volume.\n");
        return false;
    }

    char live[PATH_BUF], backup[PATH_BUF + 8], par[PATH_BUF], cmd[CMD_BUF];
    snprintf(live,   sizeof(live),   "%s/%s", BTRFS_TEMP_MOUNT, bm.subvol);
    snprintf(backup, sizeof(backup), "%s_old", live);
    parent_path(live, par, sizeof(par));

    if (!ensure_dir(par)) {
        run_cmd("umount -l " BTRFS_TEMP_MOUNT);
        printf("Error: Cannot prepare subvolume path.\n");
        return false;
    }

    if (access(backup, F_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "btrfs subvolume delete %s", backup);
        run_cmd(cmd);
    }

    snprintf(cmd, sizeof(cmd), "mv %s %s", live, backup);
    CmdResult mv = run_cmd(cmd);
    if (mv.rc != 0) {
        run_cmd("umount -l " BTRFS_TEMP_MOUNT);
        printf("Error: Cannot preserve current subvolume: %s\n", mv.out);
        return false;
    }

    snprintf(cmd, sizeof(cmd), "btrfs subvolume snapshot %s %s", snap_path, live);
    CmdResult snap = run_cmd(cmd);

    if (snap.rc == 0) {
        run_cmd("umount -l " BTRFS_TEMP_MOUNT);
        printf("SUCCESS: Home rollback complete. Old subvolume kept as %s_old. REBOOT REQUIRED.\n", bm.subvol);
        return true;
    }

    snprintf(cmd, sizeof(cmd), "mv %s %s", backup, live);
    CmdResult restore = run_cmd(cmd);
    run_cmd("umount -l " BTRFS_TEMP_MOUNT);

    if (restore.rc != 0)
        printf("Rollback failed: %s\nFailed to restore original: %s\n", snap.out, restore.out);
    else
        printf("Rollback failed: %s\nOriginal subvolume restored.\n", snap.out);

    return false;
}

static bool find_snapshot(const char *name, char *out, size_t sz)
{
    const char *dirs[] = { MANUAL_DIR, AUTO_DIR, DAEMON_AUTO_DIR };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, sz, "%s/%s", dirs[i], name);
        if (access(out, F_OK) == 0) return true;
    }
    return false;
}

static bool rollback_snapshot(const char *name)
{
    char path[PATH_BUF];
    if (!find_snapshot(name, path, sizeof(path))) {
        printf("Error: Snapshot '%s' not found.\n", name);
        return false;
    }
    if (strncmp(name, "root-", 5) == 0) return rollback_root(path);
    if (strncmp(name, "home-", 5) == 0) return rollback_home(path);
    printf("Error: Snapshot name must start with 'root-' or 'home-'.\n");
    return false;
}

static void list_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    char *roots[256], *homes[256];
    int nr = 0, nh = 0;
    struct dirent *e;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if      (strncmp(e->d_name, "root-", 5) == 0) roots[nr++] = strdup(e->d_name);
        else if (strncmp(e->d_name, "home-", 5) == 0) homes[nh++] = strdup(e->d_name);
    }
    closedir(d);

    printf("  [Root]\n");
    if (!nr) printf("    (none)\n");
    for (int i = 0; i < nr; i++) { printf("    %s\n", roots[i]); free(roots[i]); }

    printf("  [Home]\n");
    if (!nh) printf("    (none)\n");
    for (int i = 0; i < nh; i++) { printf("    %s\n", homes[i]); free(homes[i]); }
}

static void list_snapshots(void)
{
    printf("\nManual Snapshots\n");       list_dir(MANUAL_DIR);
    printf("\napt Snapshots\n");          list_dir(AUTO_DIR);
    printf("\nnsm Daemon Snapshots\n");list_dir(DAEMON_AUTO_DIR);
    printf("\n");
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void delete_subvol(const char *path)
{
    char cmd[CMD_BUF];
    snprintf(cmd, sizeof(cmd), "btrfs subvolume delete %s", path);
    if (run_cmd(cmd).rc == 0) return;
    if (rmdir(path) == 0) return;
    unlink(path);
}

static void cleanup_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    char *roots[256], *homes[256];
    int nr = 0, nh = 0;
    struct dirent *e;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if      (strncmp(e->d_name, "root-", 5) == 0) roots[nr++] = strdup(e->d_name);
        else if (strncmp(e->d_name, "home-", 5) == 0) homes[nh++] = strdup(e->d_name);
    }
    closedir(d);

    qsort(roots, nr, sizeof(char *), cmp_str);
    qsort(homes, nh, sizeof(char *), cmp_str);

    for (int i = 0; i < nr - 6; i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, roots[i]);
        delete_subvol(path);
    }
    for (int i = 0; i < nh - 6; i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/%s", dir, homes[i]);
        delete_subvol(path);
    }

    for (int i = 0; i < nr; i++) free(roots[i]);
    for (int i = 0; i < nh; i++) free(homes[i]);
}

static void autodel_snapshots(void)
{
    cleanup_dir(AUTO_DIR);
    cleanup_dir(DAEMON_AUTO_DIR);
    cleanup_dir(MANUAL_DIR);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [argument]\n\n", prog);
    printf("Commands:\n");
    printf("  create <name>    Create root+home snapshots\n");
    printf("                   Auto type if name matches 'apt', 'apt-*', or 'nftreedaemon'\n");
    printf("  rollback <name>  Roll back to named snapshot (must start with root- or home-)\n");
    printf("  list             List all snapshots\n");
    printf("  autodel          Delete oldest auto snapshots, keeping the latest 6\n");
}

int main(int argc, char *argv[])
{
    if (geteuid() != 0) {
        fprintf(stderr, "Error: root privileges required.\n");
        return 1;
    }
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        setup_dirs();
        bool isauto = is_auto_name(argv[2]);
        const char *type = isauto ? "auto" : "manual";
        create_snapshot(ROOT_SOURCE, "root", type, argv[2]);
        create_snapshot(HOME_SOURCE, "home", type, argv[2]);
        update_grub();
        if (isauto) autodel_snapshots();

    } else if (strcmp(cmd, "rollback") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        if (rollback_snapshot(argv[2])) update_grub();

    } else if (strcmp(cmd, "list") == 0) {
        list_snapshots();

    } else if (strcmp(cmd, "autodel") == 0) {
        autodel_snapshots();

    } else {
        fprintf(stderr, "Error: unknown command '%s'\n\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
