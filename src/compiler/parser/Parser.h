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

#include "../lexer/Lexer.h"
#include "../ast/AST.h"

namespace loom {
class Parser {
    std::vector<Token> ts;
    std::size_t pos = 0;
    std::vector<VarDecl> globalDeclarations;
    std::vector<EnumDecl> enumDeclarations;
    std::unordered_map<std::string, Type> enumTypes;
    std::vector<RecordDecl> recordDeclarations;
    std::unordered_map<std::string, Type> recordTypes;
    std::unordered_map<std::string, RecordDecl> genericRecordTemplates;
    std::unordered_map<std::string, Function> genericFunctionTemplates;
    std::unordered_set<std::string> instantiatedGenericFunctions;
    std::vector<Function> pendingInstantiatedFunctions;
    std::unordered_set<std::string> currentTypeParams;
    int testCounter = 0;
    std::unordered_map<std::size_t, std::string> fileModules;
    std::unordered_map<std::size_t, std::unordered_set<std::string>> fileImports;
    std::unordered_map<std::size_t, std::unordered_set<std::string>> fileUses;
    const Token& t() const {
        currentSourceIndex = ts[pos].sourceIndex;
        return ts[pos];
    }
    Token pop() {
        currentSourceIndex = ts[pos].sourceIndex;
        return ts[pos++];
    }
    bool at(K k) const {
        return t().kind == k;
    }
    Token peek(std::size_t offset = 1) const {
        if (pos + offset < ts.size()) return ts[pos + offset];
        return ts.empty() ? Token{K::Eof, "", 1, 0, currentSourceIndex} : ts.back();
    }
    bool eat(K k) {
        if (at(k)) {
            ++pos;
            return true;
        }
        return false;
    }
    [[noreturn]] void fail(const std::string& m) const {
        throw Error(m, {t().sourceIndex, t().line, t().column, static_cast<int>(t().length)});
    }
    Token expect(K k, const std::string& m) {
        if (!at(k))
            fail(m);
        return pop();
    }
    void newlines() {
        while (eat(K::Newline)) {
        }
    }
    void requireLineEnd(const std::string& context) {
        if (!at(K::Newline) && !at(K::Eof))
            fail("expected a newline after " + context);
        newlines();
    }
    std::unique_ptr<Expr> make(Expr::Kind k, const Token& op, std::unique_ptr<Expr> a = {},
                               std::unique_ptr<Expr> b = {}) {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->text = op.text;
        e->line = op.line;
        e->left = std::move(a);
        e->right = std::move(b);
        return e;
    }
    int precedence(K k) const {
        switch (k) {
            case K::Or:
                return 1;
            case K::And:
                return 2;
            case K::Pipe:
                return 3;
            case K::Caret:
                return 4;
            case K::Ampersand:
                return 5;
            case K::Equal:
            case K::NotEqual:
            case K::Less:
            case K::LessEqual:
            case K::Greater:
            case K::GreaterEqual:
                return 6;
            case K::LSL:
            case K::LSR:
                return 7;
            case K::Plus:
            case K::Minus:
                return 8;
            case K::Star:
            case K::Slash:
            case K::Mod:
            case K::Percent:
                return 9;
            case K::Pow:
                return 10;
            default:
                return 0;
        }
    }
    Type resolveType(const Token& token) const {
        if (currentTypeParams.count(token.text)) {
            Type t;
            t.kind = TypeKind::Record;
            t.name = token.text;
            return t;
        }
        auto known = enumTypes.find(token.text);
        if (known != enumTypes.end()) return known->second;
        auto record = recordTypes.find(token.text);
        return record == recordTypes.end() ? parseType(token.text, token.line) : record->second;
    }
    Type instantiateGenericRecord(const std::string& templateName, const std::vector<Type>& typeArgs, int line) {
        auto it = genericRecordTemplates.find(templateName);
        if (it == genericRecordTemplates.end()) {
            throw Error("line " + std::to_string(line) + ": '" + templateName + "' is not a generic record");
        }
        const RecordDecl& tmpl = it->second;
        if (tmpl.typeParams.size() != typeArgs.size()) {
            throw Error("line " + std::to_string(line) + ": generic record '" + templateName + "' expects " +
                        std::to_string(tmpl.typeParams.size()) + " type arguments, got " + std::to_string(typeArgs.size()));
        }
        std::string monoName = templateName;
        for (const auto& arg : typeArgs) monoName += "__" + typeName(arg);
        if (recordTypes.count(monoName)) {
            return recordTypes.at(monoName);
        }
        RecordDecl monoDecl = tmpl;
        monoDecl.name = monoName;
        monoDecl.typeParams.clear();
        for (auto& field : monoDecl.fields) {
            for (std::size_t i = 0; i < tmpl.typeParams.size(); ++i) {
                if (field.type.name == tmpl.typeParams[i]) {
                    field.type = typeArgs[i];
                } else if (field.type.pointeeType && field.type.pointeeType->name == tmpl.typeParams[i]) {
                    field.type.pointeeType = std::make_shared<Type>(typeArgs[i]);
                }
            }
        }
        Type t = Type::recordType(monoName);
        recordTypes.emplace(monoName, t);
        recordDeclarations.push_back(std::move(monoDecl));
        return t;
    }
    Function instantiateGenericFunction(const std::string& templateName, const std::vector<Type>& typeArgs, int line) {
        auto it = genericFunctionTemplates.find(templateName);
        if (it == genericFunctionTemplates.end()) {
            throw Error("line " + std::to_string(line) + ": '" + templateName + "' is not a generic block");
        }
        const Function& tmpl = it->second;
        if (tmpl.typeParams.size() != typeArgs.size()) {
            throw Error("line " + std::to_string(line) + ": generic block '" + templateName + "' expects " +
                        std::to_string(tmpl.typeParams.size()) + " type arguments, got " + std::to_string(typeArgs.size()));
        }
        std::string monoName = templateName;
        for (const auto& arg : typeArgs) monoName += "__" + typeName(arg);
        Function monoFn = cloneFunction(tmpl);
        monoFn.name = monoName;
        monoFn.typeParams.clear();
        auto substituteType = [&](Type& t) {
            for (std::size_t i = 0; i < tmpl.typeParams.size(); ++i) {
                if (t.name == tmpl.typeParams[i]) t = typeArgs[i];
                else if (t.pointeeType && t.pointeeType->name == tmpl.typeParams[i]) {
                    t.pointeeType = std::make_shared<Type>(typeArgs[i]);
                }
            }
        };
        for (auto& p : monoFn.params) substituteType(p.type);
        for (auto& l : monoFn.locals) substituteType(l.type);
        if (monoFn.result) substituteType(*monoFn.result);
        if (monoFn.errorResult) substituteType(*monoFn.errorResult);
        std::function<void(Expr&)> substituteExpr = [&](Expr& e) {
            substituteType(e.targetType);
            if (e.left) substituteExpr(*e.left);
            if (e.right) substituteExpr(*e.right);
            for (auto& a : e.arguments) substituteExpr(*a);
        };
        std::function<void(Stmt&)> substituteStmt = [&](Stmt& s) {
            if (s.expr) substituteExpr(*s.expr);
            if (s.target) substituteExpr(*s.target);
            for (auto& b : s.body) substituteStmt(b);
            for (auto& eb : s.elseBody) substituteStmt(eb);
            for (auto& mc : s.matchCases) {
                if (mc.pattern) substituteExpr(*mc.pattern);
                for (auto& b : mc.body) substituteStmt(b);
            }
            for (auto& wa : s.writeArgs) substituteExpr(*wa);
        };
        for (auto& s : monoFn.body) substituteStmt(s);
        return monoFn;
    }
    Type parseTypeRef() {
        bool isVol = false;
        if (eat(K::Volatile)) {
            isVol = true;
        }
        Type base;
        bool isNullToken = false;
        bool isNullOrNoType = false;
        if (eat(K::Null)) {
            isNullToken = true;
            isNullOrNoType = true;
        } else if (at(K::Id) && (t().text == "No_Type" || t().text == "no_type" || t().text == "NoType")) {
            pop();
            isNullOrNoType = true;
        }
        if (isNullOrNoType) {
            if (eat(K::Task_Handle) || (at(K::Id) && (t().text == "Task_Handle" || t().text == "TaskHandle"))) {
                if (at(K::Id)) pop();
                base = Type::taskHandle(Type::nullType());
            } else if (at(K::Id) && (t().text == "Channel" || t().text == "channel")) {
                pop();
                base = Type::channelType(Type::nullType());
            } else if (at(K::Id) && (t().text == "Future" || t().text == "future")) {
                pop();
                base = Type::futureType(Type::nullType());
            } else if (eat(K::Block_Pointer) || (at(K::Id) && (t().text == "Block_Pointer" || t().text == "BlockPointer"))) {
                fail("The 'No_Type' prefix has been removed for non-producing block pointers; declare as 'Block_Pointer(...)' directly");
            } else if (isNullToken) {
                fail("'Null' is a pointer literal, not a declarable type");
            } else {
                base = Type::nullType();
            }
        } else if (eat(K::Block_Pointer) || (at(K::Id) && (t().text == "Block_Pointer" || t().text == "BlockPointer"))) {
            if (at(K::Id)) pop();
            std::vector<Type> params;
            if (eat(K::LParen)) {
                if (!at(K::RParen)) {
                    do {
                        params.push_back(parseTypeRef());
                    } while (eat(K::Comma));
                }
                expect(K::RParen, "expected ')' after Block_Pointer parameter types");
            }
            base = Type::blockPointer(Type::nullType(), std::move(params));
        } else if (eat(K::Text) || (at(K::Id) && (t().text == "Text" || t().text == "String_Builder"))) {
            if (at(K::Id)) pop();
            unsigned cap = 64;
            if (eat(K::LBracket)) {
                Token sz = expect(K::Number, "expected Text capacity in Text[N]");
                cap = static_cast<unsigned>(sz.magnitude);
                expect(K::RBracket, "expected ']' after Text capacity");
            }
            base = Type::textType(cap);
        } else if (eat(K::Task_Handle) || (at(K::Id) && (t().text == "Task_Handle" || t().text == "TaskHandle"))) {
            if (at(K::Id)) pop();
            expect(K::LBracket, "expected '[' after 'Task_Handle'");
            Type res = parseTypeRef();
            expect(K::RBracket, "expected ']' after Task_Handle result type parameter");
            base = Type::taskHandle(std::move(res));
        } else if (eat(K::Pointer)) {
            expect(K::LBracket, "expected '[' after 'Pointer'");
            Type pointee = parseTypeRef();
            expect(K::RBracket, "expected ']' after Pointer type parameter");
            base = Type::pointer(std::move(pointee));
        } else if (eat(K::Smt_Pointer) || (at(K::Id) && (t().text == "Smt_Pointer" || t().text == "Smart_Pointer"))) {
            if (at(K::Id)) pop();
            expect(K::LBracket, "expected '[' after 'Smt_Pointer'");
            Type pointee = parseTypeRef();
            expect(K::RBracket, "expected ']' after Smt_Pointer type parameter");
            base = Type::smartPointer(std::move(pointee));
        } else if (eat(K::Channel) || (at(K::Id) && (t().text == "Channel" || t().text == "channel"))) {
            if (at(K::Id)) pop();
            expect(K::LBracket, "expected '[' after 'Channel'");
            Type elem = parseTypeRef();
            expect(K::RBracket, "expected ']' after Channel type parameter");
            base = Type::channelType(std::move(elem));
        } else if (eat(K::Mutex) || (at(K::Id) && (t().text == "Mutex" || t().text == "mutex"))) {
            if (at(K::Id)) pop();
            base = Type::mutexType();
        } else if (eat(K::Atomic) || (at(K::Id) && (t().text == "Atomic" || t().text == "atomic"))) {
            if (at(K::Id)) pop();
            expect(K::LBracket, "expected '[' after 'Atomic'");
            Type inner = parseTypeRef();
            expect(K::RBracket, "expected ']' after Atomic type parameter");
            base = Type::atomicType(std::move(inner));
        } else if (eat(K::Slice)) {
            expect(K::LBracket, "expected '[' after 'Slice'");
            Type elem = parseTypeRef();
            expect(K::RBracket, "expected ']' after Slice type parameter");
            base = Type::sliceType(std::move(elem));
        } else if (eat(K::Array)) {
            expect(K::LBracket, "expected '[' after 'Array'");
            Type elem = parseTypeRef();
            expect(K::Comma, "expected ',' in Array[T, N]");
            Token sz = expect(K::Number, "expected array size");
            if (sz.magnitude == 0) fail("array size must be greater than zero, found 0");
            expect(K::RBracket, "expected ']' after Array size");
            base = Type::arrayType(std::move(elem), static_cast<unsigned>(sz.magnitude));
        } else if (eat(K::LParen)) {
            std::vector<Type> tupleTypes;
            if (!at(K::RParen)) {
                do {
                    tupleTypes.push_back(parseTypeRef());
                } while (eat(K::Comma));
            }
            expect(K::RParen, "expected ')' after type list");
            if (eat(K::FatArrow)) {
                Type ret = parseTypeRef();
                base = Type::blockPointer(std::move(ret), std::move(tupleTypes));
            } else {
                base = Type::tupleType(std::move(tupleTypes));
            }
        } else if (at(K::Id)) {
            Token name = pop();
            if (name.text == "HMap") {
                expect(K::LBracket, "expected '[' after 'HMap'");
                Type k = parseTypeRef();
                expect(K::Colon, "expected ':' between key and value types in HMap[Key : Val]");
                Type v = parseTypeRef();
                expect(K::RBracket, "expected ']' after HMap types");
                base = Type::hmapType(std::move(k), std::move(v));
            } else if (name.text == "Smt_Pointer" || name.text == "Smart_Pointer") {
                expect(K::LBracket, "expected '[' after 'Smt_Pointer'");
                Type pointee = parseTypeRef();
                expect(K::RBracket, "expected ']' after Smt_Pointer type parameter");
                base = Type::smartPointer(std::move(pointee));
            } else if (name.text == "Channel" || name.text == "channel") {
                expect(K::LBracket, "expected '[' after 'Channel'");
                Type elem = parseTypeRef();
                expect(K::RBracket, "expected ']' after Channel type parameter");
                base = Type::channelType(std::move(elem));
            } else if (name.text == "Mutex" || name.text == "mutex") {
                base = Type::mutexType();
            } else if (name.text == "Atomic" || name.text == "atomic") {
                expect(K::LBracket, "expected '[' after 'Atomic'");
                Type inner = parseTypeRef();
                expect(K::RBracket, "expected ']' after Atomic type parameter");
                base = Type::atomicType(std::move(inner));
            } else if (name.text == "Future" || name.text == "future") {
                expect(K::LBracket, "expected '[' after 'Future'");
                Type res = parseTypeRef();
                expect(K::RBracket, "expected ']' after Future type parameter");
                base = Type::futureType(std::move(res));
            } else if (name.text == "Pointer") {
                expect(K::LBracket, "expected '[' after 'Pointer'");
                Type pointee = parseTypeRef();
                expect(K::RBracket, "expected ']' after Pointer type parameter");
                base = Type::pointer(std::move(pointee));
            } else if (name.text == "Slice") {
                expect(K::LBracket, "expected '[' after 'Slice'");
                Type elem = parseTypeRef();
                expect(K::RBracket, "expected ']' after Slice type parameter");
                base = Type::sliceType(std::move(elem));
            } else if (name.text == "Array") {
                expect(K::LBracket, "expected '[' after 'Array'");
                Type elem = parseTypeRef();
                expect(K::Comma, "expected ',' in Array[T, N]");
                Token sz = expect(K::Number, "expected array size");
                if (sz.magnitude == 0) fail("array size must be greater than zero, found 0");
                expect(K::RBracket, "expected ']' after Array size");
                base = Type::arrayType(std::move(elem), static_cast<unsigned>(sz.magnitude));
            } else if (eat(K::LBracket)) {
                std::vector<Type> typeArgs;
                do {
                    typeArgs.push_back(parseTypeRef());
                } while (eat(K::Comma));
                expect(K::RBracket, "expected ']' after type arguments");
                base = instantiateGenericRecord(name.text, typeArgs, name.line);
            } else {
                base = resolveType(name);
            }
        } else {
            fail("expected type name");
        }
        base.isVolatile = isVol;
        if (isVol && base.kind == TypeKind::Pointer && base.pointeeType) {
            base.pointeeType->isVolatile = true;
        }

        while (true) {
            if (eat(K::Pointer) || (at(K::Id) && t().text == "Pointer")) {
                if (at(K::Id)) pop();
                base = Type::pointer(std::move(base));
                if (isVol && base.pointeeType) base.pointeeType->isVolatile = true;
            } else if (eat(K::Smt_Pointer) || (at(K::Id) && (t().text == "Smt_Pointer" || t().text == "Smart_Pointer"))) {
                if (at(K::Id)) pop();
                base = Type::smartPointer(std::move(base));
            } else if (eat(K::Channel) || (at(K::Id) && (t().text == "Channel" || t().text == "channel"))) {
                if (at(K::Id)) pop();
                base = Type::channelType(std::move(base));
            } else if (eat(K::Atomic) || (at(K::Id) && (t().text == "Atomic" || t().text == "atomic"))) {
                if (at(K::Id)) pop();
                base = Type::atomicType(std::move(base));
            } else if (eat(K::Slice) || (at(K::Id) && t().text == "Slice")) {
                if (at(K::Id)) pop();
                base = Type::sliceType(std::move(base));
            } else if (eat(K::Task_Handle) || (at(K::Id) && (t().text == "Task_Handle" || t().text == "TaskHandle"))) {
                if (at(K::Id)) pop();
                base = Type::taskHandle(std::move(base));
            } else if (eat(K::Vector) || (at(K::Id) && (t().text == "Vector" || t().text == "vector"))) {
                if (at(K::Id)) pop();
                unsigned cap = 0;
                bool hasSize = false;
                if (eat(K::LBracket)) {
                    Token sz = expect(K::Number, "expected Vector element count in Vector[N]");
                    cap = static_cast<unsigned>(sz.magnitude);
                    expect(K::RBracket, "expected ']' after Vector element count");
                    hasSize = true;
                }
                base = Type::vectorType(std::move(base), cap, hasSize);
            } else if (eat(K::Block_Pointer) || (at(K::Id) && (t().text == "Block_Pointer" || t().text == "BlockPointer"))) {
                if (at(K::Id)) pop();
                std::vector<Type> params;
                if (eat(K::LParen)) {
                    if (!at(K::RParen)) {
                        do {
                            params.push_back(parseTypeRef());
                        } while (eat(K::Comma));
                    }
                    expect(K::RParen, "expected ')' after Block_Pointer parameter types");
                }
                base = Type::blockPointer(std::move(base), std::move(params));
            } else if (eat(K::Array) || (at(K::Id) && t().text == "Array")) {
                if (at(K::Id)) pop();
                expect(K::LBracket, "expected '[' after 'Array'");
                Token sz = expect(K::Number, "expected array size in Array[N]");
                if (sz.magnitude == 0) fail("array size must be greater than zero, found 0");
                expect(K::RBracket, "expected ']' after array size");
                base = Type::arrayType(std::move(base), static_cast<unsigned>(sz.magnitude));
            } else {
                break;
            }
        }
        if (eat(K::Or)) {
            Type err = parseTypeRef();
            base.errorType = std::make_shared<Type>(std::move(err));
        }
        return base;
    }
    void preScanTypes() {
        for (std::size_t i = 0; i < ts.size(); ++i) {
            if (ts[i].kind == K::Record && i + 1 < ts.size() && ts[i + 1].kind == K::Id) {
                recordTypes.emplace(ts[i + 1].text, Type::recordType(ts[i + 1].text));
            } else if (ts[i].kind == K::Enum && i + 1 < ts.size() && ts[i + 1].kind == K::Id) {
                enumTypes.emplace(ts[i + 1].text, Type::enumType(ts[i + 1].text, 32, false));
            }
        }
    }
    std::unique_ptr<Expr> primary() {
        Token x = pop();
        std::unique_ptr<Expr> base;
        if (x.kind == K::Number) {
            auto e = make(Expr::Kind::Integer, x);
            e->magnitude = x.magnitude;
            base = std::move(e);
        } else if (x.kind == K::FloatNumber) {
            auto e = make(Expr::Kind::Floating, x);
            char* end = nullptr;
            e->floating = std::strtod(x.text.c_str(), &end);
            if (!end || *end || !std::isfinite(e->floating))
                throw Error("line " + std::to_string(x.line) + ": floating-point literal is out of range");
            base = std::move(e);
        } else if (x.kind == K::String) {
            base = make(Expr::Kind::String, x);
        } else if (x.kind == K::Character) {
            base = make(Expr::Kind::Character, x);
        } else if (x.kind == K::True || x.kind == K::False) {
            auto e = make(Expr::Kind::Boolean, x);
            e->boolean = x.kind == K::True;
            base = std::move(e);
        } else if (x.kind == K::Null) {
            base = make(Expr::Kind::NullLiteral, x);
        } else if (x.kind == K::Address) {
            expect(K::LParen, "expected '(' after Address");
            auto target = expression();
            expect(K::RParen, "expected ')' after target in Address");
            auto e = make(Expr::Kind::Address, x);
            e->left = std::move(target);
            base = std::move(e);
        } else if (x.kind == K::Deref) {
            expect(K::LParen, "expected '(' after Deref");
            auto ptr = expression();
            expect(K::RParen, "expected ')' after pointer expression in Deref");
            auto derefExpr = make(Expr::Kind::Deref, x);
            derefExpr->left = std::move(ptr);
            base = std::move(derefExpr);
        } else if (x.kind == K::Cast) {
            Type target;
            if (eat(K::LBracket)) {
                target = parseTypeRef();
                expect(K::RBracket, "expected ']' after target type in Cast");
                expect(K::LParen, "expected '(' after Cast[T]");
                auto expr = expression();
                expect(K::RParen, "expected ')' after expression in Cast");
                auto e = make(Expr::Kind::Cast, x);
                e->targetType = std::move(target);
                e->left = std::move(expr);
                base = std::move(e);
            } else if (eat(K::LParen)) {
                target = parseTypeRef();
                expect(K::Comma, "expected ',' between type and expression in Cast(T, expr)");
                auto expr = expression();
                expect(K::RParen, "expected ')' after Cast(T, expr)");
                auto e = make(Expr::Kind::Cast, x);
                e->targetType = std::move(target);
                e->left = std::move(expr);
                base = std::move(e);
            } else {
                fail("expected '[' or '(' after Cast");
            }
        } else if (x.kind == K::Slice) {
            expect(K::LParen, "expected '(' after Slice");
            auto first = expression();
            auto e = make(Expr::Kind::SliceConstructor, x);
            e->left = std::move(first);
            if (eat(K::Comma)) {
                e->right = expression();
            }
            expect(K::RParen, "expected ')' after Slice(...) arguments");
            base = std::move(e);
        } else if (x.kind == K::Await || (x.kind == K::Id && (x.text == "Await" || x.text == "await"))) {
            auto e = make(Expr::Kind::Await, x);
            if (eat(K::LParen)) {
                e->left = expression();
                expect(K::RParen, "expected ')' after expression in Await");
            } else {
                e->left = primary();
            }
            base = std::move(e);
        } else if (x.kind == K::Channel || (x.kind == K::Id && (x.text == "Channel" || x.text == "Channel_Open" || x.text == "Channel_Create") && at(K::LBracket))) {
            expect(K::LBracket, "expected '[' after Channel");
            Type elem = parseTypeRef();
            expect(K::RBracket, "expected ']' after Channel element type");
            expect(K::LParen, "expected '(' after Channel[T]");
            auto cap = expression();
            expect(K::RParen, "expected ')' after Channel capacity");
            auto e = make(Expr::Kind::Call, x);
            e->text = "Channel_Create";
            e->targetType = Type::channelType(std::move(elem));
            e->arguments.push_back(std::move(cap));
            base = std::move(e);
        } else if (x.kind == K::Move) {
            auto e = make(Expr::Kind::Move, x);
            e->left = primary();
            base = std::move(e);
        } else if (x.kind == K::Copy) {
            auto e = make(Expr::Kind::Copy, x);
            e->left = primary();
            base = std::move(e);
        } else if (x.kind == K::Borrow) {
            auto e = make(Expr::Kind::Borrow, x);
            e->left = primary();
            base = std::move(e);
        } else if (x.kind == K::Try) {
            auto tryExpr = make(Expr::Kind::Try, x);
            tryExpr->left = expression(8);
            base = std::move(tryExpr);
        } else if (x.kind == K::Allocate) {
            expect(K::LParen, "expected '(' after Allocate");
            auto count = expression();
            expect(K::RParen, "expected ')' after Allocate argument");
            auto e = make(Expr::Kind::Allocate, x);
            e->arguments.push_back(std::move(count));
            base = std::move(e);
        } else if (x.kind == K::Resize) {
            expect(K::LParen, "expected '(' after Resize");
            auto ptr = expression();
            expect(K::Comma, "expected ',' between pointer and new count in Resize");
            auto count = expression();
            expect(K::RParen, "expected ')' after Resize arguments");
            auto e = make(Expr::Kind::Resize, x);
            e->arguments.push_back(std::move(ptr));
            e->arguments.push_back(std::move(count));
            base = std::move(e);
        } else if (x.kind == K::Release) {
            expect(K::LParen, "expected '(' after Release");
            auto ptr = expression();
            expect(K::RParen, "expected ')' after Release argument");
            auto e = make(Expr::Kind::Release, x);
            e->arguments.push_back(std::move(ptr));
            base = std::move(e);
        } else if (x.kind == K::Spawn) {
            auto target = primary();
            if (target->kind != Expr::Kind::Call) {
                throw Error("line " + std::to_string(x.line) + ": Spawn requires a procedure call, e.g. 'Spawn Calculate(10)'");
            }
            auto e = make(Expr::Kind::Spawn, x);
            e->arguments.push_back(std::move(target));
            base = std::move(e);
        } else if (x.kind == K::Join) {
            if (at(K::Id) && (t().text == "Task" || t().text == "task") && pos + 1 < ts.size() && (ts[pos + 1].kind == K::Id || ts[pos + 1].kind == K::LParen)) pop();
            auto task = primary();
            auto e = make(Expr::Kind::Join, x);
            e->arguments.push_back(std::move(task));
            if (eat(K::Within) || (at(K::Id) && (t().text == "within" || t().text == "Within"))) {
                if (at(K::Id)) pop();
                auto timeout = expression();
                e->arguments.push_back(std::move(timeout));
            }
            base = std::move(e);
        } else if (x.kind == K::Pow || (x.kind == K::Id && x.text == "Pow" && at(K::LParen))) {
            expect(K::LParen, "expected '(' after Pow");
            auto baseVal = expression();
            expect(K::Comma, "expected ',' in Pow(Base, Exp)");
            auto expVal = expression();
            expect(K::RParen, "expected ')' after Pow arguments");
            Token powTok{K::Pow, "pow", x.line, 0, currentSourceIndex};
            base = make(Expr::Kind::Binary, powTok, std::move(baseVal), std::move(expVal));
        } else if (x.kind == K::To_String || (x.kind == K::Id && (x.text == "To_String" || x.text == "ToString") && at(K::LParen))) {
            expect(K::LParen, "expected '(' after To_String");
            auto arg = expression();
            expect(K::RParen, "expected ')' after To_String argument");
            Token strTok{K::To_String, "To_String", x.line, 0, currentSourceIndex};
            auto e = make(Expr::Kind::Call, strTok);
            e->text = "To_String";
            e->arguments.push_back(std::move(arg));
            base = std::move(e);
        } else if (x.kind == K::Id && x.text == "Pop" && at(K::LParen)) {
            expect(K::LParen, "expected '(' after Pop");
            auto vec = expression();
            expect(K::RParen, "expected ')' after Pop argument");
            auto e = make(Expr::Kind::Call, x);
            e->text = "Pop";
            e->arguments.push_back(std::move(vec));
            base = std::move(e);
        } else if (x.kind == K::Id) {
            if (isBuiltinType(x.text) && at(K::LParen)) {
                expect(K::LParen, "expected '(' after type name for cast");
                auto val = expression();
                expect(K::RParen, "expected ')' after cast expression");
                auto castExpr = make(Expr::Kind::Cast, x);
                castExpr->targetType = resolveType(x);
                castExpr->left = std::move(val);
                base = std::move(castExpr);
            } else {
                if (x.text == "Call" && at(K::Id)) {
                    fail("the 'Call' keyword has been removed from Loom; call functions directly as '" + t().text + "(...)'");
                }
                if (genericFunctionTemplates.count(x.text) && at(K::LBracket)) {
                    expect(K::LBracket, "expected '[' after generic block name");
                    std::vector<Type> typeArgs;
                    do {
                        typeArgs.push_back(parseTypeRef());
                    } while (eat(K::Comma));
                    expect(K::RBracket, "expected ']' after type arguments");
                    Function mono = instantiateGenericFunction(x.text, typeArgs, x.line);
                    std::string monoName = mono.name;
                    if (!instantiatedGenericFunctions.count(monoName)) {
                        instantiatedGenericFunctions.insert(monoName);
                        pendingInstantiatedFunctions.push_back(std::move(mono));
                    }
                    x.text = monoName;
                }
                if (eat(K::LParen)) {
                    auto call = make(Expr::Kind::Call, x);
                    if (!at(K::RParen)) {
                        do { call->arguments.push_back(expression()); } while (eat(K::Comma));
                    }
                    expect(K::RParen, "expected ')' after function arguments");
                    base = std::move(call);
                } else if (eat(K::Dot)) {
                    Token field = expect(K::Id, "expected field, enum variant, or function after '.'");
                    if (eat(K::LParen)) {
                        auto call = make(Expr::Kind::Call, field);
                        call->qualifier = x.text;
                        call->left = make(Expr::Kind::Variable, x);
                        if (!at(K::RParen)) {
                            do { call->arguments.push_back(expression()); } while (eat(K::Comma));
                        }
                        expect(K::RParen, "expected ')' after function arguments");
                        base = std::move(call);
                    } else if (at(K::LBrace)) {
                        pop();
                        auto recLit = make(Expr::Kind::RecordLiteral, field);
                        recLit->qualifier = x.text;
                        newlines();
                        if (!at(K::RBrace)) {
                            do {
                                newlines();
                                if (at(K::RBrace)) break;
                                Token fn = expect(K::Id, "expected field name in record literal");
                                expect(K::Colon, "expected ':' after field name in record literal");
                                auto fval = expression();
                                recLit->fieldNames.push_back(fn.text);
                                recLit->arguments.push_back(std::move(fval));
                                newlines();
                            } while (eat(K::Comma));
                        }
                        newlines();
                        expect(K::RBrace, "expected '}' after record literal fields");
                        base = std::move(recLit);
                    } else if (enumTypes.count(x.text)) {
                        x.text += "." + field.text;
                        base = make(Expr::Kind::EnumVariant, x);
                    } else {
                        auto var = make(Expr::Kind::Variable, x);
                        base = make(Expr::Kind::Member, field, std::move(var));
                    }
                } else if (at(K::LBrace)) {
                    pop();
                    auto recLit = make(Expr::Kind::RecordLiteral, x);
                    newlines();
                    if (!at(K::RBrace)) {
                        do {
                            newlines();
                            if (at(K::RBrace)) break;
                            Token fn = expect(K::Id, "expected field name in record literal");
                            expect(K::Colon, "expected ':' after field name in record literal");
                            auto fval = expression();
                            recLit->fieldNames.push_back(fn.text);
                            recLit->arguments.push_back(std::move(fval));
                            newlines();
                        } while (eat(K::Comma));
                    }
                    newlines();
                    expect(K::RBrace, "expected '}' after record literal fields");
                    base = std::move(recLit);
                } else {
                    base = make(Expr::Kind::Variable, x);
                }
            }
        } else if (x.kind == K::LParen) {
            bool isClosure = false;
            size_t p = pos;
            int depth = 1;
            while (p < ts.size()) {
                if (ts[p].kind == K::LParen) depth++;
                else if (ts[p].kind == K::RParen) {
                    depth--;
                    if (depth == 0) {
                        if (p + 1 < ts.size() && ts[p + 1].kind == K::FatArrow) isClosure = true;
                        break;
                    }
                } else if (ts[p].kind == K::Newline || ts[p].kind == K::Eof) {
                    break;
                }
                p++;
            }
            if (isClosure) {
                auto closure = make(Expr::Kind::Closure, x);
                if (!at(K::RParen)) {
                    do {
                        Token paramName = expect(K::Id, "expected parameter name in closure");
                        expect(K::Colon, "expected ':' after closure parameter name");
                        Type paramType = parseTypeRef();
                        closure->closureParams.push_back({paramName.text, paramType});
                    } while (eat(K::Comma));
                }
                expect(K::RParen, "expected ')' after closure parameters");
                expect(K::FatArrow, "expected '=>' after closure parameter list");
                closure->closureBodyExpr = expression();
                base = std::move(closure);
            } else {
                auto first = expression();
                if (eat(K::Comma)) {
                    auto tup = make(Expr::Kind::TupleLiteral, x);
                    tup->arguments.push_back(std::move(first));
                    do {
                        newlines();
                        if (at(K::RParen)) break;
                        tup->arguments.push_back(expression());
                        newlines();
                    } while (eat(K::Comma));
                    expect(K::RParen, "expected ')' to close tuple expression");
                    base = std::move(tup);
                } else {
                    expect(K::RParen, "expected ')' to close expression");
                    base = std::move(first);
                }
            }
        } else if (x.kind == K::Minus || x.kind == K::Not || x.kind == K::Tilde) {
            return make(Expr::Kind::Unary, x, primary());
        } else if (x.kind == K::LBrace) {
            newlines();
            if (at(K::RBrace)) {
                pop();
                base = make(Expr::Kind::ArrayLiteral, x);
            } else {
                auto first = expression();
                if (eat(K::Colon)) {
                    auto firstVal = expression();
                    auto hmapLit = make(Expr::Kind::HMapLiteral, x);
                    hmapLit->arguments.push_back(std::move(first));
                    hmapLit->arguments.push_back(std::move(firstVal));
                    newlines();
                    while (eat(K::Comma)) {
                        newlines();
                        if (at(K::RBrace)) break;
                        auto k = expression();
                        expect(K::Colon, "expected ':' between key and value in HMap literal");
                        auto v = expression();
                        hmapLit->arguments.push_back(std::move(k));
                        hmapLit->arguments.push_back(std::move(v));
                        newlines();
                    }
                    newlines();
                    expect(K::RBrace, "expected '}' after HMap literal");
                    base = std::move(hmapLit);
                } else {
                    auto lit = make(Expr::Kind::ArrayLiteral, x);
                    lit->arguments.push_back(std::move(first));
                    newlines();
                    while (eat(K::Comma)) {
                        newlines();
                        if (at(K::RBrace)) break;
                        lit->arguments.push_back(expression());
                        newlines();
                    }
                    newlines();
                    expect(K::RBrace, "expected '}' after array elements");
                    base = std::move(lit);
                }
            }
        } else if (x.kind == K::Unsupported) {
            throw Error("line " + std::to_string(x.line) + ": unsupported keyword '" + x.text + "'");
        } else {
            throw Error("line " + std::to_string(x.line) + ": expected expression");
        }

        while (at(K::Dot) || at(K::LBracket)) {
            if (eat(K::Dot)) {
                Token field = expect(K::Id, "expected field after '.'");
                if (eat(K::LParen)) {
                    auto call = make(Expr::Kind::Call, field);
                    call->left = std::move(base);
                    if (!at(K::RParen)) {
                        do { call->arguments.push_back(expression()); } while (eat(K::Comma));
                    }
                    expect(K::RParen, "expected ')' after function arguments");
                    base = std::move(call);
                } else {
                    base = make(Expr::Kind::Member, field, std::move(base));
                }
            } else if (eat(K::LBracket)) {
                auto startIdx = expression();
                if (eat(K::DotDot)) {
                    auto endIdx = expression();
                    expect(K::RBracket, "expected ']' after slice range");
                    auto subSlice = make(Expr::Kind::SubSlice, x);
                    subSlice->left = std::move(base);
                    subSlice->arguments.push_back(std::move(startIdx));
                    subSlice->arguments.push_back(std::move(endIdx));
                    base = std::move(subSlice);
                } else {
                    expect(K::RBracket, "expected ']' after index");
                    auto indexed = make(Expr::Kind::Index, x);
                    indexed->left = std::move(base);
                    indexed->right = std::move(startIdx);
                    base = std::move(indexed);
                }
            }
        }
        return base;
    }
    std::unique_ptr<Expr> expression(int minPrec = 1) {
        auto lhs = primary();
        while (precedence(t().kind) >= minPrec || at(K::As)) {
            if (at(K::As)) {
                if (minPrec > 10) break;
                Token asTok = pop();
                Type target = parseTypeRef();
                auto castExpr = make(Expr::Kind::Cast, asTok);
                castExpr->targetType = std::move(target);
                castExpr->left = std::move(lhs);
                lhs = std::move(castExpr);
                continue;
            }
            Token op = pop();
            const int p = precedence(op.kind);
            lhs = make(Expr::Kind::Binary, op, std::move(lhs), expression(p + 1));
        }
        return lhs;
    }
    void endPair([[maybe_unused]] const std::string& name, const std::string& kind) {
        expect(K::End, "expected 'End " + kind + "' before function end");
        Token close = expect(kind == "If" ? K::If : (kind == "Match" ? K::Match : K::Loop), "expected 'End " + kind + "'");
        (void)close;
    }
    static bool hasInterpolation(const std::string& str) {
        for (std::size_t i = 0; i < str.size(); ++i) {
            if (str[i] == '\\') {
                ++i;
                continue;
            }
            if (str[i] == '<') {
                std::size_t close = str.find('>', i + 1);
                if (close != std::string::npos && close > i + 1) {
                    std::string inner = str.substr(i + 1, close - (i + 1));
                    std::size_t start = inner.find_first_not_of(" \t");
                    if (start != std::string::npos &&
                        (std::isalnum(static_cast<unsigned char>(inner[start])) || inner[start] == '_')) {
                        return true;
                    }
                }
            }
        }
        return false;
    }
    void parseInterpolatedWrite(const Token& strTok, std::vector<std::unique_ptr<Expr>>& out) {
        const std::string& str = strTok.text;
        std::size_t i = 0;
        std::string textBuf;
        while (i < str.size()) {
            if (str[i] == '\\' && i + 1 < str.size() && (str[i + 1] == '<' || str[i + 1] == '>')) {
                textBuf += str[i + 1];
                i += 2;
                continue;
            }
            if (str[i] == '<') {
                std::size_t close = str.find('>', i + 1);
                if (close != std::string::npos) {
                    std::string exprCode = str.substr(i + 1, close - (i + 1));
                    std::size_t start = exprCode.find_first_not_of(" \t");
                    if (start != std::string::npos &&
                        (std::isalnum(static_cast<unsigned char>(exprCode[start])) || exprCode[start] == '_')) {
                        std::string spec;
                        int parenDepth = 0;
                        std::size_t colon = std::string::npos;
                        for (std::size_t k = start; k < exprCode.size(); ++k) {
                            if (exprCode[k] == '(' || exprCode[k] == '[' || exprCode[k] == '{') parenDepth++;
                            else if (exprCode[k] == ')' || exprCode[k] == ']' || exprCode[k] == '}') parenDepth--;
                            else if (exprCode[k] == ':' && parenDepth == 0) {
                                colon = k;
                            }
                        }
                        if (colon != std::string::npos) {
                            spec = exprCode.substr(colon + 1);
                            auto sStart = spec.find_first_not_of(" \t");
                            auto sEnd = spec.find_last_not_of(" \t");
                            if (sStart != std::string::npos) spec = spec.substr(sStart, sEnd - sStart + 1);
                            else spec.clear();
                            exprCode = exprCode.substr(0, colon);
                        }
                        if (!textBuf.empty()) {
                            Token t{K::String, textBuf, strTok.line, 0, strTok.sourceIndex};
                            out.push_back(make(Expr::Kind::String, t));
                            textBuf.clear();
                        }
                        auto exprTokens = Lexer(exprCode, strTok.sourceIndex).run();
                        while (!exprTokens.empty() && exprTokens.back().kind == K::Eof)
                            exprTokens.pop_back();
                        for (auto& tok : exprTokens) {
                            tok.line = strTok.line;
                        }
                        exprTokens.push_back({K::Eof, "<eof>", strTok.line, 0, strTok.sourceIndex});
                        Parser subParser(std::move(exprTokens));
                        subParser.recordTypes = recordTypes;
                        subParser.enumTypes = enumTypes;
                        auto parsed = subParser.expression();
                        parsed->formatSpec = spec;
                        out.push_back(std::move(parsed));
                        i = close + 1;
                        continue;
                    }
                }
            }
            textBuf += str[i++];
        }
        if (!textBuf.empty()) {
            Token t{K::String, textBuf, strTok.line, 0, strTok.sourceIndex};
            out.push_back(make(Expr::Kind::String, t));
        }
    }
    void parseWriteStatement(Stmt& s, const std::string& callName) {
        s.kind = Stmt::Kind::Write;
        s.name = callName;
        expect(K::LParen, "expected '(' after " + callName);
        auto parseOneArg = [&](std::vector<std::unique_ptr<Expr>>& targetList) {
            if (at(K::String)) {
                Token strTok = pop();
                if (hasInterpolation(strTok.text)) {
                    parseInterpolatedWrite(strTok, targetList);
                } else {
                    targetList.push_back(make(Expr::Kind::String, strTok));
                }
            } else {
                targetList.push_back(expression());
            }
        };

        std::vector<std::unique_ptr<Expr>> args;
        if (!at(K::RParen)) {
            parseOneArg(args);
            while (eat(K::Comma)) {
                parseOneArg(args);
            }
        } else {
            Token emptyStr{K::String, "", s.line, 0, currentSourceIndex};
            args.push_back(make(Expr::Kind::String, emptyStr));
        }
        expect(K::RParen, "expected ')' after " + callName + " argument");

        if (args.size() == 1) {
            s.expr = std::move(args[0]);
        } else {
            s.writeArgs = std::move(args);
        }
    }
    Stmt parseSingleStmt() {
        Token x = t();
        Stmt s;
        s.line = x.line;
        if (at(K::LParen)) {
            size_t look = pos + 1;
            bool isTupleAssign = false;
            while (look < ts.size() && ts[look].kind == K::Id) {
                look++;
                if (look < ts.size() && ts[look].kind == K::Comma) {
                    look++;
                    continue;
                }
                if (look < ts.size() && ts[look].kind == K::RParen) {
                    look++;
                    if (look < ts.size() && ts[look].kind == K::Assign) {
                        isTupleAssign = true;
                    }
                    break;
                }
                break;
            }
            if (isTupleAssign) {
                pop();
                s.kind = Stmt::Kind::MultiAssign;
                do {
                    Token targetTok = expect(K::Id, "expected identifier in tuple unpacking");
                    s.multiTargets.push_back(targetTok.text);
                } while (eat(K::Comma));
                expect(K::RParen, "expected ')' after tuple targets");
                expect(K::Assign, "expected '=' after tuple unpacking targets");
                s.expr = expression();
                return s;
            }
        }
        if (eat(K::Give)) {
            s.kind = Stmt::Kind::Give;
            s.expr = expression();
        } else if (eat(K::Print) || (at(K::Id) && (t().text == "Print" || t().text == "print") && pos + 1 < ts.size() && ts[pos + 1].kind == K::LParen)) {
            if (at(K::Id)) pop();
            parseWriteStatement(s, "Print");
        } else if (eat(K::Write)) {
            parseWriteStatement(s, "Write");
        } else if (at(K::Id) && t().text == "Write_UART" && pos + 1 < ts.size() && ts[pos + 1].kind == K::LParen) {
            Token t_uart = pop();
            parseWriteStatement(s, t_uart.text);
        } else if (eat(K::Exit)) {
            s.kind = Stmt::Kind::Exit;
            if (at(K::LParen)) {
                expect(K::LParen, "expected '(' after Exit");
                s.expr = expression();
                expect(K::RParen, "expected ')' after exit code");
            }
        } else if (eat(K::Skip)) {
            s.kind = Stmt::Kind::Skip;
        } else if (eat(K::Cancel) || (at(K::Id) && (t().text == "Cancel" || t().text == "cancel") && pos + 1 < ts.size() && ts[pos + 1].kind != K::Equal)) {
            if (at(K::Id)) pop();
            if (at(K::Id) && (t().text == "Task" || t().text == "task") && pos + 1 < ts.size() && (ts[pos + 1].kind == K::Id || ts[pos + 1].kind == K::LParen)) pop();
            auto task = primary();
            Token cancelTok{K::Cancel, "Cancel", s.line, 0, currentSourceIndex};
            auto e = make(Expr::Kind::Call, cancelTok);
            e->text = "__loom_task_cancel";
            e->arguments.push_back(std::move(task));
            s.kind = Stmt::Kind::Call;
            s.expr = std::move(e);
        } else if (at(K::Id) && t().text == "Append" && pos + 1 < ts.size() && ts[pos + 1].kind == K::LParen) {
            Token appTok = pop();
            expect(K::LParen, "expected '(' after Append");
            auto vec = expression();
            expect(K::Comma, "expected ',' between vector and item in Append");
            auto item = expression();
            expect(K::RParen, "expected ')' after Append arguments");
            auto e = make(Expr::Kind::Call, appTok);
            e->text = "Append";
            e->arguments.push_back(std::move(vec));
            e->arguments.push_back(std::move(item));
            s.kind = Stmt::Kind::Call;
            s.expr = std::move(e);
        } else if (at(K::Release)) {
            Token relTok = pop();
            expect(K::LParen, "expected '(' after Release");
            auto ptr = expression();
            expect(K::RParen, "expected ')' after pointer expression in Release");
            auto e = make(Expr::Kind::Release, relTok);
            e->arguments.push_back(std::move(ptr));
            s.kind = Stmt::Kind::Call;
            s.expr = std::move(e);
        } else if (at(K::Join)) {
            Token joinTok = pop();
            if (at(K::Id) && (t().text == "Task" || t().text == "task") && pos + 1 < ts.size() && (ts[pos + 1].kind == K::Id || ts[pos + 1].kind == K::LParen)) pop();
            auto task = primary();
            auto e = make(Expr::Kind::Join, joinTok);
            e->arguments.push_back(std::move(task));
            if (eat(K::Within) || (at(K::Id) && (t().text == "within" || t().text == "Within"))) {
                if (at(K::Id)) pop();
                auto timeout = expression();
                e->arguments.push_back(std::move(timeout));
            }
            s.kind = Stmt::Kind::Call;
            s.expr = std::move(e);
        } else if (at(K::Deref)) {
            Token derefTok = pop();
            expect(K::LParen, "expected '(' after Deref");
            auto ptr = expression();
            expect(K::RParen, "expected ')' after pointer expression in Deref");
            auto target = make(Expr::Kind::Deref, derefTok);
            target->left = std::move(ptr);
            while (at(K::Dot) || at(K::LBracket)) {
                if (eat(K::Dot)) {
                    Token field = expect(K::Id, "expected field name after '.'");
                    target = make(Expr::Kind::Member, field, std::move(target));
                } else if (eat(K::LBracket)) {
                    auto idx = expression();
                    expect(K::RBracket, "expected ']' after index");
                    auto indexed = make(Expr::Kind::Index, derefTok);
                    indexed->left = std::move(target);
                    indexed->right = std::move(idx);
                    target = std::move(indexed);
                }
            }
            if (isCompoundAssign(t().kind)) {
                Token op = pop();
                Token binOp = compoundToBinaryToken(op);
                auto val = expression();
                s.kind = Stmt::Kind::TargetAssign;
                s.target = cloneExpr(*target);
                s.expr = make(Expr::Kind::Binary, binOp, std::move(target), std::move(val));
            } else {
                expect(K::Assign, "expected '=' in assignment");
                auto val = expression();
                s.kind = Stmt::Kind::TargetAssign;
                s.target = std::move(target);
                s.expr = std::move(val);
            }
        } else if (eat(K::Use)) {
            Token modTok = expect(K::Id, "expected module name after use");
            s.kind = Stmt::Kind::Use;
            s.name = modTok.text;
            return s;
        } else if (at(K::Id)) {
            if (pos + 1 < ts.size() && ts[pos + 1].kind == K::Comma) {
                size_t look = pos;
                bool isMultiAssign = false;
                while (look < ts.size() && ts[look].kind == K::Id) {
                    look++;
                    if (look < ts.size() && ts[look].kind == K::Comma) {
                        look++;
                        continue;
                    }
                    if (look < ts.size() && ts[look].kind == K::Assign) {
                        isMultiAssign = true;
                    }
                    break;
                }
                if (isMultiAssign) {
                    s.kind = Stmt::Kind::MultiAssign;
                    do {
                        Token targetTok = expect(K::Id, "expected identifier in multi-value assignment");
                        s.multiTargets.push_back(targetTok.text);
                    } while (eat(K::Comma));
                    expect(K::Assign, "expected '=' in multi-value assignment");
                    s.expr = expression();
                    return s;
                }
            }
            Token id = pop();
            if (id.text == "Call" && at(K::Id)) {
                fail("the 'Call' keyword has been removed from Loom; call procedures directly as '" + t().text + "(...)'");
            }
            if (id.text == "Write_UART" && at(K::LParen)) {
                parseWriteStatement(s, id.text);
                return s;
            }
            if (eat(K::Dot)) {
                Token field = expect(K::Id, "expected record field or function name after '.'");
                if (at(K::LParen)) {
                    if (field.text == "Write_UART" || field.text == "Write") {
                        parseWriteStatement(s, field.text);
                        return s;
                    }
                    expect(K::LParen, "expected '(' after function name");
                    auto callExpr = make(Expr::Kind::Call, field);
                    callExpr->qualifier = id.text;
                    callExpr->left = make(Expr::Kind::Variable, id);
                    if (!at(K::RParen)) {
                        do { callExpr->arguments.push_back(expression()); } while (eat(K::Comma));
                    }
                    expect(K::RParen, "expected ')' after arguments");
                    s.kind = Stmt::Kind::Call;
                    s.expr = std::move(callExpr);
                    return s;
                }
                if (at(K::Dot)) fail("nested record field assignment is not supported yet");
                s.kind = Stmt::Kind::Assign;
                s.name = id.text;
                s.member = field.text;
                if (isCompoundAssign(t().kind)) {
                    Token op = pop();
                    Token binOp = compoundToBinaryToken(op);
                    auto rhs = expression();
                    auto lhsMember = make(Expr::Kind::Member, field, make(Expr::Kind::Variable, id));
                    s.expr = make(Expr::Kind::Binary, binOp, std::move(lhsMember), std::move(rhs));
                } else {
                    expect(K::Assign, "expected '=' after record field");
                    s.expr = expression();
                }
            } else if (at(K::LParen)) {
                expect(K::LParen, "expected '(' after procedure name");
                auto callExpr = make(Expr::Kind::Call, id);
                if (!at(K::RParen)) {
                    do { callExpr->arguments.push_back(expression()); } while (eat(K::Comma));
                }
                expect(K::RParen, "expected ')' after arguments");
                s.kind = Stmt::Kind::Call;
                s.expr = std::move(callExpr);
                return s;
            } else if (eat(K::LBracket)) {
                auto idx = expression();
                expect(K::RBracket, "expected ']' after index");
                auto target = make(Expr::Kind::Index, id);
                target->left = make(Expr::Kind::Variable, id);
                target->right = std::move(idx);
                while (at(K::Dot) || at(K::LBracket)) {
                    if (eat(K::Dot)) {
                        Token field = expect(K::Id, "expected field name after '.'");
                        target = make(Expr::Kind::Member, field, std::move(target));
                    } else if (eat(K::LBracket)) {
                        auto nextIdx = expression();
                        expect(K::RBracket, "expected ']' after index");
                        auto indexed = make(Expr::Kind::Index, id);
                        indexed->left = std::move(target);
                        indexed->right = std::move(nextIdx);
                        target = std::move(indexed);
                    }
                }
                if (isCompoundAssign(t().kind)) {
                    Token op = pop();
                    Token binOp = compoundToBinaryToken(op);
                    auto rhs = expression();
                    s.kind = Stmt::Kind::TargetAssign;
                    s.target = cloneExpr(*target);
                    s.expr = make(Expr::Kind::Binary, binOp, std::move(target), std::move(rhs));
                } else {
                    expect(K::Assign, "expected '=' after index");
                    s.kind = Stmt::Kind::TargetAssign;
                    s.target = std::move(target);
                    s.expr = expression();
                }
            } else {
                s.kind = Stmt::Kind::Assign;
                s.name = id.text;
                if (isCompoundAssign(t().kind)) {
                    Token op = pop();
                    Token binOp = compoundToBinaryToken(op);
                    auto rhs = expression();
                    auto lhsVar = make(Expr::Kind::Variable, id);
                    s.expr = make(Expr::Kind::Binary, binOp, std::move(lhsVar), std::move(rhs));
                } else {
                    expect(K::Assign, "expected '=' in assignment (use '==' for equality)");
                    s.expr = expression();
                }
            }
        } else if (at(K::Unsupported)) {
            Token u = pop();
            throw Error("line " + std::to_string(u.line) + ": unsupported keyword '" + u.text + "'");
        } else {
            fail("expected assignment, procedure call, Give, Exit, Skip, If, Loop, Write, or End");
        }
        return s;
    }
    std::vector<Stmt> statementsForMatchCase() {
        std::vector<Stmt> out;
        bool hasEnsureInThisBlock = false;
        newlines();
        while (!at(K::Eof) && !at(K::When) && !at(K::Default) &&
               !(at(K::Else) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Colon) &&
               !(at(K::End) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Match)) {
            Token x = t();
            Stmt s;
            s.line = x.line;
            if (eat(K::Ensure)) {
                if (hasEnsureInThisBlock) {
                    fail("block cannot contain multiple Ensure blocks; consolidate cleanup into a single Ensure block");
                }
                hasEnsureInThisBlock = true;
                s.kind = Stmt::Kind::Ensure;
                expect(K::LBrace, "expected '{' after Ensure");
                newlines();
                if (at(K::RBrace)) fail("Ensure requires at least one cleanup statement");
                while (!at(K::RBrace) && !at(K::Eof)) {
                    if (at(K::Give) || at(K::Exit) || at(K::Skip) || at(K::Ensure))
                        fail("control-flow statements are not valid inside Ensure");
                    s.body.push_back(parseSingleStmt());
                    newlines();
                }
                expect(K::RBrace, "expected '}' after Ensure cleanup statement");
                if (s.body.empty()) fail("Ensure requires at least one cleanup statement");
                s.ensuredStmt = std::make_shared<Stmt>(cloneStmt(s.body.front()));
            } else if (at(K::If)) {
                pop();
                s.kind = Stmt::Kind::If;
                s.expr = expression();
                requireLineEnd("If condition");
                s.body = statements(true, true);
                Stmt* currentIf = &s;
                while (eat(K::Else)) {
                    if (eat(K::If)) {
                        Stmt elseIfStmt;
                        elseIfStmt.kind = Stmt::Kind::If;
                        elseIfStmt.line = t().line;
                        elseIfStmt.expr = expression();
                        requireLineEnd("Else If condition");
                        elseIfStmt.body = statements(true, true);
                        currentIf->elseBody.push_back(std::move(elseIfStmt));
                        currentIf = &currentIf->elseBody.back();
                    } else {
                        requireLineEnd("Else");
                        currentIf->elseBody = statements(true);
                        break;
                    }
                }
                endPair("", "If");
            } else if (at(K::Loop)) {
                Token loopTok = pop();
                s.kind = Stmt::Kind::Loop;
                s.line = loopTok.line;
                if (eat(K::Until)) {
                    s.until = true;
                    s.expr = expression();
                    requireLineEnd("Loop until condition");
                } else if (eat(K::While)) {
                    s.whileLoop = true;
                    s.expr = expression();
                    requireLineEnd("Loop while condition");
                } else if (at(K::Id) && (peek(1).kind == K::In || peek(1).kind == K::Comma)) {
                    s.kind = Stmt::Kind::ForIn;
                    Token v1 = pop();
                    s.loopVar = v1.text;
                    if (eat(K::Comma)) {
                        Token v2 = expect(K::Id, "expected variable name after ',' in Loop");
                        s.loopIndexVar = s.loopVar;
                        s.loopVar = v2.text;
                    }
                    expect(K::In, "expected 'in' after loop variable in Loop");
                    s.expr = expression();
                    if (eat(K::DotDot)) {
                        s.isRange = true;
                        s.rangeEnd = expression();
                    }
                    requireLineEnd("Loop in collection");
                } else {
                    requireLineEnd("Loop");
                }
                s.body = statements(true);
                endPair("", "Loop");
            } else if (at(K::Match)) {
                Token matchTok = pop();
                s.kind = Stmt::Kind::Match;
                s.line = matchTok.line;
                s.expr = expression();
                requireLineEnd("Match expression");
                newlines();
                while (at(K::When)) {
                    Token whenTok = pop();
                    MatchCase mc;
                    mc.line = whenTok.line;
                    if (at(K::Id) && t().text == "_") {
                        pop();
                        mc.isWildcard = true;
                    } else {
                        mc.pattern = expression();
                        if (eat(K::DotDot)) {
                            mc.isRange = true;
                            mc.rangeEnd = expression();
                        }
                    }
                    if (eat(K::Where)) {
                        mc.guard = expression();
                    }
                    expect(K::Colon, "expected ':' after When pattern");
                    requireLineEnd("When pattern");
                    mc.body = statementsForMatchCase();
                    s.matchCases.push_back(std::move(mc));
                    newlines();
                }
                if (at(K::Default)) {
                    Token defTok = pop();
                    MatchCase mc;
                    mc.line = defTok.line;
                    mc.isDefault = true;
                    expect(K::Colon, "expected ':' after default");
                    requireLineEnd("default in Match");
                    mc.body = statementsForMatchCase();
                    s.matchCases.push_back(std::move(mc));
                    newlines();
                } else if (at(K::Else)) {
                    fail("Match statements require 'Default:' for the fallback case; 'Else:' is not permitted in Match statements");
                }
                expect(K::End, "expected 'End Match'");
                expect(K::Match, "expected 'Match' after 'End'");
            } else if (at(K::Lock) || (at(K::Id) && (t().text == "Lock" || t().text == "lock") && pos + 1 < ts.size() && ts[pos + 1].kind != K::Equal && ts[pos + 1].kind != K::Colon)) {
                pop();
                s.kind = Stmt::Kind::Lock;
                s.expr = expression();
                requireLineEnd("Lock");
                s.body = statements(true);
                expect(K::End, "expected 'End Lock'");
                if (at(K::Lock) || (at(K::Id) && (t().text == "Lock" || t().text == "lock"))) {
                    pop();
                } else {
                    fail("expected 'Lock' after 'End'");
                }
            } else {
                s = parseSingleStmt();
            }
            out.push_back(std::move(s));
            if (!at(K::Newline) && !at(K::Eof) && !at(K::When) &&
                !(at(K::Else) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Colon) &&
                !(at(K::End) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Match))
                fail("expected end of statement");
            newlines();
        }
        return out;
    }
    std::vector<Stmt> statements(bool nested, bool allowElse = false) {
        std::vector<Stmt> out;
        bool hasEnsureInThisBlock = false;
        newlines();
        while (!at(K::Eof) && !(nested && at(K::End)) && !(allowElse && at(K::Else))) {
            if (!nested && at(K::End))
                break;
            if (at(K::Block))
                fail("missing 'End <function-name>' before next Block");
            Token x = t();
            Stmt s;
            s.line = x.line;
            if (eat(K::Ensure)) {
                if (hasEnsureInThisBlock) {
                    fail("block cannot contain multiple Ensure blocks; consolidate cleanup into a single Ensure block");
                }
                hasEnsureInThisBlock = true;
                s.kind = Stmt::Kind::Ensure;
                expect(K::LBrace, "expected '{' after Ensure");
                newlines();
                if (at(K::RBrace)) fail("Ensure requires at least one cleanup statement");
                while (!at(K::RBrace) && !at(K::Eof)) {
                    if (at(K::Give) || at(K::Exit) || at(K::Skip) || at(K::Ensure))
                        fail("control-flow statements are not valid inside Ensure");
                    s.body.push_back(parseSingleStmt());
                    newlines();
                }
                expect(K::RBrace, "expected '}' after Ensure cleanup statement");
                if (s.body.empty()) fail("Ensure requires at least one cleanup statement");
                s.ensuredStmt = std::make_shared<Stmt>(cloneStmt(s.body.front()));
            } else if (at(K::If)) {
                pop();
                s.kind = Stmt::Kind::If;
                s.expr = expression();
                requireLineEnd("If condition");
                s.body = statements(true, true);
                Stmt* currentIf = &s;
                while (eat(K::Else)) {
                    if (eat(K::If)) {
                        Stmt elseIfStmt;
                        elseIfStmt.kind = Stmt::Kind::If;
                        elseIfStmt.line = t().line;
                        elseIfStmt.expr = expression();
                        requireLineEnd("Else If condition");
                        elseIfStmt.body = statements(true, true);
                        currentIf->elseBody.push_back(std::move(elseIfStmt));
                        currentIf = &currentIf->elseBody.back();
                    } else {
                        requireLineEnd("Else");
                        currentIf->elseBody = statements(true);
                        break;
                    }
                }
                endPair("", "If");
            } else if (at(K::Loop)) {
                Token loopTok = pop();
                s.kind = Stmt::Kind::Loop;
                s.line = loopTok.line;
                if (eat(K::Until)) {
                    s.until = true;
                    s.expr = expression();
                    requireLineEnd("Loop until condition");
                } else if (eat(K::While)) {
                    s.whileLoop = true;
                    s.expr = expression();
                    requireLineEnd("Loop while condition");
                } else if (at(K::Id) && (peek(1).kind == K::In || peek(1).kind == K::Comma)) {
                    s.kind = Stmt::Kind::ForIn;
                    Token v1 = pop();
                    s.loopVar = v1.text;
                    if (eat(K::Comma)) {
                        Token v2 = expect(K::Id, "expected variable name after ',' in Loop");
                        s.loopIndexVar = s.loopVar;
                        s.loopVar = v2.text;
                    }
                    expect(K::In, "expected 'in' after loop variable in Loop");
                    s.expr = expression();
                    if (eat(K::DotDot)) {
                        s.isRange = true;
                        s.rangeEnd = expression();
                    }
                    requireLineEnd("Loop in collection");
                } else {
                    requireLineEnd("Loop");
                }
                s.body = statements(true);
                endPair("", "Loop");
            } else if (at(K::Match)) {
                Token matchTok = pop();
                s.kind = Stmt::Kind::Match;
                s.line = matchTok.line;
                s.expr = expression();
                requireLineEnd("Match expression");
                newlines();
                while (at(K::When)) {
                    Token whenTok = pop();
                    MatchCase mc;
                    mc.line = whenTok.line;
                    if (at(K::Id) && t().text == "_") {
                        pop();
                        mc.isWildcard = true;
                    } else {
                        mc.pattern = expression();
                        if (eat(K::DotDot)) {
                            mc.isRange = true;
                            mc.rangeEnd = expression();
                        }
                    }
                    if (eat(K::Where)) {
                        mc.guard = expression();
                    }
                    expect(K::Colon, "expected ':' after When pattern");
                    requireLineEnd("When pattern");
                    mc.body = statementsForMatchCase();
                    s.matchCases.push_back(std::move(mc));
                    newlines();
                }
                if (at(K::Default)) {
                    Token defTok = pop();
                    MatchCase mc;
                    mc.line = defTok.line;
                    mc.isDefault = true;
                    expect(K::Colon, "expected ':' after default");
                    requireLineEnd("default in Match");
                    mc.body = statementsForMatchCase();
                    s.matchCases.push_back(std::move(mc));
                    newlines();
                } else if (at(K::Else)) {
                    fail("Match statements require 'Default:' for the fallback case; 'Else:' is not permitted in Match statements");
                }
                expect(K::End, "expected 'End Match'");
                expect(K::Match, "expected 'Match' after 'End'");
            } else if (at(K::Lock) || (at(K::Id) && (t().text == "Lock" || t().text == "lock") && pos + 1 < ts.size() && ts[pos + 1].kind != K::Equal && ts[pos + 1].kind != K::Colon)) {
                pop();
                s.kind = Stmt::Kind::Lock;
                s.expr = expression();
                requireLineEnd("Lock");
                s.body = statements(true);
                expect(K::End, "expected 'End Lock'");
                if (at(K::Lock) || (at(K::Id) && (t().text == "Lock" || t().text == "lock"))) {
                    pop();
                } else {
                    fail("expected 'Lock' after 'End'");
                }
            } else {
                s = parseSingleStmt();
            }
            out.push_back(std::move(s));
            if (!at(K::Newline) && !at(K::Eof) && !at(K::End))
                fail("expected end of statement");
            newlines();
        }
        return out;
    }
    VarDecl declaration([[maybe_unused]] bool global = false) {
        bool isRef = eat(K::Ref);
        bool isMut = false;
        if (!isRef) isMut = eat(K::Mut);
        Token n = expect(K::Id, "expected identifier in declaration");
        expect(K::Colon, "expected ':' after '" + n.text + "'");
        bool constant = eat(K::Constant);
        Type typ = parseTypeRef();
        VarDecl value(n.text, std::move(typ), n.line, constant, nullptr, n.sourceIndex, isRef, isMut);
        if (eat(K::Assign)) value.initializer = expression();
        if (constant && !value.initializer)
            throw Error("line " + std::to_string(n.line) + ": Constant '" + n.text + "' requires an initializer");
        return value;
    }
    void parseList(std::vector<VarDecl>& out, const std::vector<K>& stops, const std::string& listName) {
        newlines();
        auto stopped = [&]() {
            for (K k : stops)
                if (at(k))
                    return true;
            return false;
        };
        while (!stopped()) {
            if (at(K::LParen))
                fail("parenthesized Takes syntax is not part of Loom; use multiline declarations");
            if (at(K::Eof) || at(K::End) || at(K::Block) || at(K::Implements) || at(K::Produces) || at(K::Declares) ||
                at(K::Takes))
                fail("expected " + listName + " declaration or its closing header keyword");
            out.push_back(declaration());
            if (at(K::Comma)) {
                pop();
            } else if (!at(K::Newline) && !stopped()) {
                fail("expected newline after " + listName + " declaration");
            }
            newlines();
        }
    }
    std::string mangledOpName(const std::string& opSym) {
        if (opSym == "+") return "add";
        if (opSym == "-") return "sub";
        if (opSym == "*") return "mul";
        if (opSym == "/") return "div";
        if (opSym == "mod") return "mod";
        if (opSym == "%") return "pct";
        if (opSym == "&") return "band";
        if (opSym == "|") return "bor";
        if (opSym == "^") return "bxor";
        if (opSym == "lsl") return "lsl";
        if (opSym == "lsr") return "lsr";
        if (opSym == "==") return "eq";
        if (opSym == "!=") return "ne";
        if (opSym == "<") return "lt";
        if (opSym == "<=") return "le";
        if (opSym == ">") return "gt";
        if (opSym == ">=") return "ge";
        if (opSym == "[]") return "get";
        if (opSym == "[]=") return "set";
        return "op";
    }

    std::string safeTypeName(const Type& t) {
        std::string s = typeName(t);
        for (char& c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        }
        return s;
    }

    void matchOperatorSymbol(const std::string& opSym) {
        if (opSym == "+") expect(K::Plus, "expected '+' after End");
        else if (opSym == "-") expect(K::Minus, "expected '-' after End");
        else if (opSym == "*") expect(K::Star, "expected '*' after End");
        else if (opSym == "/") expect(K::Slash, "expected '/' after End");
        else if (opSym == "mod") {
            if (eat(K::Mod) || (at(K::Id) && (t().text == "Mod" || t().text == "mod"))) {
                if (at(K::Id)) pop();
            } else fail("expected 'mod' after End");
        }
        else if (opSym == "%") expect(K::Percent, "expected '%' after End");
        else if (opSym == "&") expect(K::Ampersand, "expected '&' after End");
        else if (opSym == "|") expect(K::Pipe, "expected '|' after End");
        else if (opSym == "^") expect(K::Caret, "expected '^' after End");
        else if (opSym == "lsl") {
            if (eat(K::LSL) || (at(K::Id) && (t().text == "Lsl" || t().text == "lsl"))) {
                if (at(K::Id)) pop();
            } else fail("expected 'lsl' after End");
        }
        else if (opSym == "lsr") {
            if (eat(K::LSR) || (at(K::Id) && (t().text == "Lsr" || t().text == "lsr"))) {
                if (at(K::Id)) pop();
            } else fail("expected 'lsr' after End");
        }
        else if (opSym == "==") expect(K::Equal, "expected '==' after End");
        else if (opSym == "!=") expect(K::NotEqual, "expected '!=' after End");
        else if (opSym == "<") expect(K::Less, "expected '<' after End");
        else if (opSym == "<=") expect(K::LessEqual, "expected '<=' after End");
        else if (opSym == ">") expect(K::Greater, "expected '>' after End");
        else if (opSym == ">=") expect(K::GreaterEqual, "expected '>=' after End");
        else if (opSym == "[]") {
            expect(K::LBracket, "expected '[' after End");
            expect(K::RBracket, "expected ']' after '['");
        }
        else if (opSym == "[]=") {
            expect(K::LBracket, "expected '[' after End");
            expect(K::RBracket, "expected ']' after '['");
            expect(K::Assign, "expected '=' after '[]'");
        }
    }

    Function operatorBlock() {
        Token opTok = expect(K::Operator, "expected Operator");
        std::string opSym;
        if (eat(K::Plus)) opSym = "+";
        else if (eat(K::Minus)) opSym = "-";
        else if (eat(K::Star)) opSym = "*";
        else if (eat(K::Slash)) opSym = "/";
        else if (eat(K::Mod) || (at(K::Id) && (t().text == "Mod" || t().text == "mod"))) { if (at(K::Id)) pop(); opSym = "mod"; }
        else if (eat(K::Percent)) opSym = "%";
        else if (eat(K::Ampersand)) opSym = "&";
        else if (eat(K::Pipe)) opSym = "|";
        else if (eat(K::Caret)) opSym = "^";
        else if (eat(K::LSL) || (at(K::Id) && (t().text == "Lsl" || t().text == "lsl"))) { if (at(K::Id)) pop(); opSym = "lsl"; }
        else if (eat(K::LSR) || (at(K::Id) && (t().text == "Lsr" || t().text == "lsr"))) { if (at(K::Id)) pop(); opSym = "lsr"; }
        else if (eat(K::Equal)) opSym = "==";
        else if (eat(K::NotEqual)) opSym = "!=";
        else if (eat(K::Less)) opSym = "<";
        else if (eat(K::LessEqual)) opSym = "<=";
        else if (eat(K::Greater)) opSym = ">";
        else if (eat(K::GreaterEqual)) opSym = ">=";
        else if (eat(K::LBracket)) {
            expect(K::RBracket, "expected ']' in operator declaration");
            if (eat(K::Assign)) opSym = "[]=";
            else opSym = "[]";
        } else {
            fail("expected operator symbol after 'Operator'");
        }

        Function f;
        f.line = opTok.line;
        f.sourceIndex = opTok.sourceIndex;
        f.isOperator = true;
        f.operatorSymbol = opSym;

        newlines();
        auto parseBlockUses = [&]() {
            newlines();
            while (eat(K::Use)) {
                Token u = expect(K::Id, "expected module name after use");
                requireLineEnd("use declaration");
                f.uses.push_back(u);
                newlines();
            }
        };
        parseBlockUses();
        if (eat(K::Takes)) {
            parseList(f.params, {K::Produces, K::Declares, K::Implements, K::Use}, "parameter");
            parseBlockUses();
        }
        newlines();
        parseBlockUses();
        if (eat(K::Produces)) {
            newlines();
            bool hasImplements = false;
            for (size_t look = pos; look < ts.size(); ++look) {
                if (ts[look].kind == K::Implements) {
                    hasImplements = true;
                    break;
                }
                if (ts[look].kind == K::End || ts[look].kind == K::Block || ts[look].kind == K::Operator || ts[look].kind == K::Eof) {
                    break;
                }
            }
            if (!hasImplements) {
                f.isConcise = true;
                auto retExpr = expression();
                requireLineEnd("concise Produces");
                Stmt giveStmt;
                giveStmt.line = retExpr->line;
                giveStmt.kind = Stmt::Kind::Give;
                giveStmt.expr = std::move(retExpr);
                f.body.push_back(std::move(giveStmt));
                Token end = expect(K::End, "expected 'End " + opSym + "' to close Operator '" + opSym + "'");
                matchOperatorSymbol(opSym);
                requireLineEnd("End Operator");
                newlines();
                f.name = "__loom_op_" + mangledOpName(f.operatorSymbol);
                for (const auto& p : f.params) f.name += "_" + safeTypeName(p.type);
                return f;
            } else {
                f.result = parseTypeRef();
                newlines();
                if (eat(K::Or)) {
                    f.errorResult = parseTypeRef();
                    newlines();
                } else if (f.result && f.result->errorType) {
                    f.errorResult = *f.result->errorType;
                    f.result->errorType = nullptr;
                }
            }
        }
        parseBlockUses();
        if (eat(K::Declares)) {
            parseList(f.locals, {K::Implements, K::Use}, "local");
            parseBlockUses();
        }
        parseBlockUses();
        expect(K::Implements, "expected Implements after operator header");
        requireLineEnd("Implements");
        f.body = statements(false);
        Token end = expect(K::End, "expected 'End " + opSym + "' to close Operator '" + opSym + "'");
        matchOperatorSymbol(opSym);
        requireLineEnd("End Operator");
        newlines();
        f.name = "__loom_op_" + mangledOpName(f.operatorSymbol);
        for (const auto& p : f.params) f.name += "_" + safeTypeName(p.type);
        return f;
    }

    Function function(bool isAsync = false) {
        if (!isAsync && eat(K::Async)) isAsync = true;
        Token b = expect(K::Block, "expected Block");
        Function f;
        f.line = b.line;
        f.isAsync = isAsync;
        Token name = expect(K::Id, "expected function name after Block");
        f.name = name.text;
        f.sourceIndex = b.sourceIndex;
        if (eat(K::LBracket)) {
            do {
                Token tp = expect(K::Id, "expected type parameter name");
                f.typeParams.push_back(tp.text);
            } while (eat(K::Comma));
            expect(K::RBracket, "expected ']' after type parameters");
        }
        for (const auto& tp : f.typeParams) currentTypeParams.insert(tp);
        if (at(K::LParen))
            fail("parenthesized Takes syntax is not part of Loom; use multiline 'Takes' declarations");
        newlines();
        auto parseBlockUses = [&]() {
            newlines();
            while (eat(K::Use)) {
                Token u = expect(K::Id, "expected module name after use");
                requireLineEnd("use declaration");
                f.uses.push_back(u);
                newlines();
            }
        };
        parseBlockUses();
        if (eat(K::Takes)) {
            parseList(f.params, {K::Produces, K::Declares, K::Implements, K::Use}, "parameter");
            parseBlockUses();
        }
        newlines();
        parseBlockUses();
        if (eat(K::Produces)) {
            newlines();
            bool hasImplements = false;
            for (size_t look = pos; look < ts.size(); ++look) {
                if (ts[look].kind == K::Implements) {
                    hasImplements = true;
                    break;
                }
                if (ts[look].kind == K::End || ts[look].kind == K::Block || ts[look].kind == K::Eof) {
                    break;
                }
            }
            if (!hasImplements) {
                f.isConcise = true;
                auto retExpr = expression();
                requireLineEnd("concise Produces");
                Stmt giveStmt;
                giveStmt.line = retExpr->line;
                giveStmt.kind = Stmt::Kind::Give;
                giveStmt.expr = std::move(retExpr);
                f.body.push_back(std::move(giveStmt));
                Token end = expect(K::End, "expected 'End " + f.name + "' to close Block '" + f.name + "'");
                Token close = expect(K::Id, "expected function name after End");
                if (close.text != f.name)
                    throw Error("line " + std::to_string(end.line) + ": mismatched block ending: expected 'End " + f.name +
                                "', found 'End " + close.text + "'");
                if (!at(K::Newline) && !at(K::Eof))
                    fail("expected newline after 'End " + f.name + "'");
                newlines();
                for (const auto& tp : f.typeParams) currentTypeParams.erase(tp);
                return f;
            } else {
                f.result = parseTypeRef();
                newlines();
                if (eat(K::Or)) {
                    f.errorResult = parseTypeRef();
                    newlines();
                } else if (f.result && f.result->errorType) {
                    f.errorResult = *f.result->errorType;
                    f.result->errorType = nullptr;
                }
            }
        }
        parseBlockUses();
        if (eat(K::Declares)) {
            parseList(f.locals, {K::Implements, K::Use}, "local");
            parseBlockUses();
        }
        parseBlockUses();
        expect(K::Implements, "expected Implements after function header");
        requireLineEnd("Implements");
        f.body = statements(false);
        Token end = expect(K::End, "expected 'End " + f.name + "' to close Block '" + f.name + "'");
        Token close = expect(K::Id, "expected function name after End");
        if (close.text != f.name)
            throw Error("line " + std::to_string(end.line) + ": mismatched block ending: expected 'End " + f.name +
                                "', found 'End " + close.text + "'");
        if (!at(K::Newline) && !at(K::Eof))
            fail("expected newline after 'End " + f.name + "'");
        newlines();
        for (const auto& tp : f.typeParams) currentTypeParams.erase(tp);
        if (!f.typeParams.empty()) {
            genericFunctionTemplates.insert_or_assign(f.name, cloneFunction(f));
        }
        return f;
    }
    Function testBlock() {
        Token desc = expect(K::String, "expected test description string");
        Function f;
        f.line = desc.line;
        f.name = "__loom_test_" + std::to_string(++testCounter);
        f.sourceIndex = desc.sourceIndex;
        f.isTest = true;
        f.testDescription = desc.text;
        requireLineEnd("Test header");
        newlines();
        auto parseBlockUses = [&]() {
            newlines();
            while (eat(K::Use)) {
                Token u = expect(K::Id, "expected module name after use");
                requireLineEnd("use declaration");
                f.uses.push_back(u);
                newlines();
            }
        };
        parseBlockUses();
        if (eat(K::Declares)) {
            parseList(f.locals, {K::Implements, K::Use}, "local");
            parseBlockUses();
        }
        parseBlockUses();
        expect(K::Implements, "expected Implements in Test");
        requireLineEnd("Implements");
        f.body = statements(false);
        expect(K::End, "expected 'End Test'");
        if (at(K::Id) && (t().text == "Test" || t().text == "test")) pop();
        else fail("expected 'Test' after End");
        requireLineEnd("End Test");
        newlines();
        return f;
    }
    void enumeration() {
        Token start = expect(K::Enum, "expected Enum declaration");
        Token name = expect(K::Id, "expected enum name");
        expect(K::Colon, "expected ':' and an underlying integer type after enum name");
        Token underlyingToken = expect(K::Id, "expected enum underlying type");
        Type underlying = parseType(underlyingToken.text, underlyingToken.line);
        if (!underlying.integer())
            throw Error("line " + std::to_string(underlyingToken.line) + ": enum underlying type must be a fixed-width integer");
        bool isDuplicateEnum = std::any_of(enumDeclarations.begin(), enumDeclarations.end(), [&](const EnumDecl& d) {
            return d.name == name.text;
        });
        if (isDuplicateEnum) throw Error("line " + std::to_string(name.line) + ": duplicate enum '" + name.text + "'");
        EnumDecl decl{name.text, underlying, {}, start.line, start.sourceIndex};
        enumTypes.insert_or_assign(name.text, Type::enumType(name.text, underlying.bits, underlying.kind == TypeKind::Signed));
        requireLineEnd("enum header");
        newlines();
        while (!(at(K::End) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Id && ts[pos + 1].text == name.text)) {
            if (at(K::Eof)) fail("expected 'End " + name.text + "'");
            Token variant = expect(K::Id, "expected enum variant name");
            expect(K::Assign, "enum variants require explicit values using '='");
            bool negative = eat(K::Minus);
            Token number = expect(K::Number, "expected integer enum value");
            decl.variants.push_back({variant.text, number.magnitude, negative, variant.line});
            if (!at(K::Newline) && !at(K::End)) fail("expected newline after enum variant");
            newlines();
        }
        expect(K::End, "expected enum ending");
        Token close = expect(K::Id, "expected enum name after End");
        if (close.text != name.text) throw Error("line " + std::to_string(close.line) + ": mismatched enum ending; expected 'End " + name.text + "'");
        if (!at(K::Newline) && !at(K::Eof)) fail("expected newline after enum declaration");
        newlines();
        enumDeclarations.push_back(std::move(decl));
    }
    void record(bool isPacked = false) {
        Token start = expect(K::Record, "expected Record declaration");
        Token name = expect(K::Id, "expected record name");
        std::vector<std::string> typeParams;
        if (eat(K::LBracket)) {
            do {
                Token tp = expect(K::Id, "expected type parameter name");
                typeParams.push_back(tp.text);
            } while (eat(K::Comma));
            expect(K::RBracket, "expected ']' after type parameters");
        }
        unsigned alignment = 0;
        if (eat(K::Align)) {
            Token a = expect(K::Number, "expected alignment integer after Align");
            alignment = static_cast<unsigned>(a.magnitude);
        }
        bool isDuplicateRecord = std::any_of(recordDeclarations.begin(), recordDeclarations.end(), [&](const RecordDecl& d) {
            return d.name == name.text;
        });
        if (isDuplicateRecord)
            throw Error("line " + std::to_string(name.line) + ": duplicate type '" + name.text + "'");
        RecordDecl decl{name.text, {}, start.line, start.sourceIndex, isPacked, alignment, typeParams};
        requireLineEnd("record header");
        newlines();
        for (const auto& tp : typeParams) currentTypeParams.insert(tp);
        std::unordered_set<std::string> fields;
        while (!(at(K::End) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Id && ts[pos + 1].text == name.text)) {
            if (at(K::Eof)) fail("expected 'End " + name.text + "'");
            if (eat(K::Drop)) {
                decl.hasDrop = true;
                requireLineEnd("Drop header");
                newlines();
                decl.dropBody = statements(true);
                break;
            }
            Token field = expect(K::Id, "expected record field name");
            if (!fields.insert(field.text).second)
                throw Error("line " + std::to_string(field.line) + ": duplicate field '" + field.text + "' in record '" + name.text + "'");
            expect(K::Colon, "expected ':' after record field name");
            decl.fields.push_back({field.text, parseTypeRef(), field.line});
            if (!at(K::Newline) && !at(K::End)) fail("expected newline after record field");
            newlines();
        }
        for (const auto& tp : typeParams) currentTypeParams.erase(tp);
        expect(K::End, "expected record ending");
        Token close = expect(K::Id, "expected record name after End");
        if (close.text != name.text) throw Error("line " + std::to_string(close.line) + ": mismatched record ending; expected 'End " + name.text + "'");
        if (decl.fields.empty()) throw Error("line " + std::to_string(start.line) + ": Record must declare at least one field");
        recordTypes.insert_or_assign(name.text, Type::recordType(name.text));
        if (!decl.typeParams.empty()) {
            genericRecordTemplates[name.text] = decl;
        } else {
            recordDeclarations.push_back(std::move(decl));
        }
        if (!at(K::Newline) && !at(K::Eof)) fail("expected newline after record declaration");
        newlines();
    }
    void globalBlock() {
        expect(K::Block, "expected Block Global");
        Token name = expect(K::Id, "expected Global after Block");
        if (name.text != "Global") fail("expected Block Global");
        requireLineEnd("Block Global");
        expect(K::Declares, "expected Declares inside Block Global");
        newlines();
        while (!(at(K::End) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Id && ts[pos + 1].text == "Global")) {
            if (at(K::Eof)) fail("expected End Global");
            globalDeclarations.push_back(declaration(true));
            if (!at(K::Newline) && !at(K::End)) fail("expected newline after global declaration");
            newlines();
        }
        expect(K::End, "expected End Global");
        pop();
        if (!at(K::Newline) && !at(K::Eof)) fail("expected newline after End Global");
        newlines();
    }

    Function externFunction() {
        Function f;
        f.isExtern = true;
        Token name = expect(K::Id, "expected function name after Extern Block");
        f.name = name.text;
        f.line = name.line;
        f.sourceIndex = name.sourceIndex;
        newlines();
        if (eat(K::Takes)) {
            parseList(f.params, {K::Produces, K::End}, "parameter");
        }
        newlines();
        if (eat(K::Produces)) {
            newlines();
            f.result = parseTypeRef();
            newlines();
        }
        Token end = expect(K::End, "expected 'End " + f.name + "' to close Extern Block '" + f.name + "'");
        Token close = expect(K::Id, "expected function name after End");
        if (close.text != f.name)
            throw Error("line " + std::to_string(end.line) + ": mismatched block ending: expected 'End " + f.name +
                        "', found 'End " + close.text + "'");
        if (!at(K::Newline) && !at(K::Eof))
            fail("expected newline after 'End " + f.name + "'");
        newlines();
        return f;
    }

    public:
    explicit Parser(std::vector<Token> x) : ts(std::move(x)) {}
    std::vector<Function> run() {
        preScanTypes();
        std::vector<Function> fs;
        newlines();
        while (!at(K::Eof)) {
            if (at(K::End))
                fail("unexpected End outside a nested construct or Block");
            if (eat(K::Module)) {
                Token m = expect(K::Id, "expected a module name after Module");
                requireLineEnd("Module declaration");
                fileModules[m.sourceIndex] = m.text;
                continue;
            }
            if (eat(K::Import)) {
                Token imp = expect(K::Id, "expected module name after Import");
                requireLineEnd("Import declaration");
                fileImports[imp.sourceIndex].insert(lower(imp.text));
                continue;
            }
            if (eat(K::Use)) {
                Token u = expect(K::Id, "expected module name after use");
                requireLineEnd("use declaration");
                fileImports[u.sourceIndex].insert(lower(u.text));
                fileUses[u.sourceIndex].insert(lower(u.text));
                continue;
            }
            if (eat(K::Extern)) {
                if (at(K::String)) pop(); // optional "c" ABI string
                expect(K::Block, "expected Block after Extern");
                fs.push_back(externFunction());
                continue;
            }
            if (at(K::Enum)) { enumeration(); continue; }
            if (at(K::Packed)) {
                pop();
                record(/*isPacked=*/true);
                continue;
            }
            if (at(K::Record)) { record(); continue; }
            if (at(K::Id) && (t().text == "Test" || t().text == "test") && pos + 1 < ts.size() && ts[pos + 1].kind == K::String) {
                pop();
                fs.push_back(testBlock());
                continue;
            }
            if (at(K::Block) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Id &&
                ts[pos + 1].text == "Global") {
                globalBlock();
                continue;
            }
            if (at(K::Operator)) {
                fs.push_back(operatorBlock());
                continue;
            }
            if (at(K::Async)) {
                pop();
                Function fn = function(/*isAsync=*/true);
                if (fn.typeParams.empty()) {
                    fs.push_back(std::move(fn));
                }
                continue;
            }
            Function fn = function();
            if (fn.typeParams.empty()) {
                fs.push_back(std::move(fn));
            }
        }
        for (auto& pf : pendingInstantiatedFunctions) {
            fs.push_back(std::move(pf));
        }
        return fs;
    }
    const std::vector<VarDecl>& globals() const { return globalDeclarations; }
    const std::vector<EnumDecl>& enums() const { return enumDeclarations; }
    const std::vector<RecordDecl>& records() const { return recordDeclarations; }
    const std::unordered_map<std::size_t, std::string>& modules() const { return fileModules; }
    const std::unordered_map<std::size_t, std::unordered_set<std::string>>& imports() const { return fileImports; }
    const std::unordered_map<std::size_t, std::unordered_set<std::string>>& uses() const { return fileUses; }
};


} // namespace loom
