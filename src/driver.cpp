#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
namespace {
struct Colors {
    bool enabled = false;
    std::string reset;
    std::string bold;
    std::string dim;
    std::string red;
    std::string green;
    std::string yellow;
    std::string blue;
    std::string magenta;
    std::string cyan;

    static Colors detect(bool isStderr = false) {
        Colors c;
#ifndef _WIN32
        int fd = isStderr ? STDERR_FILENO : STDOUT_FILENO;
        if ((isatty(fd) || std::getenv("LOOM_COLOR") != nullptr) && std::getenv("NO_COLOR") == nullptr) {
            c.enabled = true;
        }
#endif
        if (c.enabled) {
            c.reset   = "\033[0m";
            c.bold    = "\033[1m";
            c.dim     = "\033[2m";
            c.red     = "\033[1;31m";
            c.green   = "\033[1;32m";
            c.yellow  = "\033[1;33m";
            c.blue    = "\033[1;34m";
            c.magenta = "\033[1;35m";
            c.cyan    = "\033[1;36m";
        }
        return c;
    }
};

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct ChildFailure : std::runtime_error {
    int status;
    explicit ChildFailure(int value) : std::runtime_error("child process failed"), status(value ? value : 1) {}
};

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
inline fs::path getExecutablePath(const char* argv0 = nullptr) {
    fs::path exePath;
#if defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        exePath = fs::canonical(buf, ec);
    }
#elif defined(__linux__)
    std::error_code ec;
    exePath = fs::canonical("/proc/self/exe", ec);
#elif defined(_WIN32)
    char buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, buf, MAX_PATH) != 0) {
        std::error_code ec;
        exePath = fs::canonical(buf, ec);
    }
#endif
    if (exePath.empty() && argv0 && argv0[0] != '\0') {
        std::error_code ec;
        exePath = fs::absolute(argv0, ec);
    }
    return exePath;
}
std::string unquote(const std::string& value, std::size_t line) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        throw Error("config.toml line " + std::to_string(line) + ": expected a quoted string");
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\') {
            if (++i + 1 > value.size()) throw Error("config.toml line " + std::to_string(line) + ": invalid escape");
            switch (value[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                default: throw Error("config.toml line " + std::to_string(line) + ": unsupported string escape");
            }
        } else out += value[i];
    }
    return out;
}
std::vector<std::string> parseStringArray(const std::string& value, std::size_t line) {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        throw Error("config.toml line " + std::to_string(line) + ": expected an array of quoted strings");
    std::vector<std::string> values;
    std::size_t i = 1;
    while (i + 1 < value.size()) {
        while (i + 1 < value.size() && (std::isspace(static_cast<unsigned char>(value[i])) || value[i] == ',')) ++i;
        if (i + 1 >= value.size()) break;
        if (value[i] != '"') throw Error("config.toml line " + std::to_string(line) + ": expected a quoted array item");
        const std::size_t start = i++;
        bool escaped = false;
        for (; i < value.size() - 1; ++i) {
            if (!escaped && value[i] == '"') break;
            if (!escaped && value[i] == '\\') escaped = true;
            else escaped = false;
        }
        if (i >= value.size() - 1) throw Error("config.toml line " + std::to_string(line) + ": unterminated string");
        values.push_back(unquote(value.substr(start, i - start + 1), line));
        ++i;
        while (i + 1 < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        if (i + 1 < value.size() && value[i] != ',') throw Error("config.toml line " + std::to_string(line) + ": expected ',' between array items");
    }
    return values;
}
std::string stripComment(const std::string& s) {
    bool quoted = false, escaped = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!escaped && s[i] == '"') quoted = !quoted;
        if (!quoted && (s[i] == '#' || (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')))
            return s.substr(0, i);
        if (!escaped && s[i] == '\\') escaped = true; else escaped = false;
    }
    return s;
}

void packageUF2(const fs::path& binPath, const fs::path& uf2Path, uint32_t flashBase = 0x10000000) {
    std::ifstream in(binPath, std::ios::binary);
    if (!in) throw Error("cannot open binary file for UF2 packaging: " + binPath.string());
    std::vector<uint8_t> binData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const uint32_t payloadSize = 256;
    const uint32_t numBlocks = binData.empty() ? 1 : static_cast<uint32_t>((binData.size() + payloadSize - 1) / payloadSize);
    const uint32_t rp2040FamilyID = 0xE48FF566;

    std::ofstream out(uf2Path, std::ios::binary);
    if (!out) throw Error("cannot create UF2 file: " + uf2Path.string());

    for (uint32_t blockNo = 0; blockNo < numBlocks; ++blockNo) {
        uint8_t block[512] = {0};
        uint32_t targetAddr = flashBase + blockNo * payloadSize;
        uint32_t chunkStart = blockNo * payloadSize;
        uint32_t chunkSize = 0;
        if (chunkStart < binData.size()) {
            chunkSize = std::min(payloadSize, static_cast<uint32_t>(binData.size() - chunkStart));
        }

        auto writeU32 = [&](size_t offset, uint32_t val) {
            block[offset + 0] = static_cast<uint8_t>(val & 0xFF);
            block[offset + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            block[offset + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
            block[offset + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
        };

        writeU32(0, 0x0A324655);       // magicStart0
        writeU32(4, 0x9E5D5157);       // magicStart1
        writeU32(8, 0x00002000);       // flags: familyID present
        writeU32(12, targetAddr);      // targetAddr
        writeU32(16, payloadSize);     // payloadSize
        writeU32(20, blockNo);         // blockNo
        writeU32(24, numBlocks);       // numBlocks
        writeU32(28, rp2040FamilyID);  // familyID

        if (chunkSize > 0) {
            std::memcpy(block + 32, binData.data() + chunkStart, chunkSize);
        }

        writeU32(508, 0x0AB16F30);     // magicEnd

        out.write(reinterpret_cast<const char*>(block), 512);
    }
}

fs::path loomHome();
fs::path findObjcopy(const fs::path& toolchainRoot) {
    if (const char* envObjcopy = std::getenv("LLVM_OBJCOPY")) {
        if (*envObjcopy) return envObjcopy;
    }
    for (const auto& candidate : {
        toolchainRoot / "bin" / "llvm-objcopy",
        loomHome() / "toolchains" / "rp2040" / "bin" / "llvm-objcopy",
        fs::path("/opt/homebrew/opt/llvm/bin/llvm-objcopy"),
        fs::path("/usr/local/opt/llvm/bin/llvm-objcopy"),
        fs::path("/opt/homebrew/bin/llvm-objcopy"),
        fs::path("/usr/bin/llvm-objcopy")
    }) {
        if (fs::is_regular_file(candidate)) return candidate;
    }
    return "llvm-objcopy";
}

struct Config {
    std::string name;
    std::string main = "src/main.loom";
    std::vector<std::string> sources;
    std::string targetDir = "target";
    std::string executable;
    std::string clang = "clang";
    std::string toolchain;
    std::string target;
    std::vector<std::string> targets;
    std::string cpu;
    bool baremetal = false;
    std::string linkerScript;
    std::vector<std::string> assemblySources;
    std::vector<std::string> linkArgs;
    std::string emulator;
    std::vector<std::string> runArgs;
    bool uf2Output = false;
    std::string profile;
    std::unordered_map<std::string, std::string> dependencies;
};

Config readConfig(const fs::path& file) {
    std::ifstream in(file);
    if (!in) throw Error("cannot read " + file.string());
    Config c;
    std::string section, line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(stripComment(line));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) throw Error("config.toml line " + std::to_string(lineNo) + ": expected key = value");
        std::string key = trim(line.substr(0, eq));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::replace(key.begin(), key.end(), '-', '_');
        std::string value = trim(line.substr(eq + 1));
        if (!value.empty() && value.front() == '[' && value.back() != ']') {
            std::string extra;
            while (value.find(']') == std::string::npos && std::getline(in, extra)) {
                ++lineNo;
                value += " " + trim(stripComment(extra));
            }
        }
        if (section == "project" && key == "name") c.name = unquote(value, lineNo);
        else if (section == "project" && key == "version") (void)unquote(value, lineNo);
        else if (section == "build" && (key == "main" || key == "main_file")) c.main = unquote(value, lineNo);
        else if (section == "build" && key == "sources") c.sources = parseStringArray(value, lineNo);
        else if (section == "build" && key == "target_dir") c.targetDir = unquote(value, lineNo);
        else if (section == "build" && (key == "executable" || key == "output")) c.executable = unquote(value, lineNo);
        else if (section == "build" && (key == "targets" || key == "cross_targets")) c.targets = parseStringArray(value, lineNo);
        else if (section == "build" && key == "target") c.target = unquote(value, lineNo);
        else if (section == "build" && key == "cpu") c.cpu = unquote(value, lineNo);
        else if (section == "build" && (key == "profile" || key == "mode")) c.profile = unquote(value, lineNo);
        else if (section == "build" && (key == "opt_level" || key == "optlevel")) {
            std::string lvl = unquote(value, lineNo);
            if (lvl == "z" || lvl == "s") c.profile = "size";
            else if (lvl == "3") c.profile = "release";
        }
        else if (section.rfind("profile", 0) == 0) {
            if (key == "opt_level" || key == "optlevel") {
                std::string lvl = unquote(value, lineNo);
                if (lvl == "z" || lvl == "s") c.profile = "size";
                else if (lvl == "3") c.profile = "release";
            }
        }
        else if (section == "build" && (key == "baremetal" || key == "bare_metal")) c.baremetal = (value == "true");
        else if (section == "build" && (key == "linker_script" || key == "linker")) c.linkerScript = unquote(value, lineNo);
        else if (section == "build" && (key == "assembly_sources" || key == "assembly")) c.assemblySources = parseStringArray(value, lineNo);
        else if (section == "build" && (key == "link_args" || key == "linker_args")) c.linkArgs = parseStringArray(value, lineNo);
        else if (section == "build" && (key == "uf2_output" || key == "uf2")) c.uf2Output = (value == "true");
        else if (section == "run" && (key == "emulator" || key == "qemu")) c.emulator = unquote(value, lineNo);
        else if (section == "run" && key == "args") c.runArgs = parseStringArray(value, lineNo);
        else if (section == "toolchain" && (key == "name" || key == "toolchain")) c.toolchain = unquote(value, lineNo);
        else if (section == "toolchain" && key == "clang") c.clang = unquote(value, lineNo);
        else if (section == "dependencies") {
            if (!value.empty() && value.front() == '{' && value.back() == '}') {
                auto gitPos = value.find("git");
                if (gitPos != std::string::npos) {
                    auto eqPos = value.find('=', gitPos);
                    if (eqPos != std::string::npos) {
                        auto q1 = value.find('"', eqPos);
                        auto q2 = value.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos) {
                            c.dependencies[key] = value.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            } else {
                c.dependencies[key] = unquote(value, lineNo);
            }
        }
        else throw Error("config.toml line " + std::to_string(lineNo) + ": unknown key '" + key + "' in [" + section + "]");
    }
    if (c.name.empty()) c.name = file.parent_path().filename().string();
    if (c.executable.empty()) c.executable = c.name;
    if (c.name.empty() || c.main.empty() || c.targetDir.empty() || c.executable.empty()) throw Error("config.toml is missing a required project/build value");
    if (fs::path(c.executable).filename() != c.executable || c.executable == "." || c.executable == "..")
        throw Error("build.executable must be a file name, not a path");
    return c;
}
fs::path weakCanonical(const fs::path& p) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(p, ec);
    if (ec) throw Error("cannot resolve path '" + p.string() + "': " + ec.message());
    return result;
}
bool isWithin(const fs::path& child, const fs::path& parent) {
    auto relative = child.lexically_relative(parent);
    if (relative.empty() || relative.is_absolute()) return false;
    auto it = relative.begin();
    return it != relative.end() && *it != "..";
}
fs::path workspacePath(const fs::path& root, const std::string& value, const std::string& label) {
    fs::path path(value);
    if (path.is_absolute()) throw Error(label + " must be relative to the workspace");
    fs::path result = weakCanonical(root / path);
    if (!isWithin(result, root)) throw Error(label + " must stay inside the workspace");
    return result;
}
inline bool wildcardMatch(std::string_view pattern, std::string_view value) {
    std::size_t p = 0, v = 0;
    std::size_t starP = std::string_view::npos;
    std::size_t starV = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p; ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p;
            starV = v;
            ++p;
        } else if (starP != std::string_view::npos) {
            p = starP + 1;
            ++starV;
            v = starV;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
std::vector<fs::path> getSdkIncludeDirs(const char* argv0 = nullptr, const std::string& explicitSdk = "") {
    std::vector<fs::path> sdkRoots;
    if (!explicitSdk.empty()) {
        std::error_code ec;
        if (fs::is_directory(explicitSdk, ec)) {
            sdkRoots.push_back(weakCanonical(explicitSdk));
        }
    }
    for (const char* envName : {"LOOM_SDK_PATH", "LOOM_SDK"}) {
        if (const char* env = std::getenv(envName)) {
            std::error_code ec;
            if (fs::is_directory(env, ec)) {
                sdkRoots.push_back(weakCanonical(env));
            }
        }
    }
    fs::path exePath = getExecutablePath(argv0);
    if (!exePath.empty()) {
        fs::path exeDir = exePath.parent_path();
        for (const auto& rel : {
            exeDir.parent_path() / "share" / "loom" / "sdk",
            exeDir.parent_path() / "sdk",
            exeDir / "sdk"
        }) {
            std::error_code ec;
            if (fs::is_directory(rel, ec)) {
                sdkRoots.push_back(weakCanonical(rel));
            }
        }
    }
    if (const char* home = std::getenv("HOME")) {
        fs::path homeSdk = fs::path(home) / ".loom" / "sdk";
        std::error_code ec;
        if (fs::is_directory(homeSdk, ec)) {
            sdkRoots.push_back(weakCanonical(homeSdk));
        }
    }
    for (const auto& sys : {"/usr/local/share/loom/sdk", "/usr/share/loom/sdk", "/opt/loom/sdk"}) {
        std::error_code ec;
        if (fs::is_directory(sys, ec)) {
            sdkRoots.push_back(weakCanonical(sys));
        }
    }
    {
        std::filesystem::path cwdSdk = fs::current_path() / "sdk";
        std::error_code ec;
        if (fs::is_directory(cwdSdk, ec)) {
            sdkRoots.push_back(weakCanonical(cwdSdk));
        }
    }

    std::vector<fs::path> includeDirs;
    std::set<std::string> seen;
    auto addDir = [&](const fs::path& p) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            fs::path can = weakCanonical(p);
            std::string s = can.string();
            if (seen.insert(s).second) {
                includeDirs.push_back(can);
            }
        }
    };

    for (const auto& root : sdkRoots) {
        addDir(root / "std");
        addDir(root);
        std::error_code ec;
        if (fs::is_directory(root / "targets", ec)) {
            for (const auto& t : fs::directory_iterator(root / "targets", ec)) {
                if (t.is_directory(ec)) {
                    addDir(t.path() / "bsp");
                    addDir(t.path());
                }
            }
        }
    }
    // Libraries installed by `loom install` are user-scoped and are made
    // available to every project without copying files into the SDK.
    if (const char* home = std::getenv("HOME")) {
        const fs::path libraryRoot = fs::path(home) / ".loom" / "libraries";
        std::error_code ec;
        if (fs::is_directory(libraryRoot, ec)) {
            for (const auto& package : fs::directory_iterator(libraryRoot, ec)) {
                if (!package.is_directory(ec)) continue;
                addDir(package.path());
                addDir(package.path() / "src");
            }
        }
    }
    return includeDirs;
}

std::vector<fs::path> sourceFiles(const fs::path& root, const Config& c, const std::vector<fs::path>& sdkIncludeDirs = {}) {
    std::vector<fs::path> files;
    std::set<std::string> knownModules;
    auto recordModule = [&](const fs::path& p) {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(stripComment(line));
            if (line.rfind("Module ", 0) == 0 || line.rfind("module ", 0) == 0) {
                std::string mod = trim(line.substr(7));
                if (!mod.empty()) {
                    knownModules.insert(toLower(mod));
                }
                break;
            }
        }
    };
    auto add = [&](const fs::path& f) {
        if (!fs::is_regular_file(f)) throw Error("source file not found: " + f.string());
        if (f.extension() != ".loom") throw Error("source file must have the .loom extension: " + f.string());
        const auto resolved = weakCanonical(f);
        if (!isWithin(resolved, root)) throw Error("source file resolves outside the workspace: " + f.string());
        files.push_back(resolved);
        recordModule(resolved);
    };
    auto addResolved = [&](const fs::path& f) {
        if (!fs::is_regular_file(f)) return;
        if (f.extension() != ".loom") return;
        const auto resolved = weakCanonical(f);
        files.push_back(resolved);
        recordModule(resolved);
    };
    if (c.sources.empty()) {
        const auto src = root / "src";
        if (!fs::is_directory(src)) throw Error("source directory not found: " + src.string());
        for (const auto& entry : fs::recursive_directory_iterator(src)) if (entry.is_regular_file() && entry.path().extension() == ".loom") add(entry.path());
    } else {
        for (const auto& pattern : c.sources) {
            fs::path rel(pattern);
            if (rel.is_absolute()) throw Error("build.sources entries must be relative to the workspace");
            const auto wildcard = rel.filename().string().find_first_of("*?");
            if (wildcard == std::string::npos) add(workspacePath(root, pattern, "source path"));
            else {
                if (rel.parent_path().string().find_first_of("*?") != std::string::npos) throw Error("wildcards are supported only in the source file name (for example src/*.loom)");
                const fs::path dir = workspacePath(root, (rel.parent_path().empty() ? fs::path(".") : rel.parent_path()).string(), "source directory");
                if (fs::is_directory(dir)) {
                    for (const auto& entry : fs::directory_iterator(dir))
                        if (entry.is_regular_file() && wildcardMatch(rel.filename().string(), entry.path().filename().string())) add(entry.path());
                } else if (rel.parent_path() == "src") {
                    throw Error("source directory not found: " + dir.string());
                }
            }
        }
    }
    std::size_t scanIdx = 0;
    while (scanIdx < files.size()) {
        const auto cur = files[scanIdx++];
        std::ifstream in(cur);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(stripComment(line));
            if (line.rfind("Import ", 0) == 0 || line.rfind("import ", 0) == 0 ||
                line.rfind("Use ", 0) == 0 || line.rfind("use ", 0) == 0) {
                std::string mod;
                if (line.rfind("Import ", 0) == 0 || line.rfind("import ", 0) == 0)
                    mod = trim(line.substr(7));
                else
                    mod = trim(line.substr(4));
                if (!mod.empty()) {
                    const std::string lmod = toLower(mod);
                    if (knownModules.count(lmod)) continue;
                    auto cand1 = cur.parent_path() / (mod + ".loom");
                    auto cand1b = cur.parent_path() / (lmod + ".loom");
                    auto cand2 = root / "src" / (mod + ".loom");
                    auto cand2b = root / "src" / (lmod + ".loom");
                    auto cand3 = root / (mod + ".loom");
                    auto cand3b = root / (lmod + ".loom");
                    auto cand4 = root / "src" / "bsp" / (mod + ".loom");
                    auto cand4b = root / "src" / "bsp" / (lmod + ".loom");
                    if (fs::is_regular_file(cand1)) addResolved(cand1);
                    else if (fs::is_regular_file(cand1b)) addResolved(cand1b);
                    else if (fs::is_regular_file(cand2)) addResolved(cand2);
                    else if (fs::is_regular_file(cand2b)) addResolved(cand2b);
                    else if (fs::is_regular_file(cand4)) addResolved(cand4);
                    else if (fs::is_regular_file(cand4b)) addResolved(cand4b);
                    else if (fs::is_regular_file(cand3)) addResolved(cand3);
                    else if (fs::is_regular_file(cand3b)) addResolved(cand3b);
                    else {
                        bool found = false;
                        std::error_code ec;
                        if (fs::is_directory(root / "src", ec)) {
                            for (const auto& entry : fs::directory_iterator(root / "src", ec)) {
                                if (entry.is_directory(ec)) {
                                    auto sc1 = entry.path() / (mod + ".loom");
                                    auto sc2 = entry.path() / (lmod + ".loom");
                                    if (fs::is_regular_file(sc1)) { addResolved(sc1); found = true; break; }
                                    if (fs::is_regular_file(sc2)) { addResolved(sc2); found = true; break; }
                                }
                            }
                        }
                        if (!found) {
                            for (const auto& sdkDir : sdkIncludeDirs) {
                                auto sc1 = sdkDir / (mod + ".loom");
                                auto sc2 = sdkDir / (lmod + ".loom");
                                if (fs::is_regular_file(sc1)) { addResolved(sc1); break; }
                                if (fs::is_regular_file(sc2)) { addResolved(sc2); break; }
                            }
                        }
                    }
                }
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty()) throw Error("no .loom source files found");
    const auto main = workspacePath(root, c.main, "build.main");
    if (std::find(files.begin(), files.end(), main) == files.end()) throw Error("build.main must also be included in build.sources");
    return files;
}
int runCommand(const std::vector<std::string>& args);
int runCommandQuiet(const std::vector<std::string>& args);
struct BlockNode { std::size_t source; std::string name; std::size_t line = 1; };
struct VariableNode { std::string id, owner, name, type, kind; std::size_t source = 0, line = 1; };
struct CallEdge { std::string caller, callee; std::size_t source = 0, line = 1; };
struct AccessEdge { std::string owner, variable, relation; std::size_t source = 0, line = 1; };
struct DependencyModel {
    std::vector<BlockNode> blocks;
    std::vector<VariableNode> variables;
    std::vector<CallEdge> calls;
    std::vector<AccessEdge> accesses;
};
std::string mermaidLabel(std::string value) {
    std::string out;
    for (char c : value) {
        if (c == '"') out += "&quot;";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}
DependencyModel analyzeDependencies(const std::vector<fs::path>& sources, const fs::path& compiler) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path dataPath = fs::temp_directory_path() / ("loom-deps-" + std::to_string(stamp) + ".tsv");
    std::vector<std::string> command{compiler.string(), "--graph-data", "--output", dataPath.string()};
    for (const auto& source : sources) command.push_back(source.string());
    const int status = runCommand(command);
    if (status != 0) {
        std::error_code ignored; fs::remove(dataPath, ignored);
        throw ChildFailure(status);
    }
    DependencyModel model;
    std::ifstream input(dataPath);
    std::error_code ignored; fs::remove(dataPath, ignored);
    if (!input) throw Error("compiler did not produce dependency analysis data");
    std::string row;
    std::vector<CallEdge> callRows;
    std::vector<AccessEdge> accessRows;
    std::vector<VariableNode> declarations;
    std::map<std::string, std::size_t> declarationKeys;
    while (std::getline(input, row)) {
        std::vector<std::string> columns;
        std::stringstream parser(row);
        std::string cell;
        while (std::getline(parser, cell, '\t')) columns.push_back(cell);
        if (columns.empty()) continue;
        if (columns[0] == "BLOCK" && columns.size() == 4) {
            const auto index = static_cast<std::size_t>(std::stoull(columns[1]));
            if (index >= sources.size()) throw Error("compiler returned an invalid source index during dependency analysis");
            model.blocks.push_back({index, columns[2], static_cast<std::size_t>(std::stoull(columns[3]))});
        } else if (columns[0] == "GLOBAL" && columns.size() == 5) {
            const auto index = static_cast<std::size_t>(std::stoull(columns[1]));
            if (index >= sources.size()) throw Error("compiler returned an invalid global source index");
            VariableNode v; v.source = index; v.name = columns[2]; v.type = columns[3]; v.kind = "global"; v.line = std::stoull(columns[4]);
            declarationKeys.emplace("@" + v.name, declarations.size()); declarations.push_back(std::move(v));
        } else if (columns[0] == "LOCAL" && columns.size() == 7) {
            const auto index = static_cast<std::size_t>(std::stoull(columns[1]));
            if (index >= sources.size()) throw Error("compiler returned an invalid variable source index");
            VariableNode v; v.source = index; v.owner = columns[2]; v.name = columns[3]; v.type = columns[4]; v.kind = columns[5];
            v.line = std::stoull(columns[6]);
            declarationKeys.emplace(v.owner + "::" + v.name, declarations.size()); declarations.push_back(std::move(v));
        } else if (columns[0] == "CALL" && columns.size() == 5) {
            const auto index = static_cast<std::size_t>(std::stoull(columns[1]));
            if (index >= sources.size()) throw Error("compiler returned an invalid call source index during dependency analysis");
            callRows.push_back({columns[2], columns[3], index, std::stoull(columns[4])});
        } else if ((columns[0] == "READ" || columns[0] == "WRITE") && columns.size() == 5) {
            const auto index = static_cast<std::size_t>(std::stoull(columns[1]));
            if (index >= sources.size()) throw Error("compiler returned an invalid access source index");
            accessRows.push_back({columns[2], columns[3], columns[0] == "READ" ? "reads" : "writes", index, std::stoull(columns[4])});
        } else throw Error("compiler returned malformed dependency analysis data");
    }
    std::unordered_map<std::string, std::size_t> blockByName;
    for (std::size_t i = 0; i < model.blocks.size(); ++i) blockByName.emplace(model.blocks[i].name, i);
    std::map<std::pair<std::string, std::string>, CallEdge> uniqueCalls;
    for (const auto& call : callRows) {
        const auto& caller = call.caller;
        const auto& callee = call.callee;
        if (!blockByName.count(caller))
            throw Error("compiler dependency data refers to an unresolved block");
        if (!blockByName.count(callee)) continue;
        auto [it, inserted] = uniqueCalls.emplace(std::make_pair(caller, callee), call);
        if (!inserted && call.line < it->second.line) it->second = call;
    }
    for (const auto& edge : uniqueCalls) model.calls.push_back(edge.second);
    std::map<std::string, std::string> variableIds;
    for (auto& variable : declarations) {
        variable.id = "v" + std::to_string(model.variables.size());
        const std::string key = variable.kind == "global" ? "@" + variable.name : variable.owner + "::" + variable.name;
        variableIds.emplace(key, variable.id);
        model.variables.push_back(variable);
    }
    std::map<std::tuple<std::string, std::string, std::string>, std::pair<std::size_t, std::size_t>> uniqueAccesses;
    for (const auto& access : accessRows) {
        if (!blockByName.count(access.owner)) throw Error("compiler dependency data refers to an unresolved block");
        std::string key;
        const auto localKey = access.owner + "::" + access.variable;
        if (variableIds.count(localKey)) key = localKey;
        else if (variableIds.count("@" + access.variable)) key = "@" + access.variable;
        else if (access.variable.find('.') != std::string::npos) {
            key = access.owner + "::field::" + access.variable;
            if (!variableIds.count(key)) {
                VariableNode v; v.id = "v" + std::to_string(model.variables.size()); v.owner = access.owner;
                v.source = access.source; v.line = access.line;
                v.name = access.variable; v.type = "field access"; v.kind = "field-access";
                variableIds.emplace(key, v.id); model.variables.push_back(std::move(v));
            }
        } else throw Error("compiler dependency data refers to an undeclared variable '" + access.variable + "'");
        const auto edgeKey = std::make_tuple(access.owner, variableIds.at(key), access.relation);
        auto [it, inserted] = uniqueAccesses.emplace(edgeKey, std::make_pair(access.source, access.line));
        if (!inserted && access.line < it->second.second) it->second = {access.source, access.line};
    }
    for (const auto& edge : uniqueAccesses)
        model.accesses.push_back({std::get<0>(edge.first), std::get<1>(edge.first), std::get<2>(edge.first), edge.second.first, edge.second.second});
    return model;
}
std::string mermaidGraph(const DependencyModel& model, const std::string& view) {
    std::ostringstream out;
    out << "flowchart TD\n";
    std::unordered_map<std::string, std::size_t> blockIds;
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        blockIds.emplace(model.blocks[i].name, i);
        out << "  b" << i << "[\"" << mermaidLabel(model.blocks[i].name) << "\"]\n";
    }
    if (view == "calls") {
        for (const auto& edge : model.calls)
            out << "  b" << blockIds.at(edge.caller) << " -->|calls| b" << blockIds.at(edge.callee) << "\n";
    } else {
        for (const auto& variable : model.variables)
            out << "  " << variable.id << "[\"" << mermaidLabel(variable.name + " : " + variable.type) << "\"]\n";
        std::map<std::tuple<std::string, std::string, std::string>, bool> emitted;
        for (const auto& edge : model.accesses) {
            auto key = std::make_tuple(edge.owner, edge.variable, edge.relation);
            if (!emitted.emplace(key, true).second) continue;
            const auto block = blockIds.at(edge.owner);
            if (edge.relation == "reads") out << "  b" << block << " -->|reads| " << edge.variable << "\n";
            else out << "  " << edge.variable << " -->|writes| b" << block << "\n";
        }
    }
    return out.str();
}
std::string jsonString(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 0x20) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
        else out << static_cast<char>(c);
    }
    out << '"'; return out.str();
}
std::string jsonGraph(const fs::path& root, const std::vector<fs::path>& sources, const DependencyModel& model, const std::string& view) {
    std::ostringstream out; out << "{\n  \"schemaVersion\": 1,\n  \"view\": " << jsonString(view) << ",\n  \"sourceFiles\": [";
    for (std::size_t i = 0; i < sources.size(); ++i) out << (i ? ", " : "") << jsonString(sources[i].lexically_relative(root).generic_string());
    out << "],\n  \"diagnostics\": [],\n  \"nodes\": [\n";
    bool first = true;
    auto node = [&](const std::string& id, const std::string& kind, const std::string& name, std::size_t source, std::size_t line) {
        if (!first) out << ",\n"; first = false;
        out << "    {\"id\":" << jsonString(id) << ",\"kind\":" << jsonString(kind) << ",\"name\":" << jsonString(name)
            << ",\"source\":" << source << ",\"line\":" << line << "}";
    };
    for (std::size_t i = 0; i < model.blocks.size(); ++i) node("b" + std::to_string(i), "block", model.blocks[i].name, model.blocks[i].source, model.blocks[i].line);
    if (view == "data") for (const auto& v : model.variables) node(v.id, v.kind, v.name + " : " + v.type, v.source, v.line);
    out << "\n  ],\n  \"edges\": [\n";
    first = true;
    auto edge = [&](const std::string& from, const std::string& to, const std::string& kind, std::size_t source, std::size_t line) {
        if (!first) out << ",\n"; first = false;
        out << "    {\"from\":" << jsonString(from) << ",\"to\":" << jsonString(to) << ",\"kind\":" << jsonString(kind)
            << ",\"source\":" << source << ",\"line\":" << line << "}";
    };
    std::unordered_map<std::string, std::size_t> ids;
    for (std::size_t i = 0; i < model.blocks.size(); ++i) ids.emplace(model.blocks[i].name, i);
    if (view == "calls") for (const auto& e : model.calls) edge("b" + std::to_string(ids.at(e.caller)), "b" + std::to_string(ids.at(e.callee)), "calls", e.source, e.line);
    if (view == "data") for (const auto& e : model.accesses) edge(e.relation == "reads" ? "b" + std::to_string(ids.at(e.owner)) : e.variable,
        e.relation == "reads" ? e.variable : "b" + std::to_string(ids.at(e.owner)), e.relation, e.source, e.line);
    out << "\n  ]\n}\n"; return out.str();
}
void writeGraph(const fs::path& output, const std::string& contents) {
    if (!output.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        if (ec) throw Error("cannot create parent directory for Mermaid graph '" + output.string() + "': " + ec.message());
    }
    std::ofstream out(output, std::ios::binary);
    if (!out) throw Error("cannot write Mermaid graph '" + output.string() + "'");
    out << contents;
    if (!out) throw Error("failed while writing Mermaid graph '" + output.string() + "'");
}
std::string shellQuote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char ch : s) { if (ch == '"') out += "\\\""; else out += ch; }
    return out + "\"";
#else
    std::string out = "'";
    for (char ch : s) { if (ch == '\'') out += "'\\''"; else out += ch; }
    return out + "'";
#endif
}
int runCommand(const std::vector<std::string>& args) {
    std::string command;
    for (const auto& arg : args) { if (!command.empty()) command += ' '; command += shellQuote(arg); }
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    if (status == -1) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}
int runCommandQuiet(const std::vector<std::string>& args) {
    std::string command;
    for (const auto& arg : args) { if (!command.empty()) command += ' '; command += shellQuote(arg); }
#ifdef _WIN32
    command += " > nul 2>&1";
#else
    command += " > /dev/null 2>&1";
#endif
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    if (status == -1) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}
int runCommandToFile(const std::vector<std::string>& args, const fs::path& output) {
    std::string command;
    for (const auto& arg : args) { if (!command.empty()) command += ' '; command += shellQuote(arg); }
    command += " > " + shellQuote(output.string());
    const int status = std::system(command.c_str());
#ifdef _WIN32
    return status;
#else
    if (status == -1) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}
fs::path loomcPath(const char* argv0) {
    if (const char* overridePath = std::getenv("LOOMC")) if (*overridePath) return overridePath;
    const fs::path arg(argv0);
    if (arg.has_parent_path()) {
        const auto sibling = weakCanonical(fs::absolute(arg).parent_path() / "loomc");
        if (fs::is_regular_file(sibling)) return sibling;
#ifdef _WIN32
        const auto exeSibling = sibling.parent_path() / "loomc.exe";
        if (fs::is_regular_file(exeSibling)) return exeSibling;
#endif
    }
    for (const auto& candidate : {fs::current_path() / "bin" / "loomc", fs::current_path() / "loomc"})
        if (fs::is_regular_file(candidate)) return candidate;
    return "loomc";
}

inline void setEnvVar(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

struct RegistryPackage { std::string kind, name, version, platform, url, sha256; };

fs::path loomHome() {
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".loom";
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE")) return fs::path(profile) / ".loom";
#endif
    throw Error("cannot find your home directory; set HOME and try again");
}

std::string registryUrl() {
    if (const char* custom = std::getenv("LOOM_REGISTRY_URL")) if (*custom) return custom;
    return "https://raw.githubusercontent.com/thirstymelon/loom-libraries/main/registry/loom-catalog.tsv";
}

std::string hostPlatform() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "macos-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#elif defined(_WIN32) && defined(_M_X64)
    return "windows-x64";
#else
    return "unsupported";
#endif
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot read " + path.string());
    std::ostringstream text; text << in.rdbuf(); return text.str();
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return fields;
}

std::vector<RegistryPackage> parseCatalogText(const std::string& text) {
    std::vector<RegistryPackage> packages;
    std::istringstream input(text);
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(input, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        auto f = splitTabs(line);
        if (f.empty()) continue;
        if (f[0] == "Name" || f[0] == "Kind") continue;
        RegistryPackage p;
        if (f.size() == 5 && (f[0] == "library" || f[0] == "toolchain")) p = {f[0], f[1], f[2], "any", f[3], f[4]};
        else if (f.size() == 4) p = {"library", f[0], f[1], "any", f[2], f[3]};
        else if (f.size() == 6 && f[0] == "toolchain") p = {f[0], f[1], f[2], f[3], f[4], f[5]};
        else continue;
        if (p.name.empty() || p.version.empty()) continue;
        packages.push_back(std::move(p));
    }
    return packages;
}

std::vector<RegistryPackage> readRegistry() {
    std::vector<std::string> urls;
    if (const char* custom = std::getenv("LOOM_REGISTRY_URL")) {
        if (*custom) urls.push_back(custom);
    }
    urls.push_back("https://raw.githubusercontent.com/thirstymelon/loom-libraries/main/registry/loom-catalog.tsv");
    urls.push_back("https://raw.githubusercontent.com/thirstymelon/loom-libraries/master/registry/loom-catalog.tsv");
    urls.push_back("https://raw.githubusercontent.com/thirstymelon/Loom/main/registry/loom-catalog.tsv");
    urls.push_back("https://github.com/thirstymelon/Loom/releases/latest/download/loom-catalog.tsv");

    for (const auto& url : urls) {
        fs::path temp = fs::temp_directory_path() / ("loom-reg-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".tsv");
        if (runCommandQuiet({"curl", "--fail", "--location", "--silent", "--retry", "1",
                             "--output", temp.string(), url}) == 0) {
            std::error_code ec;
            if (fs::is_regular_file(temp, ec) && fs::file_size(temp, ec) > 0) {
                std::string content = readTextFile(temp);
                fs::remove(temp, ec);
                auto pkgs = parseCatalogText(content);
                if (!pkgs.empty()) {
                    try {
                        fs::path cachePath = loomHome() / "registry" / "loom-catalog.tsv";
                        fs::create_directories(cachePath.parent_path());
                        std::ofstream out(cachePath, std::ios::binary);
                        out << content;
                    } catch (...) {}
                    return pkgs;
                }
            }
        }
        std::error_code ec;
        fs::remove(temp, ec);
    }

    std::vector<fs::path> localCandidates;
    try { localCandidates.push_back(loomHome() / "registry" / "loom-catalog.tsv"); } catch (...) {}
    if (const char* home = std::getenv("HOME")) {
        localCandidates.push_back(fs::path(home) / "Desktop" / "loom-libraries" / "registry" / "loom-catalog.tsv");
        localCandidates.push_back(fs::path(home) / "loom-libraries" / "registry" / "loom-catalog.tsv");
    }
    try {
        localCandidates.push_back(fs::current_path() / "registry" / "loom-catalog.tsv");
        localCandidates.push_back(fs::current_path() / "loom-catalog.tsv");
    } catch (...) {}

    for (const auto& lp : localCandidates) {
        std::error_code ec;
        if (fs::is_regular_file(lp, ec) && fs::file_size(lp, ec) > 0) {
            auto pkgs = parseCatalogText(readTextFile(lp));
            if (!pkgs.empty()) return pkgs;
        }
    }

    return {};
}

std::string sha256File(const fs::path& file) {
    fs::path output = fs::temp_directory_path() / ("loom-sha-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
#ifdef _WIN32
    const int status = runCommandToFile({"certutil", "-hashfile", file.string(), "SHA256"}, output);
#else
    int status = runCommandToFile({"shasum", "-a", "256", file.string()}, output);
    if (status != 0) status = runCommandToFile({"sha256sum", file.string()}, output);
#endif
    if (status != 0) throw Error("SHA-256 utility not found (shasum, sha256sum, or certutil)");
    std::istringstream parsed(readTextFile(output));
    std::string digest, line;
    while (std::getline(parsed, line)) {
        std::istringstream words(line);
        std::string candidate;
        while (words >> candidate) {
            if (candidate.size() != 64 || !std::all_of(candidate.begin(), candidate.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) continue;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            digest = candidate;
            break;
        }
        if (!digest.empty()) break;
    }
    std::error_code ec; fs::remove(output, ec);
    if (digest.empty()) throw Error("could not read the SHA-256 digest");
    return digest;
}

void installArchive(const RegistryPackage& package) {
    const fs::path home = loomHome();
    const fs::path temp = fs::temp_directory_path() / ("loom-install-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path archive = temp.string() + ".tar.gz";
    fs::create_directories(temp);
    std::error_code ec;
    try {
        int curlStatus = runCommandQuiet({"curl", "--fail", "--location", "--silent", "--retry", "1",
                        "--output", archive.string(), package.url});
        if (curlStatus != 0 || !fs::exists(archive) || fs::file_size(archive) == 0) {
            if (package.kind == "library") {
                auto toLower = [](std::string s) {
                    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return s;
                };
                std::string lowerPkg = toLower(package.name);
                std::vector<fs::path> localLibRoots;
                if (const char* h = std::getenv("HOME")) {
                    localLibRoots.push_back(fs::path(h) / "Desktop" / "loom-libraries" / "libraries");
                    localLibRoots.push_back(fs::path(h) / "loom-libraries" / "libraries");
                }
                try { localLibRoots.push_back(fs::current_path() / "libraries"); } catch (...) {}
                for (const auto& libRoot : localLibRoots) {
                    if (fs::is_directory(libRoot, ec)) {
                        for (const auto& entry : fs::directory_iterator(libRoot, ec)) {
                            if (entry.is_directory(ec) && toLower(entry.path().filename().string()) == lowerPkg) {
                                const fs::path dest = home / "libraries" / entry.path().filename().string();
                                fs::create_directories(dest.parent_path());
                                fs::copy(entry.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                                fs::path vf = dest / "loom-package-version";
                                if (!fs::exists(vf, ec)) {
                                    std::ofstream out(vf);
                                    out << package.version << "\n";
                                }
                                std::cout << "Installed library '" << entry.path().filename().string() << "' " << package.version
                                          << " to " << dest.string() << "\n";
                                fs::remove_all(temp, ec);
                                return;
                            }
                        }
                    }
                }
            }
            throw Error("download failed for " + package.name + " " + package.version);
        }
        if (sha256File(archive) != package.sha256)
            throw Error("SHA-256 verification failed for " + package.name + " " + package.version);
        const fs::path listing = temp / "contents.txt";
        if (runCommandToFile({"tar", "-tzf", archive.string()}, listing) != 0)
            throw Error("invalid package archive for " + package.name);
        std::istringstream names(readTextFile(listing)); std::string item;
        while (std::getline(names, item)) {
            if (!item.empty() && item.back() == '/') item.pop_back();
            fs::path relative(item);
            if (relative.empty() || relative.is_absolute()) throw Error("unsafe path in package archive");
            for (const auto& part : relative) if (part == "..") throw Error("unsafe path in package archive");
        }
        const fs::path verboseListing = temp / "details.txt";
        if (runCommandToFile({"tar", "-tvzf", archive.string()}, verboseListing) != 0)
            throw Error("could not inspect package archive for " + package.name);
        std::istringstream details(readTextFile(verboseListing));
        std::string detail;
        while (std::getline(details, detail)) {
            if (detail.empty()) continue;
            if (detail.front() != '-' && detail.front() != 'd')
                throw Error("links and special files are not allowed in Loom packages");
        }
        const fs::path unpacked = temp / "unpacked"; fs::create_directories(unpacked);
        if (runCommand({"tar", "-xzf", archive.string(), "-C", unpacked.string()}) != 0)
            throw Error("could not unpack " + package.name + " package");
        const fs::path source = unpacked / package.name;
        if (!fs::is_directory(source)) throw Error("package archive has no expected package directory");
        const fs::path destination = package.kind == "library"
            ? home / "libraries" / package.name
            : home / "toolchains" / package.name;
        fs::create_directories(destination.parent_path());
        const fs::path staged = destination.string() + ".installing";
        const fs::path backup = destination.string() + ".previous";
        fs::remove_all(staged, ec);
        fs::remove_all(backup, ec);
        fs::copy(source, staged, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        if (package.kind == "library") {
            std::ofstream versionFile(staged / "loom-package-version", std::ios::binary);
            versionFile << package.version << "\n";
            if (!versionFile) throw Error("could not record installed library version");
        }
        if (fs::exists(destination, ec)) fs::rename(destination, backup);
        try {
            fs::rename(staged, destination);
        } catch (...) {
            if (fs::exists(backup, ec)) fs::rename(backup, destination, ec);
            throw;
        }
        fs::remove_all(backup, ec);
        std::cout << "Installed " << package.kind << " '" << package.name << "' " << package.version
                  << " to " << destination.string() << "\n";
    } catch (...) { fs::remove_all(temp, ec); fs::remove(archive, ec); throw; }
    fs::remove_all(temp, ec); fs::remove(archive, ec);
}

void installFromRegistry(const std::string& kind, const std::string& name, const std::string& version = "") {
    const std::string allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    if (name.empty() || name.find_first_not_of(allowed) != std::string::npos)
        throw Error("invalid package name '" + name + "'");
    if (!version.empty() && version.find_first_not_of(allowed + ".+") != std::string::npos)
        throw Error("invalid package version '" + version + "'");
    const std::string platform = kind == "toolchain" ? hostPlatform() : "any";
    if (platform == "unsupported") throw Error("toolchain downloads are not yet published for this operating system and CPU");

    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string lowerName = toLower(name);

    auto packages = readRegistry();
    for (const auto& p : packages) {
        if (p.kind == kind && toLower(p.name) == lowerName && p.platform == platform && (version.empty() || p.version == version)) {
            installArchive(p);
            return;
        }
    }

    if (kind == "library") {
        std::vector<fs::path> localLibRoots;
        if (const char* home = std::getenv("HOME")) {
            localLibRoots.push_back(fs::path(home) / "Desktop" / "loom-libraries" / "libraries");
            localLibRoots.push_back(fs::path(home) / "loom-libraries" / "libraries");
        }
        try { localLibRoots.push_back(fs::current_path() / "libraries"); } catch (...) {}
        for (const auto& libRoot : localLibRoots) {
            std::error_code ec;
            if (fs::is_directory(libRoot, ec)) {
                for (const auto& entry : fs::directory_iterator(libRoot, ec)) {
                    if (entry.is_directory(ec) && toLower(entry.path().filename().string()) == lowerName) {
                        std::string ver = "1.0.0";
                        fs::path vf = entry.path() / "loom-package-version";
                        if (fs::exists(vf, ec)) {
                            std::string content = trim(readTextFile(vf));
                            if (!content.empty()) ver = content;
                        }
                        if (!version.empty() && version != ver) continue;
                        const fs::path dest = loomHome() / "libraries" / entry.path().filename().string();
                        fs::create_directories(dest.parent_path());
                        fs::copy(entry.path(), dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                        std::ofstream out(dest / "loom-package-version");
                        out << ver << "\n";
                        std::cout << "Installed library '" << entry.path().filename().string() << "' " << ver
                                  << " to " << dest.string() << "\n";
                        return;
                    }
                }
            }
        }
    }

    if (kind == "library") {
        if (!version.empty()) {
            throw Error("library '" + name + "' at version " + version + " not found");
        } else {
            throw Error("library '" + name + "' not found");
        }
    } else {
        throw Error("toolchain '" + name + "' not found" + (version.empty() ? "" : " at version " + version) + " for " + platform);
    }
}

void listLibraries() {
    const fs::path root = loomHome() / "libraries"; std::error_code ec;
    if (!fs::is_directory(root, ec)) { std::cout << "No Loom libraries installed.\n"; return; }
    bool any = false;
    for (const auto& package : fs::directory_iterator(root, ec)) if (package.is_directory(ec)) {
        const fs::path versionFile = package.path() / "loom-package-version";
        std::string version = "unknown";
        if (fs::is_regular_file(versionFile, ec)) version = trim(readTextFile(versionFile));
        std::cout << package.path().filename().string() << " " << version << "\n"; any = true;
    }
    if (!any) std::cout << "No Loom libraries installed.\n";
}

void removeLibrary(const std::string& name) {
    if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        throw Error("invalid library name");
    const fs::path target = loomHome() / "libraries" / name; std::error_code ec;
    if (!fs::exists(target, ec)) throw Error("library '" + name + "' is not installed");
    fs::remove_all(target, ec);
    if (ec) throw Error("could not remove library '" + name + "': " + ec.message());
    std::cout << "Removed library '" << name << "'.\n";
}

struct ToolchainInfo {
    std::string name;
    std::string kind;
    std::string version = "1.4.0";
    std::string description;
    fs::path rootDir;
    fs::path loomcPath;
    fs::path sdkPath;
    bool isDefault = false;
};

inline std::string getDefaultToolchainName() {
    if (const char* home = std::getenv("HOME")) {
        fs::path p = fs::path(home) / ".loom" / "default_toolchain";
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            std::ifstream in(p);
            std::string line;
            if (std::getline(in, line)) {
                line = trim(line);
                if (!line.empty()) return line;
            }
        }
    }
    for (const auto& cand : {
        fs::path("/usr/local/share/loom/default_toolchain"),
        fs::path("/usr/share/loom/default_toolchain"),
        fs::path("/opt/loom/default_toolchain")
    }) {
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) {
            std::ifstream in(cand);
            std::string line;
            if (std::getline(in, line)) {
                line = trim(line);
                if (!line.empty()) return line;
            }
        }
    }
    return "native";
}

inline void setDefaultToolchain(const std::string& rawName) {
    std::string name = trim(rawName);
    if (name.empty()) throw Error("toolchain name cannot be empty");
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            throw Error("invalid toolchain name '" + name + "': must contain alphanumeric characters, hyphens, or underscores");
        }
    }

    bool wroteUser = false;
    if (const char* home = std::getenv("HOME")) {
        fs::path dir = fs::path(home) / ".loom";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path file = dir / "default_toolchain";
        std::ofstream out(file);
        if (out) {
            out << name << "\n";
            wroteUser = true;
        }
    }

    fs::path sysFile = "/usr/local/share/loom/default_toolchain";
    std::error_code ec;
    if (fs::exists(sysFile.parent_path(), ec)) {
        std::ofstream out(sysFile);
        if (out) {
            out << name << "\n";
        }
    }

    if (!wroteUser) {
        throw Error("failed to save default toolchain to ~/.loom/default_toolchain");
    }
}

inline std::vector<ToolchainInfo> listInstalledToolchains(const char* argv0 = nullptr) {
    std::vector<ToolchainInfo> results;
    std::set<std::string> seenNames;
    std::string defaultName = getDefaultToolchainName();

    std::vector<fs::path> candidateRoots;
    if (const char* home = std::getenv("HOME")) {
        candidateRoots.push_back(fs::path(home) / ".loom" / "toolchains");
    }
    for (const auto& sys : {
        fs::path("/usr/local/share/loom/toolchains"),
        fs::path("/usr/share/loom/toolchains"),
        fs::path("/opt/loom/toolchains")
    }) {
        candidateRoots.push_back(sys);
    }

    fs::path exePath = getExecutablePath(argv0);
    if (!exePath.empty()) {
        fs::path exeDir = exePath.parent_path();
        candidateRoots.push_back(exeDir.parent_path() / "share" / "loom" / "toolchains");
        candidateRoots.push_back(exeDir.parent_path() / "toolchains");
        candidateRoots.push_back(exeDir / "toolchains");
    }

    for (const auto& root : candidateRoots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            fs::path tcDir = entry.path();
            std::string dirName = tcDir.filename().string();
            if (dirName.empty() || dirName.front() == '.') continue;
            if (seenNames.count(dirName)) continue;

            ToolchainInfo tc;
            tc.name = dirName;
            tc.rootDir = tcDir;
            tc.kind = (dirName == "rp2040" || dirName.find("cortex") != std::string::npos) ? "rp2040" : "native";

            fs::path tomlPath = tcDir / "toolchain.toml";
            if (fs::is_regular_file(tomlPath, ec)) {
                std::ifstream f(tomlPath);
                std::string line;
                while (std::getline(f, line)) {
                    line = trim(stripComment(line));
                    auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = trim(line.substr(0, eq));
                    std::string v = trim(line.substr(eq + 1));
                    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
                    if (k == "name") tc.name = v;
                    else if (k == "kind") tc.kind = v;
                    else if (k == "version") tc.version = v;
                    else if (k == "description") tc.description = v;
                }
            }

            if (tc.description.empty()) {
                if (tc.kind == "rp2040") tc.description = "Loom RP2040 Embedded Bare-Metal Toolchain (ARM Cortex-M0+)";
                else tc.description = "Loom Native Desktop Toolchain (macOS Apple Silicon arm64)";
            }

            for (const auto& c : {tcDir / "bin" / "loomc", tcDir / "loomc"}) {
                if (fs::is_regular_file(c, ec)) {
                    tc.loomcPath = c;
                    break;
                }
            }

            if (fs::is_directory(tcDir / "sdk", ec)) {
                tc.sdkPath = tcDir / "sdk";
            } else {
                tc.sdkPath = tcDir;
            }

            tc.isDefault = (tc.name == defaultName);
            seenNames.insert(tc.name);
            results.push_back(tc);
        }
    }

    if (results.empty()) {
        fs::path localLoomc = loomcPath(argv0);
        std::error_code ec;
        if (fs::is_regular_file(localLoomc, ec)) {
            ToolchainInfo nat;
            nat.name = "native";
            nat.kind = "native";
            nat.version = "1.4.0";
            nat.description = "Loom Native Desktop Toolchain (workspace development)";
            nat.rootDir = fs::current_path();
            nat.loomcPath = localLoomc;
            nat.sdkPath = fs::current_path() / "sdk";
            nat.isDefault = (defaultName == "native");
            results.push_back(nat);
        }
    }

    return results;
}

inline void printInstalledToolchains(const char* argv0 = nullptr, bool verbose = false) {
    const Colors col = Colors::detect(false);
    auto tcs = listInstalledToolchains(argv0);
    std::string def = getDefaultToolchainName();

    std::cout << col.bold << "Installed Loom toolchains:" << col.reset << "\n";
    if (tcs.empty()) {
        std::cout << col.dim << "  (no toolchains installed)\n\n"
                  << "Install a toolchain with 'loom install toolchain <name>'.\n" << col.reset;
        return;
    }

    for (const auto& tc : tcs) {
        bool isDef = (tc.name == def);
        std::string prefix = isDef ? (col.bold + col.green + "  * ") : "    ";
        std::string nameColor = isDef ? (col.bold + col.green) : col.bold;
        std::string label = tc.name;
        if (isDef) label += " (default)";

        std::string desc;
        if (tc.name == "rp2040" || tc.kind == "rp2040") {
            desc = "ARM Cortex-M0+ bare-metal";
        } else if (tc.name == "native" || tc.kind == "native") {
            desc = "macOS Apple Silicon native";
        } else {
            desc = tc.description;
        }

        std::cout << prefix << nameColor << std::left << std::setw(20) << label << col.reset
                  << col.dim << desc << col.reset;
        if (verbose && !tc.rootDir.empty()) {
            std::cout << col.dim << "  [" << tc.rootDir.generic_string() << "]" << col.reset;
        }
        std::cout << "\n";
    }
}

inline ToolchainInfo resolveActiveToolchain(
    const std::string& cliOverride,
    const std::string& configToolchain,
    const Config& config,
    const char* argv0 = nullptr
) {
    std::string requested = cliOverride;
    if (requested.empty()) {
        if (const char* env = std::getenv("LOOM_TOOLCHAIN")) {
            if (*env) requested = env;
        }
    }
    if (requested.empty()) {
        requested = getDefaultToolchainName();
    }
    if (requested.empty()) {
        requested = "native";
    }

    auto tcs = listInstalledToolchains(argv0);
    ToolchainInfo activeTc;
    bool found = false;
    for (const auto& tc : tcs) {
        if (tc.name == requested) {
            activeTc = tc;
            found = true;
            break;
        }
    }

    if (!found) {
        fs::path fallbackLoomc = loomcPath(argv0);
        std::error_code ec;
        if (fs::is_regular_file(fallbackLoomc, ec)) {
            activeTc.name = requested;
            activeTc.kind = (requested == "rp2040" || requested.find("cortex") != std::string::npos) ? "rp2040" : "native";
            activeTc.version = "1.4.0";
            activeTc.rootDir = fs::current_path();
            activeTc.loomcPath = fallbackLoomc;
            activeTc.sdkPath = fs::current_path() / "sdk";
            found = true;
        }
    }

    if (!found) {
        std::string avail;
        for (const auto& t : tcs) {
            if (!avail.empty()) avail += ", ";
            avail += "'" + t.name + "'";
        }
        throw Error("toolchain '" + requested + "' is not installed.\n"
                    "  Run 'loom toolchains' to view available toolchains.\n" +
                    (avail.empty() ? "" : ("  Installed: " + avail + "\n")));
    }

    fs::path sibling = loomcPath(argv0);
    std::error_code ecSib;
    if (fs::is_regular_file(sibling, ecSib) && sibling != "loomc") {
        activeTc.loomcPath = sibling;
    }

    const std::string targetLower = toLower(config.target);
    const bool isRp2040Project = targetLower.find("thumb") != std::string::npos ||
                                 targetLower.find("none-eabi") != std::string::npos ||
                                 config.cpu == "cortex-m0plus";
    if (isRp2040Project) {
        throw Error("target '" + (config.target.empty() ? "thumbv6m-none-eabi" : config.target) +
                    "' is not supported; embedded RP2040 support was removed in Loom v1.4.0");
    }

    return activeTc;
}

void createProject(const std::string& name) {
    fs::path namePath(name);
    if (name.empty() || namePath.has_parent_path() || name == "." || name == ".." ||
        !std::regex_match(name, std::regex("[A-Za-z0-9_-]+")))
        throw Error("project name must contain only letters, digits, '-' or '_' and be a single directory name");
    if (fs::exists(namePath)) throw Error("destination already exists: " + name);
    try {
        fs::create_directories(namePath / "src");
        std::ofstream config(namePath / "config.toml");
        config << "[project]\nname = \"" << name << "\"\nversion = \"1.4.0\"\n\n[build]\nmain = \"src/main.loom\"\nsources = [\"src/*.loom\"]\ntarget-dir = \"target\"\nexecutable = \"" << name << "\"\n\n[toolchain]\nclang = \"clang\"\n";
        if (!config) throw Error("failed to write config.toml");
        std::ofstream main(namePath / "src" / "main.loom");
        main << "Module Main\n\nBlock Main\n    Implements\n        Print(\"Hello, Loom!\\n\")\nEnd Main\n";
        if (!main) throw Error("failed to write src/main.loom");
        std::ofstream gitignore(namePath / ".gitignore");
        gitignore << "target/\n";
        std::ofstream readme(namePath / "README.md");
        readme << "# " << name << "\n\nA Loom workspace. Edit `src/main.loom`, build with `loom build`, and run `./target/" << name << "`.\n";
        if (!readme || !gitignore) throw Error("failed to write project support files");
    } catch (...) {
        std::error_code ec;
        fs::remove_all(namePath, ec);
        throw;
    }
    std::cout << "Created Loom workspace " << name << "\n"
              << "  Config: config.toml\n  Main:   src/main.loom\n\nNext steps:\n  cd " << shellQuote(name)
              << "\n  loom build\n  loom run\n";
}
std::string graphForWorkspace(const fs::path& root, const std::vector<fs::path>& sources, const fs::path& compiler,
                              const std::string& view, const std::string& format) {
    const auto model = analyzeDependencies(sources, compiler);
    return format == "json" ? jsonGraph(root, sources, model, view) : mermaidGraph(model, view);
}
void graphProject(const fs::path& configFile, const char* argv0, const std::optional<fs::path>& output,
                  bool checkOnly, const std::string& view, const std::string& format, const std::string& cliToolchain = "") {
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const ToolchainInfo activeTc = resolveActiveToolchain(cliToolchain, config.toolchain, config, argv0);
    setEnvVar("LOOM_TOOLCHAIN_KIND", activeTc.kind);
    const auto sdkDirs = getSdkIncludeDirs(argv0, activeTc.sdkPath.string());
    const auto sources = sourceFiles(root, config, sdkDirs);
    const auto compiler = activeTc.loomcPath.empty() ? loomcPath(argv0) : activeTc.loomcPath;
    const auto graph = graphForWorkspace(root, sources, compiler, view, format);
    if (checkOnly) {
        std::cout << "Dependency graph validated for " << config.name << " (" << sources.size()
                  << " source files, " << graph.size() << " bytes of " << format << " " << view << " output).\n";
        return;
    }
    const std::string defaultOutput = format == "json" ? "loom-graph.json" : (view == "calls" ? "loom-deps.mmd" : "loom-data.mmd");
    const auto destination = workspacePath(root, output ? output->string() : defaultOutput, "graph output");
    writeGraph(destination, graph);
    std::cout << format << " " << view << " graph written to " << destination.lexically_relative(root).generic_string()
              << " (" << sources.size() << " sources, " << graph.size() << " bytes).\n";
}
inline std::string computeBuildCacheKey(const std::vector<fs::path>& sources, const std::string& compilerVer, bool release, bool debug, const std::string& sanitize, const std::string& target, const std::string& cpu, bool sizeOpt = false) {
    std::ostringstream ss;
    ss << compilerVer << ";" << (sizeOpt ? "size" : (release ? "rel" : "dbg")) << ";" << (debug ? "g" : "") << ";" << sanitize << ";" << target << ";" << cpu << "\n";
    for (const auto& s : sources) {
        std::error_code ec;
        auto sz = fs::file_size(s, ec);
        auto lwt = static_cast<uint64_t>(fs::last_write_time(s, ec).time_since_epoch().count());
        ss << s.string() << ":" << sz << ":" << lwt << "\n";
    }
    uint64_t hash = 14695981039346656037ULL;
    std::string data = ss.str();
    for (unsigned char b : data) {
        hash ^= b;
        hash *= 1099511628211ULL;
    }
    std::ostringstream hexOut;
    hexOut << std::hex << std::setw(16) << std::setfill('0') << hash;
    return hexOut.str();
}

inline std::string formatFileSize(uintmax_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes < 1024 * 1024) {
        oss << (static_cast<double>(bytes) / 1024.0) << " KB";
    } else {
        oss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    return oss.str();
}

struct TargetArtifact {
    std::string target;
    std::string outputPath;
    std::string sizeStr;
    bool success = false;
    std::string errorMsg;
};

inline std::string padCell(const std::string& visible, std::size_t width, const std::string& prefix = "", const std::string& suffix = "") {
    std::string s = visible;
    std::size_t len = s.size();
    std::string out = prefix + s + suffix;
    while (len < width) {
        out += " ";
        len++;
    }
    return out;
}

inline void printTargetMatrix(const std::vector<TargetArtifact>& artifacts, const Colors& col) {
    std::size_t maxTarget = 17;
    std::size_t maxOutput = 22;
    std::size_t maxSize = 10;
    for (const auto& a : artifacts) {
        std::string tName = a.target.empty() ? "host (native)" : a.target;
        maxTarget = std::max(maxTarget, tName.size());
        maxOutput = std::max(maxOutput, a.outputPath.size());
        maxSize = std::max(maxSize, a.sizeStr.size());
    }
    maxTarget += 4;
    maxOutput += 4;
    maxSize += 4;
    const std::size_t statusWidth = 10;

    std::cout << "\n";
    std::cout << "  " << col.bold << padCell("Target Platform", maxTarget) << col.reset
              << col.bold << padCell("Output Binary", maxOutput) << col.reset
              << col.bold << padCell("Size", maxSize) << col.reset
              << col.bold << padCell("Status", statusWidth) << col.reset << "\n";

    for (const auto& a : artifacts) {
        std::string tName = a.target.empty() ? "host (native)" : a.target;
        std::string statVis = a.success ? "✔ OK" : "✖ FAIL";
        std::string statColor = a.success ? col.bold + col.green : col.bold + col.red;
        std::cout << "  " << padCell(tName, maxTarget)
                  << col.cyan << padCell(a.outputPath, maxOutput) << col.reset
                  << padCell(a.sizeStr, maxSize)
                  << padCell(statVis, statusWidth, statColor, col.reset) << "\n";
    }
    std::cout << "\n";
}

void buildProject(const fs::path& configFile, const char* argv0, bool release, bool verbose = false, bool quiet = false, bool optReport = false, const std::string& cliToolchain = "", bool debug = false, const std::string& sanitize = "", bool noCache = false, const std::string& cliTarget = "", bool sizeOpt = false) {
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    if (!sizeOpt && !release && config.profile == "size") {
        sizeOpt = true;
        release = true;
    }
    const ToolchainInfo activeTc = resolveActiveToolchain(cliToolchain, config.toolchain, config, argv0);
    setEnvVar("LOOM_TOOLCHAIN_KIND", activeTc.kind);
    const auto sdkDirs = getSdkIncludeDirs(argv0, activeTc.sdkPath.string());
    const auto sources = sourceFiles(root, config, sdkDirs);
    const auto targetBase = workspacePath(root, config.targetDir, "build.target-dir");
    if (targetBase == root) throw Error("build.target-dir cannot be the workspace root");
    const auto compiler = activeTc.loomcPath.empty() ? loomcPath(argv0) : activeTc.loomcPath;
    const Colors col = Colors::detect(false);

    std::vector<std::string> activeTargets;
    if (!cliTarget.empty()) {
        activeTargets.push_back(cliTarget);
    } else if (!config.targets.empty()) {
        activeTargets = config.targets;
    } else if (!config.target.empty()) {
        activeTargets.push_back(config.target);
    } else {
        activeTargets.push_back("");
    }

    if (!quiet) {
        std::cout << col.bold << col.cyan << "✦ Loom 1.4.0" << col.reset
                  << col.dim << " | " << col.reset
                  << col.bold << "Building " << config.name << col.reset << "\n"
                  << col.dim << "  Project:     " << col.reset << config.name << " (" << config.main << ")\n"
                  << col.dim << "  Sources:     " << col.reset << sources.size() << " files\n"
                  << col.dim << "  Toolchain:   " << col.reset << activeTc.name << " (" << activeTc.kind << ")\n"
                  << col.dim << "  Mode:        " << col.reset
                  << (sizeOpt ? (col.bold + col.magenta + "Size (-Oz, dead-strip, compact runtime)") :
                     (release ? (col.bold + col.green + "Release (-O3, native host)") :
                                (col.bold + col.yellow + "Debug (-g)"))) << col.reset << "\n";
        if (activeTargets.size() > 1 || !activeTargets[0].empty()) {
            std::cout << col.dim << "  Targets (" << activeTargets.size() << "): " << col.reset;
            for (std::size_t i = 0; i < activeTargets.size(); ++i) {
                std::cout << (activeTargets[i].empty() ? "host" : activeTargets[i]) << (i + 1 < activeTargets.size() ? ", " : "\n");
            }
        }
        if (debug) {
            std::cout << col.dim << "  Debug info:  " << col.reset << "DWARF (-g)\n";
        }
        if (!sanitize.empty()) {
            std::cout << col.dim << "  Sanitizer:   " << col.reset << sanitize << "\n";
        }
    }

    std::error_code ecMmd;
    fs::remove(root / "loom-deps.mmd", ecMmd);
    const auto analysis = analyzeDependencies(sources, compiler);
    const auto mainSource = workspacePath(root, config.main, "build.main");
    const auto mainIndex = static_cast<std::size_t>(std::distance(sources.begin(), std::find(sources.begin(), sources.end(), mainSource)));
    const auto mainDecl = std::find_if(analysis.blocks.begin(), analysis.blocks.end(), [&](const BlockNode& b) {
        return b.source == mainIndex && b.name == "Main";
    });
    if (mainDecl == analysis.blocks.end())
        throw Error("build.main ('" + config.main + "') must declare the parameterless procedure Block Main");

    const auto startTime = std::chrono::steady_clock::now();
    std::vector<TargetArtifact> artifacts;
    int succeeded = 0;

    for (std::size_t tIdx = 0; tIdx < activeTargets.size(); ++tIdx) {
        const std::string& currentTarget = activeTargets[tIdx];
        const std::string targetLower = toLower(currentTarget);
        const bool isBaremetal = config.baremetal || targetLower.find("none") != std::string::npos;
        const bool aarch64BareMetalTarget = isBaremetal && targetLower.find("aarch64") != std::string::npos &&
            (targetLower.find("none-elf") != std::string::npos || targetLower.find("unknown-elf") != std::string::npos);

        const bool isMultiTarget = activeTargets.size() > 1 || (!cliTarget.empty() && cliTarget != config.target);
        const fs::path objectDir = isMultiTarget ? (targetBase / "obj" / currentTarget) : (targetBase / "obj");
        fs::create_directories(objectDir);
        const std::string objFileName = toLower(config.name) + ".o";
        const fs::path object = objectDir / objFileName;

        const fs::path cacheDir = root / ".loom-cache";
        std::error_code ecCache;
        fs::create_directories(cacheDir, ecCache);
        const fs::path cacheFile = cacheDir / (toLower(config.name) + (currentTarget.empty() ? "" : ("_" + currentTarget)) + ".cache");
        const std::string cacheKey = computeBuildCacheKey(sources, compiler.string(), release, debug, sanitize, currentTarget, config.cpu, sizeOpt);

        bool canUseCachedObject = false;
        if (!noCache && fs::is_regular_file(object) && fs::is_regular_file(cacheFile)) {
            std::ifstream cin(cacheFile);
            std::string prevKey;
            if (cin >> prevKey && prevKey == cacheKey) {
                canUseCachedObject = true;
            }
        }
        if (!canUseCachedObject) {
            for (const auto& item : fs::directory_iterator(objectDir))
                if (item.is_regular_file() && item.path().extension() == ".o") fs::remove(item.path());
        }

        std::string displayTarget = currentTarget.empty() ? "host (native)" : currentTarget;
        if (!quiet) {
            std::cout << "\n" << col.bold << "[" << (tIdx + 1) << "/" << activeTargets.size() << "] "
                      << col.cyan << displayTarget << col.reset << "\n";
        }

        TargetArtifact art;
        art.target = currentTarget;

        if (canUseCachedObject) {
            if (!quiet) {
                std::cout << "  " << col.bold << col.cyan << "✔" << col.reset << " [cache] " << sources.size() << " Loom sources "
                          << col.dim << "->" << col.reset << " " << object.lexically_relative(root).generic_string() << "\n" << std::flush;
            }
        } else {
            std::vector<std::string> compile{compiler.string(), "--object", "--output", object.string()};
            if (isBaremetal) {
                compile.push_back("--baremetal");
            } else {
                compile.push_back("--entry");
            }
            if (!currentTarget.empty()) {
                compile.push_back("--target");
                compile.push_back(currentTarget);
            }
            if (!config.cpu.empty()) {
                compile.push_back("--cpu");
                compile.push_back(config.cpu);
            }
            if (sizeOpt) compile.push_back("--size");
            else if (release) compile.push_back("--release");
            if (debug) compile.push_back("-g");
            if (!sanitize.empty()) compile.push_back("--sanitize=" + sanitize);
            if (optReport) compile.push_back("--opt-report");
            for (const auto& sdkDir : sdkDirs) {
                compile.push_back("-I");
                compile.push_back(sdkDir.string());
            }
            for (const auto& source : sources) compile.push_back(source.string());
            if (verbose) {
                std::cout << "  Command: " << shellQuote(compiler.string()) << " --object --output "
                          << shellQuote(object.string()) << " <" << sources.size() << " sources>\n";
            }
            const int compileStatus = runCommand(compile);
            if (compileStatus != 0) {
                art.success = false;
                art.outputPath = object.lexically_relative(root).generic_string();
                art.sizeStr = "0 B";
                artifacts.push_back(art);
                if (activeTargets.size() == 1) throw ChildFailure(compileStatus);
                continue;
            }
            std::ofstream cout(cacheFile);
            cout << cacheKey << "\n";
            if (!quiet) {
                std::cout << "  " << col.bold << col.green << "✔" << col.reset << " Compiled " << sources.size() << " Loom sources "
                          << col.dim << "->" << col.reset << " " << object.lexically_relative(root).generic_string() << "\n" << std::flush;
            }
        }

        std::vector<fs::path> extraObjects;
        if (!config.assemblySources.empty()) {
            for (const auto& asmPat : config.assemblySources) {
                fs::path asmPath;
                if (fs::is_regular_file(asmPat)) asmPath = asmPat;
                else {
                    asmPath = weakCanonical(root / asmPat);
                    if (!fs::is_regular_file(asmPath)) {
                        for (const auto& sdkDir : sdkDirs) {
                            fs::path cand = sdkDir / asmPat;
                            if (fs::is_regular_file(cand)) { asmPath = cand; break; }
                        }
                    }
                }
                fs::path asmObj = objectDir / (asmPath.stem().string() + ".o");
                extraObjects.push_back(asmObj);
                std::vector<std::string> asmCmd{config.clang, "-c", asmPath.string(), "-o", asmObj.string()};
                if (!currentTarget.empty()) asmCmd.push_back("--target=" + currentTarget);
                if (!config.cpu.empty()) asmCmd.push_back("-mcpu=" + config.cpu);
                int asmStatus = runCommand(asmCmd);
                if (asmStatus != 0) {
                    if (activeTargets.size() == 1) throw ChildFailure(asmStatus);
                }
            }
        }

        fs::path targetDir = isMultiTarget ? (targetBase / currentTarget) : targetBase;
        fs::create_directories(targetDir);
        fs::path output = targetDir / config.executable;
        if (isBaremetal) {
            if (output.extension() != ".elf") output += ".elf";
        } else if (targetLower.find("windows") != std::string::npos || targetLower.find("msvc") != std::string::npos || targetLower.find("mingw") != std::string::npos) {
            if (output.extension() != ".exe") output += ".exe";
        }
#ifdef _WIN32
        else if (output.extension() != ".exe") output += ".exe";
#endif

        fs::path stagedOutput = output;
#ifdef _WIN32
        stagedOutput.replace_filename(output.stem().string() + ".tmp.exe");
#else
        stagedOutput += ".tmp";
#endif
        std::vector<std::string> command{config.clang, "-Wno-override-module"};
        if (!currentTarget.empty()) {
            command.push_back("--target=" + currentTarget);
        }
        bool isDarwin = currentTarget.empty() ?
#if defined(__APPLE__)
            true
#else
            false
#endif
            : (targetLower.find("darwin") != std::string::npos || targetLower.find("apple") != std::string::npos || targetLower.find("macos") != std::string::npos);
        bool isWindows = currentTarget.empty() ?
#if defined(_WIN32)
            true
#else
            false
#endif
            : (targetLower.find("windows") != std::string::npos || targetLower.find("msvc") != std::string::npos);

        if (isBaremetal) {
            command.push_back("-nostdlib");
            command.push_back("-fuse-ld=lld");
            if (aarch64BareMetalTarget) command.push_back("-Wl,-pie");
            if (!config.cpu.empty()) command.push_back("-mcpu=" + config.cpu);
            if (!config.linkerScript.empty()) {
                fs::path lds = fs::is_regular_file(config.linkerScript) ? fs::path(config.linkerScript) : weakCanonical(root / config.linkerScript);
                command.push_back("-T" + lds.string());
            }
            command.push_back("-Wl,--gc-sections");
            command.push_back("-Wl,--icf=all");
        } else {
            if (isDarwin) {
#if defined(__APPLE__)
                const char* envDep = std::getenv("MACOSX_DEPLOYMENT_TARGET");
                if (envDep && *envDep) {
                    command.push_back(std::string("-mmacosx-version-min=") + envDep);
                }
#endif
                if (sizeOpt) {
                    command.push_back("-Wl,-dead_strip");
                    command.push_back("-Wl,-x");
                } else if (release) {
                    command.push_back("-Wl,-dead_strip");
                }
                command.push_back("-framework");
                command.push_back("Accelerate");
            } else if (!isWindows) {
                if (sizeOpt) {
                    command.push_back("-Wl,--gc-sections");
                    command.push_back("-s");
                } else if (release) {
                    command.push_back("-Wl,--gc-sections");
                }
            }
            command.push_back("-pthread");
            if (sizeOpt) command.push_back("-Oz");
            else if (release) command.push_back("-O3");
            if (debug) command.push_back("-g");
            if (!sanitize.empty()) {
                command.push_back("-fsanitize=" + sanitize);
                command.push_back("-fno-omit-frame-pointer");
            }
            fs::path exeDir = weakCanonical(fs::path(argv0)).parent_path();
            std::vector<fs::path> rtCandidates = {
                exeDir / "libloom_rt.a",
                exeDir.parent_path() / "lib" / "libloom_rt.a",
                fs::path("/usr/local/lib/libloom_rt.a"),
                exeDir / "build" / "libloom_rt.a",
                exeDir / ".build" / "libloom_rt.a",
                root / "build" / "libloom_rt.a",
                root / ".build" / "libloom_rt.a"
            };
            for (const auto& cand : rtCandidates) {
                if (fs::is_regular_file(cand)) {
                    command.push_back(cand.string());
                    break;
                }
            }
        }
        command.push_back(object.string());
        for (const auto& extraObj : extraObjects) command.push_back(extraObj.string());
        for (const auto& linkArg : config.linkArgs) command.push_back(linkArg);
        command.push_back("-o"); command.push_back(stagedOutput.string());

        if (verbose) {
            std::cout << "  Linker Command: ";
            for (const auto& a : command) std::cout << shellQuote(a) << " ";
            std::cout << "\n";
        }

        int linkStatus = runCommand(command);
        if (linkStatus != 0) {
            std::error_code ignored; fs::remove(stagedOutput, ignored);
            art.success = false;
            art.outputPath = output.lexically_relative(root).generic_string();
            art.sizeStr = "0 B";
            artifacts.push_back(art);
            if (activeTargets.size() == 1) throw ChildFailure(1);
            continue;
        }

        if (sizeOpt && isDarwin) {
            std::vector<std::string> stripCmd{"strip", "-x", stagedOutput.string()};
            (void)runCommand(stripCmd);
        }

        std::error_code ec;
        fs::rename(stagedOutput, output, ec);
        if (ec) {
            fs::remove(output, ec);
            ec.clear(); fs::rename(stagedOutput, output, ec);
        }
        if (ec) {
            std::error_code ignored; fs::remove(stagedOutput, ignored);
            throw Error("cannot publish native executable '" + output.string() + "': " + ec.message());
        }

        std::error_code szEc;
        auto outSize = fs::file_size(output, szEc);
        art.success = true;
        art.outputPath = output.lexically_relative(root).generic_string();
        art.sizeStr = szEc ? "-" : formatFileSize(outSize);
        artifacts.push_back(art);
        succeeded++;

        if (!quiet) {
            std::cout << "  " << col.bold << col.green << "✔" << col.reset << " Linked native binary     "
                      << col.dim << "->" << col.reset << " " << col.bold << col.cyan << art.outputPath << col.reset
                      << " (" << art.sizeStr << ")\n" << std::flush;
        }
    }

    if (!quiet) {
        printTargetMatrix(artifacts, col);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        std::cout << "\n" << col.bold << col.green << "Build succeeded: " << col.reset
                  << col.bold << (artifacts.empty() ? "" : artifacts[0].outputPath) << col.reset << "\n";
        std::cout << col.bold << col.green << "✨ Build complete " << col.reset
                  << "(" << succeeded << "/" << activeTargets.size() << " succeeded in "
                  << std::fixed << std::setprecision(2) << secs << "s)\n\n" << std::flush;
    }

    if (succeeded < static_cast<int>(activeTargets.size())) {
        throw ChildFailure(1);
    }
}

void runProject(const fs::path& configFile, const char* argv0, bool release, bool verbose = false, bool optReport = false, const std::string& cliToolchain = "", bool debug = false, const std::string& sanitize = "", bool noCache = false, const std::string& cliTarget = "", bool sizeOpt = false) {
    buildProject(configFile, argv0, release, verbose, /*quiet=*/!verbose, optReport, cliToolchain, debug, sanitize, noCache, cliTarget, sizeOpt);
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const Colors col = Colors::detect(false);
    if (config.baremetal) {
        const std::string targetLower = toLower(config.target);
        const bool rp2040Target = targetLower.find("thumb") != std::string::npos ||
                                  targetLower.find("none-eabi") != std::string::npos ||
                                  config.cpu == "cortex-m0plus";
        if (!rp2040Target && targetLower.find("aarch64") != std::string::npos &&
            (targetLower.find("none-elf") != std::string::npos || targetLower.find("unknown-elf") != std::string::npos)) {
            fs::path kernel = workspacePath(root, config.targetDir, "build.target-dir") / config.executable;
            if (kernel.extension() != ".elf") kernel += ".elf";
            const std::string emulator = config.emulator.empty() ? "qemu-system-aarch64" : config.emulator;
            std::vector<std::string> command{emulator};
            if (config.runArgs.empty()) {
                command.insert(command.end(), {"-M", "virt", "-cpu", config.cpu.empty() ? "cortex-a72" : config.cpu,
                                               "-smp", "1", "-m", "128M", "-nographic", "-monitor", "none",
                                               "-serial", "stdio", "-device", "loader,file=" + kernel.string() + ",cpu-num=0"});
            } else {
                for (auto arg : config.runArgs) {
                    std::size_t pos = 0;
                    while ((pos = arg.find("{kernel}", pos)) != std::string::npos) {
                        arg.replace(pos, 8, kernel.string());
                        pos += kernel.string().size();
                    }
                    command.push_back(std::move(arg));
                }
            }
            if (verbose) std::cout << "\n" << col.bold << col.cyan << "Running AArch64 bare-metal kernel in "
                                   << col.reset << emulator << "\n" << std::flush;
            const int status = runCommand(command);
            if (status == 127) throw Error("cannot start emulator '" + emulator + "'; install QEMU or set [run].emulator");
            if (status != 0) throw Error("bare-metal emulator exited with status " + std::to_string(status));
            return;
        }
        fs::path uf2 = workspacePath(root, config.targetDir, "build.target-dir") / config.executable;
        uf2.replace_extension(".uf2");
        std::cout << "\n" << col.bold << col.yellow << "Bare-metal target: " << col.reset
                  << "Cannot execute bare-metal firmware directly on host OS.\n\n"
                  << col.bold << col.cyan << "Flashing instructions for Raspberry Pi Pico:\n" << col.reset
                  << "  1. Hold the BOOTSEL button on your Raspberry Pi Pico and plug it into USB.\n"
                  << "  2. Drag and drop " << col.bold << uf2.lexically_relative(root).generic_string() << col.reset
                  << " into the RPI-RP2 mass storage volume.\n"
                  << "  3. The Pico will reboot immediately and run your Loom firmware!\n" << std::flush;
        return;
    }
    fs::path targetDir = workspacePath(root, config.targetDir, "build.target-dir");
    fs::path executable = targetDir / config.executable;
    if (!cliTarget.empty()) {
        fs::path targetCand = targetDir / cliTarget / config.executable;
        if (fs::exists(targetCand)) executable = targetCand;
    } else if (!config.targets.empty()) {
        fs::path targetCand = targetDir / config.targets[0] / config.executable;
        if (fs::exists(targetCand)) executable = targetCand;
    }
#ifdef _WIN32
    if (executable.extension() != ".exe") executable += ".exe";
#endif
    const fs::path previous = fs::current_path();
    fs::current_path(root);
    if (verbose) {
        std::cout << "\n" << col.bold << col.cyan << "Running " << col.reset << executable.lexically_relative(root).generic_string() << "\n" << std::flush;
    }
    const int status = runCommand({executable.string()});
    fs::current_path(previous);
    if (status != 0) throw Error("program exited with status " + std::to_string(status));
}

void cleanProject(const fs::path& configFile) {
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const fs::path target = workspacePath(root, config.targetDir, "build.target-dir");
    if (target == root) throw Error("refusing to clean the workspace root");
    const Colors col = Colors::detect(false);
    if (!fs::exists(target)) { std::cout << col.dim << "Nothing to clean (" << config.targetDir << " does not exist)." << col.reset << "\n"; return; }

    const fs::path objectDir = target / "obj";
    std::error_code ec;
    bool removedAny = false;
    if (fs::exists(objectDir)) {
        fs::remove_all(objectDir, ec);
        removedAny = true;
    }
    fs::path executable = target / config.executable;
#ifdef _WIN32
    if (executable.extension() != ".exe") executable += ".exe";
#endif
    if (fs::exists(executable)) {
        fs::remove(executable, ec);
        removedAny = true;
    }
    if (fs::exists(root / "loom-deps.mmd")) {
        fs::remove(root / "loom-deps.mmd", ec);
        removedAny = true;
    }
    for (const auto& ext : {".elf", ".bin", ".uf2", ".tmp"}) {
        fs::path p = target / (config.name + ext);
        if (fs::exists(p)) { fs::remove(p, ec); removedAny = true; }
        fs::path p2 = target / (config.executable + ext);
        if (fs::exists(p2)) { fs::remove(p2, ec); removedAny = true; }
    }
    fs::path staged = executable;
    staged += ".tmp";
    if (fs::exists(staged)) {
        fs::remove(staged, ec);
        removedAny = true;
    }
    fs::path cacheDir = root / ".loom-cache";
    if (fs::exists(cacheDir)) {
        fs::remove_all(cacheDir, ec);
        removedAny = true;
    }
    if (fs::exists(target)) {
        if (fs::is_empty(target, ec)) {
            fs::remove(target, ec);
            std::cout << col.bold << col.green << "Removed " << col.reset << target.lexically_relative(root).string() << "\n";
        } else if (removedAny) {
            std::cout << col.bold << col.green << "Cleaned " << col.reset << "build artifacts in " << target.lexically_relative(root).string() << "\n";
        } else {
            std::cout << col.dim << "Nothing to clean in " << target.lexically_relative(root).string() << col.reset << "\n";
        }
    }
}

struct DocItem {
    std::string kind;
    std::string name;
    std::string signature;
    std::string doc;
    std::string file;
    int line = 1;
};

void docProject(const fs::path& configFile, const char* argv0, const std::optional<fs::path>& outputDir) {
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const Colors col = Colors::detect(false);
    const auto sdkDirs = getSdkIncludeDirs(argv0, "");
    const auto sources = sourceFiles(root, config, sdkDirs);

    fs::path outDir = outputDir ? *outputDir : (root / "docs" / "api");
    std::error_code ec;
    fs::create_directories(outDir, ec);

    std::vector<DocItem> items;
    std::string currentModule = config.name;

    for (const auto& src : sources) {
        std::ifstream in(src);
        if (!in) continue;
        std::string line;
        std::string pendingDoc;
        int lineNo = 0;
        while (std::getline(in, line)) {
            lineNo++;
            std::string tr = trim(line);
            if (tr.rfind("///", 0) == 0) {
                std::string docLine = trim(tr.substr(3));
                if (!pendingDoc.empty()) pendingDoc += "\n";
                pendingDoc += docLine;
                continue;
            }
            if (tr.rfind("Module ", 0) == 0) {
                currentModule = trim(tr.substr(7));
                pendingDoc.clear();
                continue;
            }
            if (tr.rfind("Block ", 0) == 0) {
                std::string rest = tr.substr(6);
                auto spacePos = rest.find_first_of(" \t\r\n");
                std::string name = (spacePos != std::string::npos) ? rest.substr(0, spacePos) : rest;
                items.push_back({"Block", name, tr, pendingDoc, src.filename().string(), lineNo});
                pendingDoc.clear();
                continue;
            }
            if (tr.rfind("Record ", 0) == 0) {
                std::string rest = tr.substr(7);
                auto spacePos = rest.find_first_of(" \t\r\n");
                std::string name = (spacePos != std::string::npos) ? rest.substr(0, spacePos) : rest;
                items.push_back({"Record", name, tr, pendingDoc, src.filename().string(), lineNo});
                pendingDoc.clear();
                continue;
            }
            if (tr.rfind("Enum ", 0) == 0) {
                std::string rest = tr.substr(5);
                auto spacePos = rest.find_first_of(" \t\r\n");
                std::string name = (spacePos != std::string::npos) ? rest.substr(0, spacePos) : rest;
                items.push_back({"Enum", name, tr, pendingDoc, src.filename().string(), lineNo});
                pendingDoc.clear();
                continue;
            }
            if (tr.rfind("Constant ", 0) == 0) {
                std::string rest = tr.substr(9);
                auto colonPos = rest.find_first_of(" :");
                std::string name = (colonPos != std::string::npos) ? rest.substr(0, colonPos) : rest;
                items.push_back({"Constant", name, tr, pendingDoc, src.filename().string(), lineNo});
                pendingDoc.clear();
                continue;
            }
            if (!tr.empty() && tr[0] != '#') {
                pendingDoc.clear();
            }
        }
    }

    fs::path indexFile = outDir / "index.html";
    std::ofstream out(indexFile);
    out << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
        << "<meta charset=\"UTF-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        << "<title>" << config.name << " API Documentation - Loom</title>\n"
        << "<style>\n"
        << "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0d1117; color: #c9d1d9; margin: 0; padding: 0; line-height: 1.6; }\n"
        << ".container { display: flex; min-height: 100vh; }\n"
        << "nav { width: 280px; background: #161b22; padding: 24px; border-right: 1px solid #30363d; position: sticky; top: 0; height: 100vh; overflow-y: auto; box-sizing: border-box; }\n"
        << "main { flex: 1; padding: 40px; max-width: 900px; }\n"
        << "h1, h2, h3 { color: #f0f6fc; font-weight: 600; }\n"
        << "nav h2 { font-size: 1.1rem; border-bottom: 1px solid #30363d; padding-bottom: 8px; margin-top: 24px; }\n"
        << "nav a { display: block; color: #58a6ff; text-decoration: none; padding: 4px 0; font-size: 0.95rem; }\n"
        << "nav a:hover { text-decoration: underline; }\n"
        << ".item-card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 20px; margin-bottom: 24px; }\n"
        << ".badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 0.8rem; font-weight: 600; text-transform: uppercase; margin-right: 8px; }\n"
        << ".badge-block { background: #238636; color: #fff; }\n"
        << ".badge-record { background: #1f6feb; color: #fff; }\n"
        << ".badge-enum { background: #8957e5; color: #fff; }\n"
        << ".badge-constant { background: #d29922; color: #fff; }\n"
        << ".sig { font-family: monospace; font-size: 0.95rem; background: #0d1117; padding: 10px 14px; border-radius: 6px; border: 1px solid #30363d; color: #79c0ff; margin: 12px 0; overflow-x: auto; }\n"
        << ".doc { color: #8b949e; margin-top: 8px; white-space: pre-wrap; }\n"
        << ".meta { font-size: 0.85rem; color: #8b949e; margin-top: 12px; }\n"
        << "</style>\n</head>\n<body>\n"
        << "<div class=\"container\">\n"
        << "<nav>\n"
        << "<h1>" << config.name << "</h1>\n"
        << "<p style=\"color:#8b949e; font-size:0.9rem;\">Loom v1.4.0 Documentation</p>\n"
        << "<h2>Symbols</h2>\n";
    for (const auto& item : items) {
        out << "<a href=\"#" << item.kind << "-" << item.name << "\">" << item.name << "</a>\n";
    }
    out << "</nav>\n"
        << "<main>\n"
        << "<h1>API Reference: " << config.name << "</h1>\n"
        << "<p>Documentation generated automatically by <code>loom doc</code> from source comments (<code>///</code>).</p>\n";
    for (const auto& item : items) {
        std::string badgeClass = "badge-block";
        if (item.kind == "Record") badgeClass = "badge-record";
        else if (item.kind == "Enum") badgeClass = "badge-enum";
        else if (item.kind == "Constant") badgeClass = "badge-constant";

        out << "<div class=\"item-card\" id=\"" << item.kind << "-" << item.name << "\">\n"
            << "<div><span class=\"badge " << badgeClass << "\">" << item.kind << "</span>"
            << "<h3 style=\"display:inline;\">" << item.name << "</h3></div>\n"
            << "<div class=\"sig\">" << item.signature << "</div>\n";
        if (!item.doc.empty()) {
            out << "<div class=\"doc\">" << item.doc << "</div>\n";
        } else {
            out << "<div class=\"doc\" style=\"font-style:italic;\">No description provided.</div>\n";
        }
        out << "<div class=\"meta\">Defined in " << item.file << " on line " << item.line << "</div>\n"
            << "</div>\n";
    }
    out << "</main>\n</div>\n</body>\n</html>\n";

    std::cout << col.bold << col.green << "Documentation generated: " << col.reset
              << indexFile.lexically_relative(root).generic_string() << " (" << items.size() << " documented symbols)\n";
}

std::string readFileContent(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open file '" + path + "'");
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void fetchDependencies(const fs::path& configFile) {
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const Colors col = Colors::detect(false);
    if (config.dependencies.empty()) {
        std::cout << col.dim << "No dependencies listed in config.toml" << col.reset << "\n";
        return;
    }
    fs::path depsDir = root / ".loom" / "deps";
    std::error_code ec;
    fs::create_directories(depsDir, ec);

    std::map<std::string, std::string> lockCommits;
    for (const auto& [depName, repoUrl] : config.dependencies) {
        fs::path depPath = depsDir / depName;
        if (fs::exists(depPath)) {
            std::cout << "  " << col.bold << col.cyan << "[update]" << col.reset << " " << depName << "\n";
            std::string cmd = "git -C " + shellQuote(depPath.string()) + " pull --quiet";
            (void)std::system(cmd.c_str());
        } else {
            std::cout << "  " << col.bold << col.green << "[fetch]" << col.reset << " " << depName << " from " << repoUrl << "\n";
            std::string cmd = "git clone --quiet " + shellQuote(repoUrl) + " " + shellQuote(depPath.string());
            int status = std::system(cmd.c_str());
            if (status != 0) {
                throw Error("failed to fetch dependency '" + depName + "' from " + repoUrl);
            }
        }
        std::string commitHash = "unknown";
        std::string cmdRev = "git -C " + shellQuote(depPath.string()) + " rev-parse HEAD 2>/dev/null";
        FILE* pipe = popen(cmdRev.c_str(), "r");
        if (pipe) {
            char buf[128];
            if (fgets(buf, sizeof(buf), pipe)) {
                commitHash = trim(std::string(buf));
            }
            pclose(pipe);
        }
        lockCommits[depName] = commitHash;
    }

    fs::path lockPath = root / "config.lock";
    std::ofstream lk(lockPath);
    lk << "# Loom Dependency Lockfile - Automatically generated by 'loom fetch'\n";
    lk << "# Do not edit manually.\n\n";
    for (const auto& [depName, repoUrl] : config.dependencies) {
        lk << "[[package]]\n";
        lk << "name = \"" << depName << "\"\n";
        lk << "source = \"" << repoUrl << "\"\n";
        lk << "commit = \"" << (lockCommits.count(depName) ? lockCommits[depName] : "unknown") << "\"\n\n";
    }
    std::cout << col.bold << col.green << "Dependencies up to date and locked in config.lock." << col.reset << "\n";
}

std::string formatLoomSource(const std::string& content) {
    static const std::unordered_map<std::string, std::string> keywords = {
        {"block", "Block"}, {"takes", "Takes"}, {"produces", "Produces"},
        {"declares", "Declares"}, {"implements", "Implements"}, {"end", "End"},
        {"if", "If"}, {"else", "Else"}, {"loop", "Loop"}, {"match", "Match"},
        {"when", "When"}, {"give", "Give"}, {"exit", "Exit"}, {"skip", "Skip"},
        {"true", "True"}, {"false", "False"}, {"module", "Module"},
        {"import", "Import"}, {"use", "use"}, {"ensure", "Ensure"}
    };

    std::vector<std::string> lines;
    {
        std::istringstream stream(content);
        std::string l;
        while (std::getline(stream, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }

    std::vector<std::string> processedLines;
    for (const auto& line : lines) {
        std::string stripped = trim(line);
        if (stripped.empty()) {
            processedLines.push_back("");
            continue;
        }

        std::string comment;
        std::string codePart = stripped;
        bool inQ = false;
        int hashIdx = -1;
        for (size_t idx = 0; idx < stripped.size(); ++idx) {
            char ch = stripped[idx];
            if (ch == '"') inQ = !inQ;
            else if (ch == '#' && !inQ) {
                hashIdx = static_cast<int>(idx);
                break;
            }
        }
        if (hashIdx >= 0) {
            codePart = trim(stripped.substr(0, hashIdx));
            comment = stripped.substr(hashIdx);
        }

        std::vector<std::pair<bool, std::string>> parts;
        std::string cur;
        inQ = false;
        for (size_t i = 0; i < codePart.size(); ++i) {
            char ch = codePart[i];
            if (ch == '"') {
                if (inQ) {
                    cur.push_back(ch);
                    parts.push_back({true, cur});
                    cur.clear();
                    inQ = false;
                } else {
                    if (!cur.empty()) {
                        parts.push_back({false, cur});
                        cur.clear();
                    }
                    cur.push_back(ch);
                    inQ = true;
                }
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) parts.push_back({inQ, cur});

        std::string rebuilt;
        for (const auto& p : parts) {
            if (p.first) {
                rebuilt += p.second;
            } else {
                std::string s = p.second;
                std::string wordNorm;
                for (size_t i = 0; i < s.size(); ) {
                    if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') {
                        size_t start = i;
                        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) i++;
                        std::string word = s.substr(start, i - start);
                        std::string lowerW = toLower(word);
                        auto it = keywords.find(lowerW);
                        if (it != keywords.end()) wordNorm += it->second;
                        else wordNorm += word;
                    } else {
                        wordNorm.push_back(s[i++]);
                    }
                }
                s = wordNorm;

                static const std::vector<std::string> compoundOps = {
                    "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", ".."
                };
                for (const auto& op : compoundOps) {
                    size_t pos = 0;
                    while ((pos = s.find(op, pos)) != std::string::npos) {
                        s.replace(pos, op.size(), " " + op + " ");
                        pos += op.size() + 2;
                    }
                }
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == '=') {
                        bool prevOp = (i > 0 && (s[i-1] == '+' || s[i-1] == '-' || s[i-1] == '*' || s[i-1] == '/' || s[i-1] == '=' || s[i-1] == '<' || s[i-1] == '>' || s[i-1] == '!'));
                        bool nextOp = (i + 1 < s.size() && s[i+1] == '=');
                        if (!prevOp && !nextOp) {
                            s.replace(i, 1, " = ");
                            i += 2;
                        }
                    }
                }
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == ',') {
                        s.replace(i, 1, ", ");
                        i += 1;
                    }
                }
                std::string collapsed;
                bool prevSpace = false;
                for (char ch : s) {
                    if (std::isspace(static_cast<unsigned char>(ch))) {
                        if (!prevSpace) { collapsed.push_back(' '); prevSpace = true; }
                    } else {
                        collapsed.push_back(ch);
                        prevSpace = false;
                    }
                }
                rebuilt += collapsed;
            }
        }

        std::string resLine = trim(rebuilt);
        if (!comment.empty()) {
            resLine = resLine.empty() ? comment : (resLine + " " + comment);
        }
        processedLines.push_back(resLine);
    }

    std::vector<std::string> outLines(processedLines.size());
    struct DeclItem {
        std::string id;
        std::string type;
        std::string comment;
    };
    std::vector<DeclItem> declGroup;
    std::vector<size_t> declIndices;

    auto flushDecls = [&](int baseIndent) {
        if (declGroup.empty()) return;
        size_t maxId = 0;
        for (const auto& item : declGroup) {
            maxId = std::max(maxId, item.id.size());
        }
        for (size_t k = 0; k < declGroup.size(); ++k) {
            std::string pad(maxId - declGroup[k].id.size(), ' ');
            std::string aligned = declGroup[k].id + pad + " : " + declGroup[k].type;
            if (!declGroup[k].comment.empty()) aligned += " " + declGroup[k].comment;
            outLines[declIndices[k]] = std::string(baseIndent * 4, ' ') + aligned;
        }
        declGroup.clear();
        declIndices.clear();
    };

    int indent = 0;
    bool inDecl = false;
    bool inMatch = false;

    for (size_t i = 0; i < processedLines.size(); ++i) {
        const auto& line = processedLines[i];
        if (line.empty()) {
            outLines[i] = "";
            continue;
        }

        std::istringstream ls(line);
        std::string firstWord;
        ls >> firstWord;
        std::string secondWord;
        ls >> secondWord;

        if (firstWord == "Declares" || firstWord == "Takes" || firstWord == "Implements") {
            flushDecls(1);
            inDecl = (firstWord == "Declares" || firstWord == "Takes");
            indent = 0;
            outLines[i] = line;
            indent = 1;
            continue;
        }
        if (firstWord == "Block" || firstWord == "Record" || firstWord == "Enum") {
            flushDecls(1);
            inDecl = (firstWord == "Record");
            indent = 0;
            outLines[i] = line;
            indent = 1;
            continue;
        }
        if (firstWord == "End") {
            flushDecls(1);
            inDecl = false;
            if (secondWord == "If" || secondWord == "Loop") {
                indent = std::max(1, indent - 1);
                outLines[i] = std::string(indent * 4, ' ') + line;
            } else if (secondWord == "Match") {
                indent = 1;
                inMatch = false;
                outLines[i] = std::string(indent * 4, ' ') + line;
            } else {
                indent = 0;
                outLines[i] = line;
            }
            continue;
        }
        if (firstWord == "Produces") {
            flushDecls(1);
            inDecl = false;
            outLines[i] = line;
            continue;
        }
        if (firstWord == "Match") {
            outLines[i] = std::string(indent * 4, ' ') + line;
            inMatch = true;
            continue;
        }
        if (firstWord == "When" && inMatch) {
            outLines[i] = std::string(8, ' ') + line;
            indent = 3;
            continue;
        }
        if (firstWord == "Else" && inMatch) {
            outLines[i] = std::string(8, ' ') + line;
            indent = 3;
            continue;
        }
        if (firstWord == "If" || firstWord == "Loop") {
            outLines[i] = std::string(indent * 4, ' ') + line;
            indent++;
            continue;
        }
        if (firstWord == "Else") {
            outLines[i] = std::string(std::max(0, indent - 1) * 4, ' ') + line;
            continue;
        }

        if (inDecl && line.find(':') != std::string::npos) {
            size_t colonPos = line.find(':');
            std::string idPart = trim(line.substr(0, colonPos));
            std::string rest = trim(line.substr(colonPos + 1));
            std::string comm;
            size_t commPos = rest.find('#');
            if (commPos != std::string::npos) {
                comm = rest.substr(commPos);
                rest = trim(rest.substr(0, commPos));
            }
            declGroup.push_back({idPart, rest, comm});
            declIndices.push_back(i);
        } else {
            flushDecls(1);
            outLines[i] = std::string(indent * 4, ' ') + line;
        }
    }
    flushDecls(1);

    std::ostringstream result;
    for (size_t i = 0; i < outLines.size(); ++i) {
        result << outLines[i] << "\n";
    }
    return result.str();
}

void formatFile(const fs::path& filePath, bool checkOnly) {
    std::string content = readFileContent(filePath.string());
    std::string formattedStr = formatLoomSource(content);
    if (checkOnly) {
        if (content != formattedStr) {
            throw Error("formatting differences in " + filePath.string());
        }
    } else if (content != formattedStr) {
        std::ofstream out(filePath);
        out << formattedStr;
        std::cout << "Formatted " << filePath.string() << "\n";
    }
}

void formatProject(const std::vector<std::string>& targets, bool checkOnly) {
    const Colors col = Colors::detect(false);
    for (const auto& t : targets) {
        fs::path p(t);
        if (fs::is_regular_file(p) && p.extension() == ".loom") {
            formatFile(p, checkOnly);
        } else if (fs::is_directory(p)) {
            for (const auto& entry : fs::recursive_directory_iterator(p)) {
                if (entry.is_regular_file() && entry.path().extension() == ".loom") {
                    formatFile(entry.path(), checkOnly);
                }
            }
        }
    }
    std::cout << col.bold << col.green << (checkOnly ? "All files cleanly formatted." : "Formatting complete.") << col.reset << "\n";
}

void runTests(const fs::path& configFile, const char* argv0, const std::vector<std::string>& files,
              const std::string& cliToolchain, bool verbose) {
    (void)configFile;
    (void)cliToolchain;
    const Colors col = Colors::detect(false);
    std::cout << col.bold << "Running Loom tests...\n" << col.reset;

    std::vector<fs::path> testPaths;
    if (!files.empty()) {
        for (const auto& f : files) testPaths.push_back(fs::path(f));
    } else {
        std::error_code ec;
        if (fs::exists("tests", ec)) {
            for (const auto& e : fs::recursive_directory_iterator("tests", ec)) {
                if (e.path().extension() == ".loom") testPaths.push_back(e.path());
            }
        }
        if (testPaths.empty() && fs::exists("src", ec)) {
            for (const auto& e : fs::recursive_directory_iterator("src", ec)) {
                if (e.path().extension() == ".loom") testPaths.push_back(e.path());
            }
        }
    }

    if (testPaths.empty()) {
        std::cout << col.yellow << "No test files found.\n" << col.reset;
        return;
    }

    fs::path exeDir = getExecutablePath(argv0).parent_path();
    fs::path loomc = exeDir / "loomc";
    if (!fs::exists(loomc)) loomc = "loomc";

    int passed = 0;
    int failed = 0;
    for (const auto& tp : testPaths) {
        fs::path outBin = fs::temp_directory_path() / (tp.stem().string() + "_test_runner");
#ifdef _WIN32
        outBin += ".exe";
#endif
        std::vector<std::string> compileCmd{loomc.string(), "--test-runner", "--entry", tp.string(), "-o", outBin.string()};
        if (verbose) {
            std::cout << "  Compiling: ";
            for (const auto& a : compileCmd) std::cout << a << " ";
            std::cout << "\n";
        }
        int status = runCommand(compileCmd);
        if (status != 0) {
            std::cout << col.red << "[FAIL] Compilation failed: " << col.reset << tp.string() << "\n";
            failed++;
            continue;
        }
        std::vector<std::string> execCmd{outBin.string()};
        int execStatus = runCommand(execCmd);
        std::error_code rmEc;
        fs::remove(outBin, rmEc);
        if (execStatus == 0) {
            passed++;
        } else {
            failed++;
            std::cout << col.red << "[FAIL] Test failed: " << col.reset << tp.string() << "\n";
        }
    }

    std::cout << "\nTest Results: "
              << col.bold << col.green << passed << " passed" << col.reset << ", "
              << (failed > 0 ? col.bold + col.red : col.reset) << failed << " failed" << col.reset << "\n";
    if (failed > 0) throw ChildFailure(1);
}

void runLsp(const char* argv0) {
    fs::path exeDir = getExecutablePath(argv0).parent_path();
    fs::path lsp = exeDir / "loom-lsp";
    if (!fs::exists(lsp)) lsp = "loom-lsp";
    std::vector<std::string> cmd{lsp.string()};
    int status = runCommand(cmd);
    if (status != 0) throw ChildFailure(status);
}

void usage() {
    std::cout << "Loom workspace tool 1.4.0\n\nUsage:\n"
                 "  loom new <name>\n"
                 "  loom install <library> [--version <version>]\n"
                 "  loom install toolchain <name> [--version <version>]\n"
                 "  loom toolchain install <name> [--version <version>]\n"
                 "  loom libraries\n"
                 "  loom uninstall <library>\n"
                 "  loom toolchains\n"
                 "  loom use <toolchain>\n"
                 "  loom test [files...] [--config <path>] [--verbose]\n"
                 "  loom fmt [files/dirs...] [--check]\n"
                 "  loom fetch [--config <path>]\n"
                 "  loom doc [--config <path>] [--output <path>]\n"
                 "  loom lsp\n"
                 "  loom build [--release] [--size] [-g|--debug] [--sanitize=<type>] [--no-cache] [--opt-report] [--verbose] [--config <path>] [--toolchain <name>] [--target <triple>]\n"
                 "  loom run [--release] [--size] [-g|--debug] [--sanitize=<type>] [--no-cache] [--verbose] [--config <path>] [--toolchain <name>]\n"
                 "  loom clean [--config <path>]\n"
                 "  loom graph [--config <path>] [--view calls|data] [--format mermaid|json] [--output <path>] [--check] [--toolchain <name>]\n"
                 "  loom --help | --version\n\n"
                 "Commands:\n"
                 "  new         Create a new Loom workspace\n"
                 "  test        Execute unit and integration tests declared in Test blocks\n"
                 "  fmt         Format source code with canonical Loom indentation and style\n"
                 "  fetch       Fetch git dependencies specified in config.toml and update config.lock\n"
                 "  doc         Generate static HTML API documentation from /// comments\n"
                 "  lsp         Launch the native Loom Language Server (loom-lsp)\n"
                 "  get         Fetch and install a library module (alias: loom install)\n"
                 "  install     Install a library or toolchain from the Loom release catalog\n"
                 "  libraries   List installed Loom libraries\n"
                 "  uninstall   Remove a user-installed Loom library\n"
                 "  toolchains  List all installed toolchains and show the active default\n"
                 "  use         Switch the active default toolchain (e.g. 'loom use rp2040', 'loom use native')\n"
                 "  build       Analyze, compile Loom sources, and link a native binary\n"
                 "  run         Build the workspace and run its executable\n"
                 "  clean       Remove configured target directory and compilation cache\n"
                 "  graph       Generate a call or variable-access graph from compiler semantics\n\n"
                 "Flags:\n"
                 "  --toolchain <name>  Override toolchain for this build (e.g. 'native')\n"
                 "  --release           Aggressive optimizations (-O3, native host CPU features, dead strip)\n"
                 "  --size              Optimize for minimal binary size (-Oz, -Wl,-dead_strip, strip symbols)\n"
                 "  -g, --debug         Emit full DWARF debug info for lldb/gdb source-level stepping\n"
                 "  --sanitize=<val>    Enable LLVM sanitizers (address, undefined, thread)\n"
                 "  --no-cache          Bypass incremental compilation cache\n"
                 "  --opt-report        Print comprehensive optimization report (folding, vectorization, bounds)\n"
                 "  --verbose           Print verbose build and execution information\n\n"
                 "Call Mermaid defaults to loom-deps.mmd; data Mermaid to loom-data.mmd; JSON to loom-graph.json.\n"
                 "Output emits ANSI colors when connected to a terminal (respects NO_COLOR).\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { usage(); return 0; }
        const std::string command = argv[1];
        if (command == "--version" || command == "-V") { std::cout << "loom 1.4.0\n"; return 0; }
        if (command == "install" || command == "get") {
            if (argc < 3) throw Error("usage: loom get <library> [--version <version>] or loom install <library> [--version <version>]");
            std::string kind = "library", name, version;
            int i = 2;
            if (std::string(argv[i]) == "toolchain") { kind = "toolchain"; ++i; }
            else if (std::string(argv[i]) == "library") { ++i; }
            if (i >= argc) throw Error("package name is required");
            name = argv[i++];
            while (i < argc) {
                const std::string arg = argv[i++];
                if (arg == "--version" && i < argc) version = argv[i++];
                else throw Error("unknown install argument: " + arg);
            }
            installFromRegistry(kind, name, version);
            return 0;
        }
        if (command == "libraries") { listLibraries(); return 0; }
        if (command == "uninstall" || command == "remove") {
            if (argc != 3) throw Error("usage: loom uninstall <library>");
            removeLibrary(argv[2]);
            return 0;
        }
        if (command == "toolchain" && argc >= 3 && std::string(argv[2]) == "install") {
            if (argc < 4) throw Error("usage: loom toolchain install <name> [--version <version>]");
            std::string version;
            for (int i = 4; i < argc; ++i) {
                if (std::string(argv[i]) == "--version" && i + 1 < argc) version = argv[++i];
                else throw Error("unknown install argument: " + std::string(argv[i]));
            }
            installFromRegistry("toolchain", argv[3], version);
            return 0;
        }
        if (command == "toolchains" || command == "toolchain") {
            bool verbose = false;
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "--verbose" || std::string(argv[i]) == "-v") verbose = true;
            }
            printInstalledToolchains(argv[0], verbose);
            return 0;
        }
        if (command == "use") {
            if (argc < 3) {
                const std::string current = getDefaultToolchainName();
                std::cout << "Active toolchain: " << current << "\n"
                          << "Usage: loom use <toolchain>\n";
                return 0;
            }
            const std::string tcName = argv[2];
            setDefaultToolchain(tcName);
            const Colors col = Colors::detect(false);
            std::cout << col.bold << col.green << "Using toolchain '" << tcName << "'." << col.reset << "\n";
            return 0;
        }
        if (command == "set") {
            if (argc < 4 || std::string(argv[2]) != "default") {
                throw Error("usage: loom use <toolchain> (or loom set default <toolchain>)");
            }
            const std::string tcName = argv[3];
            setDefaultToolchain(tcName);
            const Colors col = Colors::detect(false);
            std::cout << col.bold << col.green << "Using toolchain '" << tcName << "'." << col.reset << "\n";
            return 0;
        }
        if (command == "default") {
            if (argc >= 3) {
                const std::string tcName = argv[2];
                setDefaultToolchain(tcName);
                const Colors col = Colors::detect(false);
                std::cout << col.bold << col.green << "Using toolchain '" << tcName << "'." << col.reset << "\n";
                return 0;
            } else {
                std::string current = getDefaultToolchainName();
                std::cout << "Active toolchain: " << current << "\n";
                return 0;
            }
        }
        if (command == "new") {
            if (argc != 3) throw Error("usage: loom new <name>");
            std::string name = argv[2];
            createProject(name); return 0;
        }
        if (command == "test") {
            fs::path config = "config.toml";
            std::string cliToolchain;
            bool verbose = false;
            std::vector<std::string> testFiles;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--config" && i + 1 < argc) config = argv[++i];
                else if (arg == "--toolchain" && i + 1 < argc) cliToolchain = argv[++i];
                else if (arg == "--verbose" || arg == "-v") verbose = true;
                else testFiles.push_back(arg);
            }
            runTests(config, argv[0], testFiles, cliToolchain, verbose);
            return 0;
        }
        if (command == "fmt") {
            bool check = false;
            std::vector<std::string> targets;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--check") check = true;
                else targets.push_back(arg);
            }
            if (targets.empty()) targets.push_back(".");
            formatProject(targets, check);
            return 0;
        }
        if (command == "fetch") {
            fs::path config = "config.toml";
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--config" && i + 1 < argc) config = argv[++i];
            }
            fetchDependencies(config);
            return 0;
        }
        if (command == "lsp") {
            runLsp(argv[0]);
            return 0;
        }
        if (command == "doc") {
            fs::path config = "config.toml";
            std::optional<fs::path> docOutput;
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--config" && i + 1 < argc) config = argv[++i];
                else if (arg == "--output" && i + 1 < argc) docOutput = fs::path(argv[++i]);
                else throw Error("unknown argument: " + arg);
            }
            docProject(config, argv[0], docOutput);
            return 0;
        }
        if (command == "build" || command == "clean" || command == "run" || command == "graph") {
            fs::path config = "config.toml";
            bool release = false, sizeOpt = false, verbose = false, check = false;
            bool optReport = false;
            bool debug = false, noCache = false;
            std::string sanitize;
            std::string cliToolchain;
            std::optional<fs::path> graphOutput;
            std::string graphFormat = "mermaid";
            std::string graphView = "calls";
            std::string cliTarget;
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--release" && (command == "build" || command == "run")) release = true;
                else if ((arg == "--size" || arg == "-Oz") && (command == "build" || command == "run")) { sizeOpt = true; release = true; }
                else if ((arg == "-g" || arg == "--debug") && (command == "build" || command == "run")) debug = true;
                else if (arg.rfind("--sanitize=", 0) == 0 && (command == "build" || command == "run")) sanitize = arg.substr(11);
                else if (arg == "--no-cache" && (command == "build" || command == "run")) noCache = true;
                else if (arg == "--opt-report" && (command == "build" || command == "run")) optReport = true;
                else if (arg == "--verbose" && (command == "build" || command == "run")) verbose = true;
                else if (arg == "--check" && command == "graph") check = true;
                else if (arg == "--config" && i + 1 < argc) config = argv[++i];
                else if (arg == "--toolchain" && i + 1 < argc) cliToolchain = argv[++i];
                else if (arg == "--target" && i + 1 < argc) cliTarget = argv[++i];
                else if (arg.rfind("--target=", 0) == 0) cliTarget = arg.substr(9);
                else if (arg == "--output" && i + 1 < argc && command == "graph") graphOutput = fs::path(argv[++i]);
                else if (arg == "--format" && i + 1 < argc && command == "graph") graphFormat = argv[++i];
                else if (arg == "--view" && i + 1 < argc && command == "graph") graphView = argv[++i];
                else throw Error("unknown argument: " + arg);
            }
            if (command == "graph" && graphFormat != "mermaid" && graphFormat != "json") throw Error("unsupported graph format '" + graphFormat + "' (supported: mermaid, json)");
            if (command == "graph" && graphView != "calls" && graphView != "data") throw Error("unsupported graph view '" + graphView + "' (supported: calls, data)");
            if (command == "build") buildProject(config, argv[0], release, verbose, /*quiet=*/false, optReport, cliToolchain, debug, sanitize, noCache, cliTarget, sizeOpt);
            else if (command == "run") runProject(config, argv[0], release, verbose, optReport, cliToolchain, debug, sanitize, noCache, cliTarget, sizeOpt);
            else if (command == "graph") graphProject(config, argv[0], graphOutput, check, graphView, graphFormat, cliToolchain);
            else cleanProject(config);
            return 0;
        }
        throw Error("unknown command '" + command + "' (use 'loom --help')");
    } catch (const Error& e) {
        const Colors errCol = Colors::detect(true);
        std::cerr << errCol.bold << errCol.red << "loom: error: " << errCol.reset << errCol.bold << e.what() << errCol.reset << "\n";
        return 1;
    } catch (const ChildFailure& e) {
        return e.status;
    } catch (const std::exception& e) {
        const Colors errCol = Colors::detect(true);
        std::cerr << errCol.bold << errCol.red << "loom: internal error: " << errCol.reset << errCol.bold << e.what() << errCol.reset << "\n";
        return 1;
    }
}
