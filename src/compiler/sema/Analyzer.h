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

#include "../ast/AST.h"

namespace loom {
inline bool isVolatileRead(const std::string& name) {
    return name == "Vol_Read_8" || name == "Vol_Read_16" || name == "Vol_Read_32" || name == "Vol_Read_64";
}
inline bool isVolatileWrite(const std::string& name) {
    return name == "Vol_Write_8" || name == "Vol_Write_16" || name == "Vol_Write_32" || name == "Vol_Write_64";
}
inline bool isCpuIntrinsic(const std::string& name) {
    return name == "Nop" || name == "WaitForInterrupt" || name == "MemoryBarrier" ||
           name == "WaitForEvent" || name == "SendEvent" || name == "DataMemoryBarrier" ||
           name == "DataSyncBarrier" || name == "InstructionSyncBarrier";
}
inline bool isAIMLIntrinsic(const std::string& name) {
    return name == "MatMul" || name == "MatMul_Add" || name == "MatMul_TransposeB" ||
           name == "MatMul_TransposeA" || name == "Vector_Dot" || name == "ReLU" ||
           name == "ReLU_Backward" || name == "GELU" || name == "Softmax" ||
           name == "LayerNorm" || name == "Vector_Add" || name == "Vector_Sub" ||
           name == "Vector_Mul" || name == "Vector_Scale" || name == "Parallel_For";
}
inline bool isMathIntrinsic(const std::string& name) {
    return name == "Sqrt" || name == "Sin" || name == "Cos" || name == "Tan" ||
           name == "ASin" || name == "ACos" || name == "ATan" || name == "ATan2" ||
           name == "Exp" || name == "Log" || name == "Abs" || name == "Floor" ||
           name == "Ceil" || name == "Round" || name == "Min" || name == "Max" ||
           name == "Pow";
}
inline bool isFileIntrinsic(const std::string& name) {
    return name == "Open_File" || name == "Read_File" || name == "Read_All" ||
           name == "Close_File";
}
inline bool isBuiltinIntrinsic(const std::string& name) {
    return isVolatileRead(name) || isVolatileWrite(name) || isCpuIntrinsic(name) ||
           name == "Asm" || isAIMLIntrinsic(name) || isMathIntrinsic(name) || isFileIntrinsic(name) ||
           name == "Append" || name == "Pop" || name == "To_String" ||
           name == "Wrapping_Add" || name == "Wrapping_Sub" || name == "Wrapping_Mul" || name == "__loom_task_cancel" ||
           name == "Make_Smt" || name == "Smt_Count" ||
           name == "Channel_Create" || name == "Channel_Open" || name == "Channel_Send" ||
           name == "Channel_Receive" || name == "Channel_Try_Receive" || name == "Channel_Close" ||
           name == "Mutex_Create" ||
           name == "Atomic_Load" || name == "Atomic_Store" || name == "Atomic_Fetch_Add" ||
           name == "Atomic_Fetch_Sub" || name == "Atomic_CAS" ||
           name == "Tcp_Listen" || name == "Tcp_Listen_V6" || name == "Tcp_Accept" || name == "Tcp_Connect" || name == "Tcp_Connect_V6" ||
           name == "Tcp_Send" || name == "Tcp_Send_At" || name == "Tcp_Send_Slice" || name == "Tcp_Receive_Buffer" ||
           name == "Tcp_Receive" || name == "Tcp_Close" || name == "Udp_Bind" || name == "Udp_Bind_V6" || name == "Udp_Send_To" ||
           name == "Udp_Connect" || name == "Udp_Connect_V6" || name == "Udp_Send" || name == "Udp_Send_Slice" || name == "Udp_Send_To_Slice" || name == "Udp_Send_To_V6" || name == "Udp_Send_To_Slice_V6" || name == "Udp_Receive_Buffer" || name == "Udp_Receive_From" || name == "Udp_Close" ||
           name == "Socket_Set_Timeout" || name == "Socket_Wait" || name == "Socket_Set_Nonblocking" || name == "Socket_Get_Error" || name == "System_Random" ||
           name == "Tls_Client_New" || name == "Tls_Connect" || name == "Tls_Send" || name == "Tls_Send_Slice" || name == "Tls_Receive" || name == "Tls_Receive_Buffer" || name == "Tls_Close" ||
           name == "Tls_Set_Alpn" || name == "Tls_Get_Alpn" || name == "Quic_Initial_Keys" || name == "Quic_Hp_Mask" || name == "Quic_Encrypt" || name == "Quic_Decrypt" ||
           name == "__loom_usb_is_configured" || name == "__loom_usb_available" || name == "__loom_usb_read_byte" || name == "__loom_usb_putchar" || name == "__loom_usb_write_str" ||
           name == "__loom_irq_save" || name == "__loom_irq_restore";
}

struct Flow {
    bool falls = true, breaks = false, skips = false, returns = false;
};
class Analyzer {
    std::unordered_map<const Expr*, Type> expressionTypes;
    std::unordered_map<std::string, Signature> signatures;
    std::map<std::tuple<std::string, std::string, std::string>, std::string> overloadedBinaryOps;
    std::map<std::tuple<std::string, std::string>, std::string> overloadedIndexOps;
    std::map<std::tuple<std::string, std::string, std::string>, std::string> overloadedIndexSetOps;
    std::unordered_map<std::string, Type> globalTypes;
    std::unordered_set<std::string> globalConstants;
    std::unordered_map<std::string, EnumValue> enumValues;
    std::unordered_map<std::string, Type> enumVariantTypes;
    std::unordered_map<std::string, std::unordered_map<std::string, Type>> recordFieldTypes;
    std::unordered_map<std::string, std::vector<std::string>> recordFieldOrder;
    std::unordered_map<const Stmt*, Type> fieldAssignmentTypes;
    std::unordered_map<const Stmt*, Type> fieldAssignmentRecords;
    std::unordered_map<std::string, std::pair<std::string, std::size_t>> funcModule;
    std::unordered_map<std::string, std::pair<std::string, std::size_t>> recordModule;
    std::unordered_map<std::string, std::pair<std::string, std::size_t>> enumModule;
    std::unordered_map<std::string, std::pair<std::string, std::size_t>> globalModule;
    std::unordered_map<std::size_t, std::string> fileModules;
    std::unordered_map<std::size_t, std::unordered_set<std::string>> fileImports;
    std::unordered_map<std::size_t, std::unordered_set<std::string>> fileUses;
    std::unordered_map<std::size_t, std::unordered_map<std::string, std::string>> fileImportAliases;
    std::unordered_map<std::string, std::unordered_map<std::string, Signature>> moduleSignatures;
    std::unordered_map<std::string, std::unordered_map<std::string, Type>> moduleGlobalTypes;
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleGlobalConstants;
    std::unordered_map<const Expr*, std::string> resolvedCallKeys;
    std::unordered_map<const Expr*, std::string> resolvedGlobalKeys;
    std::unordered_set<std::string> ambiguousFunctions;
    std::unordered_set<std::string> ambiguousGlobals;
    std::unordered_map<std::string, bool> recordIsTagged;
    std::unordered_map<std::string, std::string> recordParent;
    std::unordered_map<std::string, std::string> recordRoot;
    std::unordered_map<std::string, std::vector<std::string>> vtableSlotNames;
    std::unordered_map<std::string, std::vector<std::string>> vtableFunctions;
    std::unordered_map<std::string, std::unordered_map<std::string, std::pair<Signature, std::string>>> taggedMethodSignatures;
    std::unordered_map<const Expr*, std::pair<std::string, std::size_t>> virtualDispatchCalls;
    std::unordered_set<std::string> currentFunctionLocals;
    std::unordered_set<std::string> currentFunctionConstants;
    std::unordered_set<std::string> currentBlockUses;
    std::optional<Type> currentFunctionErrorResult;
    std::unordered_set<std::string> dropRecords;
    std::shared_ptr<TargetProfile> targetProfile;
    bool baremetal = false;
    int unsafeDepth = 0;
    std::string currentAnalyzedFunctionKey;
    std::unordered_map<std::string, std::unordered_set<std::string>> callGraphEdges;
    std::unordered_map<std::string, std::unordered_set<std::string>> enumAllVariants;
    std::unordered_map<uint32_t, std::string> reservedPins;
    std::unordered_map<std::string, uint32_t> currentFunctionConstantValues;
    std::unordered_map<std::string, uint32_t> globalConstantValues;
    std::unordered_map<std::string, std::unordered_set<std::string>> lockOrderGraph;

public:
    explicit Analyzer(bool bm = false) : targetProfile(createTargetProfile(bm)), baremetal(bm) {}
    explicit Analyzer(std::shared_ptr<TargetProfile> tp) : targetProfile(tp ? tp : createTargetProfile(false)), baremetal(!targetProfile->supportsHeap()) {}

private:
    std::string resolveModuleAlias(const std::string& qual, std::size_t srcIdx) const {
        auto it = fileImportAliases.find(srcIdx);
        if (it == fileImportAliases.end()) return qual;
        std::string ql = lower(qual);
        auto aIt = it->second.find(ql);
        if (aIt != it->second.end()) return aIt->second;
        auto dot = qual.find('.');
        if (dot != std::string::npos) {
            std::string first = lower(qual.substr(0, dot));
            auto fIt = it->second.find(first);
            if (fIt != it->second.end()) return fIt->second + qual.substr(dot);
        }
        return qual;
    }
    bool isRecordSubtype(const std::string& child, const std::string& parent) const {
        std::string cur = child;
        auto dotC = cur.rfind('.');
        std::string shortChild = (dotC != std::string::npos) ? cur.substr(dotC + 1) : cur;
        auto dotP = parent.rfind('.');
        std::string shortParent = (dotP != std::string::npos) ? parent.substr(dotP + 1) : parent;
        if (shortChild == shortParent) return true;
        while (!cur.empty()) {
            if (cur == parent) return true;
            auto dot = cur.rfind('.');
            if (dot != std::string::npos && cur.substr(dot + 1) == shortParent) return true;
            auto it = recordParent.find(cur);
            if (it == recordParent.end()) {
                if (dot != std::string::npos) {
                    it = recordParent.find(cur.substr(dot + 1));
                }
                if (it == recordParent.end()) break;
            }
            cur = it->second;
        }
        return false;
    }
    static bool flattenMemberChain(const Expr& e, std::vector<std::string>& parts) {
        if (e.kind == Expr::Kind::Variable) {
            if (!e.qualifier.empty()) {
                std::stringstream ss(e.qualifier);
                std::string seg;
                while (std::getline(ss, seg, '.')) parts.push_back(seg);
            }
            parts.push_back(e.text);
            return true;
        }
        if (e.kind == Expr::Kind::Member && e.left) {
            if (!flattenMemberChain(*e.left, parts)) return false;
            parts.push_back(e.text);
            return true;
        }
        return false;
    }
    bool isKnownModule(const std::string& modLower) const {
        if (moduleSignatures.count(modLower) || moduleGlobalTypes.count(modLower)) return true;
        for (const auto& [idx, m] : fileModules) {
            (void)idx;
            if (lower(m) == modLower) return true;
        }
        return false;
    }
    bool isStdModule(const std::string& mod) const {
        static const std::unordered_set<std::string> stdMods = {
            "rp2040", "gpio", "time", "clock", "clocks", "system", "uart", "spi", "i2c", "pwm", "adc", "dma", "pio", "multicore", "watchdog", "rtc", "usb_cdc", "sync", "thread", "timer", "interpolation", "interp", "crash", "sio"
        };
        return stdMods.count(lower(mod)) > 0;
    }

    void checkAccess(const std::string& entityKind, const std::string& entityName,
                     const std::string& defModule, int line, std::size_t callerSourceIndex) {
        if (defModule.empty()) return;
        if (isStdModule(defModule)) return;
        std::string callerModule = fileModules.count(callerSourceIndex) ? fileModules.at(callerSourceIndex) : "";
        if (callerModule.empty()) return;
        if (lower(callerModule) == lower(defModule)) return;
        auto impIt = fileImports.find(callerSourceIndex);
        if (impIt == fileImports.end() || !impIt->second.count(lower(defModule))) {
            throw Error("line " + std::to_string(line) + ": module '" + callerModule +
                        "' cannot access " + entityKind + " '" + entityName +
                        "' from module '" + defModule + "' without 'Import " + defModule + "'");
        }
    }

    void checkTypeAccess(const Type& t, int line, std::size_t sourceIdx) {
        if (t.kind == TypeKind::Record) {
            auto it = recordModule.find(t.name);
            if (it != recordModule.end()) {
                if (isStdModule(it->second.first)) return;
                checkAccess("record", t.name, it->second.first, line, sourceIdx);
            }
        } else if (t.kind == TypeKind::Enum) {
            auto it = enumModule.find(t.name);
            if (it != enumModule.end()) {
                if (isStdModule(it->second.first)) return;
                checkAccess("enum", t.name, it->second.first, line, sourceIdx);
            }
        } else if (t.kind == TypeKind::Pointer || t.kind == TypeKind::Slice || t.kind == TypeKind::Array || t.kind == TypeKind::TaskHandle) {
            if (t.pointeeType) checkTypeAccess(*t.pointeeType, line, sourceIdx);
        }
    }

    static bool constantInitializer(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Integer: case Expr::Kind::Floating: case Expr::Kind::Boolean:
            case Expr::Kind::String: case Expr::Kind::Character: case Expr::Kind::EnumVariant: return true;
            case Expr::Kind::Unary:
                return (e.text == "-" || e.text == "not" || e.text == "~") && e.left && constantInitializer(*e.left);
            case Expr::Kind::ArrayLiteral:
            case Expr::Kind::TupleLiteral:
            case Expr::Kind::HMapLiteral: {
                for (const auto& arg : e.arguments) {
                    if (!constantInitializer(*arg)) return false;
                }
                return true;
            }
            case Expr::Kind::RecordLiteral: {
                for (const auto& arg : e.arguments) {
                    if (!constantInitializer(*arg)) return false;
                }
                return true;
            }
            default: return false;
        }
    }
    static bool isLiteral(const Expr& e) {
        return e.kind == Expr::Kind::Integer;
    }
    static bool fits(std::uint64_t n, Type t) {
        if (t.kind == TypeKind::Unsigned)
            return t.bits == 64 || n < (std::uint64_t(1) << t.bits);
        if (t.kind == TypeKind::Signed)
            return t.bits == 64 ? n <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
                                : n < (std::uint64_t(1) << (t.bits - 1));
        return false;
    }
    Type defaultLiteral(const Expr& e) const {
        if (e.magnitude <= std::uint64_t(std::numeric_limits<std::int32_t>::max()))
            return Type::signedInt(32);
        if (e.magnitude <= std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            return Type::signedInt(64);
        return Type::unsignedInt(64);
    }
    struct DiagnosticSpanGuard {
        SourceSpan prev;
        explicit DiagnosticSpanGuard(SourceSpan next) : prev(currentDiagnosticSpan) {
            currentDiagnosticSpan = next;
        }
        ~DiagnosticSpanGuard() {
            currentDiagnosticSpan = prev;
        }
    };
    Type infer(const Expr& e, const std::unordered_map<std::string, Type>& env, std::optional<Type> expected = {}) {
        DiagnosticSpanGuard spanGuard({currentSourceIndex, e.line, e.column, std::max(1, e.length)});
        Type result;
        switch (e.kind) {
            case Expr::Kind::Integer:
                if (expected && expected->kind == TypeKind::Record) {
                    auto recIt = recordFieldTypes.find(expected->name);
                    if (recIt != recordFieldTypes.end() && recIt->second.size() == 1) {
                        Type fieldType = recIt->second.begin()->second;
                        if (fieldType.integer() && fits(e.magnitude, fieldType)) {
                            result = *expected;
                            break;
                        }
                    }
                }
                result =
                    expected && expected->integer() && fits(e.magnitude, *expected) ? *expected : defaultLiteral(e);
                if (expected && expected->integer() && result != *expected)
                    throw Error("line " + std::to_string(e.line) + ": integer literal does not fit " +
                                typeName(*expected));
                break;
            case Expr::Kind::Floating:
                if (!expected || expected->kind != TypeKind::Float)
                    throw Error("line " + std::to_string(e.line) + ": floating-point literal requires an expected Float_32 or Float_64 type");
                result = *expected;
                break;
            case Expr::Kind::String:
                result = Type::string();
                break;
            case Expr::Kind::Character:
                result = Type::character();
                break;
            case Expr::Kind::NullLiteral: {
                if (expected && expected->kind == TypeKind::Pointer) {
                    result = *expected;
                } else {
                    result = Type::nullType();
                }
                break;
            }
            case Expr::Kind::Await: {
                if (!e.left) throw Error("line " + std::to_string(e.line) + ": Await requires an operand");
                Type inner = infer(*e.left, env);
                if (inner.kind == TypeKind::Future || inner.kind == TypeKind::TaskHandle) {
                    if (!inner.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid future result type");
                    result = *inner.pointeeType;
                } else {
                    throw Error("line " + std::to_string(e.line) + ": Await requires a Future or Task_Handle, found " + typeName(inner));
                }
                break;
            }
            case Expr::Kind::Closure: {
                std::unordered_map<std::string, Type> closureEnv = env;
                std::vector<Type> paramTypes;
                for (const auto& p : e.closureParams) {
                    paramTypes.push_back(p.second);
                    closureEnv[p.first] = p.second;
                }
                Type bodyType = infer(*e.closureBodyExpr, closureEnv);
                const_cast<Expr&>(e).closureReturnType = bodyType;
                result = Type::blockPointer(bodyType, paramTypes);
                break;
            }
            case Expr::Kind::Move:
            case Expr::Kind::Copy:
            case Expr::Kind::Borrow: {
                if (!e.left)
                    throw Error("line " + std::to_string(e.line) + ": Move/Copy/Borrow requires an operand expression");
                result = infer(*e.left, env, expected);
                break;
            }
            case Expr::Kind::Address: {
                if (baremetal && unsafeDepth == 0 && e.text != "__self__")
                    throw Error("line " + std::to_string(e.line) + ": raw pointer Address() on bare-metal requires an Unsafe block");
                if (e.left->kind != Expr::Kind::Variable && e.left->kind != Expr::Kind::Member &&
                    e.left->kind != Expr::Kind::Deref && e.left->kind != Expr::Kind::Index)
                    throw Error("line " + std::to_string(e.line) + ": Address() requires an addressable variable, field, or element");
                Type inner = infer(*e.left, env);
                result = Type::pointer(inner);
                break;
            }
            case Expr::Kind::Deref: {
                Type inner = infer(*e.left, env);
                if ((inner.kind != TypeKind::Pointer && inner.kind != TypeKind::SmartPointer) || !inner.pointeeType)
                    throw Error("line " + std::to_string(e.line) + ": Deref() requires a pointer type, found " + typeName(inner));
                if (baremetal && inner.kind == TypeKind::Pointer && unsafeDepth == 0)
                    throw Error("line " + std::to_string(e.line) + ": raw pointer Deref() on bare-metal requires an Unsafe block");
                result = *inner.pointeeType;
                break;
            }
            case Expr::Kind::Cast: {
                checkTypeAccess(e.targetType, e.line, currentSourceIndex);
                std::optional<Type> expForLeft = std::nullopt;
                if (e.left->kind == Expr::Kind::Floating) {
                    expForLeft = e.targetType;
                }
                infer(*e.left, env, expForLeft);
                Type src = typeOf(*e.left);
                Type dst = e.targetType;
                if (src.kind == TypeKind::Text) {
                    if (dst.kind == TypeKind::String) {
                        result = dst;
                        expressionTypes[&e] = result;
                        return result;
                    }
                    throw Error("line " + std::to_string(e.line) + ": cannot cast Text to " + typeName(dst) + "; only 'Text as String' zero-cost view is supported");
                }
                if (dst.kind == TypeKind::Text) {
                    throw Error("line " + std::to_string(e.line) + ": cannot cast " + typeName(src) + " to Text; use Text initialization and append operations to avoid implicit heap allocation");
                }
                if (src.kind == TypeKind::Record || dst.kind == TypeKind::Record)
                    throw Error("line " + std::to_string(e.line) + ": cannot cast composite Record type");
                if (src.kind == TypeKind::Slice || dst.kind == TypeKind::Slice)
                    throw Error("line " + std::to_string(e.line) + ": cannot cast Slice type; use Slice(...) constructor");
                if (src.kind == TypeKind::Array || dst.kind == TypeKind::Array)
                    throw Error("line " + std::to_string(e.line) + ": cannot cast Array type directly");
                if (dst.kind == TypeKind::Character && src.kind == TypeKind::Float)
                    throw Error("line " + std::to_string(e.line) + ": floating-point to Character casts are not supported; use an integer value known to be a Unicode scalar");
                if (dst.kind == TypeKind::Character && !src.integer() && src.kind != TypeKind::Enum &&
                    src.kind != TypeKind::Character)
                    throw Error("line " + std::to_string(e.line) + ": Character casts require an integer, enum, or Character source");
                if (dst.kind == TypeKind::Character) {
                    bool known = false, negative = false;
                    std::uint64_t scalar = 0;
                    if (e.left->kind == Expr::Kind::Integer) {
                        known = true;
                        scalar = e.left->magnitude;
                    } else if (e.left->kind == Expr::Kind::Unary && e.left->text == "-" &&
                               e.left->left && e.left->left->kind == Expr::Kind::Integer) {
                        known = true;
                        scalar = e.left->left->magnitude;
                        negative = scalar != 0;
                    }
                    if (known && (negative || !isUnicodeScalar(scalar)))
                        throw Error("line " + std::to_string(e.line) + ": integer cast is not a valid Unicode scalar value");
                }
                if (src.kind == TypeKind::String && dst.kind != TypeKind::Pointer)
                    throw Error("line " + std::to_string(e.line) + ": cannot cast String literal to non-pointer");
                unsigned srcBits = typeBitSize(src);
                unsigned dstBits = typeBitSize(dst);
                if (src.kind == TypeKind::Pointer && dst.integer() && dst.bits < 32) {
                    throw Error("line " + std::to_string(e.line) + ": cannot cast 64-bit Pointer to " + std::to_string(dst.bits) + "-bit integer '" + typeName(dst) + "'; truncation would corrupt address");
                }
                const bool isLiteralAndFits = (e.left->kind == Expr::Kind::Integer && fits(e.left->magnitude, dst));
                if (!isLiteralAndFits && ((srcBits > dstBits && dstBits > 0) || (src.kind == TypeKind::Float && dst.integer()))) {
                    bool useColor = false;
#ifndef _WIN32
                    if ((isatty(STDERR_FILENO) || std::getenv("LOOM_COLOR") != nullptr) && std::getenv("NO_COLOR") == nullptr) useColor = true;
#endif
                    const std::string yellow = useColor ? "\033[1;33m" : "";
                    const std::string bold = useColor ? "\033[1m" : "";
                    const std::string reset = useColor ? "\033[0m" : "";
                    std::cerr << yellow << "warning: " << reset
                              << bold << "line " << e.line << ": " << reset
                              << "narrowing cast from '" << typeName(src) << "' (" << srcBits
                              << " bits) to lower size '" << typeName(dst) << "' (" << dstBits
                              << " bits); data truncation may occur\n";
                }
                result = dst;
                break;
            }
            case Expr::Kind::Index: {
                Type container = infer(*e.left, env);
                if (container.kind == TypeKind::HMap) {
                    if (!container.keyType || !container.valueType)
                        throw Error("line " + std::to_string(e.line) + ": invalid HMap type");
                    Type keyType = infer(*e.right, env, *container.keyType);
                    if (keyType != *container.keyType) {
                        throw Error("line " + std::to_string(e.line) + ": HMap key type mismatch: expected " +
                                    typeName(*container.keyType) + ", found " + typeName(keyType));
                    }
                    result = *container.valueType;
                    break;
                }
                Type idxType = infer(*e.right, env);
                if (!idxType.integer())
                    throw Error("line " + std::to_string(e.line) + ": array/slice index must be an integer, found " + typeName(idxType));
                if (e.right->kind == Expr::Kind::Integer) {
                    if (e.right->magnitude == 0) {
                        throw Error("line " + std::to_string(e.line) + ": 1-based indexing requires indices >= 1; index 0 is out of bounds");
                    }
                    if (container.kind == TypeKind::Array && e.right->magnitude > container.arraySize) {
                        throw Error("line " + std::to_string(e.line) + ": index " + std::to_string(e.right->magnitude) +
                                    " exceeds array length " + std::to_string(container.arraySize));
                    }
                    if (container.kind == TypeKind::String && e.left->kind == Expr::Kind::String &&
                        e.right->magnitude > e.left->text.size()) {
                        throw Error("line " + std::to_string(e.line) + ": index " + std::to_string(e.right->magnitude) +
                                    " exceeds string length " + std::to_string(e.left->text.size()));
                    }
                } else if (e.right->kind == Expr::Kind::Unary && e.right->text == "-") {
                    throw Error("line " + std::to_string(e.line) + ": 1-based indexing requires indices >= 1; negative indices are out of bounds");
                }
                if (container.kind == TypeKind::Array) {
                    if (!container.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid array element type");
                    result = *container.pointeeType;
                } else if (container.kind == TypeKind::Slice) {
                    if (!container.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid slice element type");
                    result = *container.pointeeType;
                } else if (container.kind == TypeKind::Pointer) {
                    if (!container.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid pointer element type");
                    result = *container.pointeeType;
                } else if (container.kind == TypeKind::Vector) {
                    if (!container.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid vector element type");
                    result = *container.pointeeType;
                } else if (container.kind == TypeKind::String || container.kind == TypeKind::Text) {
                    result = Type::unsignedInt(8);
                } else {
                    auto it = overloadedIndexOps.find({typeName(container), typeName(idxType)});
                    if (it != overloadedIndexOps.end()) {
                        auto& mutableExpr = const_cast<Expr&>(e);
                        mutableExpr.kind = Expr::Kind::Call;
                        mutableExpr.text = it->second;
                        mutableExpr.arguments.clear();
                        mutableExpr.arguments.push_back(std::move(mutableExpr.left));
                        mutableExpr.arguments.push_back(std::move(mutableExpr.right));
                        return infer(mutableExpr, env, expected);
                    }
                    throw Error("line " + std::to_string(e.line) + ": indexing requires an Array, Slice, Pointer, Vector, String, or Text, found " + typeName(container));
                }
                break;
            }
            case Expr::Kind::SliceConstructor: {
                Type first = infer(*e.left, env);
                if (!e.right) {
                    if (first.kind != TypeKind::Array)
                        throw Error("line " + std::to_string(e.line) + ": single-argument Slice() requires an Array, found " + typeName(first));
                    result = Type::sliceType(*first.pointeeType);
                } else {
                    if (first.kind != TypeKind::Pointer)
                        throw Error("line " + std::to_string(e.line) + ": two-argument Slice(Ptr, Length) requires a Pointer first argument, found " + typeName(first));
                    Type lenType = infer(*e.right, env);
                    if (!lenType.integer())
                        throw Error("line " + std::to_string(e.line) + ": Slice length must be an integer, found " + typeName(lenType));
                    result = Type::sliceType(*first.pointeeType);
                }
                break;
            }
            case Expr::Kind::SubSlice: {
                Type container = infer(*e.left, env);
                if (e.arguments.size() != 2)
                    throw Error("line " + std::to_string(e.line) + ": sub-slice requires start and end bounds");
                Type startType = infer(*e.arguments[0], env);
                Type endType = infer(*e.arguments[1], env);
                if (!startType.integer() || !endType.integer())
                    throw Error("line " + std::to_string(e.line) + ": sub-slice bounds must be integers");
                if (e.arguments[0]->kind == Expr::Kind::Integer && e.arguments[0]->magnitude == 0)
                    throw Error("line " + std::to_string(e.line) + ": 1-based indexing requires indices >= 1; index 0 is out of bounds");
                if (e.arguments[0]->kind == Expr::Kind::Integer && e.arguments[1]->kind == Expr::Kind::Integer) {
                    if (e.arguments[0]->magnitude > e.arguments[1]->magnitude)
                        throw Error("line " + std::to_string(e.line) + ": sub-slice start index " + std::to_string(e.arguments[0]->magnitude) +
                                    " exceeds end index " + std::to_string(e.arguments[1]->magnitude));
                    if (container.kind == TypeKind::Array && e.arguments[1]->magnitude > container.arraySize)
                        throw Error("line " + std::to_string(e.line) + ": sub-slice end index " + std::to_string(e.arguments[1]->magnitude) +
                                    " exceeds array length " + std::to_string(container.arraySize));
                }
                if (container.kind == TypeKind::Array || container.kind == TypeKind::Slice || container.kind == TypeKind::Pointer) {
                    if (!container.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid element type");
                    result = Type::sliceType(*container.pointeeType);
                } else if (container.kind == TypeKind::Vector) {
                    if (!container.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid vector element type");
                    result = Type::sliceType(*container.pointeeType);
                } else if (container.kind == TypeKind::Text) {
                    result = Type::string();
                } else {
                    throw Error("line " + std::to_string(e.line) + ": sub-slicing requires an Array, Slice, Pointer, Vector, or Text, found " + typeName(container));
                }
                break;
            }
            case Expr::Kind::Try: {
                Type operand = infer(*e.left, env);
                if (!operand.errorType) {
                    throw Error("line " + std::to_string(e.line) + ": 'try' requires an expression with an error type ('Produces T or ...'), found " + typeName(operand));
                }
                if (!currentFunctionErrorResult) {
                    throw Error("line " + std::to_string(e.line) + ": cannot use 'try' in a block that does not produce an error type");
                }
                if (*currentFunctionErrorResult != *operand.errorType) {
                    throw Error("line " + std::to_string(e.line) + ": mismatched error type: block produces error '" + typeName(*currentFunctionErrorResult) +
                                "', but expression produces error '" + typeName(*operand.errorType) + "'");
                }
                result = operand;
                result.errorType = nullptr;
                break;
            }
            case Expr::Kind::OnError: {
                if (!e.left) throw Error("line " + std::to_string(e.line) + ": On_Error requires an expression");
                Type inner = infer(*e.left, env);
                if (!inner.errorType) {
                    throw Error("line " + std::to_string(e.line) + ": On_Error requires an error-producing expression ('Produces T or ...'), found " + typeName(inner));
                }
                std::string action = lower(e.text);
                if (action == "panic" || action == "halt") {
                    result = inner;
                    result.errorType = nullptr;
                } else if (action == "ignore") {
                    result = inner;
                    result.errorType = nullptr;
                } else if (action == "propagate" || action == "return") {
                    if (!currentFunctionErrorResult) {
                        throw Error("line " + std::to_string(e.line) + ": cannot propagate error in a block that does not produce an error type");
                    }
                    if (*currentFunctionErrorResult != *inner.errorType) {
                        throw Error("line " + std::to_string(e.line) + ": mismatched error type in On_Error(Propagate): block produces '" + typeName(*currentFunctionErrorResult) +
                                    "', but expression produces '" + typeName(*inner.errorType) + "'");
                    }
                    result = inner;
                    result.errorType = nullptr;
                } else {
                    throw Error("line " + std::to_string(e.line) + ": unknown error action '" + e.text + "' in On_Error; expected Panic, Ignore, or Propagate");
                }
                break;
            }
            case Expr::Kind::EnumVariant: {
                auto variant = enumVariantTypes.find(e.text);
                if (variant == enumVariantTypes.end())
                    throw Error("line " + std::to_string(e.line) + ": unresolved enum variant '" + e.text + "'");
                auto dot = e.text.find('.');
                if (dot != std::string::npos) {
                    std::string enumName = e.text.substr(0, dot);
                    auto eIt = enumModule.find(enumName);
                    if (eIt != enumModule.end())
                        checkAccess("enum", enumName, eIt->second.first, e.line, currentSourceIndex);
                }
                result = variant->second;
                break;
            }
            case Expr::Kind::ArrayLiteral: {
                const std::size_t count = e.arguments.size();
                if (expected && expected->kind == TypeKind::Array) {
                    if (expected->arraySize != count) {
                        throw Error("line " + std::to_string(e.line) + ": array literal has " +
                                    std::to_string(count) + " elements, but expected " +
                                    std::to_string(expected->arraySize));
                    }
                    Type elemType = *expected->pointeeType;
                    for (std::size_t i = 0; i < count; ++i) {
                        infer(*e.arguments[i], env, elemType);
                    }
                    result = *expected;
                } else if (expected && expected->kind == TypeKind::Vector) {
                    if (expected->hasExplicitSize && expected->arraySize != count) {
                        throw Error("line " + std::to_string(e.line) + ": array literal has " +
                                    std::to_string(count) + " elements, but vector declaration specified " +
                                    std::to_string(expected->arraySize));
                    }
                    Type elemType = *expected->pointeeType;
                    for (std::size_t i = 0; i < count; ++i) {
                        infer(*e.arguments[i], env, elemType);
                    }
                    result = *expected;
                    result.arraySize = count;
                    result.hasExplicitSize = true;
                } else {
                    if (count == 0) {
                        throw Error("line " + std::to_string(e.line) + ": cannot infer type of empty array literal without explicit type annotation");
                    }
                    Type elemType = infer(*e.arguments[0], env);
                    for (std::size_t i = 1; i < count; ++i) {
                        infer(*e.arguments[i], env, elemType);
                    }
                    result = Type::arrayType(elemType, static_cast<unsigned>(count));
                }
                break;
            }
            case Expr::Kind::TupleLiteral: {
                std::vector<Type> elemTypes;
                for (size_t i = 0; i < e.arguments.size(); ++i) {
                    std::optional<Type> expElem = std::nullopt;
                    if (expected && expected->kind == TypeKind::Tuple && i < expected->tupleTypes.size()) {
                        expElem = expected->tupleTypes[i];
                    }
                    if (expElem) {
                        elemTypes.push_back(infer(*e.arguments[i], env, *expElem));
                    } else {
                        elemTypes.push_back(infer(*e.arguments[i], env));
                    }
                }
                result = Type::tupleType(std::move(elemTypes));
                break;
            }
            case Expr::Kind::HMapLiteral: {

                if (e.arguments.empty()) {
                    if (expected && expected->kind == TypeKind::HMap) result = *expected;
                    else throw Error("line " + std::to_string(e.line) + ": empty HMap literal requires an expected type");
                    break;
                }
                Type kType;
                Type vType;
                if (expected && expected->kind == TypeKind::HMap && expected->keyType && expected->valueType) {
                    kType = *expected->keyType;
                    vType = *expected->valueType;
                } else {
                    kType = infer(*e.arguments[0], env);
                    vType = infer(*e.arguments[1], env);
                }
                for (size_t i = 0; i < e.arguments.size(); i += 2) {
                    Type curK = infer(*e.arguments[i], env, kType);
                    Type curV = infer(*e.arguments[i + 1], env, vType);
                    if (curK != kType) {
                        throw Error("line " + std::to_string(e.arguments[i]->line) + ": HMap key type mismatch: expected " + typeName(kType) + ", found " + typeName(curK));
                    }
                    if (curV != vType) {
                        throw Error("line " + std::to_string(e.arguments[i + 1]->line) + ": HMap value type mismatch: expected " + typeName(vType) + ", found " + typeName(curV));
                    }
                }
                result = Type::hmapType(kType, vType);
                break;
            }
            case Expr::Kind::Member: {
                std::vector<std::string> parts;
                if (flattenMemberChain(e, parts) && parts.size() >= 2 &&
                    env.find(parts[0]) == env.end() && globalTypes.find(parts[0]) == globalTypes.end()) {
                    std::string firstExpanded = resolveModuleAlias(parts[0], currentSourceIndex);
                    std::string modPath = firstExpanded;
                    for (std::size_t idx = 1; idx + 1 < parts.size(); ++idx) {
                        modPath += "." + parts[idx];
                    }
                    std::string modLower = lower(modPath);
                    if (isKnownModule(modLower)) {
                        checkAccess("module", modPath, modPath, e.line, currentSourceIndex);
                        const std::string& lastSym = parts.back();
                        auto gmIt = moduleGlobalTypes.find(modLower);
                        if (gmIt != moduleGlobalTypes.end() && gmIt->second.count(lastSym)) {
                            result = gmIt->second.at(lastSym);
                            resolvedGlobalKeys[&e] = mangleGlobalIfNeeded(modLower, lastSym);
                            break;
                        }
                        auto smIt = moduleSignatures.find(modLower);
                        if (smIt != moduleSignatures.end() && smIt->second.count(lastSym)) {
                            const Signature& sig = smIt->second.at(lastSym);
                            std::string mKey = mangleFuncIfNeeded(modLower, lastSym);
                            if (expected && expected->kind == TypeKind::BlockPointer) {
                                Type ret = sig.result ? *sig.result : Type::nullType();
                                result = Type::blockPointer(std::move(ret), sig.parameters);
                                resolvedCallKeys[&e] = mKey;
                                break;
                            }
                            if (!sig.parameters.empty()) {
                                throw Error("line " + std::to_string(e.line) + ": block '" + modPath + "." + lastSym + "' requires arguments");
                            }
                            if (!sig.result) {
                                throw Error("line " + std::to_string(e.line) + ": procedure '" + modPath + "." + lastSym + "' cannot be used as a value");
                            }
                            Expr& mutE = const_cast<Expr&>(e);
                            mutE.kind = Expr::Kind::Call;
                            mutE.qualifier = modPath;
                            mutE.text = lastSym;
                            mutE.left.reset();
                            resolvedCallKeys[&e] = mKey;
                            if (!currentAnalyzedFunctionKey.empty()) callGraphEdges[currentAnalyzedFunctionKey].insert(mKey);
                            result = sig.isAsync ? Type::futureType(*sig.result) : *sig.result;
                            break;
                        }
                    }
                    if (parts.size() >= 3) {
                        std::string enumModPath = firstExpanded;
                        for (std::size_t idx = 1; idx + 2 < parts.size(); ++idx) {
                            enumModPath += "." + parts[idx];
                        }
                        std::string enumModLower = lower(enumModPath);
                        if (isKnownModule(enumModLower)) {
                            const std::string& enumName = parts[parts.size() - 2];
                            const std::string& varName = parts.back();
                            const std::string evKey = enumName + "." + varName;
                            if (enumVariantTypes.count(evKey)) {
                                checkAccess("module", enumModPath, enumModPath, e.line, currentSourceIndex);
                                Expr& mutE = const_cast<Expr&>(e);
                                mutE.kind = Expr::Kind::EnumVariant;
                                mutE.text = evKey;
                                mutE.left.reset();
                                result = enumVariantTypes.at(evKey);
                                break;
                            }
                        }
                    }
                }
                Type base = infer(*e.left, env);
                if ((base.kind == TypeKind::Pointer || base.kind == TypeKind::SmartPointer) &&
                    base.pointeeType && base.pointeeType->kind == TypeKind::Record) {
                    base = *base.pointeeType;
                }
                if (base.kind == TypeKind::Slice && e.text == "Pointer") {
                    if (!base.pointeeType) throw Error("line " + std::to_string(e.line) + ": invalid element type");
                    result = Type::pointer(*base.pointeeType);
                    break;
                }
                if (base.kind == TypeKind::Slice && e.text == "Length") {
                    result = Type::unsignedInt(64);
                    break;
                }
                if ((base.kind == TypeKind::Vector || base.kind == TypeKind::Text || base.kind == TypeKind::HMap) && e.text == "Length") {
                    result = Type::unsignedInt(64);
                    break;
                }
                if ((base.kind == TypeKind::Vector || base.kind == TypeKind::Text) && e.text == "Capacity") {
                    result = Type::unsignedInt(64);
                    break;
                }
                if (base.kind == TypeKind::Array && e.text == "Length") {
                    result = Type::unsignedInt(64);
                    break;
                }
                if (base.kind == TypeKind::String && e.text == "Length") {
                    result = Type::unsignedInt(64);
                    break;
                }
                if (base.kind != TypeKind::Record)
                    throw Error("line " + std::to_string(e.line) + ": field access requires a Record value");
                auto record = recordFieldTypes.find(base.name);
                if (record == recordFieldTypes.end() || !record->second.count(e.text))
                    throw Error("line " + std::to_string(e.line) + ": record '" + base.name + "' has no field '" + e.text + "'");
                result = record->second.at(e.text);
                break;
            }
            case Expr::Kind::RecordLiteral: {
                auto it = recordFieldTypes.find(e.text);
                if (it == recordFieldTypes.end()) {
                    throw Error("line " + std::to_string(e.line) + ": unresolved record type '" + e.text + "'");
                }
                if (recordModule.count(e.text)) {
                    checkAccess("record", e.text, recordModule.at(e.text).first, e.line, currentSourceIndex);
                }
                const auto& expectedFields = it->second;
                const auto& order = recordFieldOrder.at(e.text);
                std::unordered_set<std::string> provided;
                for (std::size_t i = 0; i < e.fieldNames.size(); ++i) {
                    const std::string& fn = e.fieldNames[i];
                    if (!expectedFields.count(fn)) {
                        throw Error("line " + std::to_string(e.line) + ": record '" + e.text + "' has no field '" + fn + "'");
                    }
                    if (!provided.insert(fn).second) {
                        throw Error("line " + std::to_string(e.line) + ": duplicate field '" + fn + "' in record literal");
                    }
                    infer(*e.arguments[i], env, expectedFields.at(fn));
                }
                for (const auto& fn : order) {
                    if (!provided.count(fn)) {
                        throw Error("line " + std::to_string(e.line) + ": missing field '" + fn + "' in record literal of type '" + e.text + "'");
                    }
                }
                result = Type::recordType(e.text);
                break;
            }
            case Expr::Kind::Boolean:
                result = Type::boolean();
                break;
            case Expr::Kind::Variable: {
                auto i = env.find(e.text);
                if (i == env.end()) {
                    const Signature* sigPtr = nullptr;
                    std::string resolvedKey = e.text;
                    std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
                    if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(e.text)) {
                        sigPtr = &moduleSignatures.at(callerMod).at(e.text);
                        resolvedKey = mangleFuncIfNeeded(callerMod, e.text);
                    } else {
                        for (const auto& u : currentBlockUses) {
                            std::string uRes = lower(resolveModuleAlias(u, currentSourceIndex));
                            if (moduleSignatures.count(uRes) && moduleSignatures.at(uRes).count(e.text)) {
                                sigPtr = &moduleSignatures.at(uRes).at(e.text);
                                resolvedKey = mangleFuncIfNeeded(uRes, e.text);
                                break;
                            }
                        }
                        if (!sigPtr && fileUses.count(currentSourceIndex)) {
                            for (const auto& u : fileUses.at(currentSourceIndex)) {
                                std::string uRes = lower(resolveModuleAlias(u, currentSourceIndex));
                                if (moduleSignatures.count(uRes) && moduleSignatures.at(uRes).count(e.text)) {
                                    sigPtr = &moduleSignatures.at(uRes).at(e.text);
                                    resolvedKey = mangleFuncIfNeeded(uRes, e.text);
                                    break;
                                }
                            }
                        }
                    }
                    if (!sigPtr) {
                        auto sig = signatures.find(e.text);
                        if (sig != signatures.end()) sigPtr = &sig->second;
                    }
                    if (sigPtr) {
                        if (sigPtr->isExtern) resolvedKey = e.text;
                        if (expected && expected->kind == TypeKind::BlockPointer) {
                            Type ret = sigPtr->result ? *sigPtr->result : Type::nullType();
                            result = Type::blockPointer(std::move(ret), sigPtr->parameters);
                            resolvedCallKeys[&e] = resolvedKey;
                            break;
                        }
                        if (sigPtr->parameters.empty() && sigPtr->result) {
                            Expr& mutE = const_cast<Expr&>(e);
                            mutE.kind = Expr::Kind::Call;
                            resolvedCallKeys[&e] = resolvedKey;
                            if (!currentAnalyzedFunctionKey.empty()) callGraphEdges[currentAnalyzedFunctionKey].insert(resolvedKey);
                            result = sigPtr->isAsync ? Type::futureType(*sigPtr->result) : *sigPtr->result;
                            break;
                        }
                        Type ret = sigPtr->result ? *sigPtr->result : Type::nullType();
                        result = Type::blockPointer(std::move(ret), sigPtr->parameters);
                        resolvedCallKeys[&e] = resolvedKey;
                        break;
                    }
                    throw Error("line " + std::to_string(e.line) + ": undeclared variable '" + e.text + "'");
                }
                if (!currentFunctionLocals.count(e.text)) {
                    std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
                    if (!callerMod.empty() && moduleGlobalTypes.count(callerMod) && moduleGlobalTypes.at(callerMod).count(e.text)) {
                        resolvedGlobalKeys[&e] = mangleGlobalIfNeeded(callerMod, e.text);
                    } else if (globalModule.count(e.text)) {
                        checkAccess("global", e.text, globalModule.at(e.text).first, e.line, currentSourceIndex);
                        resolvedGlobalKeys[&e] = mangleGlobalIfNeeded(globalModule.at(e.text).first, e.text);
                    }
                }
                result = i->second;
                break;
            }
            case Expr::Kind::Call: {
                if (e.text == "Vector_Dot") {
                    if (e.arguments.size() != 3)
                        throw Error("line " + std::to_string(e.line) + ": Vector_Dot expects 3 arguments (A, B, N), got " + std::to_string(e.arguments.size()));
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    infer(*e.arguments[2], env);
                    result = Type::floating(32);
                    break;
                }
                if (isAIMLIntrinsic(e.text) && e.text != "Vector_Dot") {
                    throw Error("line " + std::to_string(e.line) + ": procedure '" + e.text + "' cannot be used as a value");
                }
                if (isVolatileRead(e.text)) {
                    if (unsafeDepth == 0)
                        throw Error("line " + std::to_string(e.line) + ": volatile MMIO read '" + e.text + "' requires an Unsafe block");
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects 1 argument (Address), got " + std::to_string(e.arguments.size()));
                    infer(*e.arguments[0], env);
                    if (e.text == "Vol_Read_8") result = Type::unsignedInt(8);
                    else if (e.text == "Vol_Read_16") result = Type::unsignedInt(16);
                    else if (e.text == "Vol_Read_32") result = Type::unsignedInt(32);
                    else if (e.text == "Vol_Read_64") result = Type::unsignedInt(64);
                    break;
                }
                if (isVolatileWrite(e.text) || isCpuIntrinsic(e.text)) {
                    throw Error("line " + std::to_string(e.line) + ": procedure '" + e.text + "' cannot be used as a value");
                }
                if (e.left) {
                    Type recType;
                    bool isRec = false;
                    try {
                        recType = infer(*e.left, env);
                        isRec = true;
                    } catch (...) {
                        isRec = false;
                    }
                    if (isRec && (recType.kind == TypeKind::Array || recType.kind == TypeKind::Slice)) {
                        const_cast<Expr&>(e).qualifier = "";
                        Type elemType = *recType.pointeeType;
                        if (e.text == "Length") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Array/Slice.Length expects 0 arguments");
                            result = Type::unsignedInt(64);
                            break;
                        }
                        if (e.text == "Contains") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Contains expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, elemType);
                            result = Type::boolean();
                            break;
                        }
                        if (e.text == "Index") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Index expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, elemType);
                            result = Type::unsignedInt(64);
                            break;
                        }
                        if (e.text == "Index_All") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Index_All expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, elemType);
                            result = Type::vectorType(Type::unsignedInt(64), 0, false);
                            break;
                        }
                        throw Error("line " + std::to_string(e.line) + ": unknown Array/Slice method '" + e.text + "'");
                    }
                    if (isRec && recType.kind == TypeKind::Text) {
                        const_cast<Expr&>(e).qualifier = "";
                        if (e.text == "Length" || e.text == "Capacity") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Text." + e.text + " expects 0 arguments");
                            result = Type::unsignedInt(64);
                            break;
                        }
                        if (e.text == "Contains") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Text.Contains expects 1 argument, got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, Type::string());
                            result = Type::boolean();
                            break;
                        }
                        if (e.text == "Index") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Text.Index expects 1 argument, got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, Type::string());
                            result = Type::unsignedInt(64);
                            break;
                        }
                        if (e.text == "Clear") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Text.Clear expects 0 arguments");
                            result = Type::nullType();
                            break;
                        }
                        throw Error("line " + std::to_string(e.line) + ": unknown Text method '" + e.text + "'");
                    }
                    if (isRec && recType.kind == TypeKind::Vector) {
                        const_cast<Expr&>(e).qualifier = "";
                        if (e.text == "Append" || e.text == "Push") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Vector." + e.text + " expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, *recType.pointeeType);
                            result = Type::nullType();
                            break;
                        }
                        if (e.text == "Pop") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Vector.Pop expects 0 arguments, got " + std::to_string(e.arguments.size()));
                            result = *recType.pointeeType;
                            break;
                        }
                        if (e.text == "Clear") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Vector.Clear expects 0 arguments");
                            result = Type::nullType();
                            break;
                        }
                        if (e.text == "Index") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Vector.Index expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, *recType.pointeeType);
                            result = Type::unsignedInt(64);
                            break;
                        }
                        if (e.text == "Index_All") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Vector.Index_All expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, *recType.pointeeType);
                            result = Type::vectorType(Type::unsignedInt(64), 0, false);
                            break;
                        }
                        if (e.text == "Contains") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Vector.Contains expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, *recType.pointeeType);
                            result = Type::boolean();
                            break;
                        }
                        if (e.text == "Length") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Vector.Length expects 0 arguments");
                            result = Type::unsignedInt(64);
                            break;
                        }
                        if (e.text == "Capacity") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": Vector.Capacity expects 0 arguments");
                            result = Type::unsignedInt(64);
                            break;
                        }
                        throw Error("line " + std::to_string(e.line) + ": unknown Vector method '" + e.text + "'");
                    }
                    if (isRec && recType.kind == TypeKind::HMap) {
                        const_cast<Expr&>(e).qualifier = "";
                        if (e.text == "Contains") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": HMap.Contains expects 1 argument (Key), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, *recType.keyType);
                            result = Type::boolean();
                            break;
                        }
                        if (e.text == "Remove") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": HMap.Remove expects 1 argument (Key), got " + std::to_string(e.arguments.size()));
                            infer(*e.arguments[0], env, *recType.keyType);
                            result = Type::boolean();
                            break;
                        }
                        if (e.text == "Clear") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": HMap.Clear expects 0 arguments");
                            result = Type::nullType();
                            break;
                        }
                        if (e.text == "Length") {
                            if (!e.arguments.empty())
                                throw Error("line " + std::to_string(e.line) + ": HMap.Length expects 0 arguments");
                            result = Type::unsignedInt(64);
                            break;
                        }
                        throw Error("line " + std::to_string(e.line) + ": unknown HMap method '" + e.text + "'");
                    }
                }
                if (!e.qualifier.empty()) {
                    std::string resolvedQual = resolveModuleAlias(e.qualifier, currentSourceIndex);
                    std::string qLower = lower(resolvedQual);
                    if (isKnownModule(qLower)) {
                        checkAccess("module", resolvedQual, resolvedQual, e.line, currentSourceIndex);
                        auto mIt = moduleSignatures.find(qLower);
                        if (mIt == moduleSignatures.end() || !mIt->second.count(e.text)) {
                            throw Error("line " + std::to_string(e.line) + ": unresolved block call '" +
                                        e.qualifier + "." + e.text + "'");
                        }
                        Expr& mutE = const_cast<Expr&>(e);
                        mutE.qualifier = resolvedQual;
                        mutE.left.reset();
                    } else if (!e.left) {
                        throw Error("line " + std::to_string(e.line) + ": unresolved block call '" +
                                    e.qualifier + "." + e.text + "'");
                    }
                } else if (!e.left) {
                    std::string callerMod = fileModules.count(currentSourceIndex) ? fileModules.at(currentSourceIndex) : "";
                    bool definedLocally = false;
                    if (!callerMod.empty() && moduleSignatures.count(lower(callerMod)) && moduleSignatures.at(lower(callerMod)).count(e.text)) {
                        definedLocally = true;
                    }
                    if (!definedLocally) {
                        auto fIt = funcModule.find(e.text);
                        if (fIt != funcModule.end()) {
                            const std::string& defMod = fIt->second.first;
                            if (!defMod.empty() && lower(defMod) != lower(callerMod)) {
                                checkAccess("block", e.text, defMod, e.line, currentSourceIndex);
                                bool allowed = false;
                                auto uIt = fileUses.find(currentSourceIndex);
                                if (uIt != fileUses.end() && uIt->second.count(lower(defMod))) {
                                    allowed = true;
                                }
                                if (currentBlockUses.count(lower(defMod))) {
                                    allowed = true;
                                }
                                if (!allowed) {
                                    throw Error("line " + std::to_string(e.line) + ": cannot call block '" + e.text +
                                                "' from module '" + defMod + "' unqualified; access it as '" +
                                                defMod + "." + e.text + "(...)' or add 'use " + defMod + "'");
                                }
                            }
                        }
                    }
                }
                if (signatures.find(e.text) == signatures.end() && env.find(e.text) == env.end() && isMathIntrinsic(e.text)) {
                    if (e.text == "Sqrt" || e.text == "Sin" || e.text == "Cos" || e.text == "Tan" ||
                        e.text == "ASin" || e.text == "ACos" || e.text == "ATan" || e.text == "Exp" ||
                        e.text == "Log" || e.text == "Floor" || e.text == "Ceil" || e.text == "Round") {
                        if (e.arguments.size() != 1)
                            throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects 1 argument, got " + std::to_string(e.arguments.size()));
                        Type t = infer(*e.arguments[0], env);
                        if (t.kind != TypeKind::Float)
                            throw Error("line " + std::to_string(e.line) + ": " + e.text + " requires Float argument, found " + typeName(t));
                        result = t;
                        break;
                    }
                    if (e.text == "ATan2") {
                        if (e.arguments.size() != 2)
                            throw Error("line " + std::to_string(e.line) + ": ATan2 expects 2 arguments (Y, X), got " + std::to_string(e.arguments.size()));
                        Type y = infer(*e.arguments[0], env);
                        Type x = infer(*e.arguments[1], env);
                        if (y.kind != TypeKind::Float || x.kind != TypeKind::Float)
                            throw Error("line " + std::to_string(e.line) + ": ATan2 requires Float arguments, found " + typeName(y) + " and " + typeName(x));
                        result = y;
                        break;
                    }
                    if (e.text == "Abs") {
                        if (e.arguments.size() != 1)
                            throw Error("line " + std::to_string(e.line) + ": Abs expects 1 argument, got " + std::to_string(e.arguments.size()));
                        Type t = infer(*e.arguments[0], env);
                        if (t.kind != TypeKind::Float && t.kind != TypeKind::Signed && t.kind != TypeKind::IntegerLiteral)
                            throw Error("line " + std::to_string(e.line) + ": Abs requires Float or Signed integer, found " + typeName(t));
                        result = t;
                        break;
                    }
                    if (e.text == "Min" || e.text == "Max") {
                        if (e.arguments.size() != 2)
                            throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects 2 arguments, got " + std::to_string(e.arguments.size()));
                        Type a = infer(*e.arguments[0], env);
                        Type b = infer(*e.arguments[1], env);
                        if (a.kind == TypeKind::IntegerLiteral && b.numeric()) a = b;
                        if (b.kind == TypeKind::IntegerLiteral && a.numeric()) b = a;
                        if (!a.numeric() || !b.numeric() || a != b)
                            throw Error("line " + std::to_string(e.line) + ": " + e.text + " requires arguments of matching numeric type, found " + typeName(a) + " and " + typeName(b));
                        result = a;
                        break;
                    }
                    if (e.text == "Pow") {
                        if (e.arguments.size() != 2)
                            throw Error("line " + std::to_string(e.line) + ": Pow expects 2 arguments (Base, Exp), got " + std::to_string(e.arguments.size()));
                        Type a = infer(*e.arguments[0], env);
                        Type b = infer(*e.arguments[1], env);
                        if (a.kind == TypeKind::IntegerLiteral && b.numeric()) a = b;
                        if (b.kind == TypeKind::IntegerLiteral && a.numeric()) b = a;
                        if (a.kind == TypeKind::Float || b.kind == TypeKind::Float) {
                            result = a.kind == TypeKind::Float ? a : b;
                        } else if (a.integer() && b.integer()) {
                            result = a;
                        } else {
                            throw Error("line " + std::to_string(e.line) + ": Pow requires numeric types, found " + typeName(a) + " and " + typeName(b));
                        }
                        break;
                    }
                }
                if (signatures.find(e.text) == signatures.end() && env.find(e.text) == env.end() && isFileIntrinsic(e.text)) {
                    if (baremetal || (targetProfile && !targetProfile->supportsFileIO()))
                        throw Error("line " + std::to_string(e.line) + ": file I/O operations are forbidden on bare-metal target");

                    if (e.text == "Open_File") {
                        if (e.arguments.size() != 2)
                            throw Error("line " + std::to_string(e.line) + ": Open_File expects 2 arguments (Path, Mode), got " + std::to_string(e.arguments.size()));
                        Type pathType = infer(*e.arguments[0], env);
                        Type modeType = infer(*e.arguments[1], env);
                        if (pathType.kind != TypeKind::String)
                            throw Error("line " + std::to_string(e.line) + ": Open_File path must be String, found " + typeName(pathType));
                        if (modeType.kind != TypeKind::String && !modeType.integer())
                            throw Error("line " + std::to_string(e.line) + ": Open_File mode must be String (e.g. \"r\", \"w\", \"a\") or integer (1..5), found " + typeName(modeType));
                        result = Type::fileHandle();
                        break;
                    }
                    if (e.text == "Read_File") {
                        if (e.arguments.size() != 3)
                            throw Error("line " + std::to_string(e.line) + ": Read_File expects 3 arguments (File, Buffer, Count), got " + std::to_string(e.arguments.size()));
                        Type fh = infer(*e.arguments[0], env);
                        Type buf = infer(*e.arguments[1], env);
                        Type cnt = infer(*e.arguments[2], env);
                        if (fh.kind != TypeKind::FileHandle)
                            throw Error("line " + std::to_string(e.line) + ": Read_File expects File_Handle");
                        result = Type::unsignedInt(64);
                        break;
                    }
                    if (e.text == "Read_All") {
                        if (e.arguments.size() != 1)
                            throw Error("line " + std::to_string(e.line) + ": Read_All expects 1 argument (Path), got " + std::to_string(e.arguments.size()));
                        Type pathType = infer(*e.arguments[0], env);
                        if (pathType.kind != TypeKind::String)
                            throw Error("line " + std::to_string(e.line) + ": Read_All path must be String, found " + typeName(pathType));
                        result = Type::string();
                        break;
                    }
                    if (e.text == "Close_File") {
                        if (e.arguments.size() != 1)
                            throw Error("line " + std::to_string(e.line) + ": Close_File expects 1 argument (File), got " + std::to_string(e.arguments.size()));
                        Type fh = infer(*e.arguments[0], env);
                        if (fh.kind != TypeKind::FileHandle)
                            throw Error("line " + std::to_string(e.line) + ": Close_File expects File_Handle");
                        result = Type::nullType();
                        break;
                    }
                }
                if (e.text == "Append") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Append expects 2 arguments (Vector, Item), got " + std::to_string(e.arguments.size()));
                    Type vecType = infer(*e.arguments[0], env);
                    if (vecType.kind != TypeKind::Vector)
                        throw Error("line " + std::to_string(e.line) + ": Append requires a Vector first argument, found " + typeName(vecType));
                    infer(*e.arguments[1], env, *vecType.pointeeType);
                    result = Type::nullType();
                    break;
                }
                if (e.text == "Pop") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Pop expects 1 argument (Vector), got " + std::to_string(e.arguments.size()));
                    Type vecType = infer(*e.arguments[0], env);
                    if (vecType.kind != TypeKind::Vector)
                        throw Error("line " + std::to_string(e.line) + ": Pop requires a Vector argument, found " + typeName(vecType));
                    result = *vecType.pointeeType;
                    break;
                }
                if (e.text == "Index") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Index expects 2 arguments (Collection, Item), got " + std::to_string(e.arguments.size()));
                    Type collType = infer(*e.arguments[0], env);
                    if (collType.kind == TypeKind::Vector || collType.kind == TypeKind::Array || collType.kind == TypeKind::Slice) {
                        infer(*e.arguments[1], env, *collType.pointeeType);
                    } else if (collType.kind == TypeKind::Text) {
                        infer(*e.arguments[1], env, Type::string());
                    } else {
                        throw Error("line " + std::to_string(e.line) + ": Index requires a Vector, Array, Slice, or Text, found " + typeName(collType));
                    }
                    result = Type::unsignedInt(64);
                    break;
                }
                if (e.text == "Index_All") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Index_All expects 2 arguments (Collection, Item), got " + std::to_string(e.arguments.size()));
                    Type collType = infer(*e.arguments[0], env);
                    if (collType.kind == TypeKind::Vector || collType.kind == TypeKind::Array || collType.kind == TypeKind::Slice) {
                        infer(*e.arguments[1], env, *collType.pointeeType);
                    } else {
                        throw Error("line " + std::to_string(e.line) + ": Index_All requires a Vector, Array, or Slice, found " + typeName(collType));
                    }
                    result = Type::vectorType(Type::unsignedInt(64), 0, false);
                    break;
                }
                if (e.text == "Contains") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Contains expects 2 arguments (Collection, Item/Key), got " + std::to_string(e.arguments.size()));
                    Type collType = infer(*e.arguments[0], env);
                    if (collType.kind == TypeKind::Vector || collType.kind == TypeKind::Array || collType.kind == TypeKind::Slice) {
                        infer(*e.arguments[1], env, *collType.pointeeType);
                    } else if (collType.kind == TypeKind::HMap) {
                        infer(*e.arguments[1], env, *collType.keyType);
                    } else if (collType.kind == TypeKind::Text) {
                        infer(*e.arguments[1], env, Type::string());
                    } else {
                        throw Error("line " + std::to_string(e.line) + ": Contains requires a Vector, Array, Slice, HMap, or Text, found " + typeName(collType));
                    }
                    result = Type::boolean();
                    break;
                }
                if (e.text == "To_String") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": To_String expects 1 argument, got " + std::to_string(e.arguments.size()));
                    infer(*e.arguments[0], env);
                    result = Type::string();
                    break;
                }
                if (e.text == "Wrapping_Add" || e.text == "Wrapping_Sub" || e.text == "Wrapping_Mul") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects 2 integer arguments");
                    Type a = infer(*e.arguments[0], env);
                    Type b = infer(*e.arguments[1], env, a.kind == TypeKind::IntegerLiteral ? std::nullopt : std::optional<Type>(a));
                    if (a.kind == TypeKind::IntegerLiteral && (b.kind == TypeKind::Signed || b.kind == TypeKind::Unsigned)) a = b;
                    if (b.kind == TypeKind::IntegerLiteral && (a.kind == TypeKind::Signed || a.kind == TypeKind::Unsigned)) b = a;
                    if ((a.kind != TypeKind::Signed && a.kind != TypeKind::Unsigned) || a != b)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " requires matching fixed-width integer types");
                    result = a;
                    break;
                }
                if (e.text == "__loom_task_cancel") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Cancel expects 1 task argument");
                    infer(*e.arguments[0], env);
                    result = Type::nullType();
                    break;
                }
                if (e.text == "Make_Smt") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Make_Smt expects 1 argument");
                    Type valType = infer(*e.arguments[0], env);
                    result = Type::smartPointer(valType);
                    break;
                }
                if (e.text == "Smt_Count") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Smt_Count expects 1 argument");
                    Type smtType = infer(*e.arguments[0], env);
                    if (smtType.kind != TypeKind::SmartPointer)
                        throw Error("line " + std::to_string(e.line) + ": Smt_Count requires a Smt_Pointer, found " + typeName(smtType));
                    result = Type::unsignedInt(64);
                    break;
                }
                if (e.text == "Channel_Create" || e.text == "Channel_Open") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Channel creation expects 1 argument (capacity)");
                    infer(*e.arguments[0], env);
                    if (e.targetType.kind == TypeKind::Channel) {
                        result = e.targetType;
                    } else if (expected && expected->kind == TypeKind::Channel) {
                        result = *expected;
                    } else {
                        throw Error("line " + std::to_string(e.line) + ": Channel requires explicit type parameter e.g. Channel[T](capacity)");
                    }
                    break;
                }
                if (e.text == "Channel_Send") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Channel_Send expects 2 arguments (channel, value)");
                    Type chType = infer(*e.arguments[0], env);
                    if (chType.kind != TypeKind::Channel || !chType.pointeeType)
                        throw Error("line " + std::to_string(e.line) + ": Channel_Send requires a Channel, found " + typeName(chType));
                    infer(*e.arguments[1], env, *chType.pointeeType);
                    result = Type::nullType();
                    break;
                }
                if (e.text == "Channel_Receive" || e.text == "Channel_Try_Receive") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects 1 argument (channel)");
                    Type chType = infer(*e.arguments[0], env);
                    if (chType.kind != TypeKind::Channel || !chType.pointeeType)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " requires a Channel, found " + typeName(chType));
                    result = *chType.pointeeType;
                    break;
                }
                if (e.text == "Channel_Close") {
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Channel_Close expects 1 argument (channel)");
                    Type chType = infer(*e.arguments[0], env);
                    if (chType.kind != TypeKind::Channel)
                        throw Error("line " + std::to_string(e.line) + ": Channel_Close requires a Channel, found " + typeName(chType));
                    result = Type::nullType();
                    break;
                }
                if (e.text == "Mutex_Create") {
                    result = Type::mutexType();
                    break;
                }
                if (e.text == "Atomic_Load") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Atomic_Load expects 1 argument");
                    Type aType = infer(*e.arguments[0], env);
                    if (aType.kind != TypeKind::Atomic || !aType.pointeeType)
                        throw Error("line " + std::to_string(e.line) + ": Atomic_Load requires an Atomic[T], found " + typeName(aType));
                    result = *aType.pointeeType;
                    break;
                }
                if (e.text == "Atomic_Store") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Atomic_Store expects 2 arguments");
                    Type aType = infer(*e.arguments[0], env);
                    if (aType.kind != TypeKind::Atomic || !aType.pointeeType)
                        throw Error("line " + std::to_string(e.line) + ": Atomic_Store requires an Atomic[T], found " + typeName(aType));
                    infer(*e.arguments[1], env, *aType.pointeeType);
                    result = Type::nullType();
                    break;
                }
                if (e.text == "Atomic_Fetch_Add" || e.text == "Atomic_Fetch_Sub") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects 2 arguments");
                    Type aType = infer(*e.arguments[0], env);
                    if (aType.kind != TypeKind::Atomic || !aType.pointeeType)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " requires an Atomic[T], found " + typeName(aType));
                    infer(*e.arguments[1], env, *aType.pointeeType);
                    result = *aType.pointeeType;
                    break;
                }
                if (e.text == "Atomic_CAS") {
                    if (e.arguments.size() != 3) throw Error("line " + std::to_string(e.line) + ": Atomic_CAS expects 3 arguments");
                    Type aType = infer(*e.arguments[0], env);
                    if (aType.kind != TypeKind::Atomic || !aType.pointeeType)
                        throw Error("line " + std::to_string(e.line) + ": Atomic_CAS requires an Atomic[T], found " + typeName(aType));
                    infer(*e.arguments[1], env, *aType.pointeeType);
                    infer(*e.arguments[2], env, *aType.pointeeType);
                    result = Type::boolean();
                    break;
                }
                if (e.text == "Tcp_Listen" || e.text == "Tcp_Listen_V6" || e.text == "Tcp_Accept" ||
                    e.text == "Tcp_Connect" || e.text == "Tcp_Connect_V6" || e.text == "Udp_Bind" ||
                    e.text == "Udp_Bind_V6" || e.text == "Udp_Connect" || e.text == "Udp_Connect_V6") {
                    if (baremetal)
                        throw Error("line " + std::to_string(e.line) + ": networking operations are forbidden on bare-metal target");
                }
                if (e.text == "Tcp_Listen" || e.text == "Tcp_Listen_V6") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects (host, port)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tcp_Accept") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Tcp_Accept expects (listener_fd)");
                    infer(*e.arguments[0], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tcp_Connect" || e.text == "Tcp_Connect_V6") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects (host, port)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tcp_Send") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tcp_Send expects (fd, data)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tcp_Send_At") {
                    if (e.arguments.size() != 3) throw Error("line " + std::to_string(e.line) + ": Tcp_Send_At expects (fd, Text, one_based_offset)");
                    infer(*e.arguments[0], env);
                    Type dataType = infer(*e.arguments[1], env);
                    if (dataType.kind != TypeKind::Text)
                        throw Error("line " + std::to_string(e.line) + ": Tcp_Send_At requires Text data");
                    infer(*e.arguments[2], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tcp_Send_Slice") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tcp_Send_Slice expects (fd, byte_slice)");
                    infer(*e.arguments[0], env);
                    Type bytes = infer(*e.arguments[1], env);
                    if (bytes.kind != TypeKind::Slice || !bytes.pointeeType || bytes.pointeeType->kind != TypeKind::Unsigned || bytes.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": Tcp_Send_Slice requires Unsigned_8 Slice");
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tcp_Receive_Buffer" || e.text == "Udp_Receive_Buffer") {
                    if (e.arguments.size() != 3) throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects (fd, byte_pointer, capacity)");
                    infer(*e.arguments[0], env);
                    Type buffer = infer(*e.arguments[1], env);
                    if (buffer.kind != TypeKind::Pointer || !buffer.pointeeType || buffer.pointeeType->kind != TypeKind::Unsigned || buffer.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " requires Unsigned_8 Pointer");
                    infer(*e.arguments[2], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Udp_Send_To_Slice" || e.text == "Udp_Send_To_Slice_V6") {
                    if (e.arguments.size() != 4) throw Error("line " + std::to_string(e.line) + ": Udp_Send_To_Slice expects (fd, host, port, byte_slice)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env); infer(*e.arguments[2], env);
                    Type bytes = infer(*e.arguments[3], env);
                    if (bytes.kind != TypeKind::Slice || !bytes.pointeeType || bytes.pointeeType->kind != TypeKind::Unsigned || bytes.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": Udp_Send_To_Slice requires Unsigned_8 Slice");
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Socket_Set_Timeout") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Socket_Set_Timeout expects (fd, timeout_milliseconds)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Socket_Wait") {
                    if (e.arguments.size() != 3) throw Error("line " + std::to_string(e.line) + ": Socket_Wait expects (fd, event_mask, timeout_milliseconds)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env); infer(*e.arguments[2], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Socket_Set_Nonblocking") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Socket_Set_Nonblocking expects (fd, nonblocking)");
                    infer(*e.arguments[0], env);
                    Type nb = infer(*e.arguments[1], env);
                    if (nb.kind != TypeKind::Boolean) throw Error("line " + std::to_string(e.line) + ": Socket_Set_Nonblocking requires Boolean flag");
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Socket_Get_Error") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Socket_Get_Error expects (fd)");
                    infer(*e.arguments[0], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "System_Random") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": System_Random expects (Unsigned_8 Pointer, capacity)");
                    Type buffer = infer(*e.arguments[0], env);
                    if (buffer.kind != TypeKind::Pointer || !buffer.pointeeType || buffer.pointeeType->kind != TypeKind::Unsigned || buffer.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": System_Random requires Unsigned_8 Pointer");
                    infer(*e.arguments[1], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tcp_Receive") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tcp_Receive expects (fd, max_bytes)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Tcp_Close") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Tcp_Close expects (fd)");
                    infer(*e.arguments[0], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Udp_Bind" || e.text == "Udp_Bind_V6") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects (host, port)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Udp_Connect" || e.text == "Udp_Connect_V6") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": " + e.text + " expects (host, port)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Udp_Send") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Udp_Send expects (connected_udp_socket, data)");
                    infer(*e.arguments[0], env);
                    Type dataType = infer(*e.arguments[1], env);
                    if (dataType.kind != TypeKind::Text && dataType.kind != TypeKind::String)
                        throw Error("line " + std::to_string(e.line) + ": Udp_Send expects Text or String data");
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Udp_Send_Slice") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Udp_Send_Slice expects (connected_udp_socket, byte_slice)");
                    infer(*e.arguments[0], env);
                    Type bytes = infer(*e.arguments[1], env);
                    if (bytes.kind != TypeKind::Slice || !bytes.pointeeType || bytes.pointeeType->kind != TypeKind::Unsigned || bytes.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": Udp_Send_Slice requires Unsigned_8 Slice");
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Udp_Send_To") {
                    if (e.arguments.size() != 4) throw Error("line " + std::to_string(e.line) + ": Udp_Send_To expects (fd, host, port, data)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    infer(*e.arguments[2], env);
                    infer(*e.arguments[3], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Udp_Send_To_V6") {
                    if (e.arguments.size() != 4) throw Error("line " + std::to_string(e.line) + ": Udp_Send_To_V6 expects (fd, host, port, data)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env); infer(*e.arguments[2], env); infer(*e.arguments[3], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Udp_Receive_From") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Udp_Receive_From expects (fd, max_bytes)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Udp_Close") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Udp_Close expects (fd)");
                    infer(*e.arguments[0], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tls_Client_New") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tls_Client_New expects (host, ca_path)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tls_Connect") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tls_Connect expects (handle, fd)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tls_Send") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tls_Send expects (handle, data)");
                    infer(*e.arguments[0], env);
                    Type dataType = infer(*e.arguments[1], env);
                    if (dataType.kind != TypeKind::Text && dataType.kind != TypeKind::String)
                        throw Error("line " + std::to_string(e.line) + ": Tls_Send expects Text or String data");
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tls_Send_Slice") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tls_Send_Slice expects (handle, byte_slice)");
                    infer(*e.arguments[0], env);
                    Type bytes = infer(*e.arguments[1], env);
                    if (bytes.kind != TypeKind::Slice || !bytes.pointeeType || bytes.pointeeType->kind != TypeKind::Unsigned || bytes.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": Tls_Send_Slice requires Unsigned_8 Slice");
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tls_Receive") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tls_Receive expects (handle, max_bytes)");
                    infer(*e.arguments[0], env); infer(*e.arguments[1], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Tls_Receive_Buffer") {
                    if (e.arguments.size() != 3) throw Error("line " + std::to_string(e.line) + ": Tls_Receive_Buffer expects (handle, buffer, capacity)");
                    infer(*e.arguments[0], env);
                    Type buffer = infer(*e.arguments[1], env);
                    if (buffer.kind != TypeKind::Pointer || !buffer.pointeeType || buffer.pointeeType->kind != TypeKind::Unsigned || buffer.pointeeType->bits != 8)
                        throw Error("line " + std::to_string(e.line) + ": Tls_Receive_Buffer requires Unsigned_8 Pointer");
                    infer(*e.arguments[2], env);
                    result = Type::signedInt(64);
                    break;
                }
                if (e.text == "Tls_Close") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Tls_Close expects (handle)");
                    infer(*e.arguments[0], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tls_Set_Alpn") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Tls_Set_Alpn expects (handle, protocols)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "Tls_Get_Alpn") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Tls_Get_Alpn expects (handle)");
                    infer(*e.arguments[0], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Quic_Initial_Keys") {
                    if (e.arguments.size() != 1) throw Error("line " + std::to_string(e.line) + ": Quic_Initial_Keys expects (dcid_hex)");
                    infer(*e.arguments[0], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Quic_Hp_Mask") {
                    if (e.arguments.size() != 2) throw Error("line " + std::to_string(e.line) + ": Quic_Hp_Mask expects (hp_hex, sample_hex)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Quic_Encrypt") {
                    if (e.arguments.size() != 5) throw Error("line " + std::to_string(e.line) + ": Quic_Encrypt expects (key_hex, iv_hex, pkt_num, ad_hex, plain_hex)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    infer(*e.arguments[2], env);
                    infer(*e.arguments[3], env);
                    infer(*e.arguments[4], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "Quic_Decrypt") {
                    if (e.arguments.size() != 5) throw Error("line " + std::to_string(e.line) + ": Quic_Decrypt expects (key_hex, iv_hex, pkt_num, ad_hex, cipher_hex)");
                    infer(*e.arguments[0], env);
                    infer(*e.arguments[1], env);
                    infer(*e.arguments[2], env);
                    infer(*e.arguments[3], env);
                    infer(*e.arguments[4], env);
                    result = Type::textType();
                    break;
                }
                if (e.text == "__loom_usb_is_configured" || e.text == "__loom_usb_available" || e.text == "__loom_usb_read_byte") {
                    result = Type::signedInt(32);
                    break;
                }
                if (e.text == "__loom_usb_putchar") {
                    if (!e.arguments.empty()) infer(*e.arguments[0], env);
                    result = Type::nullType();
                    break;
                }
                if (e.text == "__loom_usb_write_str") {
                    if (!e.arguments.empty()) infer(*e.arguments[0], env);
                    result = Type::nullType();
                    break;
                }
                if (e.text == "__loom_irq_save") {
                    result = Type::unsignedInt(32);
                    break;
                }
                if (e.text == "__loom_irq_restore") {
                    if (!e.arguments.empty()) infer(*e.arguments[0], env);
                    result = Type::nullType();
                    break;
                }
                if (env.count(e.text) && env.at(e.text).kind == TypeKind::BlockPointer) {
                    Type bpType = env.at(e.text);
                    if (e.arguments.size() != bpType.paramTypes.size()) {
                        throw Error("line " + std::to_string(e.line) + ": block pointer '" + e.text + "' expects " +
                                    std::to_string(bpType.paramTypes.size()) + " arguments, got " + std::to_string(e.arguments.size()));
                    }
                    for (size_t i = 0; i < e.arguments.size(); ++i) {
                        infer(*e.arguments[i], env, bpType.paramTypes[i]);
                    }
                    result = bpType.pointeeType ? *bpType.pointeeType : Type::nullType();
                    break;
                }
                if (e.left) {
                    Type objType = infer(*e.left, env);
                    std::string recName;
                    bool isPtr = false;
                    if (objType.kind == TypeKind::Record) recName = objType.name;
                    else if ((objType.kind == TypeKind::Pointer || objType.kind == TypeKind::SmartPointer) &&
                             objType.pointeeType && objType.pointeeType->kind == TypeKind::Record) {
                        recName = objType.pointeeType->name;
                        isPtr = true;
                    }
                    if (!recName.empty()) {
                        const Signature* mSig = nullptr;
                        std::string mKey;
                        auto matchSig = [&](const Signature& cand) {
                            if (cand.parameters.empty()) return false;
                            const Type& p0 = cand.parameters[0];
                            if (p0.kind == TypeKind::Record && isRecordSubtype(recName, p0.name)) return true;
                            if ((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) &&
                                p0.pointeeType && p0.pointeeType->kind == TypeKind::Record &&
                                isRecordSubtype(recName, p0.pointeeType->name)) return true;
                            return false;
                        };
                        std::string curR = recName;
                        auto dot = curR.rfind('.');
                        std::string shortR = (dot != std::string::npos) ? curR.substr(dot + 1) : curR;
                        while (!curR.empty() && !mSig) {
                            auto tmIt = taggedMethodSignatures.find(curR);
                            if (tmIt == taggedMethodSignatures.end() && !shortR.empty()) {
                                tmIt = taggedMethodSignatures.find(shortR);
                            }
                            if (tmIt != taggedMethodSignatures.end() && tmIt->second.count(e.text)) {
                                const auto& entry = tmIt->second.at(e.text);
                                if (matchSig(entry.first)) {
                                    mSig = &entry.first;
                                    mKey = entry.second;
                                    break;
                                }
                            }
                            auto rmIt = recordModule.find(curR);
                            if (rmIt == recordModule.end() && !shortR.empty()) {
                                rmIt = recordModule.find(shortR);
                            }
                            if (rmIt != recordModule.end() && !rmIt->second.first.empty()) {
                                std::string rModLower = lower(rmIt->second.first);
                                if (moduleSignatures.count(rModLower) && moduleSignatures.at(rModLower).count(e.text)) {
                                    const Signature& cand = moduleSignatures.at(rModLower).at(e.text);
                                    if (matchSig(cand)) {
                                        mSig = &cand;
                                        mKey = mangleFuncIfNeeded(rModLower, e.text);
                                        break;
                                    }
                                }
                            }
                            auto pIt = recordParent.find(curR);
                            if (pIt == recordParent.end() && !shortR.empty()) {
                                pIt = recordParent.find(shortR);
                            }
                            curR = (pIt != recordParent.end()) ? pIt->second : "";
                            dot = curR.rfind('.');
                            shortR = (dot != std::string::npos) ? curR.substr(dot + 1) : curR;
                        }
                        if (!mSig) {
                            std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
                            if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(e.text)) {
                                const Signature& cand = moduleSignatures.at(callerMod).at(e.text);
                                if (matchSig(cand)) { mSig = &cand; mKey = mangleFuncIfNeeded(callerMod, e.text); }
                            }
                        }
                        if (!mSig) {
                            auto sIt = signatures.find(e.text);
                            if (sIt != signatures.end() && matchSig(sIt->second)) {
                                mSig = &sIt->second;
                                mKey = e.text;
                            }
                        }
                        if (!mSig) {
                            throw Error("line " + std::to_string(e.line) + ": unresolved method '" + e.text + "' on record '" + recName + "'");
                        }
                        if (!mSig->result) {
                            throw Error("line " + std::to_string(e.line) + ": procedure '" + e.text + "' cannot be used as a value");
                        }
                        if (e.arguments.size() + 1 != mSig->parameters.size()) {
                            throw Error("line " + std::to_string(e.line) + ": method '" + e.text + "' expects " +
                                        std::to_string(mSig->parameters.size() - 1) + " arguments, got " +
                                        std::to_string(e.arguments.size()));
                        }
                        Expr& mutE = const_cast<Expr&>(e);
                        std::unique_ptr<Expr> selfArg;
                        if (!isPtr && mSig->parameters[0].kind == TypeKind::Pointer) {
                            selfArg = std::make_unique<Expr>();
                            selfArg->kind = Expr::Kind::Address;
                            selfArg->text = "__self__";
                            selfArg->line = e.line;
                            selfArg->left = std::move(mutE.left);
                        } else {
                            selfArg = std::move(mutE.left);
                        }
                        mutE.arguments.insert(mutE.arguments.begin(), std::move(selfArg));
                        mutE.qualifier.clear();
                        for (std::size_t idx = 0; idx < mutE.arguments.size(); ++idx) {
                            infer(*mutE.arguments[idx], env, mSig->parameters[idx]);
                        }
                        resolvedCallKeys[&e] = mKey;
                        if (isPtr && recordIsTagged[recName] && recordRoot.count(recName)) {
                            const std::string& root = recordRoot.at(recName);
                            const auto& slots = vtableSlotNames[root];
                            auto sPos = std::find(slots.begin(), slots.end(), e.text);
                            if (sPos != slots.end()) {
                                virtualDispatchCalls[&e] = {root, static_cast<std::size_t>(std::distance(slots.begin(), sPos))};
                            }
                        }
                        if (!currentAnalyzedFunctionKey.empty()) callGraphEdges[currentAnalyzedFunctionKey].insert(mKey);
                        result = mSig->isAsync ? Type::futureType(*mSig->result) : *mSig->result;
                        break;
                    }
                }
                const Signature* sigPtr = nullptr;
                std::string resolvedKey = e.text;
                if (!e.qualifier.empty()) {
                    std::string qLower = lower(e.qualifier);
                    sigPtr = &moduleSignatures.at(qLower).at(e.text);
                    resolvedKey = mangleFuncIfNeeded(qLower, e.text);
                    if (!e.arguments.empty()) {
                        Type a0 = infer(*e.arguments[0], env);
                        std::string candR = (a0.kind == TypeKind::Record) ? a0.name :
                            (((a0.kind == TypeKind::Pointer || a0.kind == TypeKind::SmartPointer) && a0.pointeeType && a0.pointeeType->kind == TypeKind::Record) ? a0.pointeeType->name : "");
                        if (!candR.empty() && taggedMethodSignatures.count(candR) && taggedMethodSignatures.at(candR).count(e.text)) {
                            resolvedKey = taggedMethodSignatures.at(candR).at(e.text).second;
                        }
                    }
                } else {
                    std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
                    if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(e.text)) {
                        sigPtr = &moduleSignatures.at(callerMod).at(e.text);
                        resolvedKey = mangleFuncIfNeeded(callerMod, e.text);
                    } else {
                        for (const auto& u : currentBlockUses) {
                            std::string uRes = lower(resolveModuleAlias(u, currentSourceIndex));
                            if (moduleSignatures.count(uRes) && moduleSignatures.at(uRes).count(e.text)) {
                                sigPtr = &moduleSignatures.at(uRes).at(e.text);
                                resolvedKey = mangleFuncIfNeeded(uRes, e.text);
                                break;
                            }
                        }
                        if (!sigPtr && fileUses.count(currentSourceIndex)) {
                            for (const auto& u : fileUses.at(currentSourceIndex)) {
                                std::string uRes = lower(resolveModuleAlias(u, currentSourceIndex));
                                if (moduleSignatures.count(uRes) && moduleSignatures.at(uRes).count(e.text)) {
                                    sigPtr = &moduleSignatures.at(uRes).at(e.text);
                                    resolvedKey = mangleFuncIfNeeded(uRes, e.text);
                                    break;
                                }
                            }
                        }
                    }
                    if (!sigPtr) {
                        auto signature = signatures.find(e.text);
                        if (signature == signatures.end())
                            throw Error("line " + std::to_string(e.line) + ": unresolved block call '" + e.text + "'");
                        if (ambiguousFunctions.count(e.text))
                            throw Error("line " + std::to_string(e.line) + ": ambiguous block call '" + e.text + "'; qualify with ModuleName." + e.text);
                        sigPtr = &signature->second;
                        auto fIt = funcModule.find(e.text);
                        if (fIt != funcModule.end() && !fIt->second.first.empty()) {
                            std::string defMod = fIt->second.first;
                            if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(e.text)) {
                                defMod = callerMod;
                            }
                            resolvedKey = mangleFuncIfNeeded(defMod, e.text);
                        }
                    }
                }
                if (sigPtr->isExtern) resolvedKey = e.text;
                else if (!sigPtr->parameters.empty()) {
                    const Type& p0 = sigPtr->parameters[0];
                    std::string candR = (p0.kind == TypeKind::Record) ? p0.name :
                        (((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) && p0.pointeeType && p0.pointeeType->kind == TypeKind::Record) ? p0.pointeeType->name : "");
                    if (!candR.empty() && taggedMethodSignatures.count(candR) && taggedMethodSignatures.at(candR).count(e.text)) {
                        resolvedKey = taggedMethodSignatures.at(candR).at(e.text).second;
                    }
                }
                resolvedCallKeys[&e] = resolvedKey;
                if (!currentAnalyzedFunctionKey.empty()) {
                    callGraphEdges[currentAnalyzedFunctionKey].insert(resolvedKey);
                }
                if (!sigPtr->result)
                    throw Error("line " + std::to_string(e.line) + ": procedure '" + e.text + "' cannot be used as a value");
                if (e.arguments.size() != sigPtr->parameters.size())
                    throw Error("line " + std::to_string(e.line) + ": block '" + e.text + "' expects " +
                                std::to_string(sigPtr->parameters.size()) + " arguments, got " +
                                std::to_string(e.arguments.size()));
                for (std::size_t i = 0; i < e.arguments.size(); ++i)
                    infer(*e.arguments[i], env, sigPtr->parameters[i]);
                if (sigPtr->isAsync) {
                    result = Type::futureType(*sigPtr->result);
                } else {
                    result = *sigPtr->result;
                }
                break;
            }
            case Expr::Kind::Unary: {
                if (e.text == "-") {
                    if (e.left->kind == Expr::Kind::Integer && expected && expected->kind == TypeKind::Signed) {
                        const auto limit = expected->bits == 64 ? (std::uint64_t(1) << 63)
                                                                : (std::uint64_t(1) << (expected->bits - 1));
                        if (e.left->magnitude > limit)
                            throw Error("line " + std::to_string(e.line) + ": negated integer literal does not fit " +
                                        typeName(*expected));
                        expressionTypes[e.left.get()] = *expected;
                        result = *expected;
                    } else {
                        Type a = infer(*e.left, env, expected);
                        if (a.kind == TypeKind::Float) {
                            result = a;
                        } else if (a.integer() && a.kind == TypeKind::Signed) {
                            result = a;
                        } else {
                            throw Error("line " + std::to_string(e.line) + ": unary '-' requires a signed integer or float");
                        }
                    }
                } else if (e.text == "~") {
                    Type a = infer(*e.left, env, expected);
                    if (!a.integer())
                        throw Error("line " + std::to_string(e.line) + ": bitwise not (~) requires a fixed-width integer");
                    result = a;
                } else {
                    Type a = infer(*e.left, env, Type::boolean());
                    if (a.kind != TypeKind::Boolean)
                        throw Error("line " + std::to_string(e.line) + ": Not requires Boolean");
                    result = Type::boolean();
                }
                break;
            }
            case Expr::Kind::Binary: {
                const std::string op = lower(e.text);
                Type a, b;
                if (!overloadedBinaryOps.empty()) {
                    Type lhsType = infer(*e.left, env);
                    Type rhsType = infer(*e.right, env);
                    auto opIt = overloadedBinaryOps.find({e.text, typeName(lhsType), typeName(rhsType)});
                    if (opIt == overloadedBinaryOps.end()) {
                        opIt = overloadedBinaryOps.find({op, typeName(lhsType), typeName(rhsType)});
                    }
                    if (opIt != overloadedBinaryOps.end()) {
                        auto& mutableExpr = const_cast<Expr&>(e);
                        mutableExpr.kind = Expr::Kind::Call;
                        mutableExpr.text = opIt->second;
                        mutableExpr.arguments.clear();
                        mutableExpr.arguments.push_back(std::move(mutableExpr.left));
                        mutableExpr.arguments.push_back(std::move(mutableExpr.right));
                        return infer(mutableExpr, env, expected);
                    }
                }
                if (op == "and" || op == "or") {
                    a = infer(*e.left, env, Type::boolean());
                    b = infer(*e.right, env, Type::boolean());
                    if (a.kind != TypeKind::Boolean || b.kind != TypeKind::Boolean)
                        throw Error("line " + std::to_string(e.line) + ": Boolean operator requires Boolean operands");
                    result = Type::boolean();
                    break;
                }
                if (op == "lsl" || op == "lsr") {
                    a = infer(*e.left, env, expected && expected->integer() ? expected : std::nullopt);
                    b = infer(*e.right, env);
                    if (!a.integer() || !b.integer())
                        throw Error("line " + std::to_string(e.line) + ": shift operator '" + e.text + "' requires integer operands");
                    result = a;
                    expressionTypes[&e] = result;
                    if (expected && result != *expected)
                        throw Error("line " + std::to_string(e.line) + ": expected " + typeName(*expected) + ", found " + typeName(result));
                    return result;
                }
                if (op == "pow" || op == "Pow") {
                    Type a_pow = infer(*e.left, env);
                    Type b_pow = infer(*e.right, env);
                    if (a_pow.kind == TypeKind::IntegerLiteral && b_pow.numeric()) a_pow = b_pow;
                    if (b_pow.kind == TypeKind::IntegerLiteral && a_pow.numeric()) b_pow = a_pow;
                    if (a_pow.kind == TypeKind::Float || b_pow.kind == TypeKind::Float) {
                        result = a_pow.kind == TypeKind::Float ? a_pow : b_pow;
                    } else if (a_pow.integer() && b_pow.integer()) {
                        result = a_pow;
                    } else {
                        throw Error("line " + std::to_string(e.line) + ": pow operator requires numeric types, found " + typeName(a_pow) + " and " + typeName(b_pow));
                    }
                    expressionTypes[&e] = result;
                    return result;
                }
                if (op == "+") {
                    Type lhsText = infer(*e.left, env);
                    if (lhsText.kind == TypeKind::Text) {
                        Type rhsVal = infer(*e.right, env);
                        result = lhsText;
                        expressionTypes[&e] = result;
                        return result;
                    }
                }
                if ((op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" || op == "%" ||
                     op == "&" || op == "|" || op == "^") && expected && expected->numeric()) {
                    a = infer(*e.left, env, *expected);
                    b = infer(*e.right, env, *expected);
                    if ((op == "mod" || op == "%" || op == "&" || op == "|" || op == "^") && !expected->integer())
                        throw Error("line " + std::to_string(e.line) + ": operator '" + e.text + "' requires integer operands");
                    result = *expected;
                    expressionTypes[&e] = result;
                    return result;
                }
                if (e.left->kind == Expr::Kind::Integer && e.right->kind != Expr::Kind::Integer) {
                    b = infer(*e.right, env);
                    a = infer(*e.left, env, b);
                } else {
                    a = infer(*e.left, env);
                    b = infer(*e.right, env, a);
                }
                if (a != b && !((op == "==" || op == "!=") &&
                                ((a.kind == TypeKind::NullType && b.kind == TypeKind::Pointer) ||
                                 (b.kind == TypeKind::NullType && a.kind == TypeKind::Pointer))))
                    throw Error("line " + std::to_string(e.line) + ": operator '" + e.text +
                                "' requires compatible operands (got " + typeName(a) + " and " + typeName(b) + ")");
                if (op == "+" || op == "-" || op == "*" || op == "/") {
                    if (!a.numeric())
                        throw Error("line " + std::to_string(e.line) + ": arithmetic requires numeric operands");
                    result = a;
                } else if (op == "mod" || op == "%" || op == "&" || op == "|" || op == "^") {
                    if (!a.integer())
                        throw Error("line " + std::to_string(e.line) + ": operator '" + e.text + "' requires integer operands");
                    result = a;
                } else if (op == "==" || op == "!=") {
                    if ((a.kind == TypeKind::NullType && b.kind == TypeKind::Pointer) ||
                        (b.kind == TypeKind::NullType && a.kind == TypeKind::Pointer) ||
                        (a.kind == TypeKind::Pointer && b.kind == TypeKind::Pointer)) {
                        result = Type::boolean();
                        break;
                    }
                    if (a.kind == TypeKind::Text && b.kind == TypeKind::Text) {
                        result = Type::boolean();
                        break;
                    }
                    if (a.kind == TypeKind::String && b.kind == TypeKind::String) {
                        result = Type::boolean();
                        break;
                    }
                    if (!a.numeric() && a.kind != TypeKind::Boolean && a.kind != TypeKind::Enum && a.kind != TypeKind::Pointer && a.kind != TypeKind::Character)
                        throw Error("line " + std::to_string(e.line) +
                                    ": equality requires compatible scalar operands");
                    result = Type::boolean();
                } else if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                    if (!a.numeric() && a.kind != TypeKind::Character)
                        throw Error("line " + std::to_string(e.line) +
                                    ": relational comparison requires numeric operands");
                    result = Type::boolean();
                } else
                    throw Error("line " + std::to_string(e.line) + ": unsupported operator '" + e.text + "'");
                break;
            }
            case Expr::Kind::Allocate: {
                if (baremetal || (targetProfile && !targetProfile->supportsHeap()))
                    throw Error("line " + std::to_string(e.line) + ": dynamic memory allocation ('Allocate') is forbidden on bare-metal target");

                if (e.arguments.size() != 1)
                    throw Error("line " + std::to_string(e.line) + ": Allocate expects 1 argument (element count), got " + std::to_string(e.arguments.size()));
                Type countType = infer(*e.arguments[0], env);
                if (!countType.integer())
                    throw Error("line " + std::to_string(e.line) + ": Allocate count must be an integer, found " + typeName(countType));
                if (!expected || expected->kind != TypeKind::Pointer)
                    throw Error("line " + std::to_string(e.line) + ": Allocate requires an expected pointer type context (e.g. 'Values : T Pointer = Allocate(N)')");
                result = *expected;
                break;
            }
            case Expr::Kind::Resize: {

                if (e.arguments.size() != 2)
                    throw Error("line " + std::to_string(e.line) + ": Resize expects 2 arguments (Pointer, new count), got " + std::to_string(e.arguments.size()));
                Type ptrType = infer(*e.arguments[0], env);
                if (ptrType.kind != TypeKind::Pointer)
                    throw Error("line " + std::to_string(e.line) + ": Resize requires a Pointer as first argument, found " + typeName(ptrType));
                Type countType = infer(*e.arguments[1], env);
                if (!countType.integer())
                    throw Error("line " + std::to_string(e.line) + ": Resize count must be an integer, found " + typeName(countType));
                result = ptrType;
                break;
            }
            case Expr::Kind::Release: {

                if (e.arguments.size() != 1)
                    throw Error("line " + std::to_string(e.line) + ": Release expects 1 argument (Pointer), got " + std::to_string(e.arguments.size()));
                Type ptrType = infer(*e.arguments[0], env);
                if (ptrType.kind != TypeKind::Pointer && ptrType.kind != TypeKind::NullType)
                    throw Error("line " + std::to_string(e.line) + ": Release requires a Pointer, found " + typeName(ptrType));
                result = Type::nullType();
                break;
            }
            case Expr::Kind::Spawn: {

                if (e.arguments.empty() || e.arguments[0]->kind != Expr::Kind::Call)
                    throw Error("line " + std::to_string(e.line) + ": Spawn requires a procedure call, e.g. 'Spawn Calculate(10)'");
                const auto& call = *e.arguments[0];
                const Signature* sigPtr = nullptr;
                std::string resolvedKey = call.text;
                std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
                if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(call.text)) {
                    sigPtr = &moduleSignatures.at(callerMod).at(call.text);
                    if (!sigPtr->isExtern && (baremetal || callerMod.find('.') != std::string::npos || ambiguousFunctions.count(call.text))) {
                        resolvedKey = mangleFuncIfNeeded(callerMod, call.text);
                    }
                } else {
                    auto sigIt = signatures.find(call.text);
                    if (sigIt != signatures.end()) sigPtr = &sigIt->second;
                }
                if (!sigPtr)
                    throw Error("line " + std::to_string(e.line) + ": unknown procedure '" + call.text + "' in Spawn");
                resolvedCallKeys[&call] = resolvedKey;
                const Signature& sig = *sigPtr;
                if (call.arguments.size() != sig.parameters.size())
                    throw Error("line " + std::to_string(e.line) + ": " + call.text + " expects " +
                                std::to_string(sig.parameters.size()) + " arguments, got " + std::to_string(call.arguments.size()));
                for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                    if (call.arguments[i]->kind == Expr::Kind::Address) {
                        throw Error("line " + std::to_string(e.line) + ": cannot pass local stack address to Spawn task (thread safety violation)");
                    }
                    infer(*call.arguments[i], env, sig.parameters[i]);
                }
                if (e.arguments.size() > 1) {
                    infer(*e.arguments[1], env);
                }
                Type retType = sig.result ? *sig.result : Type::nullType();
                expressionTypes[&call] = retType;
                result = Type::taskHandle(retType);
                break;
            }
            case Expr::Kind::Join: {

                if (e.arguments.empty())
                    throw Error("line " + std::to_string(e.line) + ": Join requires a Task_Handle argument");
                Type taskType = infer(*e.arguments[0], env);
                if (taskType.kind != TypeKind::TaskHandle)
                    throw Error("line " + std::to_string(e.line) + ": Join requires a Task_Handle, found " + typeName(taskType));
                if (e.arguments.size() == 2) {
                    Type timeoutType = infer(*e.arguments[1], env);
                    if (!timeoutType.integer())
                        throw Error("line " + std::to_string(e.line) + ": Join timeout must be an integer, found " + typeName(timeoutType));
                }
                result = taskType.pointeeType ? *taskType.pointeeType : Type::nullType();
                break;
            }
        }
        if (expected && result != *expected) {
            if (result.kind == TypeKind::NullType && expected->kind == TypeKind::Pointer) {
                result = *expected;
            } else if (result.kind == TypeKind::NullType && expected->kind == TypeKind::TaskHandle && expected->pointeeType && expected->pointeeType->kind == TypeKind::NullType) {
                result = *expected;
            } else if (result.kind == TypeKind::Pointer && expected->kind == TypeKind::Pointer &&
                       result.pointeeType && expected->pointeeType &&
                       result.pointeeType->kind == TypeKind::Record && expected->pointeeType->kind == TypeKind::Record &&
                       isRecordSubtype(result.pointeeType->name, expected->pointeeType->name)) {
                result = *expected;
            } else if (expected->kind == TypeKind::Record && result.integer()) {
                auto rfIt = recordFieldTypes.find(expected->name);
                if (rfIt != recordFieldTypes.end() && rfIt->second.size() == 1 && rfIt->second.begin()->second.integer()) {
                    result = *expected;
                } else {
                    throw Error("line " + std::to_string(e.line) + ": expected " + typeName(*expected) + ", found " +
                                typeName(result));
                }
            } else {
                throw Error("line " + std::to_string(e.line) + ": expected " + typeName(*expected) + ", found " +
                            typeName(result));
            }
        }
        expressionTypes[&e] = result;
        return result;
    }
    static bool literalTrue(const Expr& e) {
        return e.kind == Expr::Kind::Boolean && e.boolean;
    }

    std::optional<uint32_t> resolveConstInt(const Expr& expr) const {
        if (expr.kind == Expr::Kind::Integer) return static_cast<uint32_t>(expr.magnitude);
        if (expr.kind == Expr::Kind::Variable) {
            if (currentFunctionConstantValues.count(expr.text)) return currentFunctionConstantValues.at(expr.text);
            if (globalConstantValues.count(expr.text)) return globalConstantValues.at(expr.text);
            if (enumValues.count(expr.text)) return static_cast<uint32_t>(enumValues.at(expr.text).magnitude);
            auto dotPos = expr.text.rfind('.');
            std::string varName = (dotPos != std::string::npos) ? expr.text.substr(dotPos + 1) : expr.text;
            if (currentFunctionConstantValues.count(varName)) return currentFunctionConstantValues.at(varName);
            if (globalConstantValues.count(varName)) return globalConstantValues.at(varName);
            if (varName.rfind("Pin_", 0) == 0) {
                try { return static_cast<uint32_t>(std::stoul(varName.substr(4))); } catch (...) {}
            }
        }
        return std::nullopt;
    }

    void checkPinConflicts(const Expr& callExpr, int line) {
        std::string mod = callExpr.qualifier;
        std::string fn = callExpr.text;

        if (fn == "Init_Output" || fn == "Init_Input" || fn == "Init_Output_With_State") {
            if (!callExpr.arguments.empty()) {
                if (auto p = resolveConstInt(*callExpr.arguments[0])) {
                    auto it = reservedPins.find(*p);
                    if (it != reservedPins.end() && it->second != "GPIO") {
                        throw Error("line " + std::to_string(line) + ": hardware pin conflict: GPIO Pin " + std::to_string(*p) +
                                    " is already reserved for " + it->second);
                    }
                    reservedPins[*p] = "GPIO";
                }
            }
        } else if (fn == "Deinit" && !callExpr.arguments.empty()) {
            if (auto p = resolveConstInt(*callExpr.arguments[0])) {
                reservedPins.erase(*p);
            }
        } else if (fn == "Init_Pin" && !callExpr.arguments.empty()) {
            if (auto p = resolveConstInt(*callExpr.arguments[0])) {
                auto it = reservedPins.find(*p);
                if (it != reservedPins.end() && it->second != "ADC") {
                    throw Error("line " + std::to_string(line) + ": hardware pin conflict: ADC Pin " + std::to_string(*p) +
                                " is already reserved for " + it->second);
                }
                reservedPins[*p] = "ADC";
            }
        } else if (fn == "Init" && callExpr.arguments.size() >= 3) {
            std::string peripheral = "PERIPHERAL";
            std::string ql = lower(mod);
            if (ql == "i2c" || (!ql.empty() && ql.rfind("i2c", 0) != std::string::npos)) peripheral = "I2C";
            else if (ql == "spi" || (!ql.empty() && ql.rfind("spi", 0) != std::string::npos)) peripheral = "SPI";
            else if (ql == "uart" || (!ql.empty() && ql.rfind("uart", 0) != std::string::npos)) peripheral = "UART";
            else if (callExpr.arguments.size() == 4) peripheral = "I2C";
            else if (callExpr.arguments.size() == 6) peripheral = "SPI";

            for (std::size_t i = 1; i < callExpr.arguments.size() - 1; ++i) {
                if (auto p = resolveConstInt(*callExpr.arguments[i])) {
                    auto it = reservedPins.find(*p);
                    if (it != reservedPins.end() && it->second != peripheral) {
                        throw Error("line " + std::to_string(line) + ": hardware pin conflict: Pin " + std::to_string(*p) +
                                    " is already reserved for " + it->second + ", cannot configure for " + peripheral);
                    }
                    reservedPins[*p] = peripheral;
                }
            }
        }
    }
    void checkCallStatement(Expr& callExpr, int line, const std::unordered_map<std::string, Type>& env) {
        if (callExpr.kind == Expr::Kind::OnError || callExpr.kind == Expr::Kind::Spawn || callExpr.kind == Expr::Kind::Join) {
            infer(callExpr, env);
            return;
        }
        if (!callExpr.qualifier.empty()) {
            std::string resolvedQual = resolveModuleAlias(callExpr.qualifier, currentSourceIndex);
            std::string qLower = lower(resolvedQual);
            if (isKnownModule(qLower)) {
                checkAccess("module", resolvedQual, resolvedQual, line, currentSourceIndex);
                auto mIt = moduleSignatures.find(qLower);
                if (mIt == moduleSignatures.end() || !mIt->second.count(callExpr.text)) {
                    throw Error("line " + std::to_string(line) + ": unresolved block call '" +
                                callExpr.qualifier + "." + callExpr.text + "'");
                }
                const Signature& sig = mIt->second.at(callExpr.text);
                std::string mKey = mangleFuncIfNeeded(qLower, callExpr.text);
                if (!callExpr.arguments.empty()) {
                    Type a0 = infer(*callExpr.arguments[0], env);
                    std::string candR = (a0.kind == TypeKind::Record) ? a0.name :
                        (((a0.kind == TypeKind::Pointer || a0.kind == TypeKind::SmartPointer) && a0.pointeeType && a0.pointeeType->kind == TypeKind::Record) ? a0.pointeeType->name : "");
                    if (!candR.empty() && taggedMethodSignatures.count(candR) && taggedMethodSignatures.at(candR).count(callExpr.text)) {
                        mKey = taggedMethodSignatures.at(candR).at(callExpr.text).second;
                    }
                }
                resolvedCallKeys[&callExpr] = mKey;
                if (!currentAnalyzedFunctionKey.empty()) callGraphEdges[currentAnalyzedFunctionKey].insert(mKey);
                if (sig.errorResult.has_value()) {
                    std::cerr << "warning: line " << line << ": error-returning block '" <<
                                callExpr.qualifier + "." + callExpr.text << "' call result is unhandled; consider using '.On_Error(Panic)' or 'try'\n";
                }
                if (callExpr.arguments.size() != sig.parameters.size())
                    throw Error("line " + std::to_string(line) + ": block '" + callExpr.qualifier + "." + callExpr.text + "' expects " +
                                std::to_string(sig.parameters.size()) + " arguments, got " + std::to_string(callExpr.arguments.size()));
                for (std::size_t i = 0; i < callExpr.arguments.size(); ++i)
                    infer(*callExpr.arguments[i], env, sig.parameters[i]);
                callExpr.qualifier = resolvedQual;
                callExpr.left.reset();
                checkPinConflicts(callExpr, line);
                return;
            }
        }
        if (callExpr.left || (!callExpr.qualifier.empty() && env.count(callExpr.qualifier))) {
            if (callExpr.left) {
                Type objType = infer(*callExpr.left, env);
                std::string recName;
                bool isPtr = false;
                if (objType.kind == TypeKind::Record) recName = objType.name;
                else if ((objType.kind == TypeKind::Pointer || objType.kind == TypeKind::SmartPointer) &&
                         objType.pointeeType && objType.pointeeType->kind == TypeKind::Record) {
                    recName = objType.pointeeType->name;
                    isPtr = true;
                }
                if (!recName.empty()) {
                    const Signature* mSig = nullptr;
                    std::string mKey;
                    auto matchSig = [&](const Signature& cand) {
                        if (cand.parameters.empty()) return false;
                        const Type& p0 = cand.parameters[0];
                        if (p0.kind == TypeKind::Record && isRecordSubtype(recName, p0.name)) return true;
                        if ((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) &&
                            p0.pointeeType && p0.pointeeType->kind == TypeKind::Record &&
                            isRecordSubtype(recName, p0.pointeeType->name)) return true;
                        return false;
                    };
                    std::string curR = recName;
                    auto dot = curR.rfind('.');
                    std::string shortR = (dot != std::string::npos) ? curR.substr(dot + 1) : curR;
                    while (!curR.empty() && !mSig) {
                        auto tmIt = taggedMethodSignatures.find(curR);
                        if (tmIt == taggedMethodSignatures.end() && !shortR.empty()) {
                            tmIt = taggedMethodSignatures.find(shortR);
                        }
                        if (tmIt != taggedMethodSignatures.end() && tmIt->second.count(callExpr.text)) {
                            const auto& entry = tmIt->second.at(callExpr.text);
                            if (matchSig(entry.first)) { mSig = &entry.first; mKey = entry.second; break; }
                        }
                        auto rmIt = recordModule.find(curR);
                        if (rmIt == recordModule.end() && !shortR.empty()) {
                            rmIt = recordModule.find(shortR);
                        }
                        if (rmIt != recordModule.end() && !rmIt->second.first.empty()) {
                            std::string rModLower = lower(rmIt->second.first);
                            if (moduleSignatures.count(rModLower) && moduleSignatures.at(rModLower).count(callExpr.text)) {
                                const Signature& cand = moduleSignatures.at(rModLower).at(callExpr.text);
                                if (matchSig(cand)) { mSig = &cand; mKey = mangleFuncIfNeeded(rModLower, callExpr.text); break; }
                            }
                        }
                        auto pIt = recordParent.find(curR);
                        if (pIt == recordParent.end() && !shortR.empty()) {
                            pIt = recordParent.find(shortR);
                        }
                        curR = (pIt != recordParent.end()) ? pIt->second : "";
                        dot = curR.rfind('.');
                        shortR = (dot != std::string::npos) ? curR.substr(dot + 1) : curR;
                    }
                    if (!mSig) {
                        std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
                        if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(callExpr.text)) {
                            const Signature& cand = moduleSignatures.at(callerMod).at(callExpr.text);
                            if (matchSig(cand)) { mSig = &cand; mKey = mangleFuncIfNeeded(callerMod, callExpr.text); }
                        }
                    }
                    if (!mSig) {
                        auto sIt = signatures.find(callExpr.text);
                        if (sIt != signatures.end() && matchSig(sIt->second)) { mSig = &sIt->second; mKey = callExpr.text; }
                    }
                    if (!mSig)
                        throw Error("line " + std::to_string(line) + ": unresolved method '" + callExpr.text + "' on record '" + recName + "'");
                    if (mSig->errorResult.has_value()) {
                        std::cerr << "warning: line " << line << ": error-returning method '" << callExpr.text << "' call result is unhandled; consider using '.On_Error(Panic)' or 'try'\n";
                    }
                    if (callExpr.arguments.size() + 1 != mSig->parameters.size())
                        throw Error("line " + std::to_string(line) + ": method '" + callExpr.text + "' expects " +
                                    std::to_string(mSig->parameters.size() - 1) + " arguments, got " +
                                    std::to_string(callExpr.arguments.size()));
                    std::unique_ptr<Expr> selfArg;
                    if (!isPtr && mSig->parameters[0].kind == TypeKind::Pointer) {
                        selfArg = std::make_unique<Expr>();
                        selfArg->kind = Expr::Kind::Address;
                        selfArg->text = "__self__";
                        selfArg->line = line;
                        selfArg->left = std::move(callExpr.left);
                    } else {
                        selfArg = std::move(callExpr.left);
                    }
                    callExpr.arguments.insert(callExpr.arguments.begin(), std::move(selfArg));
                    callExpr.qualifier.clear();
                    for (std::size_t idx = 0; idx < callExpr.arguments.size(); ++idx)
                        infer(*callExpr.arguments[idx], env, mSig->parameters[idx]);
                    resolvedCallKeys[&callExpr] = mKey;
                    if (isPtr && recordIsTagged[recName] && recordRoot.count(recName)) {
                        const std::string& root = recordRoot.at(recName);
                        const auto& slots = vtableSlotNames[root];
                        auto sPos = std::find(slots.begin(), slots.end(), callExpr.text);
                        if (sPos != slots.end()) {
                            virtualDispatchCalls[&callExpr] = {root, static_cast<std::size_t>(std::distance(slots.begin(), sPos))};
                        }
                    }
                    if (!currentAnalyzedFunctionKey.empty()) callGraphEdges[currentAnalyzedFunctionKey].insert(mKey);
                    return;
                }
            }
            infer(callExpr, env);
            return;
        }
        if (callExpr.kind == Expr::Kind::Release || callExpr.kind == Expr::Kind::Join) {
            infer(callExpr, env);
            return;
        }
        if (callExpr.text == "Append" || callExpr.text == "Pop" || callExpr.text == "Close_File" ||
            callExpr.text == "Remove" || callExpr.text == "Atomic_Store" ||
            callExpr.text == "Channel_Send" || callExpr.text == "Channel_Close" ||
            callExpr.text == "Tcp_Send" || callExpr.text == "Tcp_Close" ||
            callExpr.text == "Udp_Send_To" || callExpr.text == "Udp_Close" ||
            callExpr.text == "__loom_usb_putchar" || callExpr.text == "__loom_usb_write_str" ||
            callExpr.text == "__loom_irq_save" || callExpr.text == "__loom_irq_restore" ||
            callExpr.text == "__loom_task_cancel" || (env.count(callExpr.text) && env.at(callExpr.text).kind == TypeKind::BlockPointer)) {
            infer(callExpr, env);
            return;
        }
        if (callExpr.text == "Write_UART" && callExpr.arguments.size() == 1) {
            Type argType = infer(*callExpr.arguments[0], env);
            if (argType.kind == TypeKind::String && signatures.count("Write_UART_String")) {
                callExpr.text = "Write_UART_String";
            } else if (argType.kind == TypeKind::Character && signatures.count("Write_UART_Char")) {
                callExpr.text = "Write_UART_Char";
            } else if ((argType.kind == TypeKind::Signed || argType.kind == TypeKind::Unsigned) && signatures.count("Write_UART_Int")) {
                callExpr.text = "Write_UART_Int";
            }
        }
        if (isVolatileWrite(callExpr.text)) {
            if (unsafeDepth == 0)
                throw Error("line " + std::to_string(line) + ": volatile MMIO write '" + callExpr.text + "' requires an Unsafe block");
            if (callExpr.arguments.size() != 2)
                throw Error("line " + std::to_string(line) + ": " + callExpr.text + " expects 2 arguments (Address, Value), got " + std::to_string(callExpr.arguments.size()));
            infer(*callExpr.arguments[0], env);
            infer(*callExpr.arguments[1], env);
            return;
        }
        if (isCpuIntrinsic(callExpr.text)) {
            if (!callExpr.arguments.empty())
                throw Error("line " + std::to_string(line) + ": " + callExpr.text + " takes no arguments");
            return;
        }
        if (callExpr.text == "Asm") {
            if (unsafeDepth == 0)
                throw Error("line " + std::to_string(line) + ": Inline assembly 'Asm' requires an Unsafe block");
            if (callExpr.arguments.empty() || callExpr.arguments[0]->kind != Expr::Kind::String)
                throw Error("line " + std::to_string(line) + ": Asm requires a string literal instruction template");
            return;
        }
        if (isAIMLIntrinsic(callExpr.text)) {
            for (const auto& arg : callExpr.arguments) {
                if (arg->kind == Expr::Kind::Variable && env.find(arg->text) == env.end()) {
                    if (signatures.find(arg->text) != signatures.end()) {
                        expressionTypes[arg.get()] = Type::pointer(Type::unsignedInt(8));
                        continue;
                    }
                }
                std::optional<Type> expType = std::nullopt;
                if (arg->kind == Expr::Kind::Floating) {
                    expType = Type::floating(32);
                }
                infer(*arg, env, expType);
            }
            if (callExpr.text == "MatMul" || callExpr.text == "MatMul_TransposeB" || callExpr.text == "MatMul_TransposeA") {
                if (callExpr.arguments.size() != 6)
                    throw Error("line " + std::to_string(line) + ": " + callExpr.text + " expects 6 arguments (A, B, C, M, N, K), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "MatMul_Add") {
                if (callExpr.arguments.size() != 7)
                    throw Error("line " + std::to_string(line) + ": MatMul_Add expects 7 arguments (A, B, Bias, C, M, N, K), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "Vector_Dot") {
                if (callExpr.arguments.size() != 3)
                    throw Error("line " + std::to_string(line) + ": Vector_Dot expects 3 arguments (A, B, N), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "ReLU" || callExpr.text == "GELU") {
                if (callExpr.arguments.size() != 2)
                    throw Error("line " + std::to_string(line) + ": " + callExpr.text + " expects 2 arguments (X, N), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "Softmax") {
                if (callExpr.arguments.size() != 2 && callExpr.arguments.size() != 3)
                    throw Error("line " + std::to_string(line) + ": Softmax expects 2 or 3 arguments (X, N) or (X, Rows, Cols), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "ReLU_Backward") {
                if (callExpr.arguments.size() != 4)
                    throw Error("line " + std::to_string(line) + ": ReLU_Backward expects 4 arguments (Grad_In, X, Grad_Out, N), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "LayerNorm") {
                if (callExpr.arguments.size() != 6)
                    throw Error("line " + std::to_string(line) + ": LayerNorm expects 6 arguments (X, Gamma, Beta, Out, N, D), got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "Vector_Add" || callExpr.text == "Vector_Sub" || callExpr.text == "Vector_Mul" || callExpr.text == "Vector_Scale") {
                if (callExpr.arguments.size() != 4)
                    throw Error("line " + std::to_string(line) + ": " + callExpr.text + " expects 4 arguments, got " + std::to_string(callExpr.arguments.size()));
            } else if (callExpr.text == "Parallel_For") {
                if (callExpr.arguments.size() != 2)
                    throw Error("line " + std::to_string(line) + ": Parallel_For expects 2 arguments (Count, WorkerBlock), got " + std::to_string(callExpr.arguments.size()));
            }
            return;
        }
        // Statement-position calls may intentionally discard a returned value.
        const Signature* sigPtr = nullptr;
        std::string resolvedKey = callExpr.text;
        std::string callerMod = fileModules.count(currentSourceIndex) ? lower(fileModules.at(currentSourceIndex)) : "";
        if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(callExpr.text)) {
            sigPtr = &moduleSignatures.at(callerMod).at(callExpr.text);
            resolvedKey = mangleFuncIfNeeded(callerMod, callExpr.text);
        } else {
            for (const auto& u : currentBlockUses) {
                std::string uRes = lower(resolveModuleAlias(u, currentSourceIndex));
                if (moduleSignatures.count(uRes) && moduleSignatures.at(uRes).count(callExpr.text)) {
                    sigPtr = &moduleSignatures.at(uRes).at(callExpr.text);
                    resolvedKey = mangleFuncIfNeeded(uRes, callExpr.text);
                    break;
                }
            }
            if (!sigPtr && fileUses.count(currentSourceIndex)) {
                for (const auto& u : fileUses.at(currentSourceIndex)) {
                    std::string uRes = lower(resolveModuleAlias(u, currentSourceIndex));
                    if (moduleSignatures.count(uRes) && moduleSignatures.at(uRes).count(callExpr.text)) {
                        sigPtr = &moduleSignatures.at(uRes).at(callExpr.text);
                        resolvedKey = mangleFuncIfNeeded(uRes, callExpr.text);
                        break;
                    }
                }
            }
        }
        if (!sigPtr) {
            bool definedLocally = false;
            if (!callerMod.empty() && moduleSignatures.count(callerMod) && moduleSignatures.at(callerMod).count(callExpr.text)) {
                definedLocally = true;
            }
            if (!definedLocally) {
                auto fIt = funcModule.find(callExpr.text);
                if (fIt != funcModule.end()) {
                    const std::string& defMod = fIt->second.first;
                    if (!defMod.empty() && lower(defMod) != callerMod) {
                        checkAccess("block", callExpr.text, defMod, line, currentSourceIndex);
                        bool allowed = false;
                        auto uIt = fileUses.find(currentSourceIndex);
                        if (uIt != fileUses.end() && uIt->second.count(lower(defMod))) allowed = true;
                        if (currentBlockUses.count(lower(defMod))) allowed = true;
                        if (!allowed) {
                            throw Error("line " + std::to_string(line) + ": cannot call block '" + callExpr.text +
                                        "' from module '" + defMod + "' unqualified; access it as '" +
                                        defMod + "." + callExpr.text + "(...)' or add 'use " + defMod + "'");
                        }
                    }
                    if (!defMod.empty()) resolvedKey = mangleFuncIfNeeded(defMod, callExpr.text);
                }
            }
            auto signature = signatures.find(callExpr.text);
            if (signature == signatures.end())
                throw Error("line " + std::to_string(line) + ": unresolved block call '" + callExpr.text + "'");
            if (ambiguousFunctions.count(callExpr.text))
                throw Error("line " + std::to_string(line) + ": ambiguous block call '" + callExpr.text + "'; qualify with ModuleName." + callExpr.text);
            sigPtr = &signature->second;
        }
        if (sigPtr->isExtern) resolvedKey = callExpr.text;
        else if (!sigPtr->parameters.empty()) {
            const Type& p0 = sigPtr->parameters[0];
            std::string candR = (p0.kind == TypeKind::Record) ? p0.name :
                (((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) && p0.pointeeType && p0.pointeeType->kind == TypeKind::Record) ? p0.pointeeType->name : "");
            if (!candR.empty() && taggedMethodSignatures.count(candR) && taggedMethodSignatures.at(candR).count(callExpr.text)) {
                resolvedKey = taggedMethodSignatures.at(candR).at(callExpr.text).second;
            }
        }
        resolvedCallKeys[&callExpr] = resolvedKey;
        if (!currentAnalyzedFunctionKey.empty()) {
            callGraphEdges[currentAnalyzedFunctionKey].insert(resolvedKey);
        }
        if (sigPtr->errorResult.has_value()) {
            std::cerr << "warning: line " << line << ": error-returning block '" << callExpr.text << "' call result is unhandled; consider using '.On_Error(Panic)' or 'try'\n";
        }
        if (callExpr.arguments.size() != sigPtr->parameters.size())
            throw Error("line " + std::to_string(line) + ": block '" + callExpr.text + "' expects " +
                        std::to_string(sigPtr->parameters.size()) + " arguments, got " +
                        std::to_string(callExpr.arguments.size()));
        for (std::size_t i = 0; i < callExpr.arguments.size(); ++i)
            infer(*callExpr.arguments[i], env, sigPtr->parameters[i]);

        checkPinConflicts(callExpr, line);
    }
    Flow sequence(const std::vector<Stmt>& ss, int loopDepth, const std::optional<Type>& result,
                  const std::unordered_map<std::string, Type>& env) {
        Flow total;
        bool active = true;
        for (const auto& s : ss) {
            DiagnosticSpanGuard sSpanGuard({currentSourceIndex, s.line, s.column, std::max(1, s.length)});
            if (s.kind == Stmt::Kind::DerefAssign || s.kind == Stmt::Kind::TargetAssign) {
                if (s.target && s.target->kind == Expr::Kind::Index) {
                    Type container = infer(*s.target->left, env);
                    if (container.kind == TypeKind::String) {
                        throw Error("line " + std::to_string(s.line) + ": String characters are read-only; use an Array or Slice for mutable buffers");
                    }
                    if (container.kind == TypeKind::HMap) {
                        Type kType = infer(*s.target->right, env, *container.keyType);
                        if (kType != *container.keyType) {
                            throw Error("line " + std::to_string(s.line) + ": HMap key type mismatch: expected " +
                                        typeName(*container.keyType) + ", found " + typeName(kType));
                        }
                        infer(*s.expr, env, *container.valueType);
                        continue;
                    }
                    if (container.kind != TypeKind::Array && container.kind != TypeKind::Slice &&
                        container.kind != TypeKind::Pointer && container.kind != TypeKind::Vector &&
                        container.kind != TypeKind::Text) {
                        Type idxType = infer(*s.target->right, env);
                        Type valType = infer(*s.expr, env);
                        auto it = overloadedIndexSetOps.find({typeName(container), typeName(idxType), typeName(valType)});
                        if (it != overloadedIndexSetOps.end()) {
                            auto& mutableStmt = const_cast<Stmt&>(s);
                            mutableStmt.kind = Stmt::Kind::Call;
                            auto call = std::make_unique<Expr>();
                            call->kind = Expr::Kind::Call;
                            call->text = it->second;
                            call->line = s.line;
                            call->arguments.push_back(std::move(mutableStmt.target->left));
                            call->arguments.push_back(std::move(mutableStmt.target->right));
                            call->arguments.push_back(std::move(mutableStmt.expr));
                            mutableStmt.expr = std::move(call);
                            mutableStmt.target.reset();
                            continue;
                        }
                    }
                }
                Type targetType = infer(*s.target, env);
                infer(*s.expr, env, targetType);
            } else if (s.kind == Stmt::Kind::Ensure) {
                auto checkInner = [&](const Stmt& inner) {
                    if (inner.kind == Stmt::Kind::Call) {
                        checkCallStatement(const_cast<Expr&>(*inner.expr), inner.line, env);
                    } else if (inner.kind == Stmt::Kind::Write) {
                        if (!inner.writeArgs.empty()) {
                            for (const auto& arg : inner.writeArgs) infer(*arg, env);
                        } else {
                            infer(*inner.expr, env);
                        }
                    } else if (inner.kind == Stmt::Kind::Assign) {
                        auto i = env.find(inner.name);
                        if (i == env.end())
                            throw Error("line " + std::to_string(inner.line) + ": assignment to undeclared variable '" + inner.name + "'");
                        infer(*inner.expr, env, i->second);
                    } else if (inner.kind == Stmt::Kind::TargetAssign || inner.kind == Stmt::Kind::DerefAssign) {
                        if (inner.target && inner.target->kind == Expr::Kind::Index) {
                            Type container = infer(*inner.target->left, env);
                            if (container.kind == TypeKind::String) {
                                throw Error("line " + std::to_string(inner.line) + ": String characters are read-only; use an Array or Slice for mutable buffers");
                            }
                        }
                        Type targetType = infer(*inner.target, env);
                        infer(*inner.expr, env, targetType);
                    }
                };
                for (const auto& inner : s.body) checkInner(inner);
                if (s.ensuredStmt) checkInner(*s.ensuredStmt);
            } else if (s.kind == Stmt::Kind::Assign) {
                // '_' is the explicit discard sink — evaluate the RHS for side-effects/type-checks
                // but do not bind the result to any variable (error-returning calls are allowed).
                if (s.name == "_") {
                    infer(*s.expr, env);
                    continue;
                }
                auto i = env.find(s.name);
                if (i == env.end())
                    throw Error("line " + std::to_string(s.line) + ": assignment to undeclared variable '" + s.name +
                                "'");
                if (s.member.empty() && ((!currentFunctionLocals.count(s.name) && globalConstants.count(s.name)) || currentFunctionConstants.count(s.name)))
                    throw Error("line " + std::to_string(s.line) + ": cannot assign to constant '" + s.name + "'");
                if (s.member.empty() && !currentFunctionLocals.count(s.name) && globalModule.count(s.name))
                    checkAccess("global", s.name, globalModule.at(s.name).first, s.line, currentSourceIndex);
                Type destination = i->second;
                if (!s.member.empty()) {
                    if (destination.kind != TypeKind::Record)
                        throw Error("line " + std::to_string(s.line) + ": field assignment requires a Record value");
                    auto record = recordFieldTypes.find(destination.name);
                    if (record == recordFieldTypes.end() || !record->second.count(s.member))
                        throw Error("line " + std::to_string(s.line) + ": record '" + destination.name + "' has no field '" + s.member + "'");
                    if (globalConstants.count(s.name) || currentFunctionConstants.count(s.name))
                        throw Error("line " + std::to_string(s.line) + ": cannot modify a field of constant '" + s.name + "'");
                    destination = record->second.at(s.member);
                    fieldAssignmentTypes[&s] = destination;
                    fieldAssignmentRecords[&s] = i->second;
                }
                infer(*s.expr, env, destination);
            } else if (s.kind == Stmt::Kind::Call) {
                checkCallStatement(const_cast<Expr&>(*s.expr), s.line, env);
            } else if (s.kind == Stmt::Kind::Give) {
                if (!result && !currentFunctionErrorResult)
                    throw Error("line " + std::to_string(s.line) + ": Give is only valid in a value-producing block");
                if (currentFunctionErrorResult && result) {
                    Type successType = *result;
                    successType.errorType = nullptr;
                    Type fullRetType = *result;
                    fullRetType.errorType = std::make_shared<Type>(*currentFunctionErrorResult);
                    try {
                        infer(*s.expr, env, successType);
                    } catch (...) {
                        try {
                            infer(*s.expr, env, *currentFunctionErrorResult);
                        } catch (...) {
                            infer(*s.expr, env, fullRetType);
                        }
                    }
                } else if (result) {
                    infer(*s.expr, env, *result);
                } else {
                    infer(*s.expr, env, *currentFunctionErrorResult);
                }
            } else if (s.kind == Stmt::Kind::Write) {
                if (s.name == "Write") {
                    Type firstType = Type::nullType();
                    if (!s.writeArgs.empty()) {
                        firstType = infer(*s.writeArgs[0], env);
                    } else if (s.expr) {
                        firstType = infer(*s.expr, env);
                    }
                    if (firstType.kind != TypeKind::FileHandle && !signatures.count("Write")) {
                        throw Error("line " + std::to_string(s.line) + ": Write(...) is reserved exclusively for file streams (e.g. Write(File, ...)). Use Print(...) for standard output.");
                    }
                }
                if (!s.writeArgs.empty()) {
                    for (const auto& arg : s.writeArgs) {
                        Type value = infer(*arg, env, arg->kind == Expr::Kind::Floating ?
                                           std::optional<Type>(Type::floating(64)) : std::nullopt);
                        if (value.kind == TypeKind::Record || value.kind == TypeKind::Array || value.kind == TypeKind::Slice ||
                            value.kind == TypeKind::Text || value.kind == TypeKind::Vector || value.kind == TypeKind::FileHandle ||
                            value.kind == TypeKind::TaskHandle || value.kind == TypeKind::BlockPointer) {
                            // Supported
                        } else if (value.kind != TypeKind::String && value.kind != TypeKind::Boolean &&
                                   value.kind != TypeKind::Signed && value.kind != TypeKind::Unsigned &&
                                   value.kind != TypeKind::Float && value.kind != TypeKind::Character &&
                                   value.kind != TypeKind::Enum && value.kind != TypeKind::Pointer) {
                            throw Error("line " + std::to_string(s.line) + ": Write does not support values of type " + typeName(value));
                        }
                    }
                } else {
                    Type value = infer(*s.expr, env, s.expr->kind == Expr::Kind::Floating ?
                                       std::optional<Type>(Type::floating(64)) : std::nullopt);
                    if (value.kind == TypeKind::Record || value.kind == TypeKind::Array || value.kind == TypeKind::Slice ||
                        value.kind == TypeKind::Text || value.kind == TypeKind::Vector || value.kind == TypeKind::FileHandle ||
                        value.kind == TypeKind::TaskHandle || value.kind == TypeKind::BlockPointer) {
                        // Supported
                    } else if (value.kind != TypeKind::String && value.kind != TypeKind::Boolean &&
                               value.kind != TypeKind::Signed && value.kind != TypeKind::Unsigned &&
                               value.kind != TypeKind::Float && value.kind != TypeKind::Character &&
                               value.kind != TypeKind::Enum && value.kind != TypeKind::Pointer) {
                        throw Error("line " + std::to_string(s.line) + ": Write does not support values of type " + typeName(value));
                    }
                }
            } else if (s.kind == Stmt::Kind::Use) {
                checkAccess("module", s.name, s.name, s.line, currentSourceIndex);
                currentBlockUses.insert(lower(s.name));
            } else if (s.kind == Stmt::Kind::Exit) {
                if (s.expr) {
                    Type t = infer(*s.expr, env);
                    if (t.kind != TypeKind::Signed && t.kind != TypeKind::Unsigned) {
                        throw Error("line " + std::to_string(s.line) + ": Exit status code must be an integer");
                    }
                } else if (loopDepth == 0) {
                    if (result.has_value()) {
                        throw Error("line " + std::to_string(s.line) + ": Exit cannot be used in a value-producing block; use Give instead");
                    }
                }
            } else if (s.kind == Stmt::Kind::Skip) {
                if (loopDepth == 0)
                    throw Error("line " + std::to_string(s.line) + ": Skip is only valid inside a Loop");
            } else if (s.kind == Stmt::Kind::If) {
                Type c = infer(*s.expr, env, Type::boolean());
                if (c.kind != TypeKind::Boolean)
                    throw Error("line " + std::to_string(s.line) + ": If condition must be Boolean");
            } else if (s.kind == Stmt::Kind::Loop && (s.until || s.whileLoop)) {
                Type c = infer(*s.expr, env, Type::boolean());
                if (c.kind != TypeKind::Boolean)
                    throw Error("line " + std::to_string(s.line) + ": Loop " + (s.until ? "until" : "while") + " condition must be Boolean");
            } else if (s.kind == Stmt::Kind::ForIn) {
                if (s.isRange) {
                    Type startT = infer(*s.expr, env);
                    Type endT = infer(*s.rangeEnd, env);
                    if (!startT.integer() || !endT.integer())
                        throw Error("line " + std::to_string(s.line) + ": Range loop bounds must be integers");
                } else {
                    Type collType = infer(*s.expr, env);
                    if (collType.kind != TypeKind::Array && collType.kind != TypeKind::Slice &&
                        collType.kind != TypeKind::Vector && collType.kind != TypeKind::Text &&
                        collType.kind != TypeKind::HMap) {
                        throw Error("line " + std::to_string(s.line) + ": For-In loop requires a collection or range, found " + typeName(collType));
                    }
                }
            } else if (s.kind == Stmt::Kind::MultiAssign) {
                Type exprType = infer(*s.expr, env);
                if (exprType.kind != TypeKind::Tuple) {
                    throw Error("line " + std::to_string(s.line) + ": multi-value assignment requires a tuple expression, found " + typeName(exprType));
                }
                if (exprType.tupleTypes.size() != s.multiTargets.size()) {
                    throw Error("line " + std::to_string(s.line) + ": cannot unpack " + std::to_string(exprType.tupleTypes.size()) +
                                " tuple elements into " + std::to_string(s.multiTargets.size()) + " variables");
                }
                for (size_t i = 0; i < s.multiTargets.size(); ++i) {
                    auto it = env.find(s.multiTargets[i]);
                    if (it == env.end()) {
                        throw Error("line " + std::to_string(s.line) + ": assignment to undeclared variable '" + s.multiTargets[i] + "'");
                    }
                    if (globalConstants.count(s.multiTargets[i]) || currentFunctionConstants.count(s.multiTargets[i])) {
                        throw Error("line " + std::to_string(s.line) + ": cannot assign to constant '" + s.multiTargets[i] + "'");
                    }
                    if (it->second != exprType.tupleTypes[i]) {
                        throw Error("line " + std::to_string(s.line) + ": type mismatch for '" + s.multiTargets[i] +
                                    "': variable has type " + typeName(it->second) + ", unpacking element has type " + typeName(exprType.tupleTypes[i]));
                    }
                }
            } else if (s.kind == Stmt::Kind::Match) {
                Type matchType = infer(*s.expr, env);
                if (!matchType.integer() && matchType.kind != TypeKind::Enum && matchType.kind != TypeKind::Boolean &&
                    matchType.kind != TypeKind::Character && matchType.kind != TypeKind::Tuple && matchType.kind != TypeKind::Record &&
                    matchType.kind != TypeKind::String)
                    throw Error("line " + std::to_string(s.line) + ": Match expression must be an integer, enum, boolean, character, string, tuple, or record, found " + typeName(matchType));
                auto getCaseEnv = [&](const MatchCase& mc) {
                    auto caseEnv = env;
                    if (mc.pattern && mc.pattern->kind == Expr::Kind::Variable && !mc.isWildcard && !mc.isDefault) {
                        bool isConst = globalConstants.count(mc.pattern->text) || currentFunctionConstants.count(mc.pattern->text) || enumValues.count(mc.pattern->text);
                        if (!isConst) {
                            caseEnv[mc.pattern->text] = matchType;
                        }
                    } else if (mc.pattern && matchType.kind == TypeKind::Tuple && mc.pattern->kind == Expr::Kind::TupleLiteral) {
                        for (size_t i = 0; i < mc.pattern->arguments.size() && i < matchType.tupleTypes.size(); ++i) {
                            const auto& arg = *mc.pattern->arguments[i];
                            if (arg.kind == Expr::Kind::Variable && !globalConstants.count(arg.text) && !enumValues.count(arg.text)) {
                                caseEnv[arg.text] = matchType.tupleTypes[i];
                            }
                        }
                    } else if (mc.pattern && matchType.kind == TypeKind::Record && mc.pattern->kind == Expr::Kind::RecordLiteral) {
                        auto it = recordFieldTypes.find(matchType.name);
                        if (it != recordFieldTypes.end()) {
                            for (size_t i = 0; i < mc.pattern->fieldNames.size() && i < mc.pattern->arguments.size(); ++i) {
                                const std::string& fname = mc.pattern->fieldNames[i];
                                if (it->second.count(fname)) {
                                    Type fType = it->second.at(fname);
                                    const auto& arg = *mc.pattern->arguments[i];
                                    if (arg.kind == Expr::Kind::Variable && !globalConstants.count(arg.text) && !enumValues.count(arg.text)) {
                                        caseEnv[arg.text] = fType;
                                    }
                                }
                            }
                        }
                    }
                    return caseEnv;
                };
                std::unordered_set<std::string> seenPats;
                std::unordered_set<std::string> matchedEnumVariants;
                bool hasFallback = !s.elseBody.empty();
                for (const auto& mc : s.matchCases) {
                    if (mc.isWildcard || mc.isDefault) {
                        if (!mc.guard) hasFallback = true;
                        if (mc.guard) {
                            Type gType = infer(*mc.guard, env, Type::boolean());
                            if (gType.kind != TypeKind::Boolean)
                                throw Error("line " + std::to_string(mc.line) + ": When guard expression must be Boolean");
                        }
                        continue;
                    }
                    auto caseEnv = getCaseEnv(mc);
                    if (mc.pattern->kind == Expr::Kind::Variable) {
                        bool isConst = globalConstants.count(mc.pattern->text) || currentFunctionConstants.count(mc.pattern->text) || enumValues.count(mc.pattern->text);
                        if (isConst) {
                            infer(*mc.pattern, env, matchType);
                            if (!mc.guard && matchType.kind == TypeKind::Enum && enumValues.count(mc.pattern->text)) {
                                auto dotPos = mc.pattern->text.rfind('.');
                                matchedEnumVariants.insert(dotPos != std::string::npos ? mc.pattern->text.substr(dotPos + 1) : mc.pattern->text);
                            }
                        }
                    } else if (mc.isRange) {
                        if (!matchType.integer() && matchType.kind != TypeKind::Character)
                            throw Error("line " + std::to_string(mc.line) + ": range pattern '..' requires an integer or character Match expression, found " + typeName(matchType));
                        Type patType = infer(*mc.pattern, env, matchType);
                        Type endType = infer(*mc.rangeEnd, env, matchType);
                        if (!patType.integer() || !endType.integer())
                            throw Error("line " + std::to_string(mc.line) + ": range pattern bounds must be integers");
                    } else if (matchType.kind == TypeKind::Tuple) {
                        if (mc.pattern->kind == Expr::Kind::TupleLiteral) {
                            if (mc.pattern->arguments.size() != matchType.tupleTypes.size())
                                throw Error("line " + std::to_string(mc.line) + ": tuple pattern size mismatch");
                            for (size_t i = 0; i < mc.pattern->arguments.size(); ++i) {
                                const auto& arg = *mc.pattern->arguments[i];
                                if (arg.kind != Expr::Kind::Variable || globalConstants.count(arg.text) || enumValues.count(arg.text)) {
                                    infer(const_cast<Expr&>(arg), env, matchType.tupleTypes[i]);
                                }
                            }
                        } else {
                            Type patType = infer(*mc.pattern, env, matchType);
                            if (patType != matchType)
                                throw Error("line " + std::to_string(mc.line) + ": tuple pattern type does not match Match expression type " + typeName(matchType));
                        }
                    } else if (matchType.kind == TypeKind::Record) {
                        if (mc.pattern->kind == Expr::Kind::RecordLiteral) {
                            auto it = recordFieldTypes.find(matchType.name);
                            if (it == recordFieldTypes.end())
                                throw Error("line " + std::to_string(mc.line) + ": unknown record type '" + matchType.name + "'");
                            for (size_t i = 0; i < mc.pattern->fieldNames.size(); ++i) {
                                const std::string& fname = mc.pattern->fieldNames[i];
                                if (!it->second.count(fname))
                                    throw Error("line " + std::to_string(mc.line) + ": record '" + matchType.name + "' has no field '" + fname + "'");
                                Type fType = it->second.at(fname);
                                const auto& arg = *mc.pattern->arguments[i];
                                if (arg.kind != Expr::Kind::Variable || globalConstants.count(arg.text) || enumValues.count(arg.text)) {
                                    infer(const_cast<Expr&>(arg), env, fType);
                                }
                            }
                        } else {
                            Type patType = infer(*mc.pattern, env, matchType);
                            if (patType != matchType)
                                throw Error("line " + std::to_string(mc.line) + ": record pattern type does not match Match expression type " + typeName(matchType));
                        }
                    } else {
                        Type patType = infer(*mc.pattern, env, matchType);
                        if (patType != matchType && !(patType.integer() && matchType.integer()))
                            throw Error("line " + std::to_string(mc.line) + ": pattern type " + typeName(patType) + " does not match Match expression type " + typeName(matchType));
                        if (!mc.guard && matchType.kind == TypeKind::Enum &&
                            (mc.pattern->kind == Expr::Kind::EnumVariant || mc.pattern->kind == Expr::Kind::Member || (mc.pattern->kind == Expr::Kind::Variable && !mc.pattern->qualifier.empty()))) {
                            auto dotPos = mc.pattern->text.rfind('.');
                            matchedEnumVariants.insert(dotPos != std::string::npos ? mc.pattern->text.substr(dotPos + 1) : mc.pattern->text);
                        }
                        if (!mc.guard) {
                            std::string patKey = mc.pattern->text;
                            if (mc.pattern->kind == Expr::Kind::Integer) patKey = std::to_string(mc.pattern->magnitude);
                            if (!seenPats.insert(patKey).second)
                                throw Error("line " + std::to_string(mc.line) + ": duplicate pattern '" + patKey + "' in Match");
                        }
                    }
                    if (mc.guard) {
                        Type gType = infer(*mc.guard, caseEnv, Type::boolean());
                        if (gType.kind != TypeKind::Boolean)
                            throw Error("line " + std::to_string(mc.line) + ": When guard expression must be Boolean");
                    }
                }
                if (matchType.kind == TypeKind::Enum && !hasFallback) {
                    auto it = enumAllVariants.find(matchType.name);
                    if (it != enumAllVariants.end()) {
                        std::string missing;
                        for (const auto& v : it->second) {
                            if (!matchedEnumVariants.count(v)) {
                                if (!missing.empty()) missing += ", ";
                                missing += v;
                            }
                        }
                        if (!missing.empty()) {
                            throw Error("line " + std::to_string(s.line) + ": non-exhaustive Match on enum '" + matchType.name + "' (missing: " + missing + "; add missing cases or a Default: branch)");
                        }
                    }
                }
            }
            Flow one;
            switch (s.kind) {
                case Stmt::Kind::Assign:
                case Stmt::Kind::DerefAssign:
                case Stmt::Kind::TargetAssign:
                case Stmt::Kind::MultiAssign:
                case Stmt::Kind::Ensure:
                case Stmt::Kind::Call:
                case Stmt::Kind::Write:
                case Stmt::Kind::Use:
                    one = {true, false, false, false};
                    break;
                case Stmt::Kind::Give:
                    one = {false, false, false, true};
                    break;
                case Stmt::Kind::Exit:
                    if (s.expr || loopDepth == 0) {
                        one = {false, false, false, true};
                    } else {
                        one = {false, true, false, false};
                    }
                    break;
                case Stmt::Kind::Skip:
                    one = {false, false, true, false};
                    break;
                case Stmt::Kind::If: {
                    Flow branch = sequence(s.body, loopDepth, result, env);
                    Flow alternative = sequence(s.elseBody, loopDepth, result, env);
                    auto known = constantBoolean(*s.expr);
                    if (known)
                        one = *known ? branch : alternative;
                    else
                        one = {branch.falls || alternative.falls,
                               branch.breaks || alternative.breaks,
                               branch.skips || alternative.skips,
                               branch.returns || alternative.returns};
                    break;
                }
                case Stmt::Kind::Loop: {
                    Flow body = sequence(s.body, loopDepth + 1, result, env);
                    if (!s.until && !s.whileLoop) one = {body.breaks, false, false, body.returns};
                    else {
                        auto known = constantBoolean(*s.expr);
                        const bool conditionMayEnd = !known || (s.until ? *known : !*known);
                        one = {conditionMayEnd || body.breaks, false, false, body.returns};
                    }
                    break;
                }
                case Stmt::Kind::ForIn: {
                    std::unordered_map<std::string, Type> loopEnv = env;
                    if (s.isRange) {
                        Type startT = infer(*s.expr, env);
                        loopEnv[s.loopVar] = startT;
                        if (!s.loopIndexVar.empty()) loopEnv[s.loopIndexVar] = Type::unsignedInt(64);
                    } else {
                        Type collType = infer(*s.expr, env);
                        if (collType.kind == TypeKind::Array || collType.kind == TypeKind::Slice || collType.kind == TypeKind::Vector) {
                            loopEnv[s.loopVar] = *collType.pointeeType;
                            if (!s.loopIndexVar.empty()) loopEnv[s.loopIndexVar] = Type::unsignedInt(64);
                        } else if (collType.kind == TypeKind::Text) {
                            loopEnv[s.loopVar] = Type::character();
                            if (!s.loopIndexVar.empty()) loopEnv[s.loopIndexVar] = Type::unsignedInt(64);
                        } else if (collType.kind == TypeKind::HMap) {
                            if (!s.loopIndexVar.empty()) {
                                loopEnv[s.loopIndexVar] = *collType.keyType;
                                loopEnv[s.loopVar] = *collType.valueType;
                            } else {
                                loopEnv[s.loopVar] = *collType.keyType;
                            }
                        }
                    }
                    Flow body = sequence(s.body, loopDepth + 1, result, loopEnv);
                    one = {true, false, false, body.returns};
                    break;
                }
                case Stmt::Kind::Match: {
                    Flow combined;
                    bool hasWildcard = false;
                    for (const auto& mc : s.matchCases) {
                        if (mc.isWildcard || mc.isDefault) hasWildcard = true;
                    }
                    Type matchType = infer(*s.expr, env);
                    bool allBranchesReturn = !s.matchCases.empty() && (!s.elseBody.empty() || hasWildcard);
                    for (const auto& mc : s.matchCases) {
                        auto caseEnv = env;
                        if (mc.pattern && mc.pattern->kind == Expr::Kind::Variable && !mc.isWildcard && !mc.isDefault) {
                            bool isConst = globalConstants.count(mc.pattern->text) || currentFunctionConstants.count(mc.pattern->text) || enumValues.count(mc.pattern->text);
                            if (!isConst) {
                                caseEnv[mc.pattern->text] = matchType;
                            }
                        } else if (mc.pattern && matchType.kind == TypeKind::Tuple && mc.pattern->kind == Expr::Kind::TupleLiteral) {
                            for (size_t i = 0; i < mc.pattern->arguments.size() && i < matchType.tupleTypes.size(); ++i) {
                                const auto& arg = *mc.pattern->arguments[i];
                                if (arg.kind == Expr::Kind::Variable && !globalConstants.count(arg.text) && !enumValues.count(arg.text)) {
                                    caseEnv[arg.text] = matchType.tupleTypes[i];
                                }
                            }
                        } else if (mc.pattern && matchType.kind == TypeKind::Record && mc.pattern->kind == Expr::Kind::RecordLiteral) {
                            auto it = recordFieldTypes.find(matchType.name);
                            if (it != recordFieldTypes.end()) {
                                for (size_t i = 0; i < mc.pattern->fieldNames.size() && i < mc.pattern->arguments.size(); ++i) {
                                    const std::string& fname = mc.pattern->fieldNames[i];
                                    if (it->second.count(fname)) {
                                        Type fType = it->second.at(fname);
                                        const auto& arg = *mc.pattern->arguments[i];
                                        if (arg.kind == Expr::Kind::Variable && !globalConstants.count(arg.text) && !enumValues.count(arg.text)) {
                                            caseEnv[arg.text] = fType;
                                        }
                                    }
                                }
                            }
                        }
                        Flow mf = sequence(mc.body, loopDepth, result, caseEnv);
                        combined.breaks |= mf.breaks;
                        combined.skips |= mf.skips;
                        combined.returns |= mf.returns;
                        if (!mf.returns) allBranchesReturn = false;
                    }
                    if (!s.elseBody.empty()) {
                        Flow ef = sequence(s.elseBody, loopDepth, result, env);
                        combined.breaks |= ef.breaks;
                        combined.skips |= ef.skips;
                        combined.returns |= ef.returns;
                        if (!ef.returns) allBranchesReturn = false;
                        combined.falls = !allBranchesReturn;
                    } else if (hasWildcard) {
                        combined.falls = !allBranchesReturn;
                    } else {
                        combined.falls = true;
                    }
                    one = combined;
                    break;
                }
                case Stmt::Kind::Lock: {
                    if (!s.expr) throw Error("line " + std::to_string(s.line) + ": Lock requires a Mutex expression");
                    Type mtxType = infer(*s.expr, env);
                    if (mtxType.kind != TypeKind::Mutex)
                        throw Error("line " + std::to_string(s.line) + ": Lock requires a Mutex, found " + typeName(mtxType));
                    one = sequence(s.body, loopDepth, result, env);
                    break;
                }
                case Stmt::Kind::UnsafeBlock: {
                    ++unsafeDepth;
                    one = sequence(s.body, loopDepth, result, env);
                    --unsafeDepth;
                    break;
                }
                case Stmt::Kind::AsmBlock: {
                    break;
                }
            }
            if (active) {
                total.breaks |= one.breaks;
                total.skips |= one.skips;
                total.returns |= one.returns;
                active = one.falls;
            }
        }
        total.falls = active;
        return total;
    }

  public:
    void run(std::vector<Function>& fs, const std::vector<VarDecl>& globals = {},
             const std::vector<EnumDecl>& enums = {}, const std::vector<RecordDecl>& records = {},
             const std::unordered_map<std::size_t, std::string>& mods = {},
             const std::unordered_map<std::size_t, std::unordered_set<std::string>>& imps = {},
             const std::unordered_map<std::size_t, std::unordered_set<std::string>>& uses = {},
             const std::unordered_map<std::size_t, std::unordered_map<std::string, std::string>>& aliases = {}) {
        fileModules = mods;
        fileImports = imps;
        fileUses = uses;
        fileImportAliases = aliases;
        reservedPins.clear();
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& [srcIdx, impSet] : fileImports) {
                std::vector<std::string> toAdd;
                for (const auto& impName : impSet) {
                    for (const auto& [otherIdx, otherModName] : fileModules) {
                        if (lower(otherModName) == lower(impName)) {
                            auto it = fileImports.find(otherIdx);
                            if (it != fileImports.end()) {
                                for (const auto& transitiveImp : it->second) {
                                    if (!impSet.count(transitiveImp)) {
                                        toAdd.push_back(transitiveImp);
                                    }
                                }
                            }
                        }
                    }
                }
                if (!toAdd.empty()) {
                    changed = true;
                    for (const auto& a : toAdd) impSet.insert(a);
                }
            }
        }
        changed = true;
        while (changed) {
            changed = false;
            for (auto& [srcIdx, useSet] : fileUses) {
                std::vector<std::string> toAdd;
                for (const auto& useName : useSet) {
                    for (const auto& [otherIdx, otherModName] : fileModules) {
                        if (lower(otherModName) == lower(useName)) {
                            auto it = fileUses.find(otherIdx);
                            if (it != fileUses.end()) {
                                for (const auto& transitiveUse : it->second) {
                                    if (!useSet.count(transitiveUse)) {
                                        toAdd.push_back(transitiveUse);
                                    }
                                }
                            }
                        }
                    }
                }
                if (!toAdd.empty()) {
                    changed = true;
                    for (const auto& a : toAdd) useSet.insert(a);
                }
            }
        }
        signatures.clear();
        moduleSignatures.clear();
        moduleGlobalTypes.clear();
        moduleGlobalConstants.clear();
        resolvedCallKeys.clear();
        resolvedGlobalKeys.clear();
        ambiguousFunctions.clear();
        ambiguousGlobals.clear();
        recordIsTagged.clear();
        recordParent.clear();
        recordRoot.clear();
        vtableSlotNames.clear();
        vtableFunctions.clear();
        taggedMethodSignatures.clear();
        virtualDispatchCalls.clear();
        globalTypes.clear();
        globalConstants.clear();
        enumValues.clear();
        enumVariantTypes.clear();
        enumAllVariants.clear();
        recordFieldTypes.clear();
        recordFieldOrder.clear();
        fieldAssignmentTypes.clear();
        fieldAssignmentRecords.clear();
        recordModule.clear();
        enumModule.clear();
        globalModule.clear();
        funcModule.clear();
        for (const auto& r : records) {
            recordModule[r.name] = { fileModules.count(r.sourceIndex) ? fileModules.at(r.sourceIndex) : "", r.sourceIndex };
            recordIsTagged[r.name] = r.isTagged || !r.parentName.empty();
            if (!r.parentName.empty()) recordParent[r.name] = r.parentName;
        }
        for (const auto& r : records) {
            std::string cur = r.name;
            std::unordered_set<std::string> seen;
            bool tagged = recordIsTagged[r.name];
            while (recordParent.count(cur)) {
                if (!seen.insert(cur).second)
                    throw Error("line " + std::to_string(r.line) + ": cyclic record inheritance involving '" + r.name + "'");
                cur = recordParent.at(cur);
                if (!recordModule.count(cur))
                    throw Error("line " + std::to_string(r.line) + ": parent record '" + cur + "' not found for '" + r.name + "'");
                if (recordIsTagged[cur]) tagged = true;
            }
            if (tagged) {
                recordIsTagged[r.name] = true;
                recordRoot[r.name] = cur;
            }
        }
        for (const auto& e : enums) enumModule[e.name] = { fileModules.count(e.sourceIndex) ? fileModules.at(e.sourceIndex) : "", e.sourceIndex };
        for (const auto& g : globals) globalModule[g.name] = { fileModules.count(g.sourceIndex) ? fileModules.at(g.sourceIndex) : "", g.sourceIndex };
        for (const auto& f : fs) funcModule[f.name] = { fileModules.count(f.sourceIndex) ? fileModules.at(f.sourceIndex) : "", f.sourceIndex };
        for (const auto& record : records) {
            currentSourceIndex = record.sourceIndex;
            auto& fields = recordFieldTypes[record.name];
            auto& order = recordFieldOrder[record.name];
            std::vector<std::string> chain;
            std::string cur = record.name;
            while (!cur.empty()) {
                chain.push_back(cur);
                cur = recordParent.count(cur) ? recordParent.at(cur) : "";
            }
            std::reverse(chain.begin(), chain.end());
            for (const auto& rName : chain) {
                auto recIt = std::find_if(records.begin(), records.end(), [&](const RecordDecl& rd) { return rd.name == rName; });
                if (recIt == records.end()) continue;
                for (const auto& field : recIt->fields) {
                    if (rName == record.name) checkTypeAccess(field.type, field.line, record.sourceIndex);
                    if (!fields.emplace(field.name, field.type).second) {
                        if (rName == record.name)
                            throw Error("line " + std::to_string(field.line) + ": duplicate field '" + field.name + "'");
                    } else {
                        order.push_back(field.name);
                    }
                }
            }
            if (record.hasDrop) {
                dropRecords.insert(record.name);
                std::unordered_map<std::string, Type> dropEnv = globalTypes;
                for (const auto& f : record.fields) {
                    dropEnv[f.name] = f.type;
                }
                sequence(record.dropBody, 0, std::nullopt, dropEnv);
            }
        }
        for (const auto& e : enums) {
            currentSourceIndex = e.sourceIndex;
            const Type enumType = Type::enumType(e.name, e.underlying.bits, e.underlying.kind == TypeKind::Signed);
            std::unordered_set<std::string> names;
            std::set<std::pair<bool, std::uint64_t>> numbers;
            for (const auto& variant : e.variants) {
                if (!names.insert(variant.name).second)
                    throw Error("line " + std::to_string(variant.line) + ": duplicate variant '" + variant.name + "' in enum '" + e.name + "'");
                if (variant.negative && e.underlying.kind != TypeKind::Signed)
                    throw Error("line " + std::to_string(variant.line) + ": negative enum value does not fit " + typeName(e.underlying));
                bool fitsValue;
                if (e.underlying.kind == TypeKind::Signed) {
                    const std::uint64_t limit = std::uint64_t(1) << (e.underlying.bits - 1);
                    fitsValue = variant.negative ? variant.magnitude <= limit :
                        (e.underlying.bits == 64 ? variant.magnitude <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) : variant.magnitude < limit);
                } else fitsValue = e.underlying.bits == 64 || variant.magnitude < (std::uint64_t(1) << e.underlying.bits);
                if (!fitsValue)
                    throw Error("line " + std::to_string(variant.line) + ": enum value does not fit " + typeName(e.underlying));
                const bool negative = variant.negative && variant.magnitude != 0;
                if (!numbers.emplace(negative, variant.magnitude).second)
                    throw Error("line " + std::to_string(variant.line) + ": duplicate numeric value in enum '" + e.name + "' (aliases are not supported)");
                const std::string key = e.name + "." + variant.name;
                enumAllVariants[e.name].insert(variant.name);
                enumValues.emplace(key, EnumValue{variant.negative, variant.magnitude, enumType});
                enumVariantTypes.emplace(key, enumType);
            }
        }
        for (const auto& g : globals) {
            currentSourceIndex = g.sourceIndex;
            checkTypeAccess(g.type, g.line, g.sourceIndex);
            if (g.type.kind == TypeKind::Vector && !g.type.hasExplicitSize && !g.initializer) {
                throw Error("line " + std::to_string(g.line) + ": Vector declaration requires an explicit element count 'T Vector[count]' or an initializer literal '= {...}'");
            }
            std::string gMod = fileModules.count(g.sourceIndex) ? lower(fileModules.at(g.sourceIndex)) : "";
            if (!gMod.empty()) {
                if (!moduleGlobalTypes[gMod].emplace(g.name, g.type).second)
                    throw Error("line " + std::to_string(g.line) + ": duplicate global '" + g.name + "' in module '" + gMod + "'");
                if (g.constant) moduleGlobalConstants[gMod].insert(g.name);
            }
            if (!globalTypes.emplace(g.name, g.type).second) {
                if (gMod.empty())
                    throw Error("line " + std::to_string(g.line) + ": duplicate global '" + g.name + "'");
                ambiguousGlobals.insert(g.name);
            }
            if (g.constant) globalConstants.insert(g.name);
        }
        for (auto& f : fs) {
            if (f.isConcise && !f.body.empty() && f.body[0].kind == Stmt::Kind::Give) {
                currentSourceIndex = f.sourceIndex;
                std::unordered_map<std::string, Type> paramEnv = globalTypes;
                for (const auto& p : f.params) {
                    checkTypeAccess(p.type, p.line, f.sourceIndex);
                    paramEnv[p.name] = p.type;
                }
                Type inferred = infer(*f.body[0].expr, paramEnv);
                f.result = inferred;
            }
        }
        overloadedBinaryOps.clear();
        overloadedIndexOps.clear();
        overloadedIndexSetOps.clear();
        for (const auto& f : fs) {
            if (f.isOperator) {
                if (f.operatorSymbol == "[]" && f.params.size() == 2) {
                    overloadedIndexOps[{typeName(f.params[0].type), typeName(f.params[1].type)}] = f.name;
                } else if (f.operatorSymbol == "[]=" && f.params.size() == 3) {
                    overloadedIndexSetOps[{typeName(f.params[0].type), typeName(f.params[1].type), typeName(f.params[2].type)}] = f.name;
                } else if (f.params.size() == 2) {
                    overloadedBinaryOps[{f.operatorSymbol, typeName(f.params[0].type), typeName(f.params[1].type)}] = f.name;
                }
            }
        }
        for (const auto& f : fs) {
            currentSourceIndex = f.sourceIndex;
            Signature sig;
            sig.isAsync = f.isAsync;
            sig.isExtern = f.isExtern;
            for (const auto& p : f.params) {
                checkTypeAccess(p.type, p.line, f.sourceIndex);
                sig.parameters.push_back(p.type);
            }
            if (f.result) {
                checkTypeAccess(*f.result, f.line, f.sourceIndex);
            }
            sig.result = f.result;
            sig.errorResult = f.errorResult;
            if (sig.result && sig.errorResult) {
                sig.result->errorType = std::make_shared<Type>(*sig.errorResult);
            }
            std::string fMod = fileModules.count(f.sourceIndex) ? lower(fileModules.at(f.sourceIndex)) : "";
            std::string taggedRec;
            if (!f.isExtern && !f.params.empty()) {
                const Type& p0 = f.params[0].type;
                std::string candR = (p0.kind == TypeKind::Record) ? p0.name :
                    (((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) && p0.pointeeType && p0.pointeeType->kind == TypeKind::Record) ? p0.pointeeType->name : "");
                if (!candR.empty() && recordIsTagged[candR]) taggedRec = candR;
            }
            if (!taggedRec.empty()) {
                std::string mLlvm = functionLLVMName(f);
                if (!taggedMethodSignatures[taggedRec].emplace(f.name, std::make_pair(sig, mLlvm)).second)
                    throw Error("line " + std::to_string(f.line) + ": duplicate method '" + f.name + "' on tagged record '" + taggedRec + "'");
                if (!fMod.empty()) {
                    moduleSignatures[fMod].emplace(f.name, sig);
                }
                signatures.emplace(f.name, sig);
            } else {
                if (!fMod.empty()) {
                    if (!moduleSignatures[fMod].emplace(f.name, sig).second)
                        throw Error("line " + std::to_string(f.line) + ": duplicate function '" + f.name + "' in module '" + fMod + "'");
                }
                if (!signatures.emplace(f.name, sig).second) {
                    if (fMod.empty())
                        throw Error("line " + std::to_string(f.line) + ": duplicate function '" + f.name + "'");
                    ambiguousFunctions.insert(f.name);
                }
            }
        }
        // Build vtable slots and function mappings for Tagged records
        for (const auto& f : fs) {
            if (f.isExtern || f.params.empty()) continue;
            const Type& p0 = f.params[0].type;
            std::string recN;
            if (p0.kind == TypeKind::Record) recN = p0.name;
            else if ((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) && p0.pointeeType && p0.pointeeType->kind == TypeKind::Record)
                recN = p0.pointeeType->name;
            if (!recN.empty() && recordIsTagged[recN] && recordRoot.count(recN)) {
                const std::string& root = recordRoot.at(recN);
                auto& slots = vtableSlotNames[root];
                if (std::find(slots.begin(), slots.end(), f.name) == slots.end()) {
                    slots.push_back(f.name);
                }
            }
        }
        for (const auto& r : records) {
            if (!recordIsTagged[r.name] || !recordRoot.count(r.name)) continue;
            const std::string& root = recordRoot.at(r.name);
            const auto& slots = vtableSlotNames[root];
            auto& tbl = vtableFunctions[r.name];
            tbl.resize(slots.size());
            for (std::size_t si = 0; si < slots.size(); ++si) {
                const std::string& mName = slots[si];
                std::string cur = r.name;
                while (!cur.empty()) {
                    std::string foundFn;
                    for (const auto& f : fs) {
                        if (f.isExtern || f.name != mName || f.params.empty()) continue;
                        const Type& p0 = f.params[0].type;
                        std::string pRec = (p0.kind == TypeKind::Record) ? p0.name :
                            (((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) && p0.pointeeType && p0.pointeeType->kind == TypeKind::Record) ? p0.pointeeType->name : "");
                        if (pRec == cur) {
                            foundFn = functionLLVMName(f);
                            break;
                        }
                    }
                    if (!foundFn.empty()) {
                        tbl[si] = foundFn;
                        break;
                    }
                    cur = recordParent.count(cur) ? recordParent.at(cur) : "";
                }
            }
        }
        for (const auto& e : enums) {
            currentSourceIndex = e.sourceIndex;
            if (signatures.count(e.name) || recordFieldTypes.count(e.name))
                throw Error("line " + std::to_string(e.line) + ": enum '" + e.name + "' conflicts with another declaration");
        }
        globalConstantValues.clear();
        for (const auto& g : globals) {
            currentSourceIndex = g.sourceIndex;
            if (signatures.count(g.name) || std::any_of(enums.begin(), enums.end(), [&](const EnumDecl& e) { return e.name == g.name; }) || recordFieldTypes.count(g.name))
                throw Error("line " + std::to_string(g.line) + ": global '" + g.name + "' conflicts with another declaration");
            if (g.initializer) {
                if (!constantInitializer(*g.initializer))
                    throw Error("line " + std::to_string(g.line) + ": global initializers must be literal constants; references and calls are not allowed");
                infer(*g.initializer, globalTypes, g.type);
                if (g.constant) {
                    if (auto val = resolveConstInt(*g.initializer)) {
                        globalConstantValues[g.name] = *val;
                    }
                }
            } else if (g.constant) {
                throw Error("line " + std::to_string(g.line) + ": Constant global requires an initializer");
            }
        }
        for (const auto& f : fs) {
            if (f.isExtern) continue;
            currentSourceIndex = f.sourceIndex;
            std::string callerMod = fileModules.count(f.sourceIndex) ? lower(fileModules.at(f.sourceIndex)) : "";
            currentAnalyzedFunctionKey = mangleFuncIfNeeded(callerMod, f.name, f.isExtern);
            currentFunctionLocals.clear();
            currentFunctionConstants.clear();
            currentFunctionConstantValues.clear();
            for (const auto& d : f.locals) {
                if (d.constant && d.initializer) {
                    if (auto val = resolveConstInt(*d.initializer)) {
                        currentFunctionConstantValues[d.name] = *val;
                    }
                }
            }
            currentBlockUses.clear();
            for (const auto& u : f.uses) {
                checkAccess("module", u.text, u.text, u.line, f.sourceIndex);
                currentBlockUses.insert(lower(u.text));
            }
            currentFunctionErrorResult = f.errorResult;
            for (const auto& s : f.body) {
                if (s.kind == Stmt::Kind::Use) {
                    checkAccess("module", s.name, s.name, s.line, f.sourceIndex);
                    currentBlockUses.insert(lower(s.name));
                }
            }
            for (const auto& p : f.params) currentFunctionLocals.insert(p.name);
            for (const auto& d : f.locals) {
                currentFunctionLocals.insert(d.name);
                if (d.constant) currentFunctionConstants.insert(d.name);
            }
            std::unordered_map<std::string, Type> env;
            std::string fModLower = fileModules.count(f.sourceIndex) ? lower(fileModules.at(f.sourceIndex)) : "";
            if (fModLower.empty()) {
                env = globalTypes;
            } else {
                if (moduleGlobalTypes.count(fModLower)) {
                    for (const auto& [gn, gt] : moduleGlobalTypes.at(fModLower)) env.emplace(gn, gt);
                }
                for (const auto& u : currentBlockUses) {
                    std::string uRes = lower(resolveModuleAlias(u, f.sourceIndex));
                    if (moduleGlobalTypes.count(uRes)) {
                        for (const auto& [gn, gt] : moduleGlobalTypes.at(uRes)) env.emplace(gn, gt);
                    }
                }
                if (fileUses.count(f.sourceIndex)) {
                    for (const auto& u : fileUses.at(f.sourceIndex)) {
                        std::string uRes = lower(resolveModuleAlias(u, f.sourceIndex));
                        if (moduleGlobalTypes.count(uRes)) {
                            for (const auto& [gn, gt] : moduleGlobalTypes.at(uRes)) env.emplace(gn, gt);
                        }
                    }
                }
            }
            auto globalEnv = env;
            for (const auto& p : f.params)
                if (globalEnv.count(p.name) || enumValues.count(p.name))
                    throw Error("line " + std::to_string(p.line) + ": parameter '" + p.name + "' shadows a global");
                else if (!env.emplace(p.name, p.type).second)
                    throw Error("line " + std::to_string(p.line) + ": duplicate parameter/local name '" + p.name + "'");
            for (const auto& d : f.locals) {
                checkTypeAccess(d.type, d.line, f.sourceIndex);
                if (globalEnv.count(d.name))
                    throw Error("line " + std::to_string(d.line) + ": local '" + d.name + "' shadows a global");
                if (enumValues.count(d.name))
                    throw Error("line " + std::to_string(d.line) + ": local '" + d.name + "' shadows an enum value");
                if (env.count(d.name))
                    throw Error("line " + std::to_string(d.line) + ": duplicate local or parameter/local name '" +
                                d.name + "'");
                if (d.type.kind == TypeKind::Vector && !d.type.hasExplicitSize && !d.initializer) {
                    throw Error("line " + std::to_string(d.line) + ": Vector declaration requires an explicit element count 'T Vector[count]' or an initializer literal '= {...}'");
                }
                if (d.initializer) infer(*d.initializer, env, d.type);
                env.emplace(d.name, d.type);
            }
            if (f.isPeriodic) {
                Type u32Ty = Type::unsignedInt(32);
                if (f.periodicIntervalExpr) infer(*f.periodicIntervalExpr, env, u32Ty);
                if (f.periodicJitterExpr) infer(*f.periodicJitterExpr, env, u32Ty);
            }
            Flow flow = sequence(f.body, 0, f.result, env);
            if (f.result && flow.falls)
                throw Error("line " + std::to_string(f.line) + ": value-producing block '" + f.name +
                            "' has a reachable path without Give");
            checkMemorySafety(f, env);
            checkDefiniteAssignment(f);
        }
        currentAnalyzedFunctionKey.clear();
        if (baremetal) {
            std::unordered_map<std::string, int> state;
            std::function<void(const std::string&)> dfs = [&](const std::string& u) {
                state[u] = 1;
                auto it = callGraphEdges.find(u);
                if (it != callGraphEdges.end()) {
                    for (const auto& v : it->second) {
                        if (state[v] == 1) {
                            throw Error("bare-metal recursion is disallowed to guarantee bounded stack usage (cycle involving '" + u + "' -> '" + v + "')");
                        }
                        if (state[v] == 0) dfs(v);
                    }
                }
                state[u] = 2;
            };
            for (const auto& f : fs) {
                std::string callerMod = fileModules.count(f.sourceIndex) ? lower(fileModules.at(f.sourceIndex)) : "";
                std::string fKey = mangleFuncIfNeeded(callerMod, f.name, f.isExtern);
                if (!f.isExtern && state[fKey] == 0) dfs(fKey);
                if (!f.isExtern && state[f.name] == 0) dfs(f.name);
            }
            std::unordered_map<std::string, uint32_t> funcFrameSize;
            for (const auto& f : fs) {
                uint32_t sz = 32;
                for (const auto& d : f.locals) {
                    uint32_t vSize = 4;
                    if (d.type.kind == TypeKind::Array) {
                        vSize = d.type.arraySize * 4;
                    } else if (d.type.kind == TypeKind::Signed || d.type.kind == TypeKind::Unsigned) {
                        vSize = (d.type.bits > 32) ? 8 : 4;
                    }
                    sz += ((vSize + 3) / 4) * 4;
                }
                funcFrameSize[f.name] = sz;
            }
            std::unordered_map<std::string, uint32_t> memoWcsd;
            std::function<uint32_t(const std::string&)> calcWcsd = [&](const std::string& fnName) -> uint32_t {
                if (memoWcsd.count(fnName)) return memoWcsd[fnName];
                uint32_t cur = funcFrameSize.count(fnName) ? funcFrameSize[fnName] : 32;
                uint32_t maxChild = 0;
                auto it = callGraphEdges.find(fnName);
                if (it != callGraphEdges.end()) {
                    for (const auto& child : it->second) {
                        uint32_t d = calcWcsd(child);
                        if (d > maxChild) maxChild = d;
                    }
                }
                uint32_t total = cur + maxChild;
                memoWcsd[fnName] = total;
                return total;
            };
            for (const auto& f : fs) {
                uint32_t wcsd = calcWcsd(f.name);
                for (const auto& d : f.locals) {
                    if (d.name == "Stack_Limit" || d.name == "Stack_Size" || d.name == "stack_limit") {
                        if (d.initializer && d.initializer->kind == Expr::Kind::Integer) {
                            uint64_t limit = d.initializer->magnitude;
                            if (wcsd > limit) {
                                throw Error("line " + std::to_string(f.line) + ": error[E4001]: Static stack overflow: Task '" + f.name + "' requires " + std::to_string(wcsd) + " bytes but stack is limited to " + std::to_string(limit) + " bytes");
                            }
                        }
                    }
                }
            }
        }
    }
    void checkMemorySafety(const Function& f, const std::unordered_map<std::string, Type>& env) {
        (void)env;
        std::unordered_set<std::string> localNames;
        for (const auto& p : f.params) localNames.insert(p.name);
        for (const auto& d : f.locals) localNames.insert(d.name);

        std::unordered_set<std::string> ensureReleased;
        for (const auto& s : f.body) {
            if (s.kind == Stmt::Kind::Ensure) {
                for (const auto& es : s.body) {
                    if (es.kind == Stmt::Kind::Call && es.expr && es.expr->text == "Release" && !es.expr->arguments.empty()) {
                        if (es.expr->arguments[0]->kind == Expr::Kind::Variable) {
                            ensureReleased.insert(es.expr->arguments[0]->text);
                        }
                    }
                }
            }
        }

        std::unordered_set<std::string> allocatedPointers;
        std::unordered_set<std::string> nullPointers;
        for (const auto& d : f.locals) {
            if (d.initializer && d.initializer->kind == Expr::Kind::Allocate) {
                allocatedPointers.insert(d.name);
            } else if (d.initializer && d.initializer->kind == Expr::Kind::NullLiteral) {
                nullPointers.insert(d.name);
            }
        }

        std::unordered_set<std::string> releasedPointers;
        std::unordered_set<std::string> transferredPointers;
        std::unordered_map<std::string, int> dmaLockedBuffers;
        std::vector<std::string> heldSpinlocks;

        std::function<std::string(const Expr&)> extractBufferRoot = [&](const Expr& e) -> std::string {
            if (e.kind == Expr::Kind::Variable) return e.text;
            if (e.kind == Expr::Kind::Address && e.left) return extractBufferRoot(*e.left);
            if (e.kind == Expr::Kind::Index && e.left) return extractBufferRoot(*e.left);
            if (e.kind == Expr::Kind::Member && e.left) return extractBufferRoot(*e.left);
            if (e.kind == Expr::Kind::Cast && e.left) return extractBufferRoot(*e.left);
            return "";
        };

        std::function<void(const Expr&)> extractNonNull = [&](const Expr& e) {
            if (e.kind == Expr::Kind::Binary) {
                if (e.text == "!=") {
                    if (e.left && e.left->kind == Expr::Kind::Variable && e.right && e.right->kind == Expr::Kind::NullLiteral) {
                        nullPointers.erase(e.left->text);
                    } else if (e.right && e.right->kind == Expr::Kind::Variable && e.left && e.left->kind == Expr::Kind::NullLiteral) {
                        nullPointers.erase(e.right->text);
                    }
                } else if (e.text == "And" || e.text == "and") {
                    if (e.left) extractNonNull(*e.left);
                    if (e.right) extractNonNull(*e.right);
                }
            }
        };

        std::function<void(const Expr&, int)> checkExpr = [&](const Expr& e, int line) {
            std::string rootBuf = extractBufferRoot(e);
            if (!rootBuf.empty() && dmaLockedBuffers.count(rootBuf)) {
                throw Error("line " + std::to_string(line) + ": error[E3005]: Use-while-DMA-active: buffer '" +
                            rootBuf + "' is currently locked by Dma.Channel_" +
                            std::to_string(dmaLockedBuffers[rootBuf]) + "; access is prohibited until Dma.Wait_Complete");
            }
            if (e.kind == Expr::Kind::Deref || e.kind == Expr::Kind::Index) {
                const Expr* base = e.left.get();
                while (base && (base->kind == Expr::Kind::Deref || base->kind == Expr::Kind::Index || base->kind == Expr::Kind::Member)) {
                    base = base->left.get();
                }
                if (base && base->kind == Expr::Kind::Variable) {
                    if (releasedPointers.count(base->text)) {
                        throw Error("line " + std::to_string(line) + ": use after free: pointer '" + base->text + "' used after being released");
                    }
                    if (nullPointers.count(base->text)) {
                        throw Error("line " + std::to_string(line) + ": null pointer dereference: pointer '" + base->text + "' is null");
                    }
                }
            }
            if (e.kind == Expr::Kind::Variable && releasedPointers.count(e.text)) {
                throw Error("line " + std::to_string(line) + ": use after free: pointer '" + e.text + "' used after being released");
            }
            if (e.kind == Expr::Kind::Binary && (e.text == "And" || e.text == "and")) {
                if (e.left) checkExpr(*e.left, line);
                auto savedNullPointers = nullPointers;
                if (e.left) extractNonNull(*e.left);
                if (e.right) checkExpr(*e.right, line);
                nullPointers = savedNullPointers;
                return;
            }
            if (e.left) checkExpr(*e.left, line);
            if (e.right) checkExpr(*e.right, line);
            for (const auto& arg : e.arguments) checkExpr(*arg, line);
        };

        std::function<std::string(const Expr&)> findStackAddress = [&](const Expr& e) -> std::string {
            if (e.kind == Expr::Kind::Address && e.left && e.left->kind == Expr::Kind::Variable && localNames.count(e.left->text)) {
                return e.left->text;
            }
            if (e.left) {
                std::string res = findStackAddress(*e.left);
                if (!res.empty()) return res;
            }
            if (e.right) {
                std::string res = findStackAddress(*e.right);
                if (!res.empty()) return res;
            }
            for (const auto& arg : e.arguments) {
                std::string res = findStackAddress(*arg);
                if (!res.empty()) return res;
            }
            return "";
        };

        std::function<void(const Stmt&)> checkStmt = [&](const Stmt& s) {
            if (s.kind == Stmt::Kind::Ensure) return;
            if (s.kind == Stmt::Kind::Assign) {
                if (dmaLockedBuffers.count(s.name)) {
                    throw Error("line " + std::to_string(s.line) + ": error[E3005]: Use-while-DMA-active: buffer '" + s.name + "' is currently locked by Dma.Channel_" + std::to_string(dmaLockedBuffers[s.name]) + "; access is prohibited until Dma.Wait_Complete");
                }
                if (s.expr) {
                    if (s.expr->kind == Expr::Kind::Allocate) {
                        allocatedPointers.insert(s.name);
                        releasedPointers.erase(s.name);
                        nullPointers.erase(s.name);
                    } else if (s.expr->kind == Expr::Kind::NullLiteral) {
                        nullPointers.insert(s.name);
                    } else {
                        checkExpr(*s.expr, s.line);
                        nullPointers.erase(s.name);
                    }
                    std::string esc = findStackAddress(*s.expr);
                    if (!esc.empty() && globalTypes.count(s.name)) {
                        throw Error("line " + std::to_string(s.line) + ": stack address escape: address of local variable '" + esc + "' cannot be assigned to global '" + s.name + "'");
                    }
                }
            } else if (s.kind == Stmt::Kind::Give) {
                if (s.expr) {
                    std::string esc = findStackAddress(*s.expr);
                    if (!esc.empty()) {
                        throw Error("line " + std::to_string(s.line) + ": stack address escape: address of local variable '" + esc + "' cannot be returned");
                    }
                    if (s.expr->kind == Expr::Kind::Variable && allocatedPointers.count(s.expr->text)) {
                        transferredPointers.insert(s.expr->text);
                    }
                    checkExpr(*s.expr, s.line);
                }
            } else if (s.kind == Stmt::Kind::Call) {
                if (s.expr) {
                    std::string qTarget = "";
                    if (!s.expr->qualifier.empty()) qTarget = s.expr->qualifier + "." + s.expr->text;
                    else qTarget = s.expr->text;
                    std::string lTarget = lower(qTarget);
                    if (lTarget == "dma.transmit" || lTarget == "dma.receive" || lTarget == "transmit" || lTarget == "receive") {
                        if (s.expr->arguments.size() >= 2) {
                            for (const auto& arg : s.expr->arguments) checkExpr(*arg, s.line);
                            int ch = 0;
                            if (s.expr->arguments[0]->kind == Expr::Kind::Integer) {
                                ch = static_cast<int>(s.expr->arguments[0]->magnitude);
                            }
                            std::string bVar = extractBufferRoot(*s.expr->arguments[1]);
                            if (!bVar.empty()) {
                                dmaLockedBuffers[bVar] = ch;
                            }
                            return;
                        }
                    } else if (lTarget == "dma.wait_complete" || lTarget == "dma.wait" || lTarget == "wait_complete" || lTarget == "wait" ||
                               lTarget == "dma.abort" || lTarget == "abort") {
                        if (!s.expr->arguments.empty()) {
                            checkExpr(*s.expr->arguments[0], s.line);
                            int ch = -1;
                            if (s.expr->arguments[0]->kind == Expr::Kind::Integer) {
                                ch = static_cast<int>(s.expr->arguments[0]->magnitude);
                            }
                            if (ch >= 0) {
                                for (auto it = dmaLockedBuffers.begin(); it != dmaLockedBuffers.end(); ) {
                                    if (it->second == ch) it = dmaLockedBuffers.erase(it);
                                    else ++it;
                                }
                            } else {
                                dmaLockedBuffers.clear();
                            }
                            return;
                        } else {
                            dmaLockedBuffers.clear();
                            return;
                        }
                    } else if (lTarget == "sync.spinlock_claim" || lTarget == "spinlock_claim") {
                        if (!s.expr->arguments.empty()) {
                            checkExpr(*s.expr->arguments[0], s.line);
                            std::string lId = "";
                            if (s.expr->arguments[0]->kind == Expr::Kind::Integer) lId = std::to_string(s.expr->arguments[0]->magnitude);
                            else if (s.expr->arguments[0]->kind == Expr::Kind::Variable) lId = s.expr->arguments[0]->text;
                            if (!lId.empty()) {
                                for (const auto& held : heldSpinlocks) {
                                    if (held != lId) {
                                        lockOrderGraph[held].insert(lId);
                                        std::unordered_set<std::string> visited;
                                        std::function<bool(const std::string&)> hasPath = [&](const std::string& curr) -> bool {
                                            if (curr == held) return true;
                                            visited.insert(curr);
                                            auto it = lockOrderGraph.find(curr);
                                            if (it != lockOrderGraph.end()) {
                                                for (const auto& next : it->second) {
                                                    if (!visited.count(next) && hasPath(next)) return true;
                                                }
                                            }
                                            return false;
                                        };
                                        if (hasPath(lId)) {
                                            throw Error("line " + std::to_string(s.line) + ": error[E2005]: Circular lock order detected between Spinlock " + held + " and Spinlock " + lId + " (Deadlock hazard)");
                                        }
                                    }
                                }
                                heldSpinlocks.push_back(lId);
                            }
                            return;
                        }
                    } else if (lTarget == "sync.spinlock_release" || lTarget == "spinlock_release") {
                        if (!s.expr->arguments.empty()) {
                            checkExpr(*s.expr->arguments[0], s.line);
                            std::string lId = "";
                            if (s.expr->arguments[0]->kind == Expr::Kind::Integer) lId = std::to_string(s.expr->arguments[0]->magnitude);
                            else if (s.expr->arguments[0]->kind == Expr::Kind::Variable) lId = s.expr->arguments[0]->text;
                            if (!lId.empty()) {
                                auto pos = std::find(heldSpinlocks.begin(), heldSpinlocks.end(), lId);
                                if (pos != heldSpinlocks.end()) heldSpinlocks.erase(pos);
                            }
                            return;
                        }
                    }
                    if (s.expr->text == "Release" || s.expr->kind == Expr::Kind::Release) {
                        if (!s.expr->arguments.empty() && s.expr->arguments[0]->kind == Expr::Kind::Variable) {
                            const std::string& pName = s.expr->arguments[0]->text;
                            if (releasedPointers.count(pName) || ensureReleased.count(pName)) {
                                throw Error("line " + std::to_string(s.line) + ": double free: pointer '" + pName + "' has already been released");
                            }
                            releasedPointers.insert(pName);
                        }
                    } else {
                        for (const auto& arg : s.expr->arguments) {
                            if (arg->kind == Expr::Kind::Variable && allocatedPointers.count(arg->text)) {
                                transferredPointers.insert(arg->text);
                            }
                            checkExpr(*arg, s.line);
                        }
                    }
                }
            } else if (s.kind == Stmt::Kind::TargetAssign || s.kind == Stmt::Kind::DerefAssign) {
                if (s.target) checkExpr(*s.target, s.line);
                if (s.expr) checkExpr(*s.expr, s.line);
            } else if (s.kind == Stmt::Kind::Write) {
                if (s.expr) checkExpr(*s.expr, s.line);
                for (const auto& w : s.writeArgs) checkExpr(*w, s.line);
            } else if (s.kind == Stmt::Kind::If) {
                if (s.expr) checkExpr(*s.expr, s.line);
                const auto beforeNullPointers = nullPointers;
                const auto beforeAllocatedPointers = allocatedPointers;
                const auto beforeReleasedPointers = releasedPointers;
                const auto beforeTransferredPointers = transferredPointers;
                if (s.expr) extractNonNull(*s.expr);
                for (const auto& is : s.body) checkStmt(is);
                const auto thenNullPointers = nullPointers;
                const auto thenAllocatedPointers = allocatedPointers;
                const auto thenReleasedPointers = releasedPointers;
                const auto thenTransferredPointers = transferredPointers;

                // Branches are alternatives: analyzing one must not make a release
                // in the other branch look like a second release on the same path.
                nullPointers = beforeNullPointers;
                allocatedPointers = beforeAllocatedPointers;
                releasedPointers = beforeReleasedPointers;
                transferredPointers = beforeTransferredPointers;
                for (const auto& es : s.elseBody) checkStmt(es);
                const auto elseNullPointers = nullPointers;
                const auto elseAllocatedPointers = allocatedPointers;
                const auto elseReleasedPointers = releasedPointers;
                const auto elseTransferredPointers = transferredPointers;

                // A release/allocation on either path is a possible state after
                // the join, so retain the union for conservative safety checks.
                allocatedPointers = thenAllocatedPointers;
                allocatedPointers.insert(elseAllocatedPointers.begin(), elseAllocatedPointers.end());
                releasedPointers = thenReleasedPointers;
                releasedPointers.insert(elseReleasedPointers.begin(), elseReleasedPointers.end());
                transferredPointers = thenTransferredPointers;
                transferredPointers.insert(elseTransferredPointers.begin(), elseTransferredPointers.end());

                // A pointer is known null after the join only when both paths
                // establish that fact. A missing Else is represented by the
                // unchanged pre-branch state above.
                nullPointers.clear();
                for (const auto& name : thenNullPointers)
                    if (elseNullPointers.count(name)) nullPointers.insert(name);
            } else if (s.kind == Stmt::Kind::Loop || s.kind == Stmt::Kind::ForIn) {
                if (s.expr) checkExpr(*s.expr, s.line);
                if (s.rangeEnd) checkExpr(*s.rangeEnd, s.line);
                for (const auto& ls : s.body) checkStmt(ls);
            } else if (s.kind == Stmt::Kind::Match) {
                if (s.expr) checkExpr(*s.expr, s.line);
                for (const auto& mc : s.matchCases) {
                    if (mc.guard) checkExpr(*mc.guard, mc.line);
                    for (const auto& ms : mc.body) checkStmt(ms);
                }
                for (const auto& es : s.elseBody) checkStmt(es);
            } else if (s.kind == Stmt::Kind::UnsafeBlock || s.kind == Stmt::Kind::Lock) {
                for (const auto& us : s.body) checkStmt(us);
            }
        };

        for (const auto& s : f.body) checkStmt(s);
    }
    bool isResourceType(const Type& t) const {
        if (t.kind == TypeKind::Pointer || t.kind == TypeKind::Vector ||
            t.kind == TypeKind::Text || t.kind == TypeKind::HMap ||
            t.kind == TypeKind::FileHandle || t.kind == TypeKind::SmartPointer) {
            return true;
        }
        if (t.kind == TypeKind::Record && dropRecords.count(t.name)) {
            return true;
        }
        return false;
    }
    bool isSyncPrimitive(const Type& t) const {
        return t.kind == TypeKind::Channel || t.kind == TypeKind::Mutex || t.kind == TypeKind::Atomic;
    }
    void checkDefiniteAssignment(const Function& f) {
        std::unordered_set<std::string> localVars;
        std::unordered_set<std::string> assigned;
        std::unordered_set<std::string> refParams;
        std::unordered_set<std::string> mutParams;
        std::unordered_set<std::string> movedVars;
        std::unordered_set<std::string> borrowedVars;

        for (const auto& p : f.params) {
            assigned.insert(p.name);
            if (p.isRef) refParams.insert(p.name);
            if (p.isMut) mutParams.insert(p.name);
        }

        for (const auto& d : f.locals) {
            if (d.type.kind == TypeKind::Text || d.type.kind == TypeKind::Vector ||
                d.type.kind == TypeKind::HMap || d.type.kind == TypeKind::Array ||
                d.type.kind == TypeKind::Atomic || d.type.kind == TypeKind::Record) {
                assigned.insert(d.name);
            } else if (d.initializer) {
                assigned.insert(d.name);
            }
            localVars.insert(d.name);
        }

        std::function<void(const Expr&, int, const std::unordered_set<std::string>&)> checkExpr =
            [&](const Expr& e, int line, const std::unordered_set<std::string>& curAssigned) {
            if (e.kind == Expr::Kind::Variable) {
                if (movedVars.count(e.text)) {
                    throw Error("line " + std::to_string(line) + ": use of moved value '" + e.text + "'");
                }
                if (localVars.count(e.text) && !curAssigned.count(e.text)) {
                    throw Error("line " + std::to_string(line) + ": variable '" + e.text + "' may be used uninitialized");
                }
            }
            if (e.kind == Expr::Kind::Move) {
                if (e.left && e.left->kind == Expr::Kind::Variable) {
                    const std::string& mv = e.left->text;
                    if (movedVars.count(mv)) {
                        throw Error("line " + std::to_string(line) + ": use of moved value '" + mv + "'");
                    }
                    if (borrowedVars.count(mv) || refParams.count(mv) || mutParams.count(mv)) {
                        throw Error("line " + std::to_string(line) + ": cannot move borrowed value '" + mv + "'");
                    }
                    if (localVars.count(mv) && !curAssigned.count(mv)) {
                        throw Error("line " + std::to_string(line) + ": variable '" + mv + "' may be used uninitialized");
                    }
                    movedVars.insert(mv);
                }
                return;
            }
            if (e.kind == Expr::Kind::Call && e.text == "Channel_Send") {
                if (e.arguments.size() >= 2) {
                    const auto& item = *e.arguments[1];
                    if (item.kind == Expr::Kind::Borrow) {
                        throw Error("line " + std::to_string(line) + ": cannot send borrowed reference across thread boundary (channel) - must transfer ownership via Move");
                    }
                    if (item.kind == Expr::Kind::Variable) {
                        Type t = typeOf(item);
                        if (t.kind == TypeKind::Vector || t.kind == TypeKind::Text ||
                            t.kind == TypeKind::HMap || t.kind == TypeKind::SmartPointer ||
                            (t.kind == TypeKind::Record && dropRecords.count(t.name))) {
                            throw Error("line " + std::to_string(line) + ": cannot send resource '" + item.text + "' across channel without explicit 'Move " + item.text + "' (transfer of ownership required)");
                        }
                    }
                }
            }
            if (e.kind == Expr::Kind::Spawn) {
                if (!e.arguments.empty() && e.arguments[0]->kind == Expr::Kind::Call) {
                    const auto& call = *e.arguments[0];
                    for (const auto& arg : call.arguments) {
                        if (arg->kind == Expr::Kind::Borrow) {
                            throw Error("line " + std::to_string(line) + ": cannot pass borrowed reference to Spawn task across thread boundaries");
                        }
                        if (arg->kind == Expr::Kind::Variable) {
                            if (refParams.count(arg->text) || mutParams.count(arg->text) || borrowedVars.count(arg->text)) {
                                throw Error("line " + std::to_string(line) + ": cannot pass borrowed reference '" + arg->text + "' to Spawn task across thread boundaries");
                            }
                            Type t = typeOf(*arg);
                            if (t.kind == TypeKind::Vector || t.kind == TypeKind::Text ||
                                t.kind == TypeKind::HMap || t.kind == TypeKind::SmartPointer ||
                                (t.kind == TypeKind::Record && dropRecords.count(t.name))) {
                                throw Error("line " + std::to_string(line) + ": cannot pass resource '" + arg->text + "' to Spawn task without explicit Move (transfer of ownership required)");
                            }
                        }
                    }
                }
            }
            if (e.left) checkExpr(*e.left, line, curAssigned);
            if (e.right) checkExpr(*e.right, line, curAssigned);
            for (const auto& arg : e.arguments) checkExpr(*arg, line, curAssigned);
            if (e.closureBodyExpr) {
                auto closureAssigned = curAssigned;
                for (const auto& cp : e.closureParams) closureAssigned.insert(cp.first);
                checkExpr(*e.closureBodyExpr, line, closureAssigned);
            }
        };

        std::function<void(const Stmt&, std::unordered_set<std::string>&)> checkStmt =
            [&](const Stmt& s, std::unordered_set<std::string>& curAssigned) {
            if (s.kind == Stmt::Kind::Ensure) return;

            if (s.kind == Stmt::Kind::Assign) {
                if (refParams.count(s.name)) {
                    throw Error("line " + std::to_string(s.line) + ": cannot mutate read-only 'Ref' parameter '" + s.name + "'");
                }
                if (borrowedVars.count(s.name)) {
                    throw Error("line " + std::to_string(s.line) + ": cannot mutate read-only borrow '" + s.name + "'");
                }
                if (s.expr) {
                    if (s.expr->kind == Expr::Kind::Variable) {
                        Type srcType = typeOf(*s.expr);
                        if (isResourceType(srcType)) {
                            throw Error("line " + std::to_string(s.line) + ": resource '" + s.expr->text + "' cannot be implicitly assigned without explicit ownership operator. Did you mean 'Move " + s.expr->text + "', 'Copy " + s.expr->text + "', or 'Borrow " + s.expr->text + "'?");
                        }
                    }
                    if (s.expr->kind == Expr::Kind::Move) {
                        checkExpr(*s.expr, s.line, curAssigned);
                        if (s.expr->left && s.expr->left->kind == Expr::Kind::Variable) {
                            curAssigned.erase(s.expr->left->text);
                        }
                    } else if (s.expr->kind == Expr::Kind::Borrow) {
                        borrowedVars.insert(s.name);
                        checkExpr(*s.expr, s.line, curAssigned);
                    } else if (s.expr->kind == Expr::Kind::Copy) {
                        if (s.expr->left && s.expr->left->kind == Expr::Kind::Variable) {
                            const std::string& cp = s.expr->left->text;
                            if (movedVars.count(cp)) {
                                throw Error("line " + std::to_string(s.line) + ": use of moved value '" + cp + "'");
                            }
                        }
                        checkExpr(*s.expr, s.line, curAssigned);
                    } else {
                        checkExpr(*s.expr, s.line, curAssigned);
                    }
                }
                curAssigned.insert(s.name);
                movedVars.erase(s.name);
            } else if (s.kind == Stmt::Kind::MultiAssign) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
                for (const auto& t : s.multiTargets) {
                    if (refParams.count(t)) {
                        throw Error("line " + std::to_string(s.line) + ": cannot mutate read-only 'Ref' parameter '" + t + "'");
                    }
                    curAssigned.insert(t);
                    movedVars.erase(t);
                }
            } else if (s.kind == Stmt::Kind::TargetAssign || s.kind == Stmt::Kind::DerefAssign) {
                if (s.target) {
                    const Expr* root = s.target.get();
                    while (root && (root->kind == Expr::Kind::Member || root->kind == Expr::Kind::Index || root->kind == Expr::Kind::Deref)) {
                        root = root->left.get();
                    }
                    if (root && root->kind == Expr::Kind::Variable) {
                        if (refParams.count(root->text)) {
                            throw Error("line " + std::to_string(s.line) + ": cannot mutate read-only 'Ref' parameter '" + root->text + "'");
                        }
                        if (borrowedVars.count(root->text)) {
                            throw Error("line " + std::to_string(s.line) + ": cannot mutate read-only borrow '" + root->text + "'");
                        }
                    }
                    checkExpr(*s.target, s.line, curAssigned);
                }
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
            } else if (s.kind == Stmt::Kind::Give) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
            } else if (s.kind == Stmt::Kind::Call) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
            } else if (s.kind == Stmt::Kind::Write) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
                for (const auto& w : s.writeArgs) checkExpr(*w, s.line, curAssigned);
            } else if (s.kind == Stmt::Kind::If) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
                auto thenAssigned = curAssigned;
                bool thenFalls = true;
                for (const auto& is : s.body) {
                    checkStmt(is, thenAssigned);
                    if (is.kind == Stmt::Kind::Exit || is.kind == Stmt::Kind::Give) thenFalls = false;
                }
                if (!s.elseBody.empty()) {
                    auto elseAssigned = curAssigned;
                    bool elseFalls = true;
                    for (const auto& es : s.elseBody) {
                        checkStmt(es, elseAssigned);
                        if (es.kind == Stmt::Kind::Exit || es.kind == Stmt::Kind::Give) elseFalls = false;
                    }
                    if (thenFalls && elseFalls) {
                        std::unordered_set<std::string> merged;
                        for (const auto& v : thenAssigned) {
                            if (elseAssigned.count(v)) merged.insert(v);
                        }
                        curAssigned = std::move(merged);
                    } else if (thenFalls) {
                        curAssigned = std::move(thenAssigned);
                    } else if (elseFalls) {
                        curAssigned = std::move(elseAssigned);
                    }
                }
            } else if (s.kind == Stmt::Kind::Match) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
                std::vector<std::pair<std::unordered_set<std::string>, bool>> branches;
                bool hasDefault = false;
                for (const auto& mc : s.matchCases) {
                    if (mc.isDefault) hasDefault = true;
                    auto caseAssigned = curAssigned;
                    bool caseFalls = true;
                    if (mc.guard) checkExpr(*mc.guard, mc.line, caseAssigned);
                    for (const auto& ms : mc.body) {
                        checkStmt(ms, caseAssigned);
                        if (ms.kind == Stmt::Kind::Exit || ms.kind == Stmt::Kind::Give) caseFalls = false;
                    }
                    branches.push_back({caseAssigned, caseFalls});
                }
                if (!s.elseBody.empty()) {
                    hasDefault = true;
                    auto elseAssigned = curAssigned;
                    bool elseFalls = true;
                    for (const auto& es : s.elseBody) {
                        checkStmt(es, elseAssigned);
                        if (es.kind == Stmt::Kind::Exit || es.kind == Stmt::Kind::Give) elseFalls = false;
                    }
                    branches.push_back({elseAssigned, elseFalls});
                }
                if (hasDefault && !branches.empty()) {
                    bool anyFalls = false;
                    std::unordered_set<std::string> merged;
                    bool first = true;
                    for (const auto& b : branches) {
                        if (b.second) {
                            anyFalls = true;
                            if (first) {
                                merged = b.first;
                                first = false;
                            } else {
                                std::unordered_set<std::string> nextMerged;
                                for (const auto& v : merged) {
                                    if (b.first.count(v)) nextMerged.insert(v);
                                }
                                merged = std::move(nextMerged);
                            }
                        }
                    }
                    if (anyFalls) {
                        curAssigned = std::move(merged);
                    }
                }
            } else if (s.kind == Stmt::Kind::Loop) {
                if (s.expr && s.whileLoop) checkExpr(*s.expr, s.line, curAssigned);
                auto loopAssigned = curAssigned;
                for (const auto& ls : s.body) checkStmt(ls, loopAssigned);
                if (s.expr && !s.whileLoop) checkExpr(*s.expr, s.line, loopAssigned);
            } else if (s.kind == Stmt::Kind::ForIn) {
                if (s.expr) checkExpr(*s.expr, s.line, curAssigned);
                if (s.rangeEnd) checkExpr(*s.rangeEnd, s.line, curAssigned);
                auto loopAssigned = curAssigned;
                if (!s.loopVar.empty()) loopAssigned.insert(s.loopVar);
                if (!s.loopIndexVar.empty()) loopAssigned.insert(s.loopIndexVar);
                for (const auto& ls : s.body) checkStmt(ls, loopAssigned);
            } else if (s.kind == Stmt::Kind::Lock || s.kind == Stmt::Kind::UnsafeBlock) {
                for (const auto& ls : s.body) checkStmt(ls, curAssigned);
            }
        };

        for (const auto& s : f.body) {
            checkStmt(s, assigned);
        }
    }
    static std::string mangleModuleSymbol(const std::string& mod, const std::string& sym) {
        if (mod.empty() || sym == "Main") return sym;
        std::string m = lower(mod);
        for (char& ch : m) if (ch == '.') ch = '_';
        return m + "__" + sym;
    }
    std::string mangleFuncIfNeeded(const std::string& mod, const std::string& sym, bool isExtern = false) const {
        if (isExtern || mod.empty() || sym == "Main" || sym.rfind("_Handler") != std::string::npos) return sym;
        if (baremetal || mod.find('.') != std::string::npos || ambiguousFunctions.count(sym)) {
            return mangleModuleSymbol(mod, sym);
        }
        return sym;
    }
    std::string mangleGlobalIfNeeded(const std::string& mod, const std::string& sym) const {
        if (mod.empty()) return sym;
        if (baremetal || mod.find('.') != std::string::npos || ambiguousGlobals.count(sym)) {
            return mangleModuleSymbol(mod, sym);
        }
        return sym;
    }
    std::string functionModulePrefix(const Function& f) const {
        std::string mod = fileModules.count(f.sourceIndex) ? fileModules.at(f.sourceIndex) : "";
        if (mod.empty()) return "";
        if (!(baremetal || mod.find('.') != std::string::npos)) return "";
        std::string m = lower(mod);
        for (char& ch : m) if (ch == '.') ch = '_';
        return m + "__";
    }
    std::string functionLLVMName(const Function& f) const {
        if (f.isInterrupt || f.name.rfind("_Handler") != std::string::npos) {
            return f.name;
        }
        std::string mod = fileModules.count(f.sourceIndex) ? fileModules.at(f.sourceIndex) : "";
        if (!f.isExtern && !f.params.empty()) {
            const Type& p0 = f.params[0].type;
            std::string candR = (p0.kind == TypeKind::Record) ? p0.name :
                (((p0.kind == TypeKind::Pointer || p0.kind == TypeKind::SmartPointer) && p0.pointeeType && p0.pointeeType->kind == TypeKind::Record) ? p0.pointeeType->name : "");
            if (!candR.empty()) {
                auto it = recordIsTagged.find(candR);
                if (it != recordIsTagged.end() && it->second) {
                    return mangleFuncIfNeeded(mod, candR + "__" + f.name, false);
                }
            }
        }
        return mangleFuncIfNeeded(mod, f.name, f.isExtern);
    }
    std::string globalLLVMName(const VarDecl& g) const {
        std::string mod = fileModules.count(g.sourceIndex) ? fileModules.at(g.sourceIndex) : "";
        return mangleGlobalIfNeeded(mod, g.name);
    }
    std::string resolvedCallName(const Expr& e) const {
        auto it = resolvedCallKeys.find(&e);
        return it == resolvedCallKeys.end() ? e.text : it->second;
    }
    std::string resolvedGlobalName(const Expr& e) const {
        auto it = resolvedGlobalKeys.find(&e);
        return it == resolvedGlobalKeys.end() ? e.text : it->second;
    }
    bool isTaggedRecord(const std::string& name) const {
        auto it = recordIsTagged.find(name);
        return it != recordIsTagged.end() && it->second;
    }
    std::optional<std::pair<std::string, std::size_t>> virtualDispatchInfo(const Expr& e) const {
        auto it = virtualDispatchCalls.find(&e);
        if (it == virtualDispatchCalls.end()) return std::nullopt;
        return it->second;
    }
    const std::vector<std::string>* getVtableFunctions(const std::string& recordName) const {
        auto it = vtableFunctions.find(recordName);
        return it == vtableFunctions.end() ? nullptr : &it->second;
    }
    Type typeOf(const Expr& e) const {
        auto i = expressionTypes.find(&e);
        return i == expressionTypes.end() ? Type{} : i->second;
    }
    EnumValue enumValueOf(const Expr& e) const { return enumValues.at(e.text); }
    std::size_t fieldIndex(const std::string& record, const std::string& field) const {
        const auto& order = recordFieldOrder.at(record);
        const auto i = std::find(order.begin(), order.end(), field);
        if (i == order.end()) throw Error("internal error: missing record field index");
        return static_cast<std::size_t>(std::distance(order.begin(), i));
    }
    Type fieldType(const std::string& record, const std::string& field) const { return recordFieldTypes.at(record).at(field); }
    const std::vector<std::string>& fieldNames(const std::string& record) const { return recordFieldOrder.at(record); }
    Type fieldAssignmentType(const Stmt& s) const { return fieldAssignmentTypes.at(&s); }
    Type fieldAssignmentRecord(const Stmt& s) const { return fieldAssignmentRecords.at(&s); }
    Type typeOfGlobal(const std::string& name) const {
        auto it = globalTypes.find(name);
        return it == globalTypes.end() ? Type{} : it->second;
    }
    const Signature* signatureOf(const std::string& name) const {
        auto it = signatures.find(name);
        return it == signatures.end() ? nullptr : &it->second;
    }
};

std::string referenceName(const Expr& expr) {
    if (expr.kind == Expr::Kind::Variable) return expr.text;
    if (expr.kind == Expr::Kind::Member && expr.left) return referenceName(*expr.left) + "." + expr.text;
    if (expr.kind == Expr::Kind::Deref && expr.left) return referenceName(*expr.left);
    if (expr.kind == Expr::Kind::Index && expr.left) return referenceName(*expr.left);
    return {};
}
void collectGraphExpr(const Expr& expr, std::size_t source, const std::string& caller, std::ostream& out) {
    if (expr.kind == Expr::Kind::Call) {
        if (!isBuiltinIntrinsic(expr.text))
            out << "CALL\t" << source << "\t" << caller << "\t" << expr.text << "\t" << expr.line << "\n";
        if (expr.left && expr.qualifier.empty()) collectGraphExpr(*expr.left, source, caller, out);
        for (const auto& arg : expr.arguments) collectGraphExpr(*arg, source, caller, out);
        return;
    }
    const auto reference = referenceName(expr);
    if (!reference.empty()) {
        out << "READ\t" << source << "\t" << caller << "\t" << reference << "\t" << expr.line << "\n";
        return;
    }
    if (expr.left) collectGraphExpr(*expr.left, source, caller, out);
    if (expr.right) collectGraphExpr(*expr.right, source, caller, out);
    for (const auto& arg : expr.arguments) collectGraphExpr(*arg, source, caller, out);
}
void collectGraphStatements(const std::vector<Stmt>& statements, std::size_t source, const std::string& caller,
                            std::ostream& out) {
    for (const auto& stmt : statements) {
        if (stmt.kind == Stmt::Kind::Assign) {
            if (stmt.expr) collectGraphExpr(*stmt.expr, source, caller, out);
            if (stmt.name != "_") {
                const std::string target = stmt.name + (stmt.member.empty() ? "" : "." + stmt.member);
                out << "WRITE\t" << source << "\t" << caller << "\t" << target << "\t" << stmt.line << "\n";
            }
        } else if (stmt.kind == Stmt::Kind::DerefAssign || stmt.kind == Stmt::Kind::TargetAssign) {
            if (stmt.expr) collectGraphExpr(*stmt.expr, source, caller, out);
            if (stmt.target) collectGraphExpr(*stmt.target, source, caller, out);
            const std::string target = referenceName(*stmt.target);
            if (!target.empty()) out << "WRITE\t" << source << "\t" << caller << "\t" << target << "\t" << stmt.line << "\n";
        } else if (stmt.kind == Stmt::Kind::Ensure) {
            if (stmt.ensuredStmt) {
                const auto& d = *stmt.ensuredStmt;
                if (d.expr) collectGraphExpr(*d.expr, source, caller, out);
                if (d.kind == Stmt::Kind::Assign) {
                    const std::string target = d.name + (d.member.empty() ? "" : "." + d.member);
                    out << "WRITE\t" << source << "\t" << caller << "\t" << target << "\t" << d.line << "\n";
                } else if (d.kind == Stmt::Kind::DerefAssign || d.kind == Stmt::Kind::TargetAssign) {
                    if (d.target) collectGraphExpr(*d.target, source, caller, out);
                    const std::string target = referenceName(*d.target);
                    if (!target.empty()) out << "WRITE\t" << source << "\t" << caller << "\t" << target << "\t" << d.line << "\n";
                } else if (d.kind == Stmt::Kind::Write) {
                    if (d.expr) collectGraphExpr(*d.expr, source, caller, out);
                    for (const auto& arg : d.writeArgs) {
                        if (arg) collectGraphExpr(*arg, source, caller, out);
                    }
                }
            }
        } else if (stmt.kind == Stmt::Kind::Write) {
            if (stmt.expr) collectGraphExpr(*stmt.expr, source, caller, out);
            for (const auto& arg : stmt.writeArgs) {
                if (arg) collectGraphExpr(*arg, source, caller, out);
            }
        } else if (stmt.expr) collectGraphExpr(*stmt.expr, source, caller, out);
        for (const auto& mc : stmt.matchCases) {
            if (mc.pattern) collectGraphExpr(*mc.pattern, source, caller, out);
            collectGraphStatements(mc.body, source, caller, out);
        }
        collectGraphStatements(stmt.body, source, caller, out);
        collectGraphStatements(stmt.elseBody, source, caller, out);
    }
}
void writeGraphData(const std::vector<Function>& functions, const std::vector<VarDecl>& globals,
                    const std::string& path) {
    std::ofstream out(path);
    if (!out) throw Error("cannot write dependency analysis output '" + path + "'");
    for (const auto& g : globals) out << "GLOBAL\t" << g.sourceIndex << "\t" << g.name << "\t" << typeName(g.type) << "\t" << g.line << "\n";
    for (const auto& f : functions) {
        out << "BLOCK\t" << f.sourceIndex << "\t" << f.name << "\t" << f.line << "\n";
        for (const auto& p : f.params) out << "LOCAL\t" << f.sourceIndex << "\t" << f.name << "\t" << p.name << "\t" << typeName(p.type) << "\tparameter\t" << p.line << "\n";
        for (const auto& local : f.locals) out << "LOCAL\t" << f.sourceIndex << "\t" << f.name << "\t" << local.name << "\t" << typeName(local.type) << "\tlocal\t" << local.line << "\n";
    }
    for (const auto& f : functions) collectGraphStatements(f.body, f.sourceIndex, f.name, out);
    if (!out) throw Error("failed while writing dependency analysis output '" + path + "'");
}

llvm::Type* llvmType(llvm::LLVMContext& c, Type t,
                     const std::unordered_map<std::string, llvm::StructType*>* records = nullptr) {
    if (t.errorType) {
        Type baseVal = t;
        baseVal.errorType = nullptr;
        return llvm::StructType::get(c, { llvmType(c, *t.errorType, records), llvmType(c, baseVal, records) });
    }
    if (t.kind == TypeKind::Boolean)
        return llvm::Type::getInt1Ty(c);
    if (t.integer() || t.kind == TypeKind::Enum)
        return llvm::IntegerType::get(c, t.bits);
    if (t.kind == TypeKind::Float)
        return t.bits == 32 ? llvm::Type::getFloatTy(c) : llvm::Type::getDoubleTy(c);
    if (t.kind == TypeKind::Character)
        return llvm::Type::getInt32Ty(c);
    if (t.kind == TypeKind::String || t.kind == TypeKind::Pointer || t.kind == TypeKind::NullType ||
        t.kind == TypeKind::TaskHandle || t.kind == TypeKind::BlockPointer || t.kind == TypeKind::FileHandle ||
        t.kind == TypeKind::SmartPointer || t.kind == TypeKind::Channel || t.kind == TypeKind::Mutex ||
        t.kind == TypeKind::Future)
        return llvm::PointerType::getUnqual(c);
    if (t.kind == TypeKind::Atomic && t.pointeeType)
        return llvmType(c, *t.pointeeType, records);
    if (t.kind == TypeKind::Text)
        return llvm::StructType::get(c, { llvm::PointerType::getUnqual(c), llvm::Type::getInt64Ty(c), llvm::Type::getInt64Ty(c) });
    if (t.kind == TypeKind::Vector)
        return llvm::StructType::get(c, { llvm::PointerType::getUnqual(c), llvm::Type::getInt64Ty(c), llvm::Type::getInt64Ty(c), llvm::Type::getInt32Ty(c), llvm::Type::getInt32Ty(c) });
    if (t.kind == TypeKind::Array)
        return llvm::ArrayType::get(llvmType(c, *t.pointeeType, records), t.arraySize);
    if (t.kind == TypeKind::Slice)
        return llvm::StructType::get(c, { llvm::PointerType::getUnqual(c), llvm::Type::getInt64Ty(c) });
    if (t.kind == TypeKind::Tuple) {
        std::vector<llvm::Type*> elemTypes;
        for (const auto& elem : t.tupleTypes) {
            elemTypes.push_back(llvmType(c, elem, records));
        }
        return llvm::StructType::get(c, elemTypes);
    }
    if (t.kind == TypeKind::HMap)
        return llvm::PointerType::getUnqual(c);
    if (t.kind == TypeKind::Record && records) {
        auto i = records->find(t.name);
        if (i != records->end()) return i->second;
    }
    throw Error("internal error: cannot map invalid Loom type to LLVM: " + typeName(t));
}


} // namespace loom
