#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <array>
#include <unistd.h>
#include <sys/stat.h>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace fs = std::filesystem;

const std::string SNAPSHOT_BASE = "/nf-tree/snapshots";
const std::string AUTO_DIR = SNAPSHOT_BASE + "/auto";
const std::string MANUAL_DIR = SNAPSHOT_BASE + "/manual";
const std::string ROOT_SOURCE = "/";
const std::string HOME_SOURCE = "/home";
const std::string SERVICE_PATH = "/etc/systemd/system/nf-tree.service";

struct CmdResult {
    int returncode;
    std::string stdout_data;
};

CmdResult run_cmd(const std::string& command) {
    CmdResult res{-1, ""};
    std::array<char, 256> buffer;
    std::string cmd = command + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return res;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        res.stdout_data += buffer.data();
    }
    int status = pclose(pipe);
    if (WIFEXITED(status)) {
        res.returncode = WEXITSTATUS(status);
    }
    return res;
}

void setup_dirs() {
    std::error_code ec;
    fs::create_directories(AUTO_DIR, ec);
    fs::create_directories(MANUAL_DIR, ec);
}

void update_grub() {
    system("update-grub > /dev/null 2>&1");
}

std::vector<std::string> split_string(const std::string& str) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string get_subvol_id(const std::string& path) {
    CmdResult res = run_cmd("btrfs subvolume show " + path);
    if (res.returncode == 0) {
        std::istringstream iss(res.stdout_data);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("Subvolume ID:") != std::string::npos) {
                std::vector<std::string> tokens = split_string(line);
                if (!tokens.empty()) {
                    return tokens.back();
                }
            }
        }
    }
    return "";
}

bool check_changes(const std::string& path, long long last_snapshot_time, bool is_root_path = false) {
    std::unordered_set<std::string> root_exclude = {"proc", "sys", "dev", "run", "tmp", "var", "mnt", "media", "boot"};
    std::unordered_set<std::string> general_exclude = {"nf-tree", ".snapshots", "swapfile", ".cache", "Downloads", "node_modules", ".local"};
    std::vector<std::string> dirs_to_scan = {path};

    while (!dirs_to_scan.empty()) {
        std::string current_dir = dirs_to_scan.back();
        dirs_to_scan.pop_back();

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(current_dir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) continue;
            if (entry.is_symlink(ec)) continue;

            std::string name = entry.path().filename().string();
            if (general_exclude.count(name)) continue;
            if (is_root_path && current_dir == path && root_exclude.count(name)) continue;

            if (entry.is_directory(ec)) {
                dirs_to_scan.push_back(entry.path().string());
            } else if (entry.is_regular_file(ec)) {
                struct stat attr;
                if (stat(entry.path().string().c_str(), &attr) == 0) {
                    if (attr.st_mtime > last_snapshot_time) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void toggle_swap(bool active) {
    if (!active) {
        system("swapoff -a > /dev/null 2>&1");
    } else {
        if (fs::exists("/dev/zram0")) {
            system("mkswap /dev/zram0 > /dev/null 2>&1");
            system("swapon /dev/zram0 -p 100 > /dev/null 2>&1");
        } else {
            system("swapon -a > /dev/null 2>&1");
        }
    }
}

bool starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

void cleanup_retention(const std::string& base_name) {
    std::vector<std::string> snapshots;
    std::error_code ec;
    if (!fs::exists(AUTO_DIR, ec)) return;

    for (const auto& entry : fs::directory_iterator(AUTO_DIR, ec)) {
        if (!ec && entry.is_directory(ec)) {
            std::string name = entry.path().filename().string();
            if (starts_with(name, base_name)) {
                snapshots.push_back(name);
            }
        }
    }
    std::sort(snapshots.begin(), snapshots.end());

    if (snapshots.size() > 6) {
        for (size_t i = 0; i < snapshots.size() - 6; ++i) {
            std::string target = AUTO_DIR + "/" + snapshots[i];
            CmdResult res = run_cmd("btrfs subvolume delete " + target);
            if (res.returncode == 0) {
                std::cout << "Auto-removed: " << snapshots[i] << "\n";
            }
        }
    }
}

bool create_snapshot(const std::string& source, const std::string& name, bool is_auto = false) {
    setup_dirs();
    
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d_%H-%M-%S");
    std::string timestamp = oss.str();
    
    std::string target_name = name + "-" + timestamp;
    std::string target_path = (is_auto ? AUTO_DIR : MANUAL_DIR) + "/" + target_name;

    if (fs::exists(target_path)) {
        return false;
    }

    bool is_root = (source == ROOT_SOURCE);
    if (is_root) {
        toggle_swap(false);
    }

    bool success = false;
    CmdResult res = run_cmd("btrfs subvolume snapshot " + source + " " + target_path);
    if (res.returncode == 0) {
        std::cout << "Created: " << target_name << "\n";
        if (is_auto) {
            cleanup_retention(name);
        }
        success = true;
    }

    if (is_root) {
        toggle_swap(true);
    }
    
    return success;
}

bool remove_snapshot(const std::string& name) {
    std::string p_auto = AUTO_DIR + "/" + name;
    std::string p_man = MANUAL_DIR + "/" + name;
    std::string target = fs::exists(p_man) ? p_man : (fs::exists(p_auto) ? p_auto : "");

    if (!target.empty()) {
        CmdResult res = run_cmd("btrfs subvolume delete " + target);
        if (res.returncode == 0) {
            std::cout << "Removed: " << name << "\n";
            return true;
        }
    }
    return false;
}

void list_snapshots() {
    setup_dirs();
    std::vector<std::pair<std::string, std::string>> dirs = {{"Manual", MANUAL_DIR}, {"Auto", AUTO_DIR}};
    
    for (const auto& pair : dirs) {
        std::cout << "\n--- " << pair.first << " Snapshots ---\n";
        if (fs::exists(pair.second)) {
            std::vector<std::string> items;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(pair.second, ec)) {
                if (!ec && entry.is_directory(ec)) {
                    items.push_back(entry.path().filename().string());
                }
            }
            std::sort(items.begin(), items.end());

            std::vector<std::string> root_snaps, home_snaps;
            for (const auto& i : items) {
                if (starts_with(i, "root-")) root_snaps.push_back(i);
                if (starts_with(i, "home-")) home_snaps.push_back(i);
            }

            std::cout << "  [Root]\n";
            if (root_snaps.empty()) {
                std::cout << "    (None)\n";
            } else {
                for (const auto& s : root_snaps) std::cout << "    " << s << "\n";
            }

            std::cout << "  [Home]\n";
            if (home_snaps.empty()) {
                std::cout << "    (None)\n";
            } else {
                for (const auto& s : home_snaps) std::cout << "    " << s << "\n";
            }
        }
    }
    std::cout << "\n";
}

bool rollback_snapshot(const std::string& name) {
    std::string p_auto = AUTO_DIR + "/" + name;
    std::string p_man = MANUAL_DIR + "/" + name;
    std::string snap_path = fs::exists(p_man) ? p_man : (fs::exists(p_auto) ? p_auto : "");

    if (snap_path.empty()) {
        std::cout << "Error: Snapshot " << name << " not found.\n";
        return false;
    }

    std::string target;
    if (starts_with(name, "root-")) target = ROOT_SOURCE;
    else if (starts_with(name, "home-")) target = HOME_SOURCE;

    if (target.empty()) {
        std::cout << "Error: Invalid target for rollback.\n";
        return false;
    }

    std::string subid = get_subvol_id(snap_path);
    if (subid.empty()) {
        std::cout << "Error: Could not retrieve subvolume ID.\n";
        return false;
    }

    std::cout << ">> Setting " << name << " (ID: " << subid << ") as default for " << target << "...\n";
    CmdResult res = run_cmd("btrfs subvolume set-default " + subid + " " + target);
    
    if (res.returncode == 0) {
        std::cout << "Success: " << name << " is now the default subvolume.\n";
        std::cout << "!! A REBOOT is required to apply the rollback !!\n";
        return true;
    }

    std::cout << "Rollback failed: " << res.stdout_data << "\n";
    return false;
}

std::string get_executable_path() {
    std::array<char, 1024> buf;
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::string(buf.data());
    }
    return "";
}

bool ensure_systemd_service() {
    if (fs::exists(SERVICE_PATH)) {
        return false;
    }

    std::string exec_path = get_executable_path();
    std::string content = "[Unit]\n"
                          "Description=NF-Tree Snapshot Daemon\n"
                          "After=local-fs.target time-sync.target\n\n"
                          "[Service]\n"
                          "Type=simple\n"
                          "ExecStart=" + exec_path + " start-daemon\n"
                          "Restart=always\n"
                          "RestartSec=60\n"
                          "User=root\n\n"
                          "[Install]\n"
                          "WantedBy=multi-user.target\n";
                          
    std::ofstream out(SERVICE_PATH);
    if (!out.is_open()) return false;
    out << content;
    out.close();

    system("systemctl daemon-reload > /dev/null 2>&1");
    system("systemctl enable --now nf-tree.service > /dev/null 2>&1");
    return true;
}

void daemon_loop() {
    auto last_run = std::chrono::system_clock::now();
    while (true) {
        auto current_time = std::chrono::system_clock::now();
        long long last_run_ts = std::chrono::system_clock::to_time_t(last_run);
        
        bool h_ch = check_changes(HOME_SOURCE, last_run_ts);
        bool r_ch = check_changes(ROOT_SOURCE, last_run_ts, true);

        if (h_ch || r_ch) {
            system("sync");
            bool upd = false;

            if (r_ch && create_snapshot(ROOT_SOURCE, "root-auto", true)) {
                upd = true;
            }
            if (h_ch && create_snapshot(HOME_SOURCE, "home-auto", true)) {
                upd = true;
            }

            if (upd) {
                update_grub();
            }
        }

        last_run = current_time;
        std::this_thread::sleep_for(std::chrono::seconds(10800));
    }
}

int main(int argc, char* argv[]) {
    if (geteuid() != 0) {
        return 1;
    }

    if (argc < 2) {
        return 0;
    }

    std::string command = argv[1];

    if (command == "create" && argc >= 3) {
        std::string name = argv[2];
        bool is_auto = starts_with(name, "apt") || name == "auto";
        bool res_root = create_snapshot(ROOT_SOURCE, "root-" + name, is_auto);
        bool res_home = create_snapshot(HOME_SOURCE, "home-" + name, is_auto);
        if (res_root || res_home) {
            update_grub();
        }
    } else if (command == "remove" && argc >= 3) {
        std::string name = argv[2];
        if (remove_snapshot(name)) {
            update_grub();
        }
    } else if (command == "rollback" && argc >= 3) {
        std::string name = argv[2];
        if (rollback_snapshot(name)) {
            update_grub();
        }
    } else if (command == "list") {
        list_snapshots();
    } else if (command == "start-daemon") {
        if (!ensure_systemd_service()) {
            daemon_loop();
        }
    }

    return 0;
}
