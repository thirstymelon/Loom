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

#include "../sema/Analyzer.h"
#include "../codegen/TargetConfig.h"

namespace loom {
enum class ConstKind {
    None,
    Integer,
    Floating,
    Boolean,
    Character,
    String,
    Record,
    Array
};

struct ConstVal {
    ConstKind kind = ConstKind::None;
    Type type;
    std::uint64_t intVal = 0;
    bool isNegative = false;
    double floatVal = 0.0;
    bool boolVal = false;
    std::string strVal;
    std::map<std::string, ConstVal> fields;
    std::vector<ConstVal> elements;

    bool isKnown() const { return kind != ConstKind::None; }

    int64_t signedVal() const {
        if (!isNegative) return static_cast<int64_t>(intVal);
        if (intVal == (std::uint64_t(1) << 63)) return std::numeric_limits<int64_t>::min();
        return -static_cast<int64_t>(intVal);
    }
    uint64_t unsignedVal() const { return intVal; }

    static ConstVal makeInt(std::uint64_t val, bool neg, Type t) {
        ConstVal c; c.kind = ConstKind::Integer; c.intVal = val; c.isNegative = neg; c.type = t; return c;
    }
    static ConstVal makeFloat(double val, Type t) {
        ConstVal c; c.kind = ConstKind::Floating; c.floatVal = val; c.type = t; return c;
    }
    static ConstVal makeBool(bool val) {
        ConstVal c; c.kind = ConstKind::Boolean; c.boolVal = val; c.type = Type::boolean(); return c;
    }
    static ConstVal makeChar(std::uint64_t scalar) {
        ConstVal c; c.kind = ConstKind::Character; c.intVal = scalar; c.type = Type::character(); return c;
    }
    static ConstVal makeString(const std::string& s) {
        ConstVal c; c.kind = ConstKind::String; c.strVal = s; c.type = Type::string(); return c;
    }
};

std::string valToString(const ConstVal& v) {
    if (v.kind == ConstKind::Integer) {
        return (v.isNegative ? "-" : "") + std::to_string(v.intVal);
    }
    if (v.kind == ConstKind::Floating) {
        std::ostringstream ss; ss << v.floatVal; return ss.str();
    }
    if (v.kind == ConstKind::Boolean) {
        return v.boolVal ? "True" : "False";
    }
    if (v.kind == ConstKind::Character) {
        return "'" + std::to_string(v.intVal) + "'";
    }
    if (v.kind == ConstKind::String) {
        return "\"" + v.strVal + "\"";
    }
    return "<const>";
}

ConstVal defaultVal(Type t) {
    if (t.kind == TypeKind::Boolean) return ConstVal::makeBool(false);
    if (t.kind == TypeKind::Float) return ConstVal::makeFloat(0.0, t);
    if (t.kind == TypeKind::Character) return ConstVal::makeChar(0);
    if (t.integer() || t.kind == TypeKind::Enum) return ConstVal::makeInt(0, false, t);
    if (t.kind == TypeKind::String) return ConstVal::makeString("");
    ConstVal c; c.kind = ConstKind::None; c.type = t; return c;
}

class ConstEvaluator {
public:
    static ConstVal signedBits(std::uint64_t bits, Type type) {
        const unsigned width = type.bits;
        const std::uint64_t mask = width >= 64 ? UINT64_MAX : ((std::uint64_t(1) << width) - 1);
        bits &= mask;
        const std::uint64_t sign = std::uint64_t(1) << (width - 1);
        if (bits & sign) return ConstVal::makeInt((std::uint64_t(0) - bits) & mask, true, type);
        return ConstVal::makeInt(bits, false, type);
    }

    const Analyzer& analyzer;
    const std::vector<Function>& functions;
    const std::vector<VarDecl>& globals;
    const std::vector<RecordDecl>& records;
    OptReport* report = nullptr;

    std::unordered_map<std::string, const Function*> functionMap;
    std::unordered_map<std::string, const VarDecl*> globalMap;
    std::unordered_map<std::string, const RecordDecl*> recordMap;
    std::unordered_set<std::string> pureFunctions;

    ConstEvaluator(const Analyzer& a,
                   const std::vector<Function>& fs,
                   const std::vector<VarDecl>& gs,
                   const std::vector<RecordDecl>& rs,
                   OptReport* rep = nullptr)
        : analyzer(a), functions(fs), globals(gs), records(rs), report(rep) {
        std::unordered_set<std::string> dupFuncs, dupGlobals;
        for (const auto& f : functions) {
            if (!functionMap.emplace(f.name, &f).second) dupFuncs.insert(f.name);
        }
        for (const auto& df : dupFuncs) functionMap.erase(df);
        for (const auto& g : globals) {
            if (!globalMap.emplace(g.name, &g).second) dupGlobals.insert(g.name);
        }
        for (const auto& dg : dupGlobals) globalMap.erase(dg);
        for (const auto& r : records) recordMap.emplace(r.name, &r);
        analyzePurity();
    }

    bool isPureStmt(const Stmt& s, const std::unordered_set<std::string>& pureSet) const {
        if (s.kind == Stmt::Kind::Write) return false;
        if (s.kind == Stmt::Kind::Ensure || s.kind == Stmt::Kind::DerefAssign ||
            s.kind == Stmt::Kind::TargetAssign) return false;
        if (s.kind == Stmt::Kind::Assign && globalMap.count(s.name)) return false;
        if (s.kind == Stmt::Kind::Call) {
            if (!pureSet.count(s.name)) return false;
        }
        if (s.expr && !isPureExpr(*s.expr, pureSet)) return false;
        if (s.target && !isPureExpr(*s.target, pureSet)) return false;
        for (const auto& child : s.body) {
            if (!isPureStmt(child, pureSet)) return false;
        }
        for (const auto& child : s.elseBody) {
            if (!isPureStmt(child, pureSet)) return false;
        }
        for (const auto& mc : s.matchCases) {
            if (mc.pattern && !isPureExpr(*mc.pattern, pureSet)) return false;
            for (const auto& child : mc.body) {
                if (!isPureStmt(child, pureSet)) return false;
            }
        }
        return true;
    }

    bool isPureExpr(const Expr& e, const std::unordered_set<std::string>& pureSet) const {
        if (e.kind == Expr::Kind::Call) {
            if (!pureSet.count(e.text)) return false;
        }
        if (e.left && !isPureExpr(*e.left, pureSet)) return false;
        if (e.right && !isPureExpr(*e.right, pureSet)) return false;
        for (const auto& arg : e.arguments) {
            if (!isPureExpr(*arg, pureSet)) return false;
        }
        return true;
    }

    void analyzePurity() {
        for (const auto& f : functions) {
            if (!f.isExtern && f.result.has_value()) {
                pureFunctions.insert(f.name);
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = pureFunctions.begin(); it != pureFunctions.end(); ) {
                auto fit = functionMap.find(*it);
                if (fit == functionMap.end()) {
                    it = pureFunctions.erase(it);
                    changed = true;
                    continue;
                }
                const auto* f = fit->second;
                bool pure = true;
                for (const auto& s : f->body) {
                    if (!isPureStmt(s, pureFunctions)) { pure = false; break; }
                }
                if (!pure) {
                    it = pureFunctions.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
    }

    bool canEvalCall(const std::string& name) const {
        return pureFunctions.count(name) != 0;
    }

    ConstVal evalExpr(const Expr& e,
                      const std::unordered_map<std::string, ConstVal>& env = {},
                      unsigned depth = 0,
                      unsigned& steps = dummySteps) {
        if (++steps > 100000 || depth > 256) return {};

        switch (e.kind) {
            case Expr::Kind::Integer: {
                Type t = analyzer.typeOf(e);
                return ConstVal::makeInt(e.magnitude, false, t);
            }
            case Expr::Kind::Floating: {
                Type t = analyzer.typeOf(e);
                return ConstVal::makeFloat(e.floating, t);
            }
            case Expr::Kind::Boolean: {
                return ConstVal::makeBool(e.boolean);
            }
            case Expr::Kind::Character: {
                return ConstVal::makeChar(decodeUtf8Scalar(e.text, e.line));
            }
            case Expr::Kind::String: {
                return ConstVal::makeString(e.text);
            }
            case Expr::Kind::EnumVariant: {
                const auto value = analyzer.enumValueOf(e);
                return ConstVal::makeInt(value.magnitude, value.negative, value.type);
            }
            case Expr::Kind::Variable: {
                auto it = env.find(e.text);
                if (it != env.end()) return it->second;
                auto gIt = globalMap.find(e.text);
                if (gIt != globalMap.end() && gIt->second->constant && gIt->second->initializer) {
                    return evalExpr(*gIt->second->initializer, {}, depth + 1, steps);
                }
                return {};
            }
            case Expr::Kind::Unary: {
                if (!e.left) return {};
                auto a = evalExpr(*e.left, env, depth + 1, steps);
                if (!a.isKnown()) return {};
                if (e.text == "-") {
                    if (a.kind == ConstKind::Floating) return ConstVal::makeFloat(-a.floatVal, a.type);
                    if (a.kind == ConstKind::Integer && a.type.kind == TypeKind::Signed)
                        return signedBits(std::uint64_t(0) - (a.isNegative ? std::uint64_t(0) - a.intVal : a.intVal), a.type);
                    if (a.kind == ConstKind::Integer && a.type.kind == TypeKind::Unsigned) {
                        const auto mask = a.type.bits >= 64 ? UINT64_MAX : ((std::uint64_t(1) << a.type.bits) - 1);
                        return ConstVal::makeInt((std::uint64_t(0) - a.intVal) & mask, false, a.type);
                    }
                } else if (e.text == "~") {
                    if (a.kind == ConstKind::Integer && a.type.kind == TypeKind::Signed) {
                        const auto bits = a.isNegative ? std::uint64_t(0) - a.intVal : a.intVal;
                        return signedBits(~bits, a.type);
                    }
                    if (a.kind == ConstKind::Integer && a.type.kind == TypeKind::Unsigned) {
                        const auto mask = a.type.bits >= 64 ? UINT64_MAX : ((std::uint64_t(1) << a.type.bits) - 1);
                        return ConstVal::makeInt((~a.intVal) & mask, false, a.type);
                    }
                } else if (lower(e.text) == "not") {
                    if (a.kind == ConstKind::Boolean) return ConstVal::makeBool(!a.boolVal);
                }
                return {};
            }
            case Expr::Kind::Binary: {
                if (!e.left || !e.right) return {};
                auto a = evalExpr(*e.left, env, depth + 1, steps);
                if (!a.isKnown()) return {};
                auto b = evalExpr(*e.right, env, depth + 1, steps);
                if (!b.isKnown()) return {};
                const std::string op = lower(e.text);
                Type t = analyzer.typeOf(*e.left);
                if (t.kind == TypeKind::Float) {
                    // Host double evaluation is not a faithful evaluator for Float_32 rounding.
                    if (t.bits == 32) return {};
                    double f1 = a.floatVal, f2 = b.floatVal;
                    if (op == "+") return ConstVal::makeFloat(f1 + f2, t);
                    if (op == "-") return ConstVal::makeFloat(f1 - f2, t);
                    if (op == "*") return ConstVal::makeFloat(f1 * f2, t);
                    if (op == "/") { if (f2 == 0.0) return {}; return ConstVal::makeFloat(f1 / f2, t); }
                    if (op == "==") return ConstVal::makeBool(f1 == f2);
                    if (op == "!=") return ConstVal::makeBool(f1 != f2);
                    if (op == "<") return ConstVal::makeBool(f1 < f2);
                    if (op == "<=") return ConstVal::makeBool(f1 <= f2);
                    if (op == ">") return ConstVal::makeBool(f1 > f2);
                    if (op == ">=") return ConstVal::makeBool(f1 >= f2);
                    return {};
                }
                if (t.kind == TypeKind::Boolean) {
                    if (op == "and") return ConstVal::makeBool(a.boolVal && b.boolVal);
                    if (op == "or") return ConstVal::makeBool(a.boolVal || b.boolVal);
                    if (op == "==") return ConstVal::makeBool(a.boolVal == b.boolVal);
                    if (op == "!=") return ConstVal::makeBool(a.boolVal != b.boolVal);
                    return {};
                }
                if (t.kind == TypeKind::Signed) {
                    int64_t v1 = a.signedVal(), v2 = b.signedVal();
                    const unsigned width = t.bits;
                    const std::uint64_t mask = width >= 64 ? UINT64_MAX : ((std::uint64_t(1) << width) - 1);
                    const std::uint64_t x = (a.isNegative ? std::uint64_t(0) - a.intVal : a.intVal) & mask;
                    const std::uint64_t y = (b.isNegative ? std::uint64_t(0) - b.intVal : b.intVal) & mask;
                    if (op == "+") return signedBits(x + y, t);
                    if (op == "-") return signedBits(x - y, t);
                    if (op == "*") return signedBits(x * y, t);
                    if (op == "/") {
                        const int64_t signedMin = width == 64 ? std::numeric_limits<int64_t>::min() : -(int64_t(1) << (width - 1));
                        if (v2 == 0 || (v1 == signedMin && v2 == -1)) return {};
                        int64_t r = v1 / v2;
                        return ConstVal::makeInt(r < 0 ? std::uint64_t(0) - static_cast<std::uint64_t>(r) : static_cast<std::uint64_t>(r), r < 0, t);
                    }
                    if (op == "mod" || op == "%") {
                        const int64_t signedMin = width == 64 ? std::numeric_limits<int64_t>::min() : -(int64_t(1) << (width - 1));
                        if (v2 == 0 || (v1 == signedMin && v2 == -1)) return {};
                        int64_t r = v1 % v2;
                        return ConstVal::makeInt(r < 0 ? std::uint64_t(0) - static_cast<std::uint64_t>(r) : static_cast<std::uint64_t>(r), r < 0, t);
                    }
                    if (op == "&") return signedBits(x & y, t);
                    if (op == "|") return signedBits(x | y, t);
                    if (op == "^") return signedBits(x ^ y, t);
                    if (op == "lsl" || op == "lsr") {
                        if (v2 < 0 || static_cast<std::uint64_t>(v2) >= width) return {};
                        const unsigned shift = static_cast<unsigned>(v2);
                        if (op == "lsl") return signedBits(x << shift, t);
                        if (shift == 0) return signedBits(x, t);
                        std::uint64_t shifted = x >> shift;
                        if (v1 < 0) shifted |= (mask << (width - shift)) & mask;
                        return signedBits(shifted, t);
                    }
                    if (op == "==") return ConstVal::makeBool(v1 == v2);
                    if (op == "!=") return ConstVal::makeBool(v1 != v2);
                    if (op == "<") return ConstVal::makeBool(v1 < v2);
                    if (op == "<=") return ConstVal::makeBool(v1 <= v2);
                    if (op == ">") return ConstVal::makeBool(v1 > v2);
                    if (op == ">=") return ConstVal::makeBool(v1 >= v2);
                    return {};
                }
                if (t.kind == TypeKind::Unsigned || t.kind == TypeKind::Character || t.kind == TypeKind::Enum) {
                    uint64_t u1 = a.unsignedVal(), u2 = b.unsignedVal();
                    const unsigned width = t.bits;
                    const std::uint64_t mask = width >= 64 ? UINT64_MAX : ((std::uint64_t(1) << width) - 1);
                    if (op == "+") return ConstVal::makeInt((u1 + u2) & mask, false, t);
                    if (op == "-") return ConstVal::makeInt((u1 - u2) & mask, false, t);
                    if (op == "*") return ConstVal::makeInt((u1 * u2) & mask, false, t);
                    if (op == "/") { if (u2 == 0) return {}; return ConstVal::makeInt(u1 / u2, false, t); }
                    if (op == "mod" || op == "%") { if (u2 == 0) return {}; return ConstVal::makeInt(u1 % u2, false, t); }
                    if (op == "&") return ConstVal::makeInt(u1 & u2, false, t);
                    if (op == "|") return ConstVal::makeInt(u1 | u2, false, t);
                    if (op == "^") return ConstVal::makeInt(u1 ^ u2, false, t);
                    if (op == "lsl" || op == "lsr") {
                        if (u2 >= t.bits) return {};
                        return ConstVal::makeInt(op == "lsl" ? (u1 << u2) : (u1 >> u2), false, t);
                    }
                    if (op == "==") return ConstVal::makeBool(u1 == u2);
                    if (op == "!=") return ConstVal::makeBool(u1 != u2);
                    if (op == "<") return ConstVal::makeBool(u1 < u2);
                    if (op == "<=") return ConstVal::makeBool(u1 <= u2);
                    if (op == ">") return ConstVal::makeBool(u1 > u2);
                    if (op == ">=") return ConstVal::makeBool(u1 >= u2);
                    return {};
                }
                return {};
            }
            case Expr::Kind::Cast: {
                if (!e.left) return {};
                auto a = evalExpr(*e.left, env, depth + 1, steps);
                if (!a.isKnown()) return {};
                Type dst = e.targetType;
                if (a.kind == ConstKind::Integer && dst.integer()) {
                    const std::uint64_t raw = a.isNegative ? std::uint64_t(0) - a.intVal : a.intVal;
                    const std::uint64_t mask = dst.bits >= 64 ? UINT64_MAX : ((std::uint64_t(1) << dst.bits) - 1);
                    const std::uint64_t narrowed = raw & mask;
                    if (dst.kind == TypeKind::Signed) return signedBits(narrowed, dst);
                    return ConstVal::makeInt(narrowed, false, dst);
                }
                if (a.kind == ConstKind::Integer && dst.kind == TypeKind::Character) {
                    const bool signedSource = a.type.kind == TypeKind::Signed ||
                                              (a.type.kind == TypeKind::Enum && a.type.enumSigned);
                    if (signedSource && a.isNegative)
                        throw Error("line " + std::to_string(e.line) + ": integer cast is not a valid Unicode scalar value");
                    const std::uint64_t scalar = a.isNegative ? std::uint64_t(0) - a.intVal : a.intVal;
                    if (!isUnicodeScalar(scalar))
                        throw Error("line " + std::to_string(e.line) + ": integer cast is not a valid Unicode scalar value");
                    return ConstVal::makeChar(scalar);
                }
                if (a.kind == ConstKind::Integer && dst.kind == TypeKind::Float) {
                    if (dst.bits == 32) return {};
                    const bool signedSource = a.type.kind == TypeKind::Signed || (a.type.kind == TypeKind::Enum && a.type.enumSigned);
                    return ConstVal::makeFloat(signedSource ? static_cast<double>(a.signedVal()) : static_cast<double>(a.unsignedVal()), dst);
                }
                if (a.kind == ConstKind::Floating && dst.integer()) return {};
                if (a.kind == ConstKind::Boolean && dst.integer()) {
                    return ConstVal::makeInt(a.boolVal ? 1 : 0, false, dst);
                }
                if (a.kind == ConstKind::Integer && dst.kind == TypeKind::Boolean) {
                    return ConstVal::makeBool(a.intVal != 0);
                }
                return {};
            }
            case Expr::Kind::Member: {
                if (!e.left) return {};
                Type baseType = analyzer.typeOf(*e.left);
                if (baseType.kind == TypeKind::Array && e.text == "Length") {
                    return ConstVal::makeInt(baseType.arraySize, false, Type::unsignedInt(64));
                }
                if (baseType.kind == TypeKind::String && e.text == "Length") {
                    auto a = evalExpr(*e.left, env, depth + 1, steps);
                    if (a.isKnown() && a.kind == ConstKind::String) {
                        const auto end = a.strVal.find('\0');
                        return ConstVal::makeInt(end == std::string::npos ? a.strVal.size() : end,
                                                 false, Type::unsignedInt(64));
                    }
                }
                auto baseVal = evalExpr(*e.left, env, depth + 1, steps);
                if (baseVal.isKnown() && baseVal.kind == ConstKind::Record) {
                    auto fIt = baseVal.fields.find(e.text);
                    if (fIt != baseVal.fields.end()) return fIt->second;
                }
                return {};
            }
            case Expr::Kind::Index: {
                if (!e.left || !e.right) return {};
                auto baseVal = evalExpr(*e.left, env, depth + 1, steps);
                auto idxVal = evalExpr(*e.right, env, depth + 1, steps);
                if (baseVal.isKnown() && idxVal.isKnown()) {
                    int64_t idx = idxVal.signedVal();
                    if (baseVal.kind == ConstKind::Array && idx >= 1 && static_cast<size_t>(idx) <= baseVal.elements.size()) {
                        return baseVal.elements[static_cast<size_t>(idx - 1)];
                    }
                    const auto stringEnd = baseVal.kind == ConstKind::String ? baseVal.strVal.find('\0') : std::string::npos;
                    const std::size_t visibleStringLength = stringEnd == std::string::npos ? baseVal.strVal.size() : stringEnd;
                    if (baseVal.kind == ConstKind::String && idx >= 1 && static_cast<size_t>(idx) <= visibleStringLength) {
                        return ConstVal::makeInt(static_cast<unsigned char>(baseVal.strVal[static_cast<size_t>(idx - 1)]),
                                                 false, Type::unsignedInt(8));
                    }
                }
                return {};
            }
            case Expr::Kind::Call: {
                if (!pureFunctions.count(e.text)) return {};
                std::vector<ConstVal> args;
                for (const auto& arg : e.arguments) {
                    auto argVal = evalExpr(*arg, env, depth + 1, steps);
                    if (!argVal.isKnown()) return {};
                    args.push_back(argVal);
                }
                return evalCall(e.text, args, depth + 1, steps);
            }
            case Expr::Kind::ArrayLiteral: {
                ConstVal res;
                res.kind = ConstKind::Array;
                res.type = analyzer.typeOf(e);
                for (const auto& arg : e.arguments) {
                    auto el = evalExpr(*arg, env, depth + 1, steps);
                    if (!el.isKnown()) return {};
                    res.elements.push_back(el);
                }
                return res;
            }
            case Expr::Kind::RecordLiteral: {
                ConstVal res;
                res.kind = ConstKind::Record;
                res.type = analyzer.typeOf(e);
                for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                    auto el = evalExpr(*e.arguments[i], env, depth + 1, steps);
                    if (!el.isKnown()) return {};
                    res.fields[e.fieldNames[i]] = el;
                }
                return res;
            }
            default:
                return {};
        }
    }

    ConstVal evalCall(const std::string& name,
                      const std::vector<ConstVal>& args,
                      unsigned depth,
                      unsigned& steps) {
        if (depth > 256 || ++steps > 100000) return {};
        auto it = functionMap.find(name);
        if (it == functionMap.end()) return {};
        const Function* f = it->second;
        if (f->isExtern || !f->result) return {};
        if (args.size() != f->params.size()) return {};

        std::unordered_map<std::string, ConstVal> localEnv;
        for (std::size_t i = 0; i < args.size(); ++i) {
            localEnv[f->params[i].name] = args[i];
        }
        for (const auto& local : f->locals) {
            if (local.initializer) {
                auto initVal = evalExpr(*local.initializer, localEnv, depth + 1, steps);
                localEnv[local.name] = initVal.isKnown() ? initVal : defaultVal(local.type);
            } else {
                localEnv[local.name] = defaultVal(local.type);
            }
        }

        struct EvalResult {
            bool hasReturned = false;
            bool hasExited = false;
            ConstVal value;
        };

        std::function<EvalResult(const std::vector<Stmt>&)> evalStmts =
            [&](const std::vector<Stmt>& stmts) -> EvalResult {
            for (const auto& s : stmts) {
                if (++steps > 100000) return {true, false, ConstVal{}};
                if (s.kind == Stmt::Kind::Assign) {
                    if (s.member.empty()) {
                        auto val = evalExpr(*s.expr, localEnv, depth + 1, steps);
                        if (!val.isKnown()) return {true, false, ConstVal{}};
                        localEnv[s.name] = val;
                    } else {
                        auto it = localEnv.find(s.name);
                        if (it == localEnv.end() || it->second.kind != ConstKind::Record)
                            return {true, false, ConstVal{}};
                        auto val = evalExpr(*s.expr, localEnv, depth + 1, steps);
                        if (!val.isKnown()) return {true, false, ConstVal{}};
                        it->second.fields[s.member] = val;
                    }
                } else if (s.kind == Stmt::Kind::Give) {
                    auto val = evalExpr(*s.expr, localEnv, depth + 1, steps);
                    return {true, false, val};
                } else if (s.kind == Stmt::Kind::Use) {
                    continue;
                } else if (s.kind == Stmt::Kind::Exit) {
                    return {false, true, ConstVal{}};
                } else if (s.kind == Stmt::Kind::Skip) {
                    break;
                } else if (s.kind == Stmt::Kind::If) {
                    auto cond = evalExpr(*s.expr, localEnv, depth + 1, steps);
                    if (!cond.isKnown()) return {true, false, ConstVal{}};
                    if (cond.boolVal) {
                        auto res = evalStmts(s.body);
                        if (res.hasReturned || res.hasExited) return res;
                    } else {
                        auto res = evalStmts(s.elseBody);
                        if (res.hasReturned || res.hasExited) return res;
                    }
                } else if (s.kind == Stmt::Kind::Loop) {
                    while (true) {
                        if (++steps > 100000) return {true, false, ConstVal{}};
                        if (s.until) {
                            auto cond = evalExpr(*s.expr, localEnv, depth + 1, steps);
                            if (!cond.isKnown()) return {true, false, ConstVal{}};
                            if (cond.boolVal) break;
                        } else if (s.whileLoop) {
                            auto cond = evalExpr(*s.expr, localEnv, depth + 1, steps);
                            if (!cond.isKnown()) return {true, false, ConstVal{}};
                            if (!cond.boolVal) break;
                        }
                        auto res = evalStmts(s.body);
                        if (res.hasReturned) return res;
                        if (res.hasExited) break;
                    }
                } else if (s.kind == Stmt::Kind::Match) {
                    auto cond = evalExpr(*s.expr, localEnv, depth + 1, steps);
                    if (!cond.isKnown()) return {true, false, ConstVal{}};
                    bool matched = false;
                    for (const auto& mc : s.matchCases) {
                        auto pat = evalExpr(*mc.pattern, localEnv, depth + 1, steps);
                        if (pat.isKnown() && pat.kind == cond.kind && pat.intVal == cond.intVal && pat.isNegative == cond.isNegative) {
                            matched = true;
                            auto res = evalStmts(mc.body);
                            if (res.hasReturned || res.hasExited) return res;
                            break;
                        }
                    }
                    if (!matched && !s.elseBody.empty()) {
                        auto res = evalStmts(s.elseBody);
                        if (res.hasReturned || res.hasExited) return res;
                    }
                } else {
                    return {true, false, ConstVal{}};
                }
            }
            return {false, false, ConstVal{}};
        };

        auto res = evalStmts(f->body);
        if (res.hasReturned && res.value.isKnown()) {
            return res.value;
        }
        return ConstVal{};
    }

private:
    static unsigned dummySteps;
};
inline unsigned ConstEvaluator::dummySteps = 0;


} // namespace loom
