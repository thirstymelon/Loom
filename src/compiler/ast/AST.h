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

#include "../common/Types.h"
#include "../lexer/Token.h"

namespace loom {
struct Expr {
    enum class Kind { Integer, Floating, Boolean, String, Character, EnumVariant, Member, Variable, Call, Unary, Binary, Address, Deref, NullLiteral, Cast, Index, SliceConstructor, ArrayLiteral, RecordLiteral, SubSlice, Try, Allocate, Resize, Release, Spawn, Join, TupleLiteral, HMapLiteral, Closure, Await, Move, Copy, Borrow } kind;
    std::string text;
    std::string qualifier;
    std::string formatSpec;
    std::vector<std::string> fieldNames;
    std::uint64_t magnitude = 0;
    double floating = 0.0;
    bool boolean = false;
    int line = 1;
    Type targetType;
    std::unique_ptr<Expr> left, right;
    std::vector<std::unique_ptr<Expr>> arguments;
    std::vector<std::pair<std::string, Type>> closureParams;
    std::unique_ptr<Expr> closureBodyExpr;
    Type closureReturnType;
};

struct Stmt;

struct MatchCase {
    std::unique_ptr<Expr> pattern;
    bool isRange = false;
    bool isWildcard = false;
    bool isDefault = false;
    std::unique_ptr<Expr> rangeEnd;
    std::unique_ptr<Expr> guard;
    std::vector<Stmt> body;
    int line = 1;

    MatchCase() = default;
    MatchCase(const MatchCase&) = delete;
    MatchCase& operator=(const MatchCase&) = delete;
    MatchCase(MatchCase&&) noexcept = default;
    MatchCase& operator=(MatchCase&&) noexcept = default;
};

struct Stmt {
    enum class Kind { Assign, Call, Give, Write, Exit, Skip, If, Loop, DerefAssign, Ensure, TargetAssign, Match, Use, MultiAssign, ForIn, Lock } kind;
    std::string name;
    std::string member;
    int line = 1;
    std::unique_ptr<Expr> expr;
    std::unique_ptr<Expr> target;
    std::shared_ptr<Stmt> ensuredStmt;
    std::vector<Stmt> body;
    std::vector<Stmt> elseBody;
    bool until = false;
    bool whileLoop = false;
    std::vector<MatchCase> matchCases;
    std::vector<std::unique_ptr<Expr>> writeArgs;
    std::vector<std::string> multiTargets;
    std::string loopVar;
    std::string loopIndexVar;
    bool isRange = false;
    std::unique_ptr<Expr> rangeEnd;

    Stmt() = default;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&&) noexcept = default;
    Stmt& operator=(Stmt&&) noexcept = default;
};

inline std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    auto c = std::make_unique<Expr>();
    c->kind = e.kind;
    c->text = e.text;
    c->qualifier = e.qualifier;
    c->formatSpec = e.formatSpec;
    c->fieldNames = e.fieldNames;
    c->magnitude = e.magnitude;
    c->floating = e.floating;
    c->boolean = e.boolean;
    c->line = e.line;
    c->targetType = e.targetType;
    if (e.left) c->left = cloneExpr(*e.left);
    if (e.right) c->right = cloneExpr(*e.right);
    for (const auto& a : e.arguments) {
        c->arguments.push_back(cloneExpr(*a));
    }
    c->closureParams = e.closureParams;
    if (e.closureBodyExpr) c->closureBodyExpr = cloneExpr(*e.closureBodyExpr);
    c->closureReturnType = e.closureReturnType;
    return c;
}

inline Stmt cloneStmt(const Stmt& s);

inline MatchCase cloneMatchCase(const MatchCase& mc) {
    MatchCase res;
    if (mc.pattern) res.pattern = cloneExpr(*mc.pattern);
    res.isRange = mc.isRange;
    res.isWildcard = mc.isWildcard;
    res.isDefault = mc.isDefault;
    if (mc.rangeEnd) res.rangeEnd = cloneExpr(*mc.rangeEnd);
    if (mc.guard) res.guard = cloneExpr(*mc.guard);
    for (const auto& b : mc.body) res.body.push_back(cloneStmt(b));
    res.line = mc.line;
    return res;
}

inline Stmt cloneStmt(const Stmt& s) {
    Stmt res;
    res.kind = s.kind;
    res.name = s.name;
    res.member = s.member;
    res.line = s.line;
    if (s.expr) res.expr = cloneExpr(*s.expr);
    if (s.target) res.target = cloneExpr(*s.target);
    if (s.ensuredStmt) res.ensuredStmt = std::make_shared<Stmt>(cloneStmt(*s.ensuredStmt));
    for (const auto& b : s.body) res.body.push_back(cloneStmt(b));
    for (const auto& eb : s.elseBody) res.elseBody.push_back(cloneStmt(eb));
    res.until = s.until;
    res.whileLoop = s.whileLoop;
    for (const auto& mc : s.matchCases) res.matchCases.push_back(cloneMatchCase(mc));
    for (const auto& wa : s.writeArgs) res.writeArgs.push_back(cloneExpr(*wa));
    res.multiTargets = s.multiTargets;
    res.loopVar = s.loopVar;
    res.loopIndexVar = s.loopIndexVar;
    res.isRange = s.isRange;
    if (s.rangeEnd) res.rangeEnd = cloneExpr(*s.rangeEnd);
    return res;
}

struct EnumVariant { std::string name; std::uint64_t magnitude; bool negative; int line; };
struct EnumDecl { std::string name; Type underlying; std::vector<EnumVariant> variants; int line; std::size_t sourceIndex = 0; };
struct EnumValue { bool negative; std::uint64_t magnitude; Type type; };
struct RecordField { std::string name; Type type; int line; };
struct RecordDecl {
    std::string name;
    std::vector<RecordField> fields;
    int line = 1;
    std::size_t sourceIndex = 0;
    bool isPacked = false;
    unsigned alignment = 0;
    std::vector<std::string> typeParams;
    std::vector<Stmt> dropBody;
    bool hasDrop = false;

    RecordDecl() = default;
    RecordDecl(std::string n, std::vector<RecordField> f = {}, int l = 1, std::size_t src = 0,
               bool packed = false, unsigned align = 0, std::vector<std::string> tp = {},
               std::vector<Stmt> drop = {}, bool hasD = false)
        : name(std::move(n)), fields(std::move(f)), line(l), sourceIndex(src),
          isPacked(packed), alignment(align), typeParams(std::move(tp)),
          dropBody(std::move(drop)), hasDrop(hasD) {}
    RecordDecl(const RecordDecl& o) {
        name = o.name;
        fields = o.fields;
        line = o.line;
        sourceIndex = o.sourceIndex;
        isPacked = o.isPacked;
        alignment = o.alignment;
        typeParams = o.typeParams;
        hasDrop = o.hasDrop;
        for (const auto& s : o.dropBody) dropBody.push_back(cloneStmt(s));
    }
    RecordDecl& operator=(const RecordDecl& o) {
        if (this != &o) {
            name = o.name;
            fields = o.fields;
            line = o.line;
            sourceIndex = o.sourceIndex;
            isPacked = o.isPacked;
            alignment = o.alignment;
            typeParams = o.typeParams;
            hasDrop = o.hasDrop;
            dropBody.clear();
            for (const auto& s : o.dropBody) dropBody.push_back(cloneStmt(s));
        }
        return *this;
    }
    RecordDecl(RecordDecl&&) noexcept = default;
    RecordDecl& operator=(RecordDecl&&) noexcept = default;
};
struct VarDecl {
    std::string name;
    Type type;
    int line = 1;
    bool constant = false;
    bool isRef = false;
    bool isMut = false;
    std::unique_ptr<Expr> initializer;
    std::size_t sourceIndex = 0;

    VarDecl() = default;
    VarDecl(const VarDecl&) = delete;
    VarDecl& operator=(const VarDecl&) = delete;
    VarDecl(VarDecl&&) noexcept = default;
    VarDecl& operator=(VarDecl&&) noexcept = default;
    VarDecl(std::string n, Type t, int l, bool c = false, std::unique_ptr<Expr> init = nullptr, std::size_t src = 0, bool r = false, bool m = false)
        : name(std::move(n)), type(std::move(t)), line(l), constant(c), isRef(r), isMut(m), initializer(std::move(init)), sourceIndex(src) {}
};

inline VarDecl cloneVarDecl(const VarDecl& v) {
    VarDecl res;
    res.name = v.name;
    res.type = v.type;
    res.line = v.line;
    res.constant = v.constant;
    res.isRef = v.isRef;
    res.isMut = v.isMut;
    if (v.initializer) res.initializer = cloneExpr(*v.initializer);
    res.sourceIndex = v.sourceIndex;
    return res;
}

struct Function {
    std::string name;
    int line = 1;
    std::vector<VarDecl> params, locals;
    std::optional<Type> result;
    std::optional<Type> errorResult;
    std::vector<Stmt> body;
    std::vector<Token> uses;
    std::size_t sourceIndex = 0;
    bool isExtern = false;
    bool isConcise = false;
    bool isTest = false;
    std::string testDescription;
    std::vector<std::string> typeParams;
    bool isOperator = false;
    std::string operatorSymbol;
    bool isAsync = false;

    Function() = default;
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;
    Function(Function&&) noexcept = default;
    Function& operator=(Function&&) noexcept = default;
};

inline Function cloneFunction(const Function& fn) {
    Function res;
    res.name = fn.name;
    res.line = fn.line;
    for (const auto& p : fn.params) res.params.push_back(cloneVarDecl(p));
    for (const auto& l : fn.locals) res.locals.push_back(cloneVarDecl(l));
    res.result = fn.result;
    res.errorResult = fn.errorResult;
    res.uses = fn.uses;
    for (const auto& b : fn.body) res.body.push_back(cloneStmt(b));
    res.sourceIndex = fn.sourceIndex;
    res.isExtern = fn.isExtern;
    res.isConcise = fn.isConcise;
    res.isTest = fn.isTest;
    res.testDescription = fn.testDescription;
    res.typeParams = fn.typeParams;
    res.isOperator = fn.isOperator;
    res.operatorSymbol = fn.operatorSymbol;
    res.isAsync = fn.isAsync;
    return res;
}
struct Signature {
    std::vector<Type> parameters;
    std::optional<Type> result;
    std::optional<Type> errorResult;
    std::vector<std::string> typeParams;
    bool isAsync = false;
};
std::optional<bool> constantBoolean(const Expr& e) {
    if (e.kind == Expr::Kind::Boolean)
        return e.boolean;
    if (e.kind == Expr::Kind::Unary && lower(e.text) == "not") {
        auto value = constantBoolean(*e.left);
        if (value)
            return !*value;
    }
    if (e.kind == Expr::Kind::Binary) {
        const std::string op = lower(e.text);
        auto left = constantBoolean(*e.left);
        auto right = constantBoolean(*e.right);
        if (left && right) {
            if (op == "and")
                return *left && *right;
            if (op == "or")
                return *left || *right;
            if (op == "==")
                return *left == *right;
            if (op == "!=")
                return *left != *right;
        }
    }
    return std::nullopt;
}

inline bool isCompoundAssign(K k) {
    return k == K::PlusAssign || k == K::MinusAssign || k == K::StarAssign ||
           k == K::SlashAssign || k == K::AmpersandAssign || k == K::PipeAssign ||
           k == K::CaretAssign;
}

inline Token compoundToBinaryToken(const Token& op) {
    K bk = K::Plus;
    std::string text = "+";
    switch (op.kind) {
        case K::PlusAssign: bk = K::Plus; text = "+"; break;
        case K::MinusAssign: bk = K::Minus; text = "-"; break;
        case K::StarAssign: bk = K::Star; text = "*"; break;
        case K::SlashAssign: bk = K::Slash; text = "/"; break;
        case K::AmpersandAssign: bk = K::Ampersand; text = "&"; break;
        case K::PipeAssign: bk = K::Pipe; text = "|"; break;
        case K::CaretAssign: bk = K::Caret; text = "^"; break;
        default: break;
    }
    Token res{bk, text, op.line, 0, currentSourceIndex};
    res.column = op.column;
    return res;
}


} // namespace loom
