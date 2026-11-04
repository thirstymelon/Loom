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

fs::path findObjcopy() {
    if (const char* envObjcopy = std::getenv("LLVM_OBJCOPY")) {
        if (*envObjcopy) return envObjcopy;
    }
    for (const auto& candidate : {
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
    std::string cpu;
    bool baremetal = false;
    std::string linkerScript;
    std::vector<std::string> assemblySources;
    bool uf2Output = false;
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
        std::replace(key.begin(), key.end(), '-', '_');
        const std::string value = trim(line.substr(eq + 1));
        if (section == "project" && key == "name") c.name = unquote(value, lineNo);
        else if (section == "project" && key == "version") (void)unquote(value, lineNo);
        else if (section == "build" && (key == "main" || key == "main_file")) c.main = unquote(value, lineNo);
        else if (section == "build" && key == "sources") c.sources = parseStringArray(value, lineNo);
        else if (section == "build" && key == "target_dir") c.targetDir = unquote(value, lineNo);
        else if (section == "build" && (key == "executable" || key == "output")) c.executable = unquote(value, lineNo);
        else if (section == "build" && key == "target") c.target = unquote(value, lineNo);
        else if (section == "build" && key == "cpu") c.cpu = unquote(value, lineNo);
        else if (section == "build" && key == "baremetal") c.baremetal = (value == "true");
        else if (section == "build" && (key == "linker_script" || key == "linker")) c.linkerScript = unquote(value, lineNo);
        else if (section == "build" && (key == "assembly_sources" || key == "assembly")) c.assemblySources = parseStringArray(value, lineNo);
        else if (section == "build" && (key == "uf2_output" || key == "uf2")) c.uf2Output = (value == "true");
        else if (section == "toolchain" && (key == "name" || key == "toolchain")) c.toolchain = unquote(value, lineNo);
        else if (section == "toolchain" && key == "clang") c.clang = unquote(value, lineNo);
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
        addDir(root / "targets" / "rp2040" / "bsp");
        addDir(root / "targets" / "rp2040");
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

struct ToolchainInfo {
    std::string name;
    std::string kind;
    std::string version = "1.1.0";
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
            nat.version = "1.1.0";
            nat.description = "Loom Native Desktop Toolchain (workspace development)";
            nat.rootDir = fs::current_path();
            nat.loomcPath = localLoomc;
            nat.sdkPath = fs::current_path() / "sdk";
            nat.isDefault = (defaultName == "native");
            results.push_back(nat);

            ToolchainInfo rp;
            rp.name = "rp2040";
            rp.kind = "rp2040";
            rp.version = "1.1.0";
            rp.description = "Loom RP2040 Embedded Bare-Metal Toolchain (workspace development)";
            rp.rootDir = fs::current_path();
            rp.loomcPath = localLoomc;
            rp.sdkPath = fs::current_path() / "sdk";
            rp.isDefault = (defaultName == "rp2040");
            results.push_back(rp);
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
                  << "Install a toolchain package (.pkg) from Loom releases.\n" << col.reset;
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
            activeTc.version = "1.1.0";
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

    bool isRp2040Project = config.baremetal ||
                           config.target.find("thumb") != std::string::npos ||
                           config.target.find("none-eabi") != std::string::npos ||
                           config.cpu == "cortex-m0plus";

    // If config.toml explicitly configured a toolchain name and it conflicts with active toolchain:
    if (!configToolchain.empty() && configToolchain != activeTc.name && configToolchain != activeTc.kind && cliOverride.empty()) {
        if (configToolchain == "native" && activeTc.kind == "rp2040") {
            throw Error("project '" + config.name + "' is configured for the 'native' toolchain, but active toolchain is 'rp2040'.\n"
                        "  Please switch to the 'native' desktop toolchain:\n"
                        "    loom use native\n"
                        "  or specify the toolchain for this build:\n"
                        "    loom build --toolchain native");
        }
        if (configToolchain == "rp2040" && activeTc.kind == "native") {
            throw Error("project '" + config.name + "' is configured for the 'rp2040' toolchain, but active toolchain is 'native'.\n"
                        "  Please switch to the 'rp2040' embedded toolchain:\n"
                        "    loom use rp2040\n"
                        "  or specify the toolchain for this build:\n"
                        "    loom build --toolchain rp2040");
        }
    }

    if (activeTc.kind == "native" && isRp2040Project) {
        throw Error("target '" + (config.target.empty() ? "thumbv6m-none-eabi" : config.target) +
                    "' is not supported by the Loom native desktop toolchain.\n"
                    "  Please install and switch to the 'rp2040' embedded toolchain:\n"
                    "    loom use rp2040\n"
                    "  or specify the toolchain for this build:\n"
                    "    loom build --toolchain rp2040");
    }

    if (activeTc.kind == "rp2040" && !isRp2040Project) {
        throw Error("desktop host target is not supported by the Loom 'rp2040' embedded toolchain.\n"
                    "  Please switch to the 'native' desktop toolchain:\n"
                    "    loom use native\n"
                    "  or specify the toolchain for this build:\n"
                    "    loom build --toolchain native");
    }

    return activeTc;
}

void createProject(const std::string& name, bool rp2040 = false) {
    fs::path namePath(name);
    if (name.empty() || namePath.has_parent_path() || name == "." || name == ".." ||
        !std::regex_match(name, std::regex("[A-Za-z0-9_-]+")))
        throw Error("project name must contain only letters, digits, '-' or '_' and be a single directory name");
    if (fs::exists(namePath)) throw Error("destination already exists: " + name);
    try {
        fs::create_directories(namePath / "src");
        if (rp2040) {
            std::ofstream config(namePath / "config.toml");
            config << "[project]\n"
                   << "name = \"" << name << "\"\n"
                   << "version = \"1.1.0\"\n\n"
                   << "[build]\n"
                   << "main = \"src/main.loom\"\n"
                   << "sources = [\"src/*.loom\"]\n"
                   << "target = \"thumbv6m-none-eabi\"\n"
                   << "cpu = \"cortex-m0plus\"\n"
                   << "baremetal = true\n"
                   << "uf2-output = true\n\n"
                   << "[toolchain]\n"
                   << "clang = \"clang\"\n";
            if (!config) throw Error("failed to write config.toml");

            std::ofstream wokwi(namePath / "wokwi.toml");
            wokwi << "[wokwi]\n"
                  << "version = 1\n"
                  << "firmware = \"target/" << name << ".elf\"\n"
                  << "elf = \"target/" << name << ".elf\"\n";
            if (!wokwi) throw Error("failed to write wokwi.toml");

            std::ofstream diagram(namePath / "diagram.json");
            diagram << "{\n"
                    << "  \"version\": 1,\n"
                    << "  \"author\": \"Loom Language Team\",\n"
                    << "  \"editor\": \"wokwi\",\n"
                    << "  \"parts\": [\n"
                    << "    { \"type\": \"board-pi-pico\", \"id\": \"pico\", \"top\": 0, \"left\": 0, \"attrs\": {} }\n"
                    << "  ],\n"
                    << "  \"connections\": [\n"
                    << "    [ \"pico:GP0\", \"$serialMonitor:RX\", \"\", [] ],\n"
                    << "    [ \"pico:GP1\", \"$serialMonitor:TX\", \"\", [] ]\n"
                    << "  ]\n"
                    << "}\n";
            if (!diagram) throw Error("failed to write diagram.json");

            std::ofstream main(namePath / "src" / "main.loom");
            main << "Module Main\n\n"
                 << "Import System\n"
                 << "Import GPIO\n"
                 << "Import Time\n"
                 << "Import UART\n\n"
                 << "use System\n"
                 << "use GPIO\n"
                 << "use Time\n"
                 << "use UART\n\n"
                 << "Block Global\n"
                 << "Declares\n"
                 << "    LED_PIN : Constant Signed_64 = 25\n"
                 << "End Global\n\n"
                 << "Block Main\n"
                 << "Implements\n"
                 << "    System_Init()\n"
                 << "    Init_Pin(LED_PIN)\n"
                 << "    Set_Direction(LED_PIN, PIN_OUTPUT)\n"
                 << "    UART_Init(115200)\n\n"
                 << "    Write_UART(\"=================================\\n\")\n"
                 << "    Write_UART(\"Raspberry Pi Pico initialized!\\n\")\n"
                 << "    Write_UART(\"=================================\\n\")\n\n"
                 << "    Loop\n"
                 << "        Toggle(LED_PIN)\n"
                 << "        Write_UART(\"Heartbeat at uptime: <Get_Time()> ms\\n\")\n"
                 << "        Delay(1000)\n"
                 << "    End Loop\n"
                 << "End Main\n";
            if (!main) throw Error("failed to write src/main.loom");

            std::ofstream gitignore(namePath / ".gitignore");
            gitignore << "target/\n";

            std::ofstream readme(namePath / "README.md");
            readme << "# " << name << "\n\n"
                   << "Bare-metal firmware written in Loom for the Raspberry Pi RP2040 (Raspberry Pi Pico), powered by the Loom SDK.\n\n"
                   << "## Building\n\n"
                   << "```bash\nloom build\n```\n\n"
                   << "Produces:\n"
                   << "- `target/" << name << ".elf` — ELF binary with debug symbols\n"
                   << "- `target/" << name << ".bin` — Raw flat binary\n"
                   << "- `target/" << name << ".uf2` — Flashable image for the Pico\n\n"
                   << "## Flashing\n\n"
                   << "1. Hold the BOOTSEL button while plugging your Pico into USB.\n"
                   << "2. Drag and drop `target/" << name << ".uf2` into the `RPI-RP2` volume.\n\n"
                   << "## Wokwi Simulation\n\n"
                   << "Run in VS Code with the Wokwi extension or headlessly with `wokwi-cli`.\n";

            std::cout << "Created bare-metal RP2040 Loom workspace " << name << " (using Loom SDK)\n"
                      << "  Config:    config.toml\n"
                      << "  Firmware:  src/main.loom\n"
                      << "  Wokwi:     wokwi.toml, diagram.json\n\n"
                      << "Next steps:\n"
                      << "  cd " << shellQuote(name) << "\n"
                      << "  loom build\n"
                      << "  # Flashing: Drag target/" << name << ".uf2 to RPI-RP2 volume\n"
                      << "  # Simulation: Open in VS Code and run 'Wokwi: Start Simulator'\n";
            return;
        }

        std::ofstream config(namePath / "config.toml");
        config << "[project]\nname = \"" << name << "\"\nversion = \"1.1.0\"\n\n[build]\nmain = \"src/main.loom\"\nsources = [\"src/*.loom\"]\ntarget-dir = \"target\"\nexecutable = \"" << name << "\"\n\n[toolchain]\nclang = \"clang\"\n";
        if (!config) throw Error("failed to write config.toml");
        std::ofstream main(namePath / "src" / "main.loom");
        main << "Module Main\n\nBlock Main\n    Implements\n        Write(\"Hello, Loom!\\n\")\nEnd Main\n";
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
void buildProject(const fs::path& configFile, const char* argv0, bool release, bool verbose = false, bool quiet = false, bool optReport = false, const std::string& cliToolchain = "") {
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const ToolchainInfo activeTc = resolveActiveToolchain(cliToolchain, config.toolchain, config, argv0);
    setEnvVar("LOOM_TOOLCHAIN_KIND", activeTc.kind);
    const auto sdkDirs = getSdkIncludeDirs(argv0, activeTc.sdkPath.string());
    const auto sources = sourceFiles(root, config, sdkDirs);
    const auto target = workspacePath(root, config.targetDir, "build.target-dir");
    if (target == root) throw Error("build.target-dir cannot be the workspace root");
    const auto compiler = activeTc.loomcPath.empty() ? loomcPath(argv0) : activeTc.loomcPath;
    const Colors col = Colors::detect(false);
    if (!quiet) {
        std::cout << col.bold << col.cyan << "Loom 1.1.0" << col.reset
                  << col.dim << " | " << col.reset
                  << col.bold << "Building " << config.name << col.reset << "\n\n"
                  << col.bold << col.blue << "Configuration" << col.reset << "\n"
                  << col.dim << "  Project:     " << col.reset << config.name << "\n"
                  << col.dim << "  Main source: " << col.reset << config.main << "\n"
                  << col.dim << "  Sources:     " << col.reset << sources.size() << " files\n"
                  << col.dim << "  Output:      " << col.reset << config.targetDir << "/" << config.executable << "\n"
                  << col.dim << "  Toolchain:   " << col.reset << activeTc.name << " (" << activeTc.kind << ")\n"
                  << col.dim << "  Mode:        " << col.reset
                  << (release ? (col.bold + col.green + "Release") : (col.bold + col.yellow + "Debug")) << col.reset << "\n";
        if (!config.target.empty()) {
            std::cout << col.dim << "  Target:      " << col.reset << config.target << "\n";
        }
        if (!config.cpu.empty()) {
            std::cout << col.dim << "  CPU:         " << col.reset << config.cpu << "\n";
        }
        if (config.baremetal) {
            std::cout << col.dim << "  Bare-metal:  " << col.reset << "true\n";
        }
        if (verbose) {
            std::cout << col.dim << "  Compiler:    " << col.reset << compiler.string() << "\n";
            if (!activeTc.rootDir.empty()) {
                std::cout << col.dim << "  Root:        " << col.reset << activeTc.rootDir.string() << "\n";
            }
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
    const fs::path objectDir = target / "obj";
    fs::create_directories(objectDir);
    for (const auto& item : fs::directory_iterator(objectDir))
        if (item.is_regular_file() && item.path().extension() == ".o") fs::remove(item.path());
    const std::string objFileName = toLower(config.name) + ".o";
    const fs::path object = objectDir / objFileName;
    if (!quiet) {
        std::cout << "\n" << col.bold << col.blue << "Compilation" << col.reset << "\n"
                  << "  " << col.bold << col.green << "[1/1]" << col.reset << " " << sources.size() << " Loom source files "
                  << col.dim << "->" << col.reset << " "
                  << col.cyan << object.lexically_relative(root).generic_string() << col.reset << "\n" << std::flush;
    }
    std::vector<std::string> compile{compiler.string(), "--object", "--output", object.string()};
    if (config.baremetal) {
        compile.push_back("--baremetal");
    } else {
        compile.push_back("--entry");
    }
    if (!config.target.empty()) {
        compile.push_back("--target");
        compile.push_back(config.target);
    }
    if (!config.cpu.empty()) {
        compile.push_back("--cpu");
        compile.push_back(config.cpu);
    }
    if (release) compile.push_back("--release");
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
    if (compileStatus != 0) throw ChildFailure(compileStatus);

    std::vector<std::string> assemblySources = config.assemblySources;
    if (assemblySources.empty() && config.baremetal && (config.target == "thumbv6m-none-eabi" || config.cpu == "cortex-m0plus")) {
        for (const auto& sdkDir : sdkDirs) {
            fs::path cand = sdkDir / "boot.s";
            if (fs::is_regular_file(cand)) {
                assemblySources.push_back(cand.string());
                break;
            }
        }
    }

    std::vector<fs::path> extraObjects;
    if (!assemblySources.empty()) {
        if (!quiet) {
            std::cout << "\n" << col.bold << col.blue << "Assembling" << col.reset << "\n";
        }
        for (const auto& asmPat : assemblySources) {
            fs::path asmPath;
            if (fs::is_regular_file(asmPat)) {
                asmPath = asmPat;
            } else {
                asmPath = weakCanonical(root / asmPat);
                if (!fs::is_regular_file(asmPath)) {
                    bool foundSdk = false;
                    for (const auto& sdkDir : sdkDirs) {
                        fs::path cand = sdkDir / asmPat;
                        if (fs::is_regular_file(cand)) { asmPath = cand; foundSdk = true; break; }
                        fs::path candStem = sdkDir / fs::path(asmPat).filename();
                        if (fs::is_regular_file(candStem)) { asmPath = candStem; foundSdk = true; break; }
                    }
                    if (!foundSdk) throw Error("assembly source file not found: " + asmPath.string());
                }
            }
            fs::path asmObj = objectDir / (asmPath.stem().string() + ".o");
            extraObjects.push_back(asmObj);
            if (!quiet) {
                std::cout << "  " << col.bold << col.green << "[asm]" << col.reset << " "
                          << (isWithin(asmPath, root) ? asmPath.lexically_relative(root).generic_string() : asmPath.filename().generic_string())
                          << " " << col.dim << "->" << col.reset << " "
                          << asmObj.lexically_relative(root).generic_string() << "\n";
            }
            std::vector<std::string> asmCmd{config.clang, "-c", asmPath.string(), "-o", asmObj.string()};
            if (!config.target.empty()) {
                asmCmd.push_back("--target=" + config.target);
            }
            if (!config.cpu.empty()) {
                asmCmd.push_back("-mcpu=" + config.cpu);
            }
            if (verbose) {
                std::cout << "  Command: ";
                for (const auto& a : asmCmd) std::cout << shellQuote(a) << " ";
                std::cout << "\n";
            }
            int asmStatus = runCommand(asmCmd);
            if (asmStatus != 0) throw ChildFailure(asmStatus);
        }
    }

    fs::path output = target / config.executable;
    if (config.baremetal) {
        if (output.extension() != ".elf") output += ".elf";
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
    if (config.baremetal) {
        command.push_back("-nostdlib");
        command.push_back("-fuse-ld=lld");
        if (!config.target.empty()) command.push_back("--target=" + config.target);
        if (!config.cpu.empty()) command.push_back("-mcpu=" + config.cpu);
        std::string linkerScript = config.linkerScript;
        if (linkerScript.empty()) {
            for (const auto& sdkDir : sdkDirs) {
                fs::path cand = sdkDir / "linker.ld";
                if (fs::is_regular_file(cand)) {
                    linkerScript = cand.string();
                    break;
                }
            }
        }
        if (!linkerScript.empty()) {
            fs::path lds;
            if (fs::is_regular_file(linkerScript)) {
                lds = linkerScript;
            } else {
                lds = weakCanonical(root / linkerScript);
                if (!fs::is_regular_file(lds)) {
                    bool foundSdk = false;
                    for (const auto& sdkDir : sdkDirs) {
                        fs::path cand = sdkDir / linkerScript;
                        if (fs::is_regular_file(cand)) { lds = cand; foundSdk = true; break; }
                        fs::path candStem = sdkDir / fs::path(linkerScript).filename();
                        if (fs::is_regular_file(candStem)) { lds = candStem; foundSdk = true; break; }
                    }
                    if (!foundSdk) throw Error("linker script not found: " + lds.string());
                }
            }
            command.push_back("-T" + lds.string());
        }
        command.push_back("-Wl,--gc-sections");
        command.push_back("-Wl,--icf=all");
        command.push_back("-Wl,--print-memory-usage");
    } else {
#if defined(__APPLE__)
        const char* envDep = std::getenv("MACOSX_DEPLOYMENT_TARGET");
        if (envDep && *envDep) {
            command.push_back(std::string("-mmacosx-version-min=") + envDep);
        }
#endif
        if (release) {
            command.push_back("-O3");
#if defined(__APPLE__)
            command.push_back("-Wl,-dead_strip");
#elif !defined(_WIN32)
            command.push_back("-Wl,--gc-sections");
#endif
        }
    }
    command.push_back(object.string());
    for (const auto& extraObj : extraObjects) {
        command.push_back(extraObj.string());
    }
    command.push_back("-o"); command.push_back(stagedOutput.string());
    if (!quiet) {
        std::cout << "\n" << col.bold << col.blue << "Linking" << col.reset << "\n"
                  << col.dim << "  Objects:     " << col.reset << (1 + extraObjects.size()) << " (" << object.lexically_relative(root).generic_string();
        for (const auto& eo : extraObjects) {
            std::cout << ", " << eo.lexically_relative(root).generic_string();
        }
        std::cout << ")\n"
                  << col.dim << "  Executable:  " << col.reset << col.cyan << output.lexically_relative(root).generic_string() << col.reset << "\n" << std::flush;
        if (verbose) std::cout << col.dim << "  Linker:      " << col.reset << config.clang << "\n";
    }
    if (runCommand(command) != 0) {
        std::error_code ignored; fs::remove(stagedOutput, ignored);
        throw ChildFailure(1);
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
    if (config.baremetal) {
        fs::path binPath = output;
        binPath.replace_extension(".bin");
        fs::path objcopy = findObjcopy();
        std::vector<std::string> objcopyCmd{objcopy.string(), "-O", "binary", output.string(), binPath.string()};
        if (verbose) {
            std::cout << "  Command: ";
            for (const auto& a : objcopyCmd) std::cout << shellQuote(a) << " ";
            std::cout << "\n";
        }
        if (runCommand(objcopyCmd) != 0) {
            throw Error("failed to generate flat binary with objcopy: " + binPath.string());
        }
        if (!quiet) {
            std::cout << col.dim << "  Binary:      " << col.reset << binPath.lexically_relative(root).generic_string() << "\n";
        }
        if (config.uf2Output) {
            fs::path uf2Path = output;
            uf2Path.replace_extension(".uf2");
            packageUF2(binPath, uf2Path, 0x10000000);
            if (!quiet) {
                std::cout << col.dim << "  UF2 image:   " << col.reset << col.bold << col.cyan << uf2Path.lexically_relative(root).generic_string() << col.reset << "\n";
            }
        }
    }
    if (!quiet) {
        std::cout << "\n" << col.bold << col.green << "Build succeeded: " << col.reset
                  << col.bold << output.lexically_relative(root).generic_string() << col.reset << "\n" << std::flush;
    }
}
void runProject(const fs::path& configFile, const char* argv0, bool release, bool verbose = false, bool optReport = false, const std::string& cliToolchain = "") {
    buildProject(configFile, argv0, release, verbose, /*quiet=*/!verbose, optReport, cliToolchain);
    const fs::path configPath = weakCanonical(configFile);
    const fs::path root = weakCanonical(configPath.parent_path());
    const Config config = readConfig(configPath);
    const Colors col = Colors::detect(false);
    if (config.baremetal) {
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
    fs::path executable = workspacePath(root, config.targetDir, "build.target-dir") / config.executable;
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
void usage() {
    std::cout << "Loom workspace tool 1.1.0\n\nUsage:\n"
                 "  loom new <name> [--rp2040]\n"
                 "  loom toolchains\n"
                 "  loom use <toolchain>\n"
                 "  loom build [--release] [--opt-report] [--verbose] [--config <path>] [--toolchain <name>]\n"
                 "  loom run [--release] [--verbose] [--config <path>] [--toolchain <name>]\n"
                 "  loom clean [--config <path>]\n"
                 "  loom graph [--config <path>] [--view calls|data] [--format mermaid|json] [--output <path>] [--check] [--toolchain <name>]\n"
                 "  loom --help | --version\n\n"
                 "Commands:\n"
                 "  new         Create a workspace (use --rp2040 for Raspberry Pi Pico bare-metal)\n"
                 "  toolchains  List all installed toolchains and show the active default\n"
                 "  use         Switch the active default toolchain (e.g. 'loom use rp2040', 'loom use native')\n"
                 "  build       Analyze, compile Loom sources, and link a native or bare-metal binary\n"
                 "  run         Build the workspace and run its executable (or show Pico flashing steps)\n"
                 "  clean       Remove only the configured target directory\n"
                 "  graph       Generate a call or variable-access graph from compiler semantics\n\n"
                 "Flags:\n"
                 "  --toolchain <name>  Override toolchain for this build (e.g. 'native', 'rp2040')\n"
                 "  --release           Aggressive optimizations (-O3, native host CPU features, dead strip)\n"
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
        if (command == "--version" || command == "-V") { std::cout << "loom 1.1.0\n"; return 0; }
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
            if (argc < 3 || argc > 4) throw Error("usage: loom new <name> [--rp2040]");
            std::string name;
            bool rp2040 = false;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--rp2040") rp2040 = true;
                else if (name.empty()) name = arg;
                else throw Error("unexpected argument: " + arg);
            }
            if (name.empty()) throw Error("usage: loom new <name> [--rp2040]");
            createProject(name, rp2040); return 0;
        }
        if (command == "build" || command == "clean" || command == "run" || command == "graph") {
            fs::path config = "config.toml";
            bool release = false, verbose = false, check = false;
            bool optReport = false;
            std::string cliToolchain;
            std::optional<fs::path> graphOutput;
            std::string graphFormat = "mermaid";
            std::string graphView = "calls";
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--release" && (command == "build" || command == "run")) release = true;
                else if (arg == "--opt-report" && (command == "build" || command == "run")) optReport = true;
                else if (arg == "--verbose" && (command == "build" || command == "run")) verbose = true;
                else if (arg == "--check" && command == "graph") check = true;
                else if (arg == "--config" && i + 1 < argc) config = argv[++i];
                else if (arg == "--toolchain" && i + 1 < argc) cliToolchain = argv[++i];
                else if (arg == "--output" && i + 1 < argc && command == "graph") graphOutput = fs::path(argv[++i]);
                else if (arg == "--format" && i + 1 < argc && command == "graph") graphFormat = argv[++i];
                else if (arg == "--view" && i + 1 < argc && command == "graph") graphView = argv[++i];
                else throw Error("unknown argument: " + arg);
            }
            if (command == "graph" && graphFormat != "mermaid" && graphFormat != "json") throw Error("unsupported graph format '" + graphFormat + "' (supported: mermaid, json)");
            if (command == "graph" && graphView != "calls" && graphView != "data") throw Error("unsupported graph view '" + graphView + "' (supported: calls, data)");
            if (command == "build") buildProject(config, argv[0], release, verbose, /*quiet=*/false, optReport, cliToolchain);
            else if (command == "run") runProject(config, argv[0], release, verbose, optReport, cliToolchain);
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
