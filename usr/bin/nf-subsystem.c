#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <glob.h>
#include <dirent.h>
#include <errno.h>

#define BASE_DIR "/nf-tree/subsystems"

void run(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "Command failed: %s\n", cmd);
    }
}

char *get_host_user() {
    char *user = getenv("SUDO_USER");
    if (!user) user = getenv("USER");
    return user;
}

gid_t get_host_group_id() {
    struct passwd *pw = getpwnam(get_host_user());
    return pw ? pw->pw_gid : 0;
}

char *get_host_home() {
    char *user = get_host_user();
    struct passwd *pw = getpwnam(user);
    return pw ? pw->pw_dir : NULL;
}

void mkdir_p(const char *path) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

void mount_subsystem(const char *path) {
    const char *mounts[] = {"/proc", "/sys", "/dev", "/dev/pts", "/tmp", "/dev/dri"};
    char target[1024];
    char cmd[2048];

    for (int i = 0; i < 6; i++) {
        struct stat st;
        if (stat(mounts[i], &st) == 0) {
            snprintf(target, sizeof(target), "%s%s", path, mounts[i]);
            mkdir_p(target);
            snprintf(cmd, sizeof(cmd), "mountpoint -q %s || mount --bind %s %s", target, mounts[i], target);
            run(cmd);
        }
    }

    const char *x11 = "/tmp/.X11-unix";
    if (access(x11, F_OK) == 0) {
        snprintf(target, sizeof(target), "%s%s", path, x11);
        mkdir_p(target);
        snprintf(cmd, sizeof(cmd), "mountpoint -q %s || mount --bind %s %s", target, x11, target);
        run(cmd);
    }

    snprintf(cmd, sizeof(cmd), "chmod -R 1777 %s/tmp", path);
    system(cmd);
}

void umount_subsystem(const char *path) {
    const char *mounts[] = {"/dev/dri", "/tmp/.X11-unix", "/tmp", "/dev/pts", "/dev", "/sys", "/proc"};
    char target[1024];
    char cmd[2048];

    for (int i = 0; i < 7; i++) {
        snprintf(target, sizeof(target), "%s%s", path, mounts[i]);
        snprintf(cmd, sizeof(cmd), "umount -l %s 2>/dev/null", target);
        system(cmd);
    }
}

void copy_file(const char *src, const char *dst) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cp %s %s", src, dst);
    run(cmd);
}

void integrate_apps(const char *name, const char *path) {
    char host_app_dir[1024];
    snprintf(host_app_dir, sizeof(host_app_dir), "%s/.local/share/applications", get_host_home());
    mkdir_p(host_app_dir);

    char pattern1[1024], pattern2[1024];
    snprintf(pattern1, sizeof(pattern1), "%s/usr/share/applications/*.desktop", path);
    snprintf(pattern2, sizeof(pattern2), "%s/home/*/.local/share/applications/*.desktop", path);

    glob_t g;
    const char *patterns[] = {pattern1, pattern2};

    for (int i = 0; i < 2; i++) {
        if (glob(patterns[i], 0, NULL, &g) == 0) {
            for (size_t j = 0; j < g.gl_pathc; j++) {
                char *filename = strrchr(g.gl_pathv[j], '/') + 1;
                char target_path[1024];
                snprintf(target_path, sizeof(target_path), "%s/nf-%s-%s", host_app_dir, name, filename);

                FILE *in = fopen(g.gl_pathv[j], "r");
                FILE *out = fopen(target_path, "w");
                if (!in || !out) continue;

                char line[2048];
                while (fgets(line, sizeof(line), in)) {
                    if (strncmp(line, "Exec=", 5) == 0) {
                        fprintf(out, "Exec=sudo nf-subsystem enter --name %s --run '%s'\n", name, line + 5);
                    } else if (strncmp(line, "Name=", 5) == 0) {
                        line[strcspn(line, "\n")] = 0;
                        fprintf(out, "Name=%s (%s)\n", line + 5, name);
                    } else {
                        fputs(line, out);
                    }
                }
                fclose(in);
                fclose(out);
                chown(target_path, getuid(), get_host_group_id());
            }
            globfree(&g);
        }
    }
}

void remove_apps(const char *name) {
    char host_app_dir[1024];
    snprintf(host_app_dir, sizeof(host_app_dir), "%s/.local/share/applications/nf-%s-*.desktop", get_host_home(), name);
    
    glob_t g;
    if (glob(host_app_dir, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            remove(g.gl_pathv[i]);
        }
        globfree(&g);
    }
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Run as root.\n");
        return 1;
    }

    if (argc < 2) {
        printf("Usage: %s {list|create|enter|delete} [args]\n", argv[0]);
        return 0;
    }

    char *command = argv[1];

    if (strcmp(command, "list") == 0) {
        DIR *d = opendir(BASE_DIR);
        if (d) {
            struct dirent *dir;
            printf("Subsystems: ");
            int first = 1;
            while ((dir = readdir(d)) != NULL) {
                if (dir->d_name[0] != '.') {
                    if (!first) printf(", ");
                    printf("%s", dir->d_name);
                    first = 0;
                }
            }
            printf("\n");
            closedir(d);
        }
    } else if (strcmp(command, "create") == 0) {
        char *name = NULL, *release = NULL, *arch = NULL, *distro = "stable";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--name") == 0) name = argv[++i];
            else if (strcmp(argv[i], "--release") == 0) { release = argv[++i]; arch = argv[++i]; }
            else if (strcmp(argv[i], "--distro") == 0) distro = argv[++i];
        }

        if (!name) return 1;
        char target[1024];
        snprintf(target, sizeof(target), "%s/%s", BASE_DIR, name);
        mkdir_p(target);

        char cmd[4096];
        if (release) snprintf(cmd, sizeof(cmd), "debootstrap %s %s %s", release, target, arch);
        else snprintf(cmd, sizeof(cmd), "debootstrap %s %s", distro, target);
        run(cmd);

        char resolv[1024];
        snprintf(resolv, sizeof(resolv), "%s/etc/resolv.conf", target);
        copy_file("/etc/resolv.conf", resolv);

        mount_subsystem(target);
        
        snprintf(cmd, sizeof(cmd), "chroot %s apt-get update", target); run(cmd);
        snprintf(cmd, sizeof(cmd), "chroot %s apt-get install -y mesa-utils libgl1-mesa-dri libglx-mesa0 libwayland-client0 libwayland-cursor0 sudo", target); run(cmd);

        char *h_user = get_host_user();
        snprintf(cmd, sizeof(cmd), "chroot %s groupadd %s", target, h_user); system(cmd);
        snprintf(cmd, sizeof(cmd), "chroot %s useradd -m -g %s -s /bin/bash %s", target, h_user, h_user); system(cmd);
        snprintf(cmd, sizeof(cmd), "chroot %s usermod -aG sudo %s", target, h_user); system(cmd);

        printf("Set password for %s? (y/N): ", h_user);
        char ans = getchar();
        if (ans == 'y' || ans == 'Y') snprintf(cmd, sizeof(cmd), "chroot %s passwd %s", target, h_user);
        else snprintf(cmd, sizeof(cmd), "chroot %s passwd -d %s", target, h_user);
        run(cmd);

        snprintf(cmd, sizeof(cmd), "chroot %s sh -c 'echo \"%s ALL=(ALL) NOPASSWD: ALL\" > /etc/sudoers.d/%s'", target, h_user, h_user); run(cmd);
        snprintf(cmd, sizeof(cmd), "chroot %s chmod 0440 /etc/sudoers.d/%s", target, h_user); run(cmd);

        integrate_apps(name, target);
        umount_subsystem(target);
    } else if (strcmp(command, "enter") == 0) {
        char *name = NULL, *run_cmd = NULL;
        int root = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--name") == 0) name = argv[++i];
            else if (strcmp(argv[i], "--run") == 0) run_cmd = argv[++i];
            else if (strcmp(argv[i], "--root") == 0) root = 1;
        }

        if (!name) return 1;
        char target[1024];
        snprintf(target, sizeof(target), "%s/%s", BASE_DIR, name);
        mount_subsystem(target);

        char *h_user = get_host_user();
        char *h_home = get_host_home();
        char *wayland = getenv("WAYLAND_DISPLAY");
        char *display = getenv("DISPLAY");
        if (!display) display = ":0";

        char xauth_cmd[1024];
        snprintf(xauth_cmd, sizeof(xauth_cmd), "xhost +local:%s > /dev/null 2>&1", h_user);
        system(xauth_cmd);

        char xauth[1024], chroot_auth[1024];
        snprintf(xauth, sizeof(xauth), "%s/.Xauthority", h_home);
        int xauth_mounted = 0;
        if (access(xauth, F_OK) == 0) {
            snprintf(chroot_auth, sizeof(chroot_auth), "%s/home/%s/.Xauthority", target, h_user);
            run("touch "); // dummy for logic
            char mnt_cmd[2048];
            snprintf(mnt_cmd, sizeof(mnt_cmd), "touch %s && mount --bind %s %s", chroot_auth, xauth, chroot_auth);
            if (system(mnt_cmd) == 0) xauth_mounted = 1;
        }

        if (wayland) {
            char socket[1024], wayland_mnt[1024];
            snprintf(socket, sizeof(socket), "%s/%s", getenv("XDG_RUNTIME_DIR"), wayland);
            snprintf(wayland_mnt, sizeof(wayland_mnt), "%s/tmp/%s", target, wayland);
            char mnt_cmd[2048];
            snprintf(mnt_cmd, sizeof(mnt_cmd), "mountpoint -q %s || mount --bind %s %s", wayland_mnt, socket, wayland_mnt);
            system(mnt_cmd);
        }

        char full_cmd[4096];
        snprintf(full_cmd, sizeof(full_cmd), 
            "export DISPLAY=%s; export WAYLAND_DISPLAY=%s; export XDG_RUNTIME_DIR=/tmp; export QT_QPA_PLATFORM=wayland; export XAUTHORITY=/home/%s/.Xauthority; %s",
            display, wayland ? wayland : "", h_user, run_cmd ? run_cmd : "/bin/bash");

        char chroot_exec[5000];
        if (root) snprintf(chroot_exec, sizeof(chroot_exec), "chroot %s /bin/bash -c \"%s\"", target, full_cmd);
        else snprintf(chroot_exec, sizeof(chroot_exec), "chroot %s /bin/su - %s -c \"%s\"", target, h_user, full_cmd);
        
        system(chroot_exec);

        if (xauth_mounted) {
            char umnt[1024];
            snprintf(umnt, sizeof(umnt), "umount -l %s/home/%s/.Xauthority", target, h_user);
            system(umnt);
        }
        umount_subsystem(target);
    } else if (strcmp(command, "delete") == 0) {
        char *name = NULL;
        for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--name") == 0) name = argv[++i];
        if (!name) return 1;

        char target[1024];
        snprintf(target, sizeof(target), "%s/%s", BASE_DIR, name);
        umount_subsystem(target);
        remove_apps(name);
        char rm_cmd[1024];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", target);
        run(rm_cmd);
    }

    return 0;
}
