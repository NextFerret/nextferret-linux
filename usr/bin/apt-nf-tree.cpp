#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <curl/curl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <map>
#include <algorithm>
#include <set>

namespace fs = std::filesystem;

const std::string APT_NF_PATH = "/usr/share/apt-nf-tree/apt";
const std::string TREE_ROOT = "/nf-tree/apt-nf-tree/root";
const std::string BASE_URL = "https://nextferret.github.io";
const std::string DUR_BASE = "https://nextferretdur.github.io";
const std::string VERSION = "1.3r2";

std::string apt_bin;

struct DurInfo {
    std::string repo;
    std::string file;
    std::string version;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string fetch_url(const std::string& url) {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (res == CURLE_OK) return readBuffer;
    }
    return "";
}

bool download_file(const std::string& url, const std::string& path) {
    CURL* curl;
    FILE* fp;
    CURLcode res;
    curl = curl_easy_init();
    if (curl) {
        fp = fopen(path.c_str(), "wb");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        fclose(fp);
        return res == CURLE_OK;
    }
    return false;
}

int exec(const std::string& cmd, bool quiet = true) {
    std::string command = cmd;
    if (quiet) command += " > /dev/null 2>&1";
    return std::system(command.c_str());
}

std::string exec_out(const std::string& cmd) {
    std::string data;
    FILE* stream = popen(cmd.c_str(), "r");
    if (stream) {
        char buffer[256];
        while (!feof(stream)) {
            if (fgets(buffer, 256, stream) != NULL) data.append(buffer);
        }
        pclose(stream);
    }
    return data;
}

void mount_pseudo_fs() {
    exec("mount --bind /dev " + TREE_ROOT + "/dev");
    exec("mount --bind /dev/pts " + TREE_ROOT + "/dev/pts");
    exec("mount --bind /proc " + TREE_ROOT + "/proc");
    exec("mount --bind /sys " + TREE_ROOT + "/sys");
}

void umount_pseudo_fs() {
    std::vector<std::string> pts = {"/dev/pts", "/dev", "/proc", "/sys"};
    for (const auto& p : pts) {
        exec("umount -l " + TREE_ROOT + p);
    }
    usleep(500000);
}

void manage_sandbox(const std::string& action) {
    if (action == "create") {
        if (fs::exists(TREE_ROOT)) {
            umount_pseudo_fs();
            exec("btrfs subvolume delete -c " + TREE_ROOT);
        }
        fs::create_directories(fs::path(TREE_ROOT).parent_path());
        exec("btrfs subvolume snapshot / " + TREE_ROOT);
    } else if (action == "delete") {
        if (fs::exists(TREE_ROOT)) {
            umount_pseudo_fs();
            exec("btrfs subvolume delete -c " + TREE_ROOT);
        }
    }
}

void show_warning() {
    std::cout << "\n======================================================================" << std::endl;
    std::cout << "  ALERT: nf-tree disables swap during operation" << std::endl;
    std::cout << "  This can trigger OOM (Out Of Memory) killer!" << std::endl;
    std::cout << "  Close heavy applications if you have low RAM!" << std::endl;
    std::cout << "======================================================================\n" << std::endl;
}

bool ask_confirm(const std::string& prompt = "Apply this transaction to the REAL system? [y/N]: ") {
    std::cout << prompt;
    std::string ans;
    std::getline(std::cin, ans);
    std::transform(ans.begin(), ans.end(), ans.begin(), ::tolower);
    return (ans == "y" || ans == "yes");
}

bool is_package_already_installed(const std::string& pkg_name) {
    std::string out = exec_out("dpkg-query --show --showformat='${Status}' " + pkg_name + " 2>/dev/null");
    return out.find("install ok installed") != std::string::npos;
}

void cleanup_old_snapshots() {
    try {
        std::string out = exec_out("nf-tree list");
        std::vector<std::string> snapshots;
        std::stringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line[0] != '-') snapshots.push_back(line);
        }

        std::vector<std::pair<std::string, std::time_t>> date_map;
        std::regex date_regex("(\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2})");
        
        for (const auto& snap : snapshots) {
            std::smatch match;
            if (std::regex_search(snap, match, date_regex)) {
                std::tm tm = {};
                std::istringstream iss(match[1]);
                iss >> std::get_time(&tm, "%Y-%m-%d_%H-%M-%S");
                date_map.push_back({snap, std::mktime(&tm)});
            } else {
                date_map.push_back({snap, 0});
            }
        }

        std::sort(date_map.begin(), date_map.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

        int keep_count = 0, pre_count = 0, post_count = 0;
        std::set<std::string> to_delete;

        for (const auto& item : date_map) {
            const std::string& snap = item.first;
            bool is_pre = snap.find("host-pre") != std::string::npos;
            bool is_post = snap.find("host-post") != std::string::npos;

            if (keep_count < 12) {
                keep_count++;
                if (is_pre) pre_count++;
                if (is_post) post_count++;
            } else {
                if (is_pre && pre_count < 3) {
                    pre_count++;
                } else if (is_post && post_count < 3) {
                    post_count++;
                } else {
                    to_delete.insert(snap);
                }
            }
        }

        for (const auto& snap : to_delete) {
            exec("nf-tree remove " + snap);
        }
        if (!to_delete.empty()) {
            std::cout << "→ Cleaned " << to_delete.size() << " old snapshots" << std::endl;
        }
    } catch (...) {}
}

std::vector<std::string> create_host_snapshot(const std::string& base_name) {
    std::string out = exec_out("nf-tree create " + base_name);
    std::vector<std::string> snapshots;
    std::stringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("Created: ") == 0) {
            snapshots.push_back(line.substr(9));
        }
    }
    return snapshots;
}

void rollback_snapshots(const std::vector<std::string>& snapshot_list) {
    for (const auto& snap : snapshot_list) {
        exec("nf-tree rollback " + snap, false);
    }
}

std::string get_debian_version(const std::string& pkg) {
    std::string out = exec_out("apt-cache policy " + pkg);
    std::stringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("Candidate:") != std::string::npos) {
            size_t pos = line.find(':');
            std::string ver = line.substr(pos + 1);
            ver.erase(0, ver.find_first_not_of(" \t"));
            return ver.substr(0, ver.find_first_of(" \t\n"));
        }
    }
    return "";
}

std::map<std::string, DurInfo> get_dur_info() {
    std::string content = fetch_url(DUR_BASE + "/METADATA");
    std::map<std::string, DurInfo> dur_pkgs;
    if (content.empty()) return dur_pkgs;

    std::regex dur_regex("\"([^\"]+)\"\\s+Repo=(\\d+)\\s+.*File=([^ ]+\\.deb)\\s+Version=([0-9.]+)");
    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        std::smatch match;
        if (std::regex_search(line, match, dur_regex)) {
            dur_pkgs[match[1]] = {match[2], match[3], match[4]};
        }
    }
    return dur_pkgs;
}

void run_sandboxed(std::vector<std::string> args);

void install_from_dur_or_debian(const std::string& pkg_name) {
    if (is_package_already_installed(pkg_name)) {
        std::cout << pkg_name << " already installed → skipping" << std::endl;
        return;
    }
    auto dur_pkgs = get_dur_info();
    if (dur_pkgs.count(pkg_name)) {
        std::string debian_ver = get_debian_version(pkg_name);
        std::string dur_ver = dur_pkgs[pkg_name].version;
        if (debian_ver.empty() || dur_ver > debian_ver) {
            std::string url = DUR_BASE + "/repo" + dur_pkgs[pkg_name].repo + "/" + dur_pkgs[pkg_name].file;
            fs::create_directories("/tmp/nf-tree-debs");
            std::string path = "/tmp/nf-tree-debs/" + dur_pkgs[pkg_name].file;
            std::cout << "Fetching " << pkg_name << " from DUR (newer version)..." << std::endl;
            if (download_file(url, path)) {
                run_sandboxed({"install", path});
                return;
            }
        } else {
            std::cout << pkg_name << " using Debian repo (newer or equal)" << std::endl;
        }
    } else {
        std::cout << pkg_name << " not found in DUR → using Debian repo" << std::endl;
    }
    run_sandboxed({"install", pkg_name});
}

void run_direct(std::vector<std::string> args) {
    show_warning();
    std::cout << "\n================================================================================" << std::endl;
    std::cout << "  !!! --FORCE MODE ACTIVATED !!!" << std::endl;
    std::cout << "  Running DIRECTLY on host WITHOUT sandbox test." << std::endl;
    std::cout << "  This may result in an unstable or broken system!" << std::endl;
    std::cout << "  Use at your own risk!" << std::endl;
    std::cout << "================================================================================\n" << std::endl;

    if (!ask_confirm("Continue with --force? [y/N]: ")) {
        std::cout << "Cancelled by user." << std::endl;
        exit(0);
    }

    std::cout << ">> Host: creating pre-installation snapshot ..." << std::endl;
    auto pre_snaps = create_host_snapshot("apt-host-pre");
    
    std::string cmd = apt_bin;
    bool has_yes = false;
    for (const auto& a : args) {
        cmd += " " + a;
        if (a == "-y" || a == "--yes") has_yes = true;
    }
    if (!has_yes) cmd += " -y";

    if (exec(cmd, false) != 0) {
        std::cout << ">> Real apt FAILED! Rolling back..." << std::endl;
        rollback_snapshots(pre_snaps);
        std::cout << "Rollback initiated. REBOOT REQUIRED!" << std::endl;
        exit(1);
    }

    create_host_snapshot("apt-host-post");
    cleanup_old_snapshots();
}

void run_sandboxed(std::vector<std::string> args) {
    show_warning();
    if (!args.empty() && args[0] == "install" && args.size() > 1) {
        bool only_pkgs = true;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i][0] == '-' || args[i][0] == '/' || args[i].find(".deb") != std::string::npos) {
                only_pkgs = false; break;
            }
        }
        if (only_pkgs) {
            for (size_t i = 1; i < args.size(); ++i) install_from_dur_or_debian(args[i]);
            return;
        }
    }

    std::map<std::string, std::string> deb_map;
    for (const auto& arg : args) {
        if (arg.find(".deb") != std::string::npos && fs::exists(arg)) {
            deb_map[arg] = fs::path(arg).filename().string();
        }
    }

    manage_sandbox("create");
    mount_pseudo_fs();

    for (auto const& [host_path, base] : deb_map) {
        fs::copy_file(host_path, TREE_ROOT + "/tmp/" + base, fs::copy_options::overwrite_existing);
    }

    std::string chroot_cmd = "chroot " + TREE_ROOT + " " + apt_bin;
    bool has_yes = false;
    for (const auto& a : args) {
        if (deb_map.count(a)) chroot_cmd += " /tmp/" + deb_map[a];
        else chroot_cmd += " " + a;
        if (a == "-y" || a == "--yes") has_yes = true;
    }
    if (!has_yes) chroot_cmd += " -y";

    int res = exec(chroot_cmd);
    umount_pseudo_fs();

    if (res != 0) {
        std::cout << ">> Sandbox test: FAILED → no changes applied" << std::endl;
        manage_sandbox("delete");
        return;
    }

    std::cout << ">> Sandbox test: SUCCESS\n>> Transaction appears safe." << std::endl;
    if (!ask_confirm()) {
        manage_sandbox("delete");
        return;
    }

    std::cout << ">> Host: creating pre-installation snapshot ..." << std::endl;
    auto pre_snaps = create_host_snapshot("apt-host-pre");

    std::string real_cmd = apt_bin;
    for (const auto& a : args) real_cmd += " " + a;
    if (!has_yes) real_cmd += " -y";

    if (exec(real_cmd, false) != 0) {
        rollback_snapshots(pre_snaps);
        manage_sandbox("delete");
        exit(1);
    }

    create_host_snapshot("apt-host-post");
    manage_sandbox("delete");
    cleanup_old_snapshots();
}

void process_dur_packages() {
    auto dur_pkgs = get_dur_info();
    for (auto const& [pkg_name, info] : dur_pkgs) {
        std::string debian_ver = get_debian_version(pkg_name);
        if (debian_ver.empty() || info.version > debian_ver) {
            std::string url = DUR_BASE + "/repo" + info.repo + "/" + info.file;
            std::string path = "/tmp/nf-tree-debs/" + info.file;
            fs::create_directories("/tmp/nf-tree-debs");
            if (download_file(url, path)) run_sandboxed({"install", path});
        }
    }
}

void system_upgrade() {
    show_warning();
    if (fs::exists("/etc/apt/sources.list")) fs::copy_file("/etc/apt/sources.list", "/etc/apt/sources.list.nf-backup", fs::copy_options::overwrite_existing);
    
    std::string remote_codename = fetch_url(BASE_URL + "/debian_codename");
    remote_codename.erase(std::remove(remote_codename.begin(), remote_codename.end(), '\n'), remote_codename.end());

    if (remote_codename.length() > 2) {
        std::ofstream ofs("/etc/apt/sources.list");
        ofs << "deb http://deb.debian.org/debian " << remote_codename << " main contrib non-free non-free-firmware\n";
        ofs << "deb http://deb.debian.org/debian " << remote_codename << "-updates main contrib non-free non-free-firmware\n";
        ofs << "deb http://security.debian.org/debian-security " << remote_codename << "-security main contrib non-free non-free-firmware\n";
        ofs.close();
    }

    process_dur_packages();
    std::string to_remove = fetch_url(BASE_URL + "/packages-to-remove");
    if (!to_remove.empty()) {
        std::vector<std::string> rem_args = {"remove"};
        std::stringstream ss(to_remove);
        std::string p;
        while(ss >> p) rem_args.push_back(p);
        if (ask_confirm("Remove recommended packages? [y/N]: ")) run_sandboxed(rem_args);
    }
    run_sandboxed({"full-upgrade"});
}

void print_help() {
    std::cout << "Usage: apt-nf-tree <command> [options] [packages...] [--force]\n\n"
              << "Allowed commands:\n"
              << "  install      [packages]\n"
              << "  remove       [packages]\n"
              << "  autoremove\n"
              << "  update\n"
              << "  upgrade\n"
              << "  upgrade --system\n"
              << "  --system     (shortcut for upgrade --system)\n"
              << "  -v\n\n"
              << "--force : run directly on host without sandbox\n";
}

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);
    if (argc < 2) { print_help(); return 0; }

    apt_bin = (fs::exists(APT_NF_PATH) && (fs::status(APT_NF_PATH).permissions() & fs::perms::others_exec) != fs::perms::none) ? APT_NF_PATH : "apt";

    std::vector<std::string> raw_args;
    bool force_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--force") force_mode = true;
        else raw_args.push_back(a);
    }

    if (raw_args.empty()) { print_help(); return 0; }
    if (raw_args[0] == "-v") { std::cout << VERSION << std::endl; return 0; }
    
    if (getuid() != 0) { std::cout << "Need root." << std::endl; return 1; }

    if (raw_args[0] == "--system") { system_upgrade(); return 0; }

    std::string cmd = raw_args[0];
    std::vector<std::string> cmd_args(raw_args.begin() + 1, raw_args.end());

    if (cmd == "upgrade" && std::find(raw_args.begin(), raw_args.end(), "--system") != raw_args.end()) {
        system_upgrade();
    } else if (force_mode) {
        run_direct(raw_args);
    } else if (cmd == "update") {
        exec(apt_bin + " update", false);
    } else if (cmd == "install") {
        for (const auto& p : cmd_args) install_from_dur_or_debian(p);
    } else {
        run_sandboxed(raw_args);
    }

    curl_global_cleanup();
    return 0;
}
