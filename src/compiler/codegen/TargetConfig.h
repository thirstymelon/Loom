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

#include "../common/Types.h"

namespace loom {
enum class TargetArch {
    Unknown,
    AArch64,
    X86_64,
    Arm,
    RiscV64
};

enum class TargetOS {
    Unknown,
    Darwin,
    Linux,
    Windows,
};

enum class TargetFormat {
    Unknown,
    MachO,
    ELF,
    COFF
};

inline std::filesystem::path getExecutablePath(const char* argv0 = nullptr) {
    std::filesystem::path exePath;
#if defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        exePath = std::filesystem::canonical(buf, ec);
    }
#elif defined(__linux__)
    std::error_code ec;
    exePath = std::filesystem::canonical("/proc/self/exe", ec);
#elif defined(_WIN32)
    char buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, buf, MAX_PATH) != 0) {
        std::error_code ec;
        exePath = std::filesystem::canonical(buf, ec);
    }
#endif
    if (exePath.empty() && argv0 && argv0[0] != '\0') {
        std::error_code ec;
        exePath = std::filesystem::absolute(argv0, ec);
    }
    return exePath;
}

inline std::string detectToolchainKind(const char* argv0 = nullptr) {
#ifdef LOOM_TOOLCHAIN_NATIVE_ONLY
    return "native";
#endif
#ifdef LOOM_TOOLCHAIN_RP2040_ONLY
    return "rp2040";
#endif
    if (const char* env = std::getenv("LOOM_TOOLCHAIN_KIND")) {
        if (*env) return env;
    }
    std::error_code ec;
    std::filesystem::path exe = getExecutablePath(argv0);
    if (!exe.empty()) {
        std::filesystem::path dir = exe.parent_path();
        for (const auto& cand : {dir / "toolchain.toml", dir.parent_path() / "toolchain.toml", dir / "manifest.toml", dir.parent_path() / "manifest.toml"}) {
            if (std::filesystem::is_regular_file(cand, ec)) {
                std::ifstream f(cand);
                std::string line;
                while (std::getline(f, line)) {
                    if (line.find("kind") != std::string::npos && line.find("native") != std::string::npos) return "native";
                    if (line.find("kind") != std::string::npos && (line.find("embedded") != std::string::npos || line.find("rp2040") != std::string::npos)) return "rp2040";
                    if (line.find("name") != std::string::npos && line.find("native") != std::string::npos) return "native";
                    if (line.find("name") != std::string::npos && line.find("rp2040") != std::string::npos) return "rp2040";
                }
            }
        }
    }
    return "universal";
}

struct TargetConfig {
    std::string triple;
    TargetArch arch = TargetArch::Unknown;
    TargetOS os = TargetOS::Unknown;
    TargetFormat format = TargetFormat::Unknown;
    std::string cpu;
    std::string features;
    unsigned pointerWidth = 64;
    bool isLittleEndian = true;
    bool isAggressive = false;
    llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::None;
    llvm::Reloc::Model relocModel = llvm::Reloc::PIC_;
    llvm::CodeModel::Model codeModel = llvm::CodeModel::Small;

    static TargetConfig resolve(bool releaseMode, const std::string& customTriple = "",
                                const std::string& customCpu = "", bool baremetal = false,
                                bool sizeOpt = false) {
        TargetConfig cfg;
        cfg.isAggressive = releaseMode;
        if (sizeOpt) {
            cfg.optLevel = llvm::CodeGenOptLevel::Default;
        } else {
            cfg.optLevel = releaseMode ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::None;
        }

        if (!customTriple.empty()) {
            cfg.triple = customTriple;
        } else {
            const char* customEnv = std::getenv("LOOM_TARGET_TRIPLE");
            if (customEnv && *customEnv) {
                cfg.triple = customEnv;
            } else {
                cfg.triple = llvm::sys::getDefaultTargetTriple();
                llvm::Triple t(cfg.triple);
                if (t.isMacOSX() || t.isOSDarwin()) {
                    const char* envDep = std::getenv("MACOSX_DEPLOYMENT_TARGET");
                    if (envDep && *envDep) {
                        t.setOSName(std::string("macosx") + envDep);
                        cfg.triple = t.str();
                    }
                }
            }
        }

        llvm::Triple t(cfg.triple);
        if (t.isAArch64() || t.getArch() == llvm::Triple::aarch64) cfg.arch = TargetArch::AArch64;
        else if (t.isX86()) cfg.arch = (t.getArch() == llvm::Triple::x86_64) ? TargetArch::X86_64 : TargetArch::Unknown;
        else if (t.isARM() || t.isThumb()) cfg.arch = TargetArch::Arm;
        else if (t.isRISCV64()) cfg.arch = TargetArch::RiscV64;
        else cfg.arch = TargetArch::Unknown;

        if (t.isMacOSX() || t.isOSDarwin()) {
            cfg.os = TargetOS::Darwin;
        } else if (t.isOSLinux()) {
            cfg.os = TargetOS::Linux;
        } else if (t.isOSWindows()) {
            cfg.os = TargetOS::Windows;
        } else {
            cfg.os = TargetOS::Unknown;
        }

        if (t.isOSBinFormatMachO()) cfg.format = TargetFormat::MachO;
        else if (t.isOSBinFormatELF()) cfg.format = TargetFormat::ELF;
        else if (t.isOSBinFormatCOFF()) cfg.format = TargetFormat::COFF;

        const std::string tcKind = detectToolchainKind();
        const std::string targetName = lower(customTriple);
        const bool rp2040Target = t.isThumb() || targetName.find("thumb") != std::string::npos ||
                                  targetName.find("none-eabi") != std::string::npos || customCpu == "cortex-m0plus";
        const bool nativeAArch64Baremetal = baremetal && t.isAArch64() && t.isOSBinFormatELF() &&
                                            (targetName.find("none-elf") != std::string::npos ||
                                             targetName.find("unknown-elf") != std::string::npos);
        if (tcKind == "native") {
            if (rp2040Target) {
                throw Error("target '" + (customTriple.empty() ? "thumbv6m-none-eabi" : customTriple) +
                            "' is not supported by the Loom native desktop toolchain.\n"
                            "  Please install and switch to the 'rp2040' embedded toolchain:\n"
                            "    loom use rp2040");
            }
        } else if (tcKind == "rp2040") {
            if (!rp2040Target) {
                throw Error("target '" + cfg.triple +
                            "' is not supported by the Loom 'rp2040' embedded toolchain.\n"
                            "  Please install and switch to the 'native' desktop toolchain:\n"
                            "    loom use native");
            }
        }
        if (baremetal && !rp2040Target && !nativeAArch64Baremetal) {
            throw Error("native Loom bare-metal mode currently supports AArch64 ELF targets such as 'aarch64-none-elf'; RP2040 targets remain on the RP2040 toolchain");
        }

        if (!customCpu.empty()) {
            cfg.cpu = customCpu;
            cfg.features = "";
        } else if (baremetal && (t.isARM() || t.isThumb())) {
            cfg.cpu = "cortex-m0plus";
            cfg.features = "";
        } else if (releaseMode && customTriple.empty()) {
            cfg.cpu = llvm::sys::getHostCPUName().str();
            if (cfg.cpu.empty() || cfg.cpu == "generic") {
                if (t.isAArch64()) cfg.cpu = "apple-m1";
            }
            auto featureMap = llvm::sys::getHostCPUFeatures();
            llvm::SubtargetFeatures feat;
            for (const auto& f : featureMap) feat.AddFeature(f.first(), f.second);
            if (t.isAArch64()) {
                feat.AddFeature("neon", true);
                feat.AddFeature("fp-armv8", true);
                feat.AddFeature("dotprod", true);
            }
            cfg.features = feat.getString();
        } else {
            cfg.cpu = t.isAArch64() ? "apple-m1" : "generic";
            cfg.features = t.isAArch64() ? "+neon,+fp-armv8,+dotprod" : "";
        }

        cfg.pointerWidth = t.isArch64Bit() ? 64 : 32;
        cfg.isLittleEndian = t.isLittleEndian();

        if (baremetal) {
            cfg.relocModel = llvm::Reloc::Static;
            cfg.codeModel = llvm::CodeModel::Small;
        } else {
            cfg.relocModel = llvm::Reloc::PIC_;
        }

        return cfg;
    }

    std::unique_ptr<llvm::TargetMachine> createMachine(std::string& errorOut) const {
        llvm::Triple targetTriple(triple);
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, errorOut);
        if (!target) return nullptr;

        llvm::TargetOptions options;
        options.FunctionSections = true;
        options.DataSections = true;
        options.UniqueSectionNames = true;
        if (isAggressive) {
            options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
            options.GuaranteedTailCallOpt = true;
        }
        return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
            targetTriple, cpu, features, options, relocModel, codeModel, optLevel));
    }

    std::string archName() const {
        switch (arch) {
            case TargetArch::AArch64: return "AArch64 (ARM64)";
            case TargetArch::X86_64: return "x86-64";
            case TargetArch::Arm: return "ARM32";
            case TargetArch::RiscV64: return "RISC-V 64";
            default: return "Generic";
        }
    }

    std::string osName() const {
        switch (os) {
            case TargetOS::Darwin: return "Darwin / macOS";
            case TargetOS::Linux: return "Linux";
            case TargetOS::Windows: return "Windows";
            default: return "Unknown OS";
        }
    }
};

struct OptReport {
    bool enabled = false;
    std::string targetTriple;
    std::string cpu;
    std::string arch;
    std::string osPlatform;
    std::string features;
    std::vector<std::string> frontendFolds;
    std::vector<std::string> boundsEliminations;
    std::vector<std::string> inlinedBlocks;
    std::vector<std::string> vectorizations;
    std::vector<std::string> autoParallelizations;
    std::vector<std::string> loopLowerings;

    void print(std::ostream& os) const {
        os << "================================================================================\n";
        os << "                         Loom Optimization Report\n";
        os << "================================================================================\n";
        os << "Target Machine:      " << (targetTriple.empty() ? "host-native" : targetTriple) << "\n";
        os << "Target Architecture: " << (arch.empty() ? "host" : arch) << "\n";
        os << "Target OS / ABI:     " << (osPlatform.empty() ? "host default" : osPlatform) << "\n";
        os << "Target CPU:          " << (cpu.empty() ? "generic" : cpu) << "\n";
        if (!features.empty()) {
            os << "Target Features:     " << features << "\n";
        }
        os << "Optimization Level:  LLVM O3 when --release is selected; otherwise LLVM codegen defaults\n";
        os << "--------------------------------------------------------------------------------\n";
        os << "[1] Frontend Compile-Time Evaluation (ConstEvaluator):\n";
        if (frontendFolds.empty()) {
            os << "    * None triggered\n";
        } else {
            for (const auto& fold : frontendFolds) {
                os << "    * " << fold << "\n";
            }
        }
        os << "--------------------------------------------------------------------------------\n";
        os << "[2] Bounds Check Elimination (constant indices only):\n";
        if (boundsEliminations.empty()) {
            os << "    * No array bounds checks needed elimination\n";
        } else {
            for (const auto& elim : boundsEliminations) {
                os << "    * " << elim << "\n";
            }
        }
        os << "--------------------------------------------------------------------------------\n";
        os << "[3] LLVM Optimization Pipeline:\n";
        os << "    * Release builds use LLVM's default per-module O3 pipeline\n";
        os << "    * Exact transformations depend on LLVM version, target, and program semantics\n";
        os << "--------------------------------------------------------------------------------\n";
        os << "[4] SIMD:\n";
        os << "    * No custom NEON lowering is implemented; LLVM may vectorize eligible loops\n";
        os << "--------------------------------------------------------------------------------\n";
        os << "[5] Memory & Zero-Abstraction Audit:\n";
        os << "    * Loom does not provide a garbage collector or implicit reference counting\n";
        os << "    * Runtime allocation behavior depends on the program and linked libraries\n";
        os << "--------------------------------------------------------------------------------\n";
        os << "[6] Automatic Parallelization (proven transformations only):\n";
        if (autoParallelizations.empty()) {
            os << "    * No proven automatic parallel transformation was selected\n";
        } else {
            for (const auto& item : autoParallelizations) {
                os << "    * " << item << "\n";
            }
        }
        os << "--------------------------------------------------------------------------------\n";
        os << "[7] Loop Lowerings and Profitability:\n";
        if (loopLowerings.empty()) {
            os << "    * No specialized loop lowering selected\n";
        } else {
            for (const auto& item : loopLowerings) os << "    * " << item << "\n";
        }
        os << "================================================================================\n";
    }
};


} // namespace loom
