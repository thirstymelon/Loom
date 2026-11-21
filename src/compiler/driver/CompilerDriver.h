#pragma once
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "llvm/IR/BasicBlock.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "../../targets/TargetProfile.h"

#include "../codegen/Generator.h"
#include "../parser/Parser.h"

namespace loom {
inline std::string cTypeName(const Type& t) {
    switch (t.kind) {
        case TypeKind::Boolean: return "bool";
        case TypeKind::Signed:
            if (t.bits <= 8) return "int8_t";
            if (t.bits <= 16) return "int16_t";
            if (t.bits <= 32) return "int32_t";
            return "int64_t";
        case TypeKind::Unsigned:
            if (t.bits <= 8) return "uint8_t";
            if (t.bits <= 16) return "uint16_t";
            if (t.bits <= 32) return "uint32_t";
            return "uint64_t";
        case TypeKind::Float:
            return t.bits <= 32 ? "float" : "double";
        case TypeKind::Character: return "uint32_t";
        case TypeKind::String: return "const char*";
        case TypeKind::Pointer:
            return (t.pointeeType ? cTypeName(*t.pointeeType) : "void") + "*";
        case TypeKind::Slice: return "loom_slice_t";
        case TypeKind::Array:
            return (t.pointeeType ? cTypeName(*t.pointeeType) : "void") + "*";
        case TypeKind::Enum: return t.name;
        case TypeKind::Record: return t.name;
        default: return "void";
    }
}

void generateCHeader(const std::string& path, const std::vector<Function>& fs,
                     const std::vector<RecordDecl>& records, const std::vector<EnumDecl>& enums) {
    std::ofstream out(path);
    if (!out) throw Error("cannot open file for writing C header: " + path);
    out << "/* Generated automatically by Loom Compiler (loomc) */\n";
    out << "#ifndef LOOM_GENERATED_H\n";
    out << "#define LOOM_GENERATED_H\n\n";
    out << "#include <stdint.h>\n";
    out << "#include <stdbool.h>\n";
    out << "#include <stddef.h>\n\n";
    out << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
    out << "typedef struct {\n    void* data;\n    size_t length;\n} loom_slice_t;\n\n";

    for (const auto& e : enums) {
        out << "typedef enum {\n";
        for (std::size_t i = 0; i < e.variants.size(); ++i) {
            out << "    " << e.name << "_" << e.variants[i].name << " = " << e.variants[i].magnitude;
            if (i + 1 < e.variants.size()) out << ",";
            out << "\n";
        }
        out << "} " << e.name << ";\n\n";
    }

    for (const auto& r : records) {
        if (r.isPacked) out << "#pragma pack(push, 1)\n";
        out << "typedef struct " << (r.isPacked ? "__attribute__((packed)) " : "") << "{\n";
        for (const auto& f : r.fields) {
            out << "    " << cTypeName(f.type) << " " << f.name << ";\n";
        }
        out << "} " << r.name << ";\n";
        out << "typedef " << r.name << " " << r.name << "_t;\n";
        if (r.isPacked) out << "#pragma pack(pop)\n";
        out << "\n";
    }

    for (const auto& f : fs) {
        if (f.isTest) continue;
        std::string ret = f.result ? cTypeName(*f.result) : "void";
        out << ret << " " << f.name << "(";
        if (f.params.empty()) {
            out << "void";
        } else {
            for (std::size_t i = 0; i < f.params.size(); ++i) {
                out << cTypeName(f.params[i].type) << " " << f.params[i].name;
                if (i + 1 < f.params.size()) out << ", ";
            }
        }
        out << ");\n";
    }

    out << "\n#ifdef __cplusplus\n}\n#endif\n\n";
    out << "#endif /* LOOM_GENERATED_H */\n";
}



inline int compilerMain(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "loomc 1.4.0 (LLVM " << LLVM_VERSION_STRING << ")\n";
        return 0;
    }
    bool nativeEntry = false, objectOutput = false, release = false, graphData = false;
    bool sizeOpt = false;
    bool optReport = false;
    bool namedOutput = false;
    bool checkOnly = false;
    bool baremetal = false;
    bool emitDebug = false;
    std::string sanitizeFlags;
    std::string targetTriple;
    std::string targetCpu;
    std::string outputPath;
    std::string explicitSdkPath;
    std::string emitCHeaderPath;
    bool emitTestRunner = false;
    std::vector<std::string> inputs;
    std::vector<std::string> includeDirs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--entry") nativeEntry = true;
        else if (arg == "--object") objectOutput = true;
        else if (arg == "--release") release = true;
        else if (arg == "--size" || arg == "-Oz") { sizeOpt = true; release = true; }
        else if (arg == "--opt-report") optReport = true;
        else if (arg == "--graph-data") graphData = true;
        else if (arg == "--check") checkOnly = true;
        else if (arg == "--baremetal") baremetal = true;
        else if (arg == "-g" || arg == "--debug") emitDebug = true;
        else if (arg.rfind("--sanitize=", 0) == 0) { sanitizeFlags = arg.substr(11); }
        else if (arg == "--emit-c-header" && i + 1 < argc) { emitCHeaderPath = argv[++i]; }
        else if (arg == "--test-runner") { emitTestRunner = true; }
        else if (arg == "--target" && i + 1 < argc) { targetTriple = argv[++i]; }
        else if (arg == "--cpu" && i + 1 < argc) { targetCpu = argv[++i]; }
        else if (arg == "--output" && i + 1 < argc) { namedOutput = true; outputPath = argv[++i]; }
        else if (arg == "--sdk" && i + 1 < argc) { explicitSdkPath = argv[++i]; }
        else if ((arg == "-I" || arg == "--include") && i + 1 < argc) { includeDirs.push_back(argv[++i]); }
        else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) { includeDirs.push_back(arg.substr(2)); }
        else if (arg.rfind("--", 0) == 0) {
            bool useColor = false;
#ifndef _WIN32
            if ((isatty(STDERR_FILENO) || std::getenv("LOOM_COLOR") != nullptr) && std::getenv("NO_COLOR") == nullptr) useColor = true;
#endif
            const std::string red = useColor ? "\033[1;31m" : "";
            const std::string bold = useColor ? "\033[1m" : "";
            const std::string reset = useColor ? "\033[0m" : "";
            std::cerr << red << "loomc: error: " << reset << bold << "unknown option '" << arg << "'" << reset << "\n";
            return 2;
        }
        else inputs.push_back(arg);
    }
    if (checkOnly && inputs.size() > 1 && !namedOutput && !objectOutput) {
        if (inputs.size() == 2) inputs.pop_back();
    }
    if (inputs.empty() || (!checkOnly && !namedOutput && (inputs.size() > 2 || (objectOutput && inputs.size() != 2))) ||
        (namedOutput && !(objectOutput || graphData)) || (graphData && !namedOutput)) {
        std::cerr << "usage: loomc <input.loom> [output.ll] [--entry] [--release] [--size] [--opt-report] [-I <dir>] [--sdk <dir>]\n"
                     "       loomc --check <input.loom> [-I <dir>] [--sdk <dir>]\n"
                     "       loomc --object <input.loom> <output.o> [--entry] [--release] [--size] [--opt-report] [-I <dir>] [--sdk <dir>]\n"
                     "       loomc --object --output <output.o> [--entry] [--release] [--size] [--opt-report] [-I <dir>] [--sdk <dir>] <source.loom>...\n"
                     "       loomc --graph-data --output <data.tsv> <source.loom>...\n";
        return 2;
    }
    std::vector<std::filesystem::path> sdkIncludeDirs = loom::getSdkIncludeDirs(argv[0], explicitSdkPath);
    std::vector<std::string> sourceTexts;
    try {
        if (!namedOutput && objectOutput) { outputPath = inputs.back(); inputs.pop_back(); }
        else if (!checkOnly && !namedOutput && !objectOutput && inputs.size() == 2) { outputPath = inputs.back(); inputs.pop_back(); }
        std::unordered_set<std::string> loadedFiles;
        std::unordered_map<std::string, std::string> knownModuleFiles;
        for (const auto& inPath : inputs) {
            std::error_code ec;
            std::filesystem::path canonical = std::filesystem::canonical(inPath, ec);
            std::string key = ec ? inPath : canonical.string();
            loadedFiles.insert(key);
            std::string content = loom::readFile(inPath);
            std::istringstream stream(content);
            std::string line;
            while (std::getline(stream, line)) {
                std::size_t start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                if (line.substr(start, 6) == "Module" && (start + 6 == line.size() || isspace(static_cast<unsigned char>(line[start + 6])))) {
                    std::string rest = line.substr(start + 6);
                    std::size_t modStart = rest.find_first_not_of(" \t\r\n");
                    if (modStart != std::string::npos) {
                        std::size_t modEnd = rest.find_first_of(" \t\r\n;", modStart);
                        std::string modName = rest.substr(modStart, modEnd == std::string::npos ? std::string::npos : modEnd - modStart);
                        if (!modName.empty()) {
                            knownModuleFiles[loom::lower(modName)] = key;
                        }
                    }
                    break;
                }
            }
        }
        std::size_t qIdx = 0;
        while (qIdx < inputs.size()) {
            std::string path = inputs[qIdx++];
            std::string content = loom::readFile(path);
            sourceTexts.push_back(content);
            std::istringstream stream(content);
            std::string line;
            int importLineNo = 0;
            while (std::getline(stream, line)) {
                ++importLineNo;
                std::size_t start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                bool isImport = (line.substr(start, 6) == "Import" || line.substr(start, 6) == "import") && (start + 6 == line.size() || isspace(static_cast<unsigned char>(line[start + 6])));
                bool isUse = (line.substr(start, 3) == "use" || line.substr(start, 3) == "Use") && (start + 3 == line.size() || isspace(static_cast<unsigned char>(line[start + 3])));
                if (isImport || isUse) {
                    std::size_t kwLen = isImport ? 6 : 3;
                    std::string rest = line.substr(start + kwLen);
                    std::size_t modStart = rest.find_first_not_of(" \t\r\n");
                    if (modStart == std::string::npos) continue;
                    std::size_t modEnd = rest.find_first_of(" \t\r\n;", modStart);
                    std::string modName = rest.substr(modStart, modEnd == std::string::npos ? std::string::npos : modEnd - modStart);
                    if (modName.empty()) continue;

                    if (knownModuleFiles.count(loom::lower(modName))) {
                        continue;
                    }

                    std::filesystem::path cur(path);
                    std::filesystem::path parent = cur.parent_path();
                    std::vector<std::filesystem::path> candidates;
                    auto addDirCand = [&](const std::filesystem::path& base) {
                        if (base.empty()) return;
                        std::error_code ec;
                        if (!std::filesystem::is_directory(base, ec)) return;
                        candidates.push_back(base / (modName + ".loom"));
                        candidates.push_back(base / (loom::lower(modName) + ".loom"));
                        candidates.push_back(base / modName / "mod.loom");
                        candidates.push_back(base / "src" / (modName + ".loom"));
                        candidates.push_back(base / "src" / (loom::lower(modName) + ".loom"));
                        candidates.push_back(base / "bsp" / (modName + ".loom"));
                        candidates.push_back(base / "bsp" / (loom::lower(modName) + ".loom"));
                        candidates.push_back(base / "src" / "bsp" / (modName + ".loom"));
                        candidates.push_back(base / "src" / "bsp" / (loom::lower(modName) + ".loom"));
                        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
                            if (entry.is_directory(ec)) {
                                candidates.push_back(entry.path() / (modName + ".loom"));
                                candidates.push_back(entry.path() / (loom::lower(modName) + ".loom"));
                            }
                        }
                        if (std::filesystem::is_directory(base / "src", ec)) {
                            for (const auto& entry : std::filesystem::directory_iterator(base / "src", ec)) {
                                if (entry.is_directory(ec)) {
                                    candidates.push_back(entry.path() / (modName + ".loom"));
                                    candidates.push_back(entry.path() / (loom::lower(modName) + ".loom"));
                                }
                            }
                        }
                    };

                    addDirCand(parent);
                    for (const auto& inc : includeDirs) {
                        addDirCand(inc);
                    }
                    std::filesystem::path checkDir = parent;
                    for (int d = 0; d < 8; ++d) {
                        std::error_code ec;
                        if (std::filesystem::is_regular_file(checkDir / "config.toml", ec)) {
                            addDirCand(checkDir);
                            break;
                        }
                        if (!checkDir.has_parent_path() || checkDir.parent_path() == checkDir) break;
                        checkDir = checkDir.parent_path();
                    }
                    std::filesystem::path cwd = std::filesystem::current_path();
                    addDirCand(cwd);
                    checkDir = cwd;
                    for (int d = 0; d < 8; ++d) {
                        std::error_code ec;
                        if (std::filesystem::is_regular_file(checkDir / "config.toml", ec)) {
                            addDirCand(checkDir);
                            break;
                        }
                        if (!checkDir.has_parent_path() || checkDir.parent_path() == checkDir) break;
                        checkDir = checkDir.parent_path();
                    }
                    for (const auto& sdkDir : sdkIncludeDirs) {
                        addDirCand(sdkDir);
                    }

                    std::string found;
                    for (const auto& cand : candidates) {
                        std::error_code checkEc;
                        if (std::filesystem::is_regular_file(cand, checkEc)) {
                            found = cand.string();
                            break;
                        }
                    }
                    if (found.empty()) {
                        loom::Error err("line " + std::to_string(importLineNo) + ": cannot find imported module '" + modName + "' for '" + path + "'");
                        err.span.source = qIdx - 1;
                        err.span.line = importLineNo;
                        err.span.column = start + 1;
                        throw err;
                    }
                    std::error_code ec;
                    std::filesystem::path candCanonical = std::filesystem::canonical(found, ec);
                    std::string candKey = ec ? found : candCanonical.string();
                    if (!loadedFiles.count(candKey)) {
                        loadedFiles.insert(candKey);
                        inputs.push_back(found);
                        knownModuleFiles[loom::lower(modName)] = candKey;
                    }
                }
            }
        }
        std::vector<loom::Token> tokens;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            loom::currentSourceIndex = i;
            auto sourceTokens = loom::Lexer(sourceTexts[i], i).run();
            std::size_t firstIdx = 0;
            while (firstIdx < sourceTokens.size() && sourceTokens[firstIdx].kind == loom::K::Newline) {
                ++firstIdx;
            }
            if (firstIdx >= sourceTokens.size() || sourceTokens[firstIdx].kind != loom::K::Module ||
                firstIdx + 1 >= sourceTokens.size() || sourceTokens[firstIdx + 1].kind != loom::K::Id) {
                int errLine = (firstIdx < sourceTokens.size()) ? sourceTokens[firstIdx].line : 1;
                loom::Error err("line " + std::to_string(errLine) + ": Every Loom file must begin with 'Module <module_name>' at the top of the file");
                err.span.source = i;
                err.span.line = errLine;
                throw err;
            }
            sourceTokens.pop_back();
            tokens.insert(tokens.end(), std::make_move_iterator(sourceTokens.begin()),
                          std::make_move_iterator(sourceTokens.end()));
            tokens.push_back({loom::K::Newline, "newline", 1, 0, i});
        }
        int finalLine = 1;
        for (char ch : sourceTexts.back()) if (ch == '\n') ++finalLine;
        tokens.push_back({loom::K::Eof, "<eof>", finalLine, 0, inputs.size() - 1});
        loom::Parser parser(std::move(tokens));
        auto functions = parser.run();
        const auto& globals = parser.globals();
        const auto& enums = parser.enums();
        const auto& records = parser.records();
        if (functions.empty()) throw loom::Error("source contains no Block declarations");
        if (emitTestRunner) {
            loom::Function testRunner;
            testRunner.name = "Main";
            testRunner.line = 1;
            for (const auto& f : functions) {
                if (f.isTest) {
                    loom::Stmt callTest;
                    callTest.kind = loom::Stmt::Kind::Call;
                    callTest.line = f.line;
                    auto callExpr = std::make_unique<loom::Expr>();
                    callExpr->kind = loom::Expr::Kind::Call;
                    callExpr->text = f.name;
                    callTest.expr = std::move(callExpr);
                    testRunner.body.push_back(std::move(callTest));

                    loom::Stmt writePass;
                    writePass.kind = loom::Stmt::Kind::Write;
                    writePass.line = f.line;
                    auto strLit = std::make_unique<loom::Expr>();
                    strLit->kind = loom::Expr::Kind::String;
                    strLit->text = "[PASS] \"" + f.testDescription + "\"";
                    writePass.writeArgs.push_back(std::move(strLit));
                    testRunner.body.push_back(std::move(writePass));
                }
            }
            functions.push_back(std::move(testRunner));
            nativeEntry = true;
        }
        loom::Analyzer analyzer(baremetal);
        analyzer.run(functions, globals, enums, records, parser.modules(), parser.imports(), parser.uses());
        if (!emitCHeaderPath.empty()) {
            loom::generateCHeader(emitCHeaderPath, functions, records, enums);
            if (outputPath.empty() && !objectOutput && !nativeEntry && !emitTestRunner) return 0;
        }
        if (checkOnly) {
            return 0;
        }
        if (graphData) {
            loom::writeGraphData(functions, globals, outputPath);
            return 0;
        }
        loom::OptReport report;
        report.enabled = optReport;
        loom::Generator generator(inputs.front(), analyzer, release, &report, baremetal, nullptr, emitDebug);
        auto module = generator.run(functions, globals, records, nativeEntry);
        if (objectOutput) loom::writeObject(*module, outputPath, release, targetTriple, targetCpu, baremetal, &report, sizeOpt);
        else loom::writeIR(*module, outputPath.empty() ? std::nullopt : std::optional<std::string>(outputPath), release, targetTriple, targetCpu, baremetal, &report, sizeOpt);
        if (optReport) {
            report.print(std::cout);
        }
        return 0;
    } catch (const loom::Error& e) {
        loom::printDiagnostic(e, inputs, sourceTexts);
        return 1;
    } catch (const std::exception& e) {
        bool useColor = false;
#ifndef _WIN32
        if ((isatty(STDERR_FILENO) || std::getenv("LOOM_COLOR") != nullptr) && std::getenv("NO_COLOR") == nullptr) useColor = true;
#endif
        const std::string red = useColor ? "\033[1;31m" : "";
        const std::string bold = useColor ? "\033[1m" : "";
        const std::string reset = useColor ? "\033[0m" : "";
        std::cerr << red << "loomc: internal compiler error: " << reset << bold << e.what() << reset << "\n";
        return 1;
    }
}

} // namespace loom
