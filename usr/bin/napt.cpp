#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cerrno>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <utility>
#include <set>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <apt-pkg/init.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/depcache.h>
#include <apt-pkg/acquire.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/packagemanager.h>
#include <apt-pkg/pkgrecords.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/policy.h>
#include <apt-pkg/upgrade.h>
#include <apt-pkg/error.h>
#include <apt-pkg/version.h>

using namespace std;

namespace fs = std::filesystem;

const string TREE_ROOT = "/nsm/napt/root";
const string NF_TREE_BIN = "/usr/bin/nsm";
const string AUTO_SNAP_DIR = "/nsm/snapshots/auto";
const string NAPT_ETC_DIR = "/etc/napt";
const string NAPT_SOURCES_PATH = "/etc/napt/sources.list";
const string NAPT_CACHE_DIR = "/etc/napt/cache";
const string NAPT_ALLOWED_FILE = "/etc/napt/allowed";

struct NaptSource {
    string base_url;
    string release;
};

struct NaptRepoMetadata {
    string base_url;
    string release;
    map<string, pair<string, string>> packages;
    vector<string> required_packages;
};

struct NaptPackageCandidate {
    bool found = false;
    string base_url;
    string release;
    string file_name;
    string version;
    string sha256;
};

struct AptPackageState {
    bool found = false;
    bool installed = false;
    string installed_version;
    string candidate_version;
};

struct InstallDecision {
    string package_name;
    string apt_argument;
    string selected_version;
    bool from_napt = false;
};

void perform_install_transaction(const vector<string>& pkgs, bool apply_host);

bool nf_tree_available() {
    return access(NF_TREE_BIN.c_str(), X_OK) == 0;
}

void show_help() {
    cout << "New Advanced Packaging Tool - napt 2.0\n\n"
         << "Usage: napt [command] [options]\n\n"
         << "Commands:\n"
         << "  install          Installs packages or local .deb files in a chroot; applies to host only if successful.\n"
         << "  remove           Removes packages in a chroot; applies to host only if successful.\n"
         << "  sync             Updates repository metadata.\n"
         << "  upgrade          Upgrades all packages, or selected packages, using the chroot-first method.\n"
         << "  dist-upgrade     Full release upgrade\n"
         << "  purge            Removes packages and their configuration files.\n"
         << "  clean            Cleans the Napt download cache.\n\n"
         << "Options:\n"
         << "  --apply-host     Skip the chroot and apply changes directly to the host.\n"
         << "  --v              Show version information.\n"
         << "  --vb             Enable verbose logging for debugging libapt transactions.\n"
         << "  -h               Show this help message.\n\n"
         << "                 This napt Has Super Cow Powers.\n";
}

int run_cmd(const string& cmd) {
    int rc = system(cmd.c_str());
    if (rc == -1) return 1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 1;
}

bool create_snapshot(const string& name) {
    if (!nf_tree_available()) {
        return false;
    }
    return run_cmd(NF_TREE_BIN + " create " + name) == 0;
}

bool wait_for_child(pid_t pid, int& status) {
    status = 0;
    while (true) {
        pid_t ret = waitpid(pid, &status, 0);
        if (ret == pid) {
            return true;
        }
        if (ret == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

enum class PrecheckResult {
    Proceed,
    NoChanges,
    Failed
};

PrecheckResult precheck_transaction(const string& action, const vector<string>& pkgs, bool quiet) {
    if (action != "install" && action != "remove" && action != "purge") {
        return PrecheckResult::Proceed;
    }

    if (pkgs.empty()) {
        if (!quiet) {
            cout << "No packages were specified.\n";
        }
        return PrecheckResult::Failed;
    }

    pkgCacheFile cache_file;
    pkgCache* cache = cache_file.GetPkgCache();
    pkgDepCache* dep_cache = cache_file.GetDepCache();

    if (cache == nullptr || dep_cache == nullptr) {
        return PrecheckResult::Proceed;
    }

    bool has_changes = false;

    for (const auto& pkg_name : pkgs) {
        pkgCache::PkgIterator pkg = cache->FindPkg(pkg_name);
        if (pkg.end()) {
            if (!quiet) {
                cout << "Package " << pkg_name << " not found.\n";
            }
            return PrecheckResult::Failed;
        }

        if (action == "install") {
            pkgCache::VerIterator cand = dep_cache->GetCandidateVersion(pkg);
            if (pkg->CurrentVer != 0 && (cand.end() || cand == pkg.CurrentVer())) {
                if (!quiet) {
                    cout << pkg_name << " is already the newest version.\n";
                }
                continue;
            }
            has_changes = true;
            continue;
        }

        if (pkg->CurrentVer == 0) {
            if (!quiet) {
                cout << "Package " << pkg_name << " is not installed.\n";
            }
            continue;
        }

        has_changes = true;
    }

    if (!has_changes) {
        return PrecheckResult::NoChanges;
    }

    return PrecheckResult::Proceed;
}

string shell_quote(const string& s) {
    string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

string build_apt_cmd(const string& action, const vector<string>& pkgs, bool quiet) {
    string cmd = "DEBIAN_FRONTEND=noninteractive apt-get ";
    if (quiet) {
        cmd += "-qq ";
    }
    cmd += "-y ";

    if (action == "install" || action == "remove" || action == "purge") {
        if (pkgs.empty()) {
            return "";
        }
        cmd += action;
        for (const auto& p : pkgs) {
            cmd += " ";
            cmd += shell_quote(p);
        }
        return cmd;
    }

    if (action == "upgrade" || action == "dist-upgrade") {
        cmd += action;
        return cmd;
    }

    return "";
}

void mount_fs() {
    run_cmd("mount --bind /dev " + TREE_ROOT + "/dev >/dev/null 2>&1");
    run_cmd("mount --bind /dev/pts " + TREE_ROOT + "/dev/pts >/dev/null 2>&1");
    run_cmd("mount --bind /proc " + TREE_ROOT + "/proc >/dev/null 2>&1");
    run_cmd("mount --bind /sys " + TREE_ROOT + "/sys >/dev/null 2>&1");
}

void umount_fs() {
    run_cmd("umount -l " + TREE_ROOT + "/dev/pts >/dev/null 2>&1");
    run_cmd("umount -l " + TREE_ROOT + "/dev >/dev/null 2>&1");
    run_cmd("umount -l " + TREE_ROOT + "/proc >/dev/null 2>&1");
    run_cmd("umount -l " + TREE_ROOT + "/sys >/dev/null 2>&1");
}

static string get_btrfs_root_subvol_path() {
    FILE* pipe = popen("btrfs subvolume show / 2>/dev/null | grep 'Name:' | head -1 | awk '{print $2}'", "r");
    if (!pipe) return "";
    char buf[512];
    string name;
    if (fgets(buf, sizeof(buf), pipe)) {
        name = buf;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
            name.pop_back();
    }
    pclose(pipe);

    if (name.empty() || name == "/" || name == "<FS_TREE>") return "/";

    pipe = popen("btrfs subvolume show / 2>/dev/null | grep 'Path:' | head -1 | sed 's/.*Path:[[:space:]]*//'", "r");
    if (!pipe) return name;
    string path;
    if (fgets(buf, sizeof(buf), pipe)) {
        path = buf;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
            path.pop_back();
    }
    pclose(pipe);
    return path.empty() ? name : path;
}

static string get_root_btrfs_device() {
    FILE* pipe = popen("findmnt -n -o SOURCE / 2>/dev/null", "r");
    if (!pipe) return "";
    char buf[512];
    string dev;
    if (fgets(buf, sizeof(buf), pipe)) {
        dev = buf;
        while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r' || dev.back() == ' '))
            dev.pop_back();
    }
    pclose(pipe);
    size_t bracket = dev.find('[');
    if (bracket != string::npos) dev = dev.substr(0, bracket);
    return dev;
}

static const string BTRFS_TOP_MNT = "/nsm/napt/_btrfs_top";

static bool mount_btrfs_toplevel(const string& device) {
    run_cmd("mkdir -p " + shell_quote(BTRFS_TOP_MNT));
    int rc = run_cmd("mount -o subvolid=5 " + shell_quote(device) + " " + shell_quote(BTRFS_TOP_MNT) + " 2>/dev/null");
    return rc == 0;
}

static void umount_btrfs_toplevel() {
    run_cmd("umount " + shell_quote(BTRFS_TOP_MNT) + " 2>/dev/null");
    run_cmd("rmdir " + shell_quote(BTRFS_TOP_MNT) + " 2>/dev/null");
}

bool manage_sandbox(const string& action) {
    if (action == "create") {
        umount_fs();
        run_cmd("btrfs subvolume delete -c " + shell_quote(TREE_ROOT) + " >/dev/null 2>&1");
        run_cmd("mkdir -p /nsm/napt");

        string device = get_root_btrfs_device();
        if (device.empty()) {
            cout << "Error: could not determine root btrfs device.\n";
            return false;
        }

        if (!mount_btrfs_toplevel(device)) {
            cout << "Error: could not mount btrfs top-level subvolume from " << device << ".\n";
            return false;
        }

        string subvol_path = get_btrfs_root_subvol_path();
        string src = BTRFS_TOP_MNT;
        if (subvol_path != "/" && !subvol_path.empty()) {
            src = BTRFS_TOP_MNT + "/" + subvol_path;
        }

        int rc = run_cmd("btrfs subvolume snapshot " + shell_quote(src) + " " + shell_quote(TREE_ROOT) + " >/dev/null 2>&1");
        umount_btrfs_toplevel();

        if (rc != 0) {
            cout << "Error: btrfs snapshot of " << src << " -> " << TREE_ROOT << " failed (rc=" << rc << ").\n";
            return false;
        }

        struct stat st;
        if (stat(TREE_ROOT.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            cout << "Error: chroot root " << TREE_ROOT << " was not created.\n";
            return false;
        }

        run_cmd("mkdir -p " + shell_quote(TREE_ROOT + "/tmp"));
        return true;

    } else if (action == "delete") {
        umount_fs();
        run_cmd("btrfs subvolume delete -c " + shell_quote(TREE_ROOT) + " >/dev/null 2>&1");
        return true;
    }
    return false;
}

string get_latest_snapshot(const string& prefix) {
    DIR* dir = opendir(AUTO_SNAP_DIR.c_str());
    if (!dir) return "";
    struct dirent* entry;
    vector<string> matches;
    while ((entry = readdir(dir)) != NULL) {
        string name = entry->d_name;
        if (name.find(prefix) == 0) {
            matches.push_back(name);
        }
    }
    closedir(dir);
    if (matches.empty()) return "";
    sort(matches.begin(), matches.end());
    return matches.back();
}

void do_rollback(const string& prefix) {
    if (!nf_tree_available()) {
        return;
    }

    string root_snap = get_latest_snapshot("root-auto-" + prefix);
    if (!root_snap.empty()) {
        cout << "Rolling back root: " << root_snap << "\n";
        run_cmd(NF_TREE_BIN + " rollback " + root_snap);
    }
}

string fetch_url(const string& url) {
    string cmd = "curl -fsSL " + shell_quote(url);
    char buffer[512];
    string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }
    int rc = pclose(pipe);
    if (rc == -1) return "";
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) return result;
    return "";
}

string trim_copy(const string& s) {
    size_t start = s.find_first_not_of(" \n\r\t");
    if (start == string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \n\r\t");
    return s.substr(start, end - start + 1);
}

bool starts_with(const string& value, const string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const string& value, const string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool path_is_directory(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_is_regular_file(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

string path_basename(const string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

string normalize_napt_base_url(const string& raw_url) {
    string url = trim_copy(raw_url);
    if (url.find("http://") != 0 && url.find("https://") != 0) {
        url = "https://" + url;
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

bool is_repo_allowed(const string& url) {
#ifdef allowrepo
    if (normalize_napt_base_url(string(allowrepo)) == normalize_napt_base_url(url)) {
        return true;
    }
#endif

    ifstream in(NAPT_ALLOWED_FILE);
    if (!in) return false;
    string line;
    string norm_url = normalize_napt_base_url(url);
    while (getline(in, line)) {
        if (normalize_napt_base_url(line) == norm_url) {
            return true;
        }
    }
    return false;
}

void print_napt_repo_warning(const string& url) {
    if (is_repo_allowed(url)) {
        return;
    }

    static set<string> warned_repos;
    if (warned_repos.count(url)) {
        return;
    }
    warned_repos.insert(url);

    cout << " You are installing packages from an unauthorized third-party napt repository:\n"
         << " " << url << "\n"
         << " These packages are UNVERIFIED and could contain MALWARE, RANSOMWARE, or\n"
         << " utterly DESTROY your operating system.\n"
         << " ONLY PROCEED IF YOU ABSOLUTELY TRUST THE SOURCE!\n";
}

bool parse_napt_source_line(const string& raw_line, NaptSource& source) {
    string line = trim_copy(raw_line);
    if (line.empty() || line[0] == '#') {
        return false;
    }

    istringstream iss(line);
    string type;
    string base_url;
    string release;
    if (!(iss >> type >> base_url >> release)) {
        return false;
    }

    if (type != "deb") {
        return false;
    }

    base_url = normalize_napt_base_url(base_url);
    release = trim_copy(release);
    if (base_url.empty() || release.empty()) {
        return false;
    }

    source.base_url = base_url;
    source.release = release;
    return true;
}

void load_napt_sources_from_file(const string& path, vector<NaptSource>& sources) {
    ifstream in(path);
    if (!in) {
        return;
    }

    string line;
    while (getline(in, line)) {
        NaptSource source;
        if (parse_napt_source_line(line, source)) {
            sources.push_back(source);
        }
    }
}

vector<NaptSource> load_napt_sources() {
    vector<NaptSource> sources;
    if (path_is_regular_file(NAPT_SOURCES_PATH)) {
        load_napt_sources_from_file(NAPT_SOURCES_PATH, sources);
        return sources;
    }

    if (path_is_directory(NAPT_SOURCES_PATH)) {
        DIR* dir = opendir(NAPT_SOURCES_PATH.c_str());
        if (!dir) {
            return sources;
        }

        vector<string> files;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            string path = NAPT_SOURCES_PATH + "/" + name;
            if (path_is_regular_file(path)) {
                files.push_back(path);
            }
        }
        closedir(dir);

        sort(files.begin(), files.end());
        for (const auto& path : files) {
            load_napt_sources_from_file(path, sources);
        }
    }

    return sources;
}

bool write_text_file(const string& path, const string& content) {
    ofstream out(path);
    if (!out) {
        return false;
    }
    out << content;
    return out.good();
}

bool read_text_file(const string& path, string& content) {
    ifstream in(path);
    if (!in) {
        return false;
    }
    stringstream buffer;
    buffer << in.rdbuf();
    content = buffer.str();
    return true;
}

bool parse_napt_repo_metadata(const string& text, NaptRepoMetadata& metadata) {
    metadata.packages.clear();
    metadata.required_packages.clear();
    string line;
    bool in_packages = false;
    bool in_required = false;
    stringstream ss(text);
    while (getline(ss, line)) {
        string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed == "[napt repository]") {
            continue;
        }

        if (starts_with(trimmed, "release=")) {
            metadata.release = trim_copy(trimmed.substr(8));
            continue;
        }

        if (trimmed == "packages:") {
            in_packages = true;
            in_required = false;
            continue;
        }

        if (trimmed == "required:") {
            in_required = true;
            in_packages = false;
            continue;
        }

        if (in_required) {
            size_t start = trimmed.find('{');
            size_t end = trimmed.find('}');
            if (start != string::npos && end != string::npos && end > start) {
                string req_pkg = trim_copy(trimmed.substr(start + 1, end - start - 1));
                if (!req_pkg.empty()) {
                    metadata.required_packages.push_back(req_pkg);
                }
            }
            continue;
        }

        if (in_packages) {
            size_t pos = trimmed.find('=');
            if (pos == string::npos) {
                continue;
            }

            string pkg = trim_copy(trimmed.substr(0, pos));
            string rest = trim_copy(trimmed.substr(pos + 1));
            
            string file_name;
            string hash;

            size_t sha_pos = rest.find("sha256=");
            if (sha_pos != string::npos) {
                file_name = trim_copy(rest.substr(0, sha_pos));
                hash = trim_copy(rest.substr(sha_pos + 7));
            } else {
                file_name = rest;
            }

            if (!pkg.empty() && !file_name.empty()) {
                metadata.packages[pkg] = {file_name, hash};
            }
        }
    }

    return !metadata.release.empty();
}

bool sync_napt_metadata() {
    vector<NaptSource> sources = load_napt_sources();
    if (sources.empty()) {
        return true;
    }

    run_cmd("mkdir -p " + shell_quote(NAPT_ETC_DIR));

    bool ok = true;
    for (const auto& source : sources) {
        print_napt_repo_warning(source.base_url);
        string url = source.base_url + "/releases/" + source.release + "/repo-metadata";
        string metadata = fetch_url(url);
        if (metadata.empty()) {
            cout << "Failed to fetch metadata: " << url << "\n";
            ok = false;
            continue;
        }

        string release_dir = NAPT_ETC_DIR + "/" + source.release;
        if (run_cmd("mkdir -p " + shell_quote(release_dir)) != 0) {
            cout << "Failed to create metadata directory: " << release_dir << "\n";
            ok = false;
            continue;
        }

        string output_path = release_dir + "/repo-metadata";
        if (!write_text_file(output_path, metadata)) {
            cout << "Failed to write metadata: " << output_path << "\n";
            ok = false;
            continue;
        }

        cout << "Synced metadata for " << source.release << " from " << source.base_url << "\n";
    }

    return ok;
}

bool clean_napt_cache() {
    error_code ec;
    if (!fs::exists(NAPT_CACHE_DIR, ec)) {
        if (!fs::create_directories(NAPT_CACHE_DIR, ec)) {
            cout << "Failed to create cache directory: " << NAPT_CACHE_DIR << "\n";
            return false;
        }
        cout << "Cache is already clean.\n";
        return true;
    }

    bool removed_any = false;
    for (const auto& entry : fs::directory_iterator(NAPT_CACHE_DIR, ec)) {
        if (ec) {
            cout << "Failed to read cache directory: " << NAPT_CACHE_DIR << "\n";
            return false;
        }
        fs::remove_all(entry.path(), ec);
        if (ec) {
            cout << "Failed to remove: " << entry.path().string() << "\n";
            return false;
        }
        removed_any = true;
    }

    if (!fs::exists(NAPT_CACHE_DIR, ec) && !fs::create_directories(NAPT_CACHE_DIR, ec)) {
        cout << "Failed to recreate cache directory: " << NAPT_CACHE_DIR << "\n";
        return false;
    }

    if (removed_any) {
        cout << "Cache cleaned: " << NAPT_CACHE_DIR << "\n";
    } else {
        cout << "Cache is already clean.\n";
    }

    return true;
}

void print_install_already_present_message(const string& pkg_name) {
    cout << pkg_name << " is already installed. For upgrades, use napt upgrade "
         << pkg_name << " or just napt upgrade. If it is a huge package, use --apply-host.\n";
}

vector<NaptRepoMetadata> load_cached_napt_metadata() {
    vector<NaptRepoMetadata> repos;
    vector<NaptSource> sources = load_napt_sources();
    for (const auto& source : sources) {
        string path = NAPT_ETC_DIR + "/" + source.release + "/repo-metadata";
        string content;
        if (!read_text_file(path, content)) {
            continue;
        }

        NaptRepoMetadata metadata;
        metadata.base_url = source.base_url;
        metadata.release = source.release;
        if (!parse_napt_repo_metadata(content, metadata)) {
            continue;
        }
        if (metadata.release.empty()) {
            metadata.release = source.release;
        }
        repos.push_back(metadata);
    }

    return repos;
}

int compare_versions(const string& a, const string& b) {
    if (a.empty() && b.empty()) {
        return 0;
    }
    if (a.empty()) {
        return -1;
    }
    if (b.empty()) {
        return 1;
    }
    if (_system != nullptr && _system->VS != nullptr) {
        return _system->VS->CmpVersion(a.c_str(), b.c_str());
    }
    if (a == b) {
        return 0;
    }
    return a < b ? -1 : 1;
}

string extract_napt_version(const string& pkg_name, const string& file_name) {
    string base = path_basename(trim_copy(file_name));
    if (!ends_with(base, ".deb")) {
        return "";
    }

    string stem = base.substr(0, base.size() - 4);
    string rest;
    if (starts_with(stem, pkg_name + "_")) {
        rest = stem.substr(pkg_name.size() + 1);
    } else if (starts_with(stem, pkg_name + "-")) {
        rest = stem.substr(pkg_name.size() + 1);
    } else {
        return "";
    }

    size_t split = rest.find_last_of('_');
    if (split != string::npos && split > 0) {
        return rest.substr(0, split);
    }

    split = rest.find_last_of('-');
    if (split != string::npos && split > 0) {
        return rest.substr(0, split);
    }

    return rest;
}

AptPackageState get_apt_package_state(pkgCacheFile& cache_file, const string& pkg_name) {
    AptPackageState state;
    pkgCache* cache = cache_file.GetPkgCache();
    pkgDepCache* dep_cache = cache_file.GetDepCache();
    if (cache == nullptr || dep_cache == nullptr) {
        return state;
    }

    pkgCache::PkgIterator pkg = cache->FindPkg(pkg_name);
    if (pkg.end()) {
        return state;
    }

    state.found = true;
    if (pkg->CurrentVer != 0) {
        state.installed = true;
        state.installed_version = pkg.CurrentVer().VerStr();
    }

    pkgCache::VerIterator cand = dep_cache->GetCandidateVersion(pkg);
    if (!cand.end()) {
        state.candidate_version = cand.VerStr();
    }

    return state;
}

NaptPackageCandidate find_best_napt_candidate(const vector<NaptRepoMetadata>& repos, const string& pkg_name) {
    NaptPackageCandidate best;
    for (const auto& repo : repos) {
        auto it = repo.packages.find(pkg_name);
        if (it == repo.packages.end()) {
            continue;
        }

        NaptPackageCandidate candidate;
        candidate.found = true;
        candidate.base_url = repo.base_url;
        candidate.release = repo.release;
        candidate.file_name = it->second.first;
        candidate.sha256 = it->second.second;
        candidate.version = extract_napt_version(pkg_name, candidate.file_name);
        if (!best.found || compare_versions(candidate.version, best.version) > 0) {
            best = candidate;
        }
    }

    return best;
}

string build_napt_download_url(const NaptPackageCandidate& candidate) {
    string file_name = trim_copy(candidate.file_name);
    while (!file_name.empty() && file_name.front() == '/') {
        file_name.erase(file_name.begin());
    }
    return candidate.base_url + "/releases/" + candidate.release + "/" + file_name;
}

bool cache_napt_package(const NaptPackageCandidate& candidate, string& local_path) {
    string release_dir = NAPT_CACHE_DIR + "/" + candidate.release;
    if (run_cmd("mkdir -p " + shell_quote(release_dir)) != 0) {
        return false;
    }

    local_path = release_dir + "/" + path_basename(candidate.file_name);
    string url = build_napt_download_url(candidate);
    return run_cmd("curl -fsSL -o " + shell_quote(local_path) + " " + shell_quote(url)) == 0;
}

string calculate_sha256(const string& file_path) {
    string cmd = "sha256sum " + shell_quote(file_path);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[128];
    string result = "";
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result = buffer;
    }
    pclose(pipe);
    size_t space_pos = result.find(' ');
    if (space_pos != string::npos) {
        return result.substr(0, space_pos);
    }
    return trim_copy(result);
}

string build_apt_install_args_cmd(const vector<string>& args, bool quiet) {
    if (args.empty()) {
        return "";
    }

    string cmd = "DEBIAN_FRONTEND=noninteractive apt-get ";
    if (quiet) {
        cmd += "-qq ";
    }
    cmd += "-y install";

    for (const auto& arg : args) {
        cmd += " ";
        cmd += shell_quote(arg);
    }

    return cmd;
}

struct DebFileInfo {
    string package_name;
    string version;
    string architecture;
};

bool get_deb_file_info(const string& path, DebFileInfo& info) {
    string cmd = "dpkg-deb --field " + shell_quote(path) + " Package Version Architecture 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buffer[256];
    string output;
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        output += buffer;
    }
    int rc = pclose(pipe);
    if (rc == -1 || !(WIFEXITED(rc) && WEXITSTATUS(rc) == 0)) return false;

    istringstream ss(output);
    string line;
    while (getline(ss, line)) {
        size_t colon = line.find(':');
        if (colon == string::npos) continue;
        string key   = trim_copy(line.substr(0, colon));
        string value = trim_copy(line.substr(colon + 1));
        if (key == "Package")      info.package_name = value;
        else if (key == "Version") info.version      = value;
        else if (key == "Architecture") info.architecture = value;
    }
    return !info.package_name.empty();
}

bool resolve_install_decisions(const vector<string>& pkgs, vector<InstallDecision>& decisions, bool quiet) {
    if (pkgs.empty()) {
        if (!quiet) {
            cout << "No packages were specified.\n";
        }
        return false;
    }

    pkgCacheFile cache_file;
    vector<NaptRepoMetadata> repos = load_cached_napt_metadata();
    bool had_error = false;

    for (const auto& pkg_name : pkgs) {
        if (ends_with(pkg_name, ".deb")) {
            if (!path_is_regular_file(pkg_name)) {
                if (!quiet) {
                    cout << "Local .deb file not found: " << pkg_name << "\n";
                }
                had_error = true;
                continue;
            }

            DebFileInfo deb_info;
            bool has_info = get_deb_file_info(pkg_name, deb_info);

            if (!quiet) {
                cout << "Installing local .deb: ";
                if (has_info) {
                    cout << deb_info.package_name;
                    if (!deb_info.version.empty())      cout << " (" << deb_info.version << ")";
                    if (!deb_info.architecture.empty()) cout << " [" << deb_info.architecture << "]";
                } else {
                    cout << pkg_name;
                }
                cout << "\n";
            }

            InstallDecision decision;
            decision.package_name   = has_info ? deb_info.package_name : pkg_name;
            decision.apt_argument   = pkg_name;
            decision.selected_version = has_info ? deb_info.version : "";
            decision.from_napt      = false;
            decisions.push_back(decision);
            continue;
        }

        AptPackageState apt_state = get_apt_package_state(cache_file, pkg_name);
        NaptPackageCandidate napt_candidate = find_best_napt_candidate(repos, pkg_name);

        if (!apt_state.found && !napt_candidate.found) {
            if (!quiet) {
                cout << "Package " << pkg_name << " not found.\n";
            }
            had_error = true;
            continue;
        }

        bool use_napt = false;
        if (napt_candidate.found) {
            if (!apt_state.found || apt_state.candidate_version.empty()) {
                use_napt = true;
            } else if (compare_versions(napt_candidate.version, apt_state.candidate_version) > 0) {
                use_napt = true;
            }
        }

        if (use_napt) {
            print_napt_repo_warning(napt_candidate.base_url);
            if (apt_state.installed && compare_versions(apt_state.installed_version, napt_candidate.version) >= 0) {
                if (!quiet) {
                    print_install_already_present_message(pkg_name);
                }
                continue;
            }

            string local_path;
            if (!cache_napt_package(napt_candidate, local_path)) {
                if (!quiet) {
                    cout << "Failed to download Napt package for " << pkg_name << ".\n";
                }
                had_error = true;
                continue;
            }

            if (!napt_candidate.sha256.empty()) {
                string local_hash = calculate_sha256(local_path);
                if (local_hash != napt_candidate.sha256) {
                    if (!quiet) {
                        cout << "SHA256 checksum mismatch for " << pkg_name << ".\n"
                             << "Expected: " << napt_candidate.sha256 << "\n"
                             << "Got:      " << local_hash << "\n"
                             << "Aborting installation of this package.\n";
                    }
                    had_error = true;
                    run_cmd("rm -f " + shell_quote(local_path));
                    continue;
                }
            }

            InstallDecision decision;
            decision.package_name = pkg_name;
            decision.apt_argument = local_path;
            decision.selected_version = napt_candidate.version;
            decision.from_napt = true;
            decisions.push_back(decision);
            if (!quiet) {
                cout << "Using Napt package for " << pkg_name;
                if (!napt_candidate.version.empty()) {
                    cout << " (" << napt_candidate.version << ")";
                }
                cout << ".\n";
            }
            continue;
        }

        if (!apt_state.found || apt_state.candidate_version.empty()) {
            if (!quiet) {
                cout << "Package " << pkg_name << " not found.\n";
            }
            had_error = true;
            continue;
        }

        if (apt_state.installed && compare_versions(apt_state.installed_version, apt_state.candidate_version) >= 0) {
            if (!quiet) {
                print_install_already_present_message(pkg_name);
            }
            continue;
        }

        InstallDecision decision;
        decision.package_name = pkg_name;
        decision.apt_argument = pkg_name;
        decision.selected_version = apt_state.candidate_version;
        decision.from_napt = false;
        decisions.push_back(decision);

        if (!quiet) {
            cout << "Using Debian package for " << pkg_name;
            if (!apt_state.candidate_version.empty()) {
                cout << " (" << apt_state.candidate_version << ")";
            }
            cout << ".\n";
        }
    }

    return !had_error;
}

void do_nflinux_upgrade(bool apply_host) {
#ifdef nflinux
    string os_release = fetch_url("https://nextferret.github.io/etc/os-release");
    if (!os_release.empty()) {
        ofstream out("/etc/os-release");
        out << os_release;
        out.close();
    }

    string codenames = fetch_url("https://nextferret.github.io/version_codename");
    string repo_number_str = trim_copy(fetch_url("https://nextferret.github.io/repo-number"));
    
    if (!codenames.empty() && !repo_number_str.empty()) {
        size_t comma = codenames.find(',');
        if (comma != string::npos) {
            string napt_code = trim_copy(codenames.substr(0, comma));
            string debian_code = trim_copy(codenames.substr(comma + 1));
            
            string base_repo_url = "https://nextferretdur.github.io/repo-nflinux-" + repo_number_str;
            string meta_url = base_repo_url + "/releases/" + napt_code + "/repo-metadata";
            
            if (!fetch_url(meta_url).empty()) {
                run_cmd("rm -f /etc/napt/sources.list");
                write_text_file("/etc/napt/sources.list", "deb " + base_repo_url + " " + napt_code + "\n");
                
                string apt_sources = "deb http://deb.debian.org/debian " + debian_code + " main contrib non-free non-free-firmware\n";
                apt_sources += "deb http://deb.debian.org/debian-security " + debian_code + "-security main contrib non-free non-free-firmware\n";
                apt_sources += "deb http://deb.debian.org/debian " + debian_code + "-updates main contrib non-free non-free-firmware\n";
                write_text_file("/etc/apt/sources.list", apt_sources);
            }
        }
    }

    vector<NaptRepoMetadata> repos = load_cached_napt_metadata();
    vector<string> pkgs_to_install;
    for (const auto& repo : repos) {
        for (const auto& req : repo.required_packages) {
            pkgs_to_install.push_back(req);
        }
    }

    if (!pkgs_to_install.empty()) {
        sort(pkgs_to_install.begin(), pkgs_to_install.end());
        pkgs_to_install.erase(unique(pkgs_to_install.begin(), pkgs_to_install.end()), pkgs_to_install.end());
        
        cout << "Installing required packages from repositories...\n";
        perform_install_transaction(pkgs_to_install, apply_host);
    }
#endif
}

bool do_libapt_transaction(const string& action, const vector<string>& pkgs, bool quiet) {
    string cmd = build_apt_cmd(action, pkgs, quiet);
    if (cmd.empty()) {
        if (!quiet) {
            cout << "Invalid transaction request.\n";
        }
        return false;
    }
    return run_cmd(cmd) == 0;
}

bool do_command_transaction(const string& cmd) {
    if (cmd.empty()) {
        return false;
    }
    return run_cmd(cmd) == 0;
}

static const int PROGRESS_BAR_WIDTH = 18;

static string format_remaining(double seconds) {
    int s = static_cast<int>(seconds);
    if (s <= 0) return "0s";
    if (s < 60) return to_string(s) + "s";
    int m = s / 60;
    int r = s % 60;
    return to_string(m) + "m " + to_string(r) + "s";
}

static void render_chroot_bar(int filled, const string& time_str) {
    string bar(filled, '#');
    bar += string(PROGRESS_BAR_WIDTH - filled, ' ');
    string line = "\rTesting on the chroot                        ["
                  + bar + "] Estimated Time:" + time_str;
    line += string(max(0, 20 - static_cast<int>(time_str.size())), ' ');
    cout << line;
    cout.flush();
}

static string rewrite_cmd_stage_debs(const string& cmd, vector<string>& staged_host_paths) {
    string result;
    bool has_debs = false;
    size_t i = 0;
    while (i < cmd.size()) {
        if (cmd[i] == '\'') {
            size_t end = cmd.find('\'', i + 1);
            if (end == string::npos) { result += cmd.substr(i); break; }
            string token = cmd.substr(i + 1, end - i - 1);
            if (token.size() > 4 && token.substr(token.size() - 4) == ".deb") {
                has_debs = true;
                string filename = token.substr(token.find_last_of('/') + 1);
                string chroot_tmp_dir  = TREE_ROOT + "/tmp";
                string chroot_tmp_host = chroot_tmp_dir + "/" + filename;
                string chroot_tmp_inner = "/tmp/" + filename;
                run_cmd("mkdir -p " + shell_quote(chroot_tmp_dir));
                int rc = run_cmd("cp " + shell_quote(token) + " " + shell_quote(chroot_tmp_host));
                if (rc != 0) {
                    cout << "Error: failed to stage " << token << " into chroot tmp.\n";
                }
                staged_host_paths.push_back(chroot_tmp_host);
                result += "'" + chroot_tmp_inner + "'";
            } else {
                result += cmd.substr(i, end - i + 1);
            }
            i = end + 1;
        } else {
            result += cmd[i++];
        }
    }
    return result;
}

static void cleanup_staged_debs(const vector<string>& staged_host_paths) {
    for (const auto& p : staged_host_paths) {
        run_cmd("rm -f " + shell_quote(p));
    }
}

void perform_transaction_cmd(const string& transaction_cmd, bool apply_host) {
    if (!apply_host) {
        if (!manage_sandbox("create")) {
            cout << "Aborting transaction: chroot could not be created.\n";
            return;
        }
        mount_fs();

        int apt_pipe[2] = {-1, -1};
        bool have_pipe = (pipe(apt_pipe) == 0);

        int err_pipe[2] = {-1, -1};
        bool have_err_pipe = (pipe2(err_pipe, O_CLOEXEC) == 0);

        vector<string> staged_debs;
        string chroot_cmd = rewrite_cmd_stage_debs(transaction_cmd, staged_debs);

        pid_t pid = fork();
        if (pid == 0) {
            if (chroot(TREE_ROOT.c_str()) != 0 || chdir("/") != 0) {
                if (have_err_pipe) {
                    string msg = "chroot/chdir failed\n";
                    write(err_pipe[1], msg.c_str(), msg.size());
                }
                exit(1);
            }

            dup2(open("/dev/null", O_WRONLY | O_CLOEXEC), STDOUT_FILENO);

            if (have_err_pipe) {
                close(err_pipe[0]);
                if (err_pipe[1] != STDERR_FILENO) {
                    dup2(err_pipe[1], STDERR_FILENO);
                    close(err_pipe[1]);
                }
            } else {
                dup2(open("/dev/null", O_WRONLY | O_CLOEXEC), STDERR_FILENO);
            }

            if (have_pipe) {
                close(apt_pipe[0]);
                if (apt_pipe[1] != 3) {
                    dup2(apt_pipe[1], 3);
                    close(apt_pipe[1]);
                }
                fcntl(3, F_SETFD, 0);
            }

            pkgInitConfig(*_config);
            pkgInitSystem(*_config, _system);
            string cmd = chroot_cmd;
            if (have_pipe) cmd += " -o APT::Status-Fd=3";
            bool res = do_command_transaction(cmd);
            exit(res ? 0 : 1);

        } else if (pid > 0) {
            if (have_pipe)     close(apt_pipe[1]);
            if (have_err_pipe) close(err_pipe[1]);

            std::atomic<double> apt_percent(0.0);
            std::atomic<bool>   display_done(false);
            string child_stderr_output;

            std::thread stderr_reader([&]() {
                if (!have_err_pipe) return;
                char buf[256];
                ssize_t n;
                while ((n = read(err_pipe[0], buf, sizeof(buf) - 1)) > 0) {
                    buf[n] = '\0';
                    child_stderr_output += buf;
                }
                close(err_pipe[0]);
            });

            std::thread reader_thread([&]() {
                if (!have_pipe) return;
                FILE* f = fdopen(apt_pipe[0], "r");
                if (!f) { close(apt_pipe[0]); return; }
                char line[512];
                while (fgets(line, sizeof(line), f) != NULL) {
                    string s(line);
                    bool is_pm = (s.size() > 9 && s.substr(0, 9) == "pmstatus:");
                    bool is_dl = (!is_pm && s.size() > 9 && s.substr(0, 9) == "dlstatus:");
                    if (!is_pm && !is_dl) continue;
                    size_t c1 = s.find(':');
                    if (c1 == string::npos) continue;
                    size_t c2 = s.find(':', c1 + 1);
                    if (c2 == string::npos) continue;
                    size_t c3 = s.find(':', c2 + 1);
                    if (c3 == string::npos) continue;
                    string pct_str = s.substr(c2 + 1, c3 - c2 - 1);
                    try {
                        double pct = stod(pct_str);
                        if (pct > apt_percent.load()) {
                            apt_percent.store(pct);
                        }
                    } catch (...) {}
                }
                fclose(f);
            });

            std::thread display_thread([&]() {
                auto start = std::chrono::steady_clock::now();
                while (!display_done.load()) {
                    double pct = apt_percent.load();
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() / 1000.0;

                    int filled;
                    string time_str;

                    if (have_pipe) {
                        filled = static_cast<int>((pct / 100.0) * PROGRESS_BAR_WIDTH);
                        filled = min(filled, PROGRESS_BAR_WIDTH - 1);
                        if (pct > 2.0 && elapsed > 1.0) {
                            double remaining = elapsed * (100.0 - pct) / pct;
                            time_str = format_remaining(remaining);
                        } else {
                            time_str = "estimating...";
                        }
                    } else {
                        filled = min(PROGRESS_BAR_WIDTH - 1,
                                     static_cast<int>(elapsed / 120.0 * PROGRESS_BAR_WIDTH));
                        double remaining = max(0.0, 120.0 - elapsed);
                        time_str = format_remaining(remaining);
                    }

                    render_chroot_bar(filled, time_str);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            });

            int status = 0;
            bool waited = wait_for_child(pid, status);

            display_done.store(true);
            display_thread.join();
            reader_thread.join();
            stderr_reader.join();

            int filled_final;
            {
                double pct = apt_percent.load();
                filled_final = static_cast<int>((pct / 100.0) * PROGRESS_BAR_WIDTH);
                filled_final = min(filled_final, PROGRESS_BAR_WIDTH - 1);
            }

            umount_fs();
            cleanup_staged_debs(staged_debs);
            manage_sandbox("delete");

            if (!waited) {
                string bar(filled_final, '#');
                bar += string(PROGRESS_BAR_WIDTH - filled_final, ' ');
                cout << "\rTesting on the chroot                        ["
                     << bar << "]                           ...  chroot test interrupted.\n";
                return;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                string bar(filled_final, '#');
                bar += string(PROGRESS_BAR_WIDTH - filled_final, ' ');
                cout << "\rTesting on the chroot                        ["
                     << bar << "]                           ...  chroot test failed.\n";
                if (!child_stderr_output.empty()) {
                    cout << "\n--- chroot apt error ---\n" << child_stderr_output;
                    if (child_stderr_output.back() != '\n') cout << '\n';
                    cout << "------------------------\n";
                }
                return;
            }
            string full_bar(PROGRESS_BAR_WIDTH, '#');
            cout << "\rTesting on the chroot                        ["
                 << full_bar << "] Estimated Time:done          \n";
            cout << "Chroot test successful. Applying to host...\n";
            cout << "Do you want to apply the transaction to the host system?\n";
            cout << "This action cannot be undone without rollback if it fails.\n";
            cout << "Type 'yes' to confirm or anything else to abort: ";
            string confirm;
            getline(cin, confirm);
            if (confirm != "yes" && confirm != "YES") {
                cout << "Transaction aborted by user.\n";
                return;
            }
        } else {
            if (have_pipe)     { close(apt_pipe[0]); close(apt_pipe[1]); }
            if (have_err_pipe) { close(err_pipe[0]); close(err_pipe[1]); }
            cout << "Fork failed for chroot test.\n";
            return;
        }
    }

    bool snapshot_created = create_snapshot("apt-pre");
    bool res = do_command_transaction(transaction_cmd);
    if (res) {
        create_snapshot("apt-post");
        cout << "Transaction applied successfully.\n";
    } else {
        cout << "Host transaction failed. Rolling back...\n";
        if (snapshot_created) {
            do_rollback("apt-pre");
        }
    }
}

void perform_transaction(const string& action, const vector<string>& pkgs, bool apply_host) {
    PrecheckResult precheck = precheck_transaction(action, pkgs, false);
    if (precheck == PrecheckResult::Failed || precheck == PrecheckResult::NoChanges) {
        return;
    }

    string transaction_cmd = build_apt_cmd(action, pkgs, false);
    if (transaction_cmd.empty()) {
        cout << "Invalid transaction request.\n";
        return;
    }

    perform_transaction_cmd(transaction_cmd, apply_host);
}

void perform_install_transaction(const vector<string>& pkgs, bool apply_host) {
    vector<InstallDecision> decisions;
    if (!resolve_install_decisions(pkgs, decisions, false)) {
        return;
    }

    if (decisions.empty()) {
        return;
    }

    vector<string> args;
    for (const auto& decision : decisions) {
        args.push_back(decision.apt_argument);
    }

    string transaction_cmd = build_apt_install_args_cmd(args, false);
    if (transaction_cmd.empty()) {
        cout << "Invalid transaction request.\n";
        return;
    }

    perform_transaction_cmd(transaction_cmd, apply_host);
}

void perform_global_upgrade(bool apply_host) {
    pkgCacheFile cache_file;
    pkgCache* cache = cache_file.GetPkgCache();
    if (cache == nullptr) {
        return;
    }

    vector<NaptRepoMetadata> repos = load_cached_napt_metadata();
    vector<string> napt_upgrade_args;
    for (pkgCache::PkgIterator pkg = cache->PkgBegin(); !pkg.end(); ++pkg) {
        if (pkg->CurrentVer == 0) {
            continue;
        }

        string pkg_name = pkg.Name();
        AptPackageState apt_state = get_apt_package_state(cache_file, pkg_name);
        NaptPackageCandidate napt_candidate = find_best_napt_candidate(repos, pkg_name);

        if (napt_candidate.found && compare_versions(napt_candidate.version, apt_state.installed_version) > 0) {
            print_napt_repo_warning(napt_candidate.base_url);
            string local_path;
            if (cache_napt_package(napt_candidate, local_path)) {
                bool checksum_ok = true;
                if (!napt_candidate.sha256.empty()) {
                    string local_hash = calculate_sha256(local_path);
                    if (local_hash != napt_candidate.sha256) {
                        checksum_ok = false;
                        run_cmd("rm -f " + shell_quote(local_path));
                    }
                }
                if (checksum_ok) {
                    napt_upgrade_args.push_back(local_path);
                }
            }
        }
    }

    if (!napt_upgrade_args.empty()) {
        cout << "Upgrading NAPT packages first...\n";
        string napt_cmd = build_apt_install_args_cmd(napt_upgrade_args, false);
        perform_transaction_cmd(napt_cmd, apply_host);
    }

    cout << "Proceeding with standard apt upgrade...\n";
    perform_transaction("upgrade", vector<string>(), apply_host);
}

void perform_upgrade_transaction(const vector<string>& pkgs, bool apply_host) {
    if (pkgs.empty()) {
        perform_global_upgrade(apply_host);
        return;
    }

    perform_install_transaction(pkgs, apply_host);
}

int main(int argc, char** argv) {
    pkgInitConfig(*_config);
    pkgInitSystem(*_config, _system);
    string command;
    vector<string> pkgs;
    bool apply_host = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "-h") {
            show_help();
            return 0;
        } else if (arg == "--v") {
            cout << "napt 2.0\n";
            return 0;
        } else if (arg == "--vb") {
            _config->Set("Debug::pkgAcquire", "true");
        } else if (arg == "--apply-host") {
            apply_host = true;
        } else if (command.empty() && arg[0] != '-') {
            command = arg;
        } else if (arg[0] != '-') {
            pkgs.push_back(arg);
        }
    }

    if (command.empty()) {
        show_help();
        return 0;
    }

    if (geteuid() != 0) {
        cout << "Root privileges required.\n";
        return 1;
    }

    if (command == "sync") {
        int apt_rc = run_cmd("apt-get update");
        bool napt_ok = sync_napt_metadata();
        return (apt_rc == 0 && napt_ok) ? 0 : 1;
    } else if (command == "clean") {
        return clean_napt_cache() ? 0 : 1;
    } else if (command == "dist-upgrade") {
#ifdef nflinux
        do_nflinux_upgrade(apply_host);
#else
        perform_transaction("dist-upgrade", pkgs, apply_host);
#endif
    } else if (command == "install") {
        perform_install_transaction(pkgs, apply_host);
    } else if (command == "upgrade") {
        perform_upgrade_transaction(pkgs, apply_host);
    } else if (command == "remove" || command == "purge") {
        perform_transaction(command, pkgs, apply_host);
    } else {
        cout << "Unknown command: " << command << "\n";
        show_help();
        return 1;
    }

    return 0;
}
