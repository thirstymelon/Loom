#include <cerrno>
#include <algorithm>
#include <cctype>
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
#include "targets/TargetProfile.h"

namespace loom {
struct SourceSpan {
    std::size_t source = std::numeric_limits<std::size_t>::max();
    int line = 0;
    int column = 0;
    int length = 1;
};
thread_local std::size_t currentSourceIndex = 0;
struct Error : std::runtime_error {
    SourceSpan span;
    explicit Error(const std::string& message, SourceSpan location = {}) : std::runtime_error(message), span(location) {
        if (span.source == std::numeric_limits<std::size_t>::max()) span.source = currentSourceIndex;
        if (span.line == 0) {
            std::smatch match;
            const std::string text = message;
            if (std::regex_search(text, match, std::regex(R"((?:^|\b)line ([0-9]+):)"))) span.line = std::stoi(match[1]);
        }
    }
};

enum class TypeKind { Invalid, Boolean, Signed, Unsigned, Float, String, Character, Enum, Record, Pointer, IntegerLiteral, Array, Slice, NullType, TaskHandle, BlockPointer, Text, Vector, FileHandle, Tuple, HMap };
struct Type {
    TypeKind kind = TypeKind::Invalid;
    unsigned bits = 0;
    std::string name;
    bool enumSigned = false;
    unsigned arraySize = 0;
    bool hasExplicitSize = false;
    std::shared_ptr<Type> pointeeType = nullptr;
    bool isVolatile = false;
    std::shared_ptr<Type> errorType = nullptr;
    std::vector<Type> paramTypes;
    std::shared_ptr<Type> keyType = nullptr;
    std::shared_ptr<Type> valueType = nullptr;
    std::vector<Type> tupleTypes;

    Type() = default;
    Type(TypeKind k, unsigned b = 0, std::string n = "", bool es = false)
        : kind(k), bits(b), name(std::move(n)), enumSigned(es), arraySize(0), hasExplicitSize(false), pointeeType(nullptr), isVolatile(false), errorType(nullptr) {}

    static Type boolean() {
        return {TypeKind::Boolean, 1};
    }
    static Type signedInt(unsigned n) {
        return {TypeKind::Signed, n};
    }
    static Type unsignedInt(unsigned n) {
        return {TypeKind::Unsigned, n};
    }
    static Type floating(unsigned n) { return {TypeKind::Float, n}; }
    static Type string() { return {TypeKind::String, 0}; }
    static Type character() { return {TypeKind::Character, 32}; }
    static Type enumType(std::string n, unsigned width, bool isSigned) {
        Type t{TypeKind::Enum, width}; t.name = std::move(n); t.enumSigned = isSigned; return t;
    }
    static Type recordType(std::string n) { Type t{TypeKind::Record, 0}; t.name = std::move(n); return t; }
    static Type pointer(Type pointee) {
        Type t{TypeKind::Pointer, 64};
        t.pointeeType = std::make_shared<Type>(std::move(pointee));
        return t;
    }
    static Type tupleType(std::vector<Type> elems) {
        Type t{TypeKind::Tuple, 0};
        t.tupleTypes = std::move(elems);
        return t;
    }
    static Type hmapType(Type k, Type v) {
        Type t{TypeKind::HMap, 64};
        t.keyType = std::make_shared<Type>(std::move(k));
        t.valueType = std::make_shared<Type>(std::move(v));
        return t;
    }
    static Type sliceType(Type elem) {
        Type t{TypeKind::Slice, 0};
        t.pointeeType = std::make_shared<Type>(std::move(elem));
        return t;
    }
    static Type taskHandle(Type resType) {
        Type t{TypeKind::TaskHandle, 64};
        t.pointeeType = std::make_shared<Type>(std::move(resType));
        return t;
    }
    static Type blockPointer(Type retType, std::vector<Type> params = {}) {
        Type t{TypeKind::BlockPointer, 64};
        t.pointeeType = std::make_shared<Type>(std::move(retType));
        t.paramTypes = std::move(params);
        return t;
    }
    static Type textType(unsigned cap = 64) {
        Type t{TypeKind::Text, 64};
        t.arraySize = cap;
        return t;
    }
    static Type vectorType(Type elem, unsigned cap = 0, bool hasSize = false) {
        Type t{TypeKind::Vector, 64};
        t.pointeeType = std::make_shared<Type>(std::move(elem));
        t.arraySize = cap;
        t.hasExplicitSize = hasSize;
        return t;
    }
    static Type fileHandle() {
        Type t{TypeKind::FileHandle, 64};
        t.name = "File_Handle";
        return t;
    }
    static Type arrayType(Type elem, unsigned size) {
        Type t{TypeKind::Array, 0};
        t.pointeeType = std::make_shared<Type>(std::move(elem));
        t.arraySize = size;
        return t;
    }
    static Type nullType() {
        return {TypeKind::NullType, 0};
    }
    static Type literal() {
        return {TypeKind::IntegerLiteral, 0};
    }
    bool integer() const {
        return kind == TypeKind::Signed || kind == TypeKind::Unsigned;
    }
    bool numeric() const { return integer() || kind == TypeKind::Float; }
    bool operator==(const Type& o) const {
        if (kind == TypeKind::NullType && (o.kind == TypeKind::Pointer || o.kind == TypeKind::BlockPointer)) return true;
        if ((kind == TypeKind::Pointer || kind == TypeKind::BlockPointer) && o.kind == TypeKind::NullType) return true;
        if (kind != o.kind || bits != o.bits || name != o.name || enumSigned != o.enumSigned)
            return false;
        if (kind == TypeKind::Pointer || kind == TypeKind::Slice || kind == TypeKind::TaskHandle) {
            if (!pointeeType || !o.pointeeType) return pointeeType == o.pointeeType;
            return *pointeeType == *o.pointeeType;
        }
        if (kind == TypeKind::BlockPointer) {
            if (!pointeeType || !o.pointeeType) {
                if (pointeeType != o.pointeeType) return false;
            } else if (*pointeeType != *o.pointeeType) {
                return false;
            }
            if (paramTypes.size() != o.paramTypes.size()) return false;
            for (size_t i = 0; i < paramTypes.size(); ++i) {
                if (paramTypes[i] != o.paramTypes[i]) return false;
            }
            return true;
        }
        if (kind == TypeKind::Text) {
            return o.kind == TypeKind::Text;
        }
        if (kind == TypeKind::Vector) {
            if (o.kind != TypeKind::Vector) return false;
            if (!pointeeType || !o.pointeeType) return pointeeType == o.pointeeType;
            return *pointeeType == *o.pointeeType;
        }
        if (kind == TypeKind::FileHandle) {
            return o.kind == TypeKind::FileHandle;
        }
        if (kind == TypeKind::Array) {
            if (arraySize != o.arraySize) return false;
            if (!pointeeType || !o.pointeeType) return pointeeType == o.pointeeType;
            return *pointeeType == *o.pointeeType;
        }
        if (kind == TypeKind::Tuple) {
            if (o.kind != TypeKind::Tuple || tupleTypes.size() != o.tupleTypes.size()) return false;
            for (size_t i = 0; i < tupleTypes.size(); ++i) {
                if (tupleTypes[i] != o.tupleTypes[i]) return false;
            }
            return true;
        }
        if (kind == TypeKind::HMap) {
            if (o.kind != TypeKind::HMap) return false;
            if (!keyType || !o.keyType || !valueType || !o.valueType) return false;
            return *keyType == *o.keyType && *valueType == *o.valueType;
        }
        if (errorType || o.errorType) {
            if (!errorType || !o.errorType) return false;
            if (*errorType != *o.errorType) return false;
        }
        return true;
    }
    bool operator!=(const Type& o) const {
        return !(*this == o);
    }
};
std::string typeName(Type t) {
    std::string prefix = t.isVolatile ? "volatile " : "";
    if (t.errorType) {
        Type base = t;
        base.errorType = nullptr;
        return prefix + typeName(base) + " or " + typeName(*t.errorType);
    }
    if (t.kind == TypeKind::Boolean)
        return prefix + "Boolean";
    if (t.kind == TypeKind::Signed)
        return prefix + "Signed_" + std::to_string(t.bits);
    if (t.kind == TypeKind::Unsigned)
        return prefix + "Unsigned_" + std::to_string(t.bits);
    if (t.kind == TypeKind::Float)
        return prefix + "Float_" + std::to_string(t.bits);
    if (t.kind == TypeKind::String) return prefix + "String";
    if (t.kind == TypeKind::Character) return prefix + "Character";
    if (t.kind == TypeKind::Enum) return prefix + t.name;
    if (t.kind == TypeKind::Record) return prefix + t.name;
    if (t.kind == TypeKind::Pointer)
        return prefix + typeName(*t.pointeeType) + " Pointer";
    if (t.kind == TypeKind::Slice)
        return prefix + typeName(*t.pointeeType) + " Slice";
    if (t.kind == TypeKind::TaskHandle)
        return prefix + (t.pointeeType ? typeName(*t.pointeeType) : "Null") + " Task_Handle";
    if (t.kind == TypeKind::BlockPointer) {
        std::string s = prefix;
        if (t.pointeeType && t.pointeeType->kind != TypeKind::NullType) s += typeName(*t.pointeeType) + " ";
        s += "Block_Pointer";
        if (!t.paramTypes.empty()) {
            s += "(";
            for (size_t i = 0; i < t.paramTypes.size(); ++i) {
                if (i > 0) s += ", ";
                s += typeName(t.paramTypes[i]);
            }
            s += ")";
        }
        return s;
    }
    if (t.kind == TypeKind::Text) {
        return prefix + "Text";
    }
    if (t.kind == TypeKind::Vector) {
        return prefix + (t.pointeeType ? typeName(*t.pointeeType) : "Unknown") + " Vector";
    }
    if (t.kind == TypeKind::FileHandle) {
        return prefix + "File_Handle";
    }
    if (t.kind == TypeKind::Array)
        return prefix + typeName(*t.pointeeType) + " Array[" + std::to_string(t.arraySize) + "]";
    if (t.kind == TypeKind::Tuple) {
        std::string s = prefix + "(";
        for (size_t i = 0; i < t.tupleTypes.size(); ++i) {
            if (i > 0) s += ", ";
            s += typeName(t.tupleTypes[i]);
        }
        s += ")";
        return s;
    }
    if (t.kind == TypeKind::HMap) {
        return prefix + "HMap[" + (t.keyType ? typeName(*t.keyType) : "?") + " : " + (t.valueType ? typeName(*t.valueType) : "?") + "]";
    }
    if (t.kind == TypeKind::NullType)
        return "Null";
    if (t.kind == TypeKind::IntegerLiteral)
        return "integer literal";
    return "<invalid>";
}
unsigned typeBitSize(const Type& t) {
    switch (t.kind) {
        case TypeKind::Boolean: return 1;
        case TypeKind::Signed:
        case TypeKind::Unsigned:
        case TypeKind::Float: return t.bits;
        case TypeKind::Character: return 32;
        case TypeKind::Pointer: return 64;
        case TypeKind::TaskHandle: return 64;
        case TypeKind::BlockPointer: return 64;
        case TypeKind::FileHandle: return 64;
        case TypeKind::Text: return 192;
        case TypeKind::Vector: return 256;
        case TypeKind::Enum: return t.bits;
        case TypeKind::Array: return t.pointeeType ? typeBitSize(*t.pointeeType) * t.arraySize : 0;
        case TypeKind::Slice: return 128;
        case TypeKind::HMap: return 64;
        case TypeKind::Tuple: {
            unsigned sz = 0;
            for (const auto& el : t.tupleTypes) sz += typeBitSize(el);
            return sz;
        }
        default: return 0;
    }
}
std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::uint32_t decodeUtf8Scalar(const std::string& s, int line) {
    if (s.empty()) throw Error("line " + std::to_string(line) + ": empty character literal");
    const auto a = static_cast<unsigned char>(s[0]);
    std::uint32_t cp = 0;
    std::size_t count = 0;
    if (a < 0x80) { cp = a; count = 1; }
    else if ((a & 0xE0) == 0xC0) { cp = a & 0x1F; count = 2; }
    else if ((a & 0xF0) == 0xE0) { cp = a & 0x0F; count = 3; }
    else if ((a & 0xF8) == 0xF0) { cp = a & 0x07; count = 4; }
    else throw Error("line " + std::to_string(line) + ": invalid UTF-8 character literal");
    if (s.size() != count) throw Error("line " + std::to_string(line) + ": character literal must contain one Unicode scalar value");
    for (std::size_t i = 1; i < count; ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if ((b & 0xC0) != 0x80) throw Error("line " + std::to_string(line) + ": invalid UTF-8 character literal");
        cp = (cp << 6) | (b & 0x3F);
    }
    if ((count == 2 && cp < 0x80) || (count == 3 && cp < 0x800) || (count == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        throw Error("line " + std::to_string(line) + ": invalid Unicode scalar value");
    return cp;
}
bool isUnicodeScalar(std::uint64_t cp) {
    return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}
inline bool isBuiltinType(const std::string& s) {
    if (s == "Boolean" || s == "String" || s == "Character" || s == "Integer" || s == "Int" ||
        s == "Float_32" || s == "Float_64" || s == "Text" || s == "String_Builder" ||
        s == "File_Handle" || s == "File") return true;
    for (unsigned bits : {8u, 16u, 32u, 64u}) {
        if (s == "Signed_" + std::to_string(bits) || s == "Unsigned_" + std::to_string(bits))
            return true;
    }
    return false;
}
Type parseType(const std::string& s, int line) {
    if (s == "Boolean")
        return Type::boolean();
    if (s == "Float_32") return Type::floating(32);
    if (s == "Float_64") return Type::floating(64);
    if (s == "String") return Type::string();
    if (s == "Character") return Type::character();
    if (s == "Integer" || s == "Int") return Type::signedInt(64);
    if (s == "Null") return Type::nullType();
    if (s == "Text" || s == "String_Builder") return Type::textType(64);
    if (s == "File_Handle" || s == "File") return Type::fileHandle();
    for (unsigned bits : {8u, 16u, 32u, 64u}) {
        if (s == "Signed_" + std::to_string(bits))
            return Type::signedInt(bits);
        if (s == "Unsigned_" + std::to_string(bits))
            return Type::unsignedInt(bits);
    }
    throw Error("line " + std::to_string(line) + ": unsupported type '" + s +
                "' (supported: Signed_8/16/32/64, Unsigned_8/16/32/64, Float_32, Float_64, Boolean, String, Character, Text, Pointer[T])");
}

// Keywords are matched strictly case-sensitively; all keywords start with a capital letter.
enum class K {
    Eof,
    Id,
    Number,
    FloatNumber,
    String,
    Character,
    Newline,
    LParen,
    RParen,
    Colon,
    Dot,
    Comma,
    Plus,
    Minus,
    Star,
    Slash,
    Assign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Block,
    Takes,
    Produces,
    Declares,
    Implements,
    Give,
    Write,
    Exit,
    Skip,
    If,
    End,
    Loop,
    True,
    False,
    And,
    Or,
    Not,
    Else,
    Until,
    Return,
    Record,
    Enum,
    Module,
    Constant,
    Address,
    Deref,
    Pointer,
    Slice,
    Ensure,
    Cast,
    Null,
    Array,
    Extern,
    Import,
    Collection,
    Ampersand,
    Pipe,
    Caret,
    Tilde,
    Percent,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    LSL,
    LSR,
    Mod,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    AmpersandAssign,
    PipeAssign,
    CaretAssign,
    While,
    Match,
    When,
    Use,
    As,
    Try,
    Volatile,
    Packed,
    Align,
    DotDot,
    Test,
    Allocate,
    Resize,
    Release,
    Spawn,
    Join,
    Task_Handle,
    Block_Pointer,
    Text,
    Vector,
    Pow,
    Print,
    Within,
    Cancel,
    To_String,
    Unsupported
};
struct Token {
    K kind = K::Id;
    std::string text;
    int line = 1;
    std::uint64_t magnitude = 0;
    std::size_t sourceIndex = 0;
    int column = 1;
    std::size_t offset = 0;
    std::size_t length = 1;

    Token() = default;
    Token(K k, std::string t, int l, std::uint64_t m = 0, std::size_t src = 0, int col = 1, std::size_t off = 0, std::size_t len = 1)
        : kind(k), text(std::move(t)), line(l), magnitude(m), sourceIndex(src), column(col), offset(off), length(len) {}
};
class Lexer {
    std::string source;
    std::size_t pos = 0;
    int line = 1;
    std::size_t sourceIndex = 0;
    int column = 1;
    int tokenColumn = 1;
    std::size_t tokenOffset = 0;
    std::vector<Token> tokens;
    char peek(std::size_t n = 0) const {
        return pos + n < source.size() ? source[pos + n] : '\0';
    }
    char take() {
        char c = peek();
        if (c) {
            ++pos;
            if (c == '\n') {
                ++line;
                column = 1;
            } else if (c == '\t') {
                column += 4 - ((column - 1) % 4);
            } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                const auto byte = static_cast<unsigned char>(c);
                column += (byte & 0xF8) == 0xF0 ? 2 : 1;
            }
        }
        return c;
    }
    void emit(K k, std::string s, int l) {
        Token token{k, std::move(s), l, 0, sourceIndex};
        token.column = tokenColumn; token.offset = tokenOffset; token.length = std::max<std::size_t>(1, pos - tokenOffset);
        tokens.push_back(std::move(token));
    }
    K keyword(const std::string& s) const {
        static const std::unordered_map<std::string, K> words = {{"Block", K::Block},
                                                                 {"Takes", K::Takes},
                                                                 {"Produces", K::Produces},
                                                                 {"Declares", K::Declares},
                                                                 {"Implements", K::Implements},
                                                                 {"Give", K::Give},
                                                                 {"Write", K::Write},
                                                                 {"Exit", K::Exit},
                                                                 {"Skip", K::Skip},
                                                                 {"If", K::If},
                                                                 {"End", K::End},
                                                                 {"Loop", K::Loop},
                                                                 {"True", K::True},
                                                                 {"False", K::False},
                                                                 {"And", K::And},
                                                                 {"Or", K::Or},
                                                                 {"Not", K::Not},
                                                                 {"Else", K::Else},
                                                                 {"Until", K::Until},
                                                                 {"until", K::Until},
                                                                 {"While", K::While},
                                                                 {"while", K::While},
                                                                 {"Match", K::Match},
                                                                 {"match", K::Match},
                                                                 {"When", K::When},
                                                                 {"when", K::When},
                                                                 {"Use", K::Use},
                                                                 {"use", K::Use},
                                                                 {"Return", K::Return},
                                                                 {"Record", K::Record},
                                                                 {"Enum", K::Enum},
                                                                 {"Module", K::Module},
                                                                 {"Constant", K::Constant},
                                                                 {"Address", K::Address},
                                                                 {"Deref", K::Deref},
                                                                 {"Pointer", K::Pointer},
                                                                 {"Slice", K::Slice},
                                                                 {"Ensure", K::Ensure},
                                                                 {"Cast", K::Cast},
                                                                 {"Null", K::Null},
                                                                 {"Array", K::Array},
                                                                 {"Extern", K::Extern},
                                                                 {"Import", K::Import},
                                                                 {"Collection", K::Collection},
                                                                 {"LSL", K::LSL},
                                                                 {"LSR", K::LSR},
                                                                 {"as", K::As},
                                                                 {"As", K::As},
                                                                 {"try", K::Try},
                                                                 {"Try", K::Try},
                                                                 {"or", K::Or},
                                                                 {"volatile", K::Volatile},
                                                                 {"Volatile", K::Volatile},
                                                                 {"Packed", K::Packed},
                                                                 {"packed", K::Packed},
                                                                 {"Align", K::Align},
                                                                 {"align", K::Align},
                                                                 {"Allocate", K::Allocate},
                                                                 {"Resize", K::Resize},
                                                                 {"Release", K::Release},
                                                                 {"Spawn", K::Spawn},
                                                                 {"Join", K::Join},
                                                                 {"pow", K::Pow},
                                                                 {"Mod", K::Mod}};
        auto i = words.find(s);
        return i == words.end() ? K::Id : i->second;
    }

  public:
    explicit Lexer(std::string s, std::size_t source = 0) : source(std::move(s)), sourceIndex(source) {}
    std::vector<Token> run() {
        while (pos < source.size()) {
            const char c = peek();
            const int atLine = line;
            tokenColumn = column;
            tokenOffset = pos;
            currentSourceIndex = sourceIndex;
            if (c == ' ' || c == '\t' || c == '\r') {
                take();
                continue;
            }
            if (c == '\n' || c == ';') {
                take();
                emit(K::Newline, "newline", atLine);
                continue;
            }
            if (c == '#' || (c == '/' && peek(1) == '/')) {
                while (peek() && peek() != '\n')
                    take();
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                std::string s;
                while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
                    s += take();
                const K k = keyword(s);
                const bool unsupportedKeyword = k == K::Return || k == K::Collection;
                emit(unsupportedKeyword ? K::Unsupported : k, s, atLine);
                continue;
            }
            if (c == '"') {
                take();
                std::string value;
                while (peek() && peek() != '"') {
                    char ch = take();
                    if (ch == '\n')
                        throw Error("line " + std::to_string(atLine) + ": newline in string literal");
                    if (ch == '\\') {
                        char escaped = take();
                        switch (escaped) {
                            case 'n': value += '\n'; break;
                            case 'r': value += '\r'; break;
                            case 't': value += '\t'; break;
                            case '0': value += '\0'; break;
                            case '\\': value += '\\'; break;
                            case '"': value += '"'; break;
                            default: throw Error("line " + std::to_string(atLine) + ": unsupported string escape");
                        }
                    } else value += ch;
                }
                if (!peek()) throw Error("line " + std::to_string(atLine) + ": unterminated string literal");
                take();
                emit(K::String, std::move(value), atLine);
                continue;
            }
            if (c == '\'') {
                take();
                std::string value;
                while (peek() && peek() != '\'') {
                    char ch = take();
                    if (ch == '\n') throw Error("line " + std::to_string(atLine) + ": newline in character literal");
                    if (ch == '\\') {
                        char escaped = take();
                        switch (escaped) {
                            case 'n': value += '\n'; break;
                            case 'r': value += '\r'; break;
                            case 't': value += '\t'; break;
                            case '0': value += '\0'; break;
                            case '\\': value += '\\'; break;
                            case '\'': value += '\''; break;
                            case '"': value += '"'; break;
                            default: throw Error("line " + std::to_string(atLine) + ": unsupported character escape");
                        }
                    } else value += ch;
                }
                if (!peek()) throw Error("line " + std::to_string(atLine) + ": unterminated character literal");
                take();
                (void)decodeUtf8Scalar(value, atLine);
                emit(K::Character, std::move(value), atLine);
                continue;
            }
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                std::string s;
                s += take();
                s += take();
                if (!std::isxdigit(static_cast<unsigned char>(peek())) && peek() != '_')
                    throw Error("line " + std::to_string(atLine) + ": expected hexadecimal digits after '0x'");
                while (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_')
                    s += take();
                std::string s_clean;
                for (char ch : s) if (ch != '_') s_clean += ch;
                if (s_clean.size() <= 2)
                    throw Error("line " + std::to_string(atLine) + ": expected hexadecimal digits after '0x'");
                errno = 0;
                char* end = nullptr;
                const unsigned long long value = std::strtoull(s_clean.c_str() + 2, &end, 16);
                if (errno == ERANGE || !end || *end != '\0')
                    throw Error("line " + std::to_string(atLine) + ": integer literal is too large");
                Token number{K::Number, s, atLine, static_cast<std::uint64_t>(value), sourceIndex};
                number.column = tokenColumn; number.offset = tokenOffset; number.length = pos - tokenOffset;
                tokens.push_back(std::move(number));
                continue;
            }
            if (c == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
                std::string s;
                s += take();
                s += take();
                if (peek() != '0' && peek() != '1' && peek() != '_')
                    throw Error("line " + std::to_string(atLine) + ": expected binary digits after '0b'");
                while (peek() == '0' || peek() == '1' || peek() == '_')
                    s += take();
                std::string s_clean;
                for (char ch : s) if (ch != '_') s_clean += ch;
                if (s_clean.size() <= 2)
                    throw Error("line " + std::to_string(atLine) + ": expected binary digits after '0b'");
                errno = 0;
                char* end = nullptr;
                const unsigned long long value = std::strtoull(s_clean.c_str() + 2, &end, 2);
                if (errno == ERANGE || !end || *end != '\0')
                    throw Error("line " + std::to_string(atLine) + ": integer literal is too large");
                Token number{K::Number, s, atLine, static_cast<std::uint64_t>(value), sourceIndex};
                number.column = tokenColumn; number.offset = tokenOffset; number.length = pos - tokenOffset;
                tokens.push_back(std::move(number));
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                std::string s;
                while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')
                    s += take();
                if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                    s += take();
                    while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') s += take();
                    if (peek() == 'e' || peek() == 'E') {
                        s += take();
                        if (peek() == '+' || peek() == '-') s += take();
                        if (!std::isdigit(static_cast<unsigned char>(peek())) && peek() != '_')
                            throw Error("line " + std::to_string(atLine) + ": malformed floating-point exponent");
                        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_') s += take();
                    }
                    std::string s_clean;
                    for (char ch : s) if (ch != '_') s_clean += ch;
                    emit(K::FloatNumber, std::move(s_clean), atLine);
                    continue;
                }
                std::string s_clean;
                for (char ch : s) if (ch != '_') s_clean += ch;
                errno = 0;
                char* end = nullptr;
                const unsigned long long value = std::strtoull(s_clean.c_str(), &end, 10);
                if (errno == ERANGE || !end || *end != '\0')
                    throw Error("line " + std::to_string(atLine) + ": integer literal is too large");
                Token number{K::Number, s, atLine, static_cast<std::uint64_t>(value), sourceIndex};
                number.column = tokenColumn; number.offset = tokenOffset; number.length = pos - tokenOffset;
                tokens.push_back(std::move(number));
                continue;
            }
            take();
            switch (c) {
                case '(':
                    emit(K::LParen, "(", atLine);
                    break;
                case ')':
                    emit(K::RParen, ")", atLine);
                    break;
                case ':':
                    emit(K::Colon, ":", atLine);
                    break;
                case '.':
                    if (peek() == '.') {
                        take();
                        emit(K::DotDot, "..", atLine);
                    } else {
                        emit(K::Dot, ".", atLine);
                    }
                    break;
                case ',':
                    emit(K::Comma, ",", atLine);
                    break;
                case '+':
                    if (peek() == '=') {
                        take();
                        emit(K::PlusAssign, "+=", atLine);
                    } else
                        emit(K::Plus, "+", atLine);
                    break;
                case '-':
                    if (peek() == '=') {
                        take();
                        emit(K::MinusAssign, "-=", atLine);
                    } else
                        emit(K::Minus, "-", atLine);
                    break;
                case '*':
                    if (peek() == '=') {
                        take();
                        emit(K::StarAssign, "*=", atLine);
                    } else
                        emit(K::Star, "*", atLine);
                    break;
                case '/':
                    if (peek() == '=') {
                        take();
                        emit(K::SlashAssign, "/=", atLine);
                    } else
                        emit(K::Slash, "/", atLine);
                    break;
                case '%':
                    emit(K::Percent, "%", atLine);
                    break;
                case '&':
                    if (peek() == '=') {
                        take();
                        emit(K::AmpersandAssign, "&=", atLine);
                    } else
                        emit(K::Ampersand, "&", atLine);
                    break;
                case '|':
                    if (peek() == '=') {
                        take();
                        emit(K::PipeAssign, "|=", atLine);
                    } else
                        emit(K::Pipe, "|", atLine);
                    break;
                case '^':
                    if (peek() == '=') {
                        take();
                        emit(K::CaretAssign, "^=", atLine);
                    } else
                        emit(K::Caret, "^", atLine);
                    break;
                case '~':
                    emit(K::Tilde, "~", atLine);
                    break;
                case '[':
                    emit(K::LBracket, "[", atLine);
                    break;
                case ']':
                    emit(K::RBracket, "]", atLine);
                    break;
                case '{':
                    emit(K::LBrace, "{", atLine);
                    break;
                case '}':
                    emit(K::RBrace, "}", atLine);
                    break;
                case '=':
                    if (peek() == '=') {
                        take();
                        emit(K::Equal, "==", atLine);
                    } else
                        emit(K::Assign, "=", atLine);
                    break;
                case '!':
                    if (peek() == '=') {
                        take();
                        emit(K::NotEqual, "!=", atLine);
                    } else
                        throw Error("line " + std::to_string(atLine) +
                                    ": unsupported character '!'; use '!=' for inequality");
                    break;
                case '<':
                    if (peek() == '=') {
                        take();
                        emit(K::LessEqual, "<=", atLine);
                    } else
                        emit(K::Less, "<", atLine);
                    break;
                case '>':
                    if (peek() == '=') {
                        take();
                        emit(K::GreaterEqual, ">=", atLine);
                    } else
                        emit(K::Greater, ">", atLine);
                    break;
                default:
                    throw Error("line " + std::to_string(atLine) + ": unsupported character '" + std::string(1, c) +
                                "'");
            }
        }
        tokens.push_back({K::Eof, "<eof>", line, 0});
        return tokens;
    }
};

struct Expr {
    enum class Kind { Integer, Floating, Boolean, String, Character, EnumVariant, Member, Variable, Call, Unary, Binary, Address, Deref, NullLiteral, Cast, Index, SliceConstructor, ArrayLiteral, RecordLiteral, SubSlice, Try, Allocate, Resize, Release, Spawn, Join, TupleLiteral, HMapLiteral } kind;
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
};

struct Stmt;

struct MatchCase {
    std::unique_ptr<Expr> pattern;
    bool isRange = false;
    bool isWildcard = false;
    std::unique_ptr<Expr> rangeEnd;
    std::vector<Stmt> body;
    int line = 1;

    MatchCase() = default;
    MatchCase(const MatchCase&) = delete;
    MatchCase& operator=(const MatchCase&) = delete;
    MatchCase(MatchCase&&) noexcept = default;
    MatchCase& operator=(MatchCase&&) noexcept = default;
};

struct Stmt {
    enum class Kind { Assign, Call, Give, Write, Exit, Skip, If, Loop, DerefAssign, Ensure, TargetAssign, Match, Use, MultiAssign } kind;
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
    return c;
}

inline Stmt cloneStmt(const Stmt& s);

inline MatchCase cloneMatchCase(const MatchCase& mc) {
    MatchCase res;
    if (mc.pattern) res.pattern = cloneExpr(*mc.pattern);
    res.isRange = mc.isRange;
    res.isWildcard = mc.isWildcard;
    if (mc.rangeEnd) res.rangeEnd = cloneExpr(*mc.rangeEnd);
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
};
struct VarDecl {
    std::string name;
    Type type;
    int line = 1;
    bool constant = false;
    std::unique_ptr<Expr> initializer;
    std::size_t sourceIndex = 0;

    VarDecl() = default;
    VarDecl(const VarDecl&) = delete;
    VarDecl& operator=(const VarDecl&) = delete;
    VarDecl(VarDecl&&) noexcept = default;
    VarDecl& operator=(VarDecl&&) noexcept = default;
    VarDecl(std::string n, Type t, int l, bool c = false, std::unique_ptr<Expr> init = nullptr, std::size_t src = 0)
        : name(std::move(n)), type(std::move(t)), line(l), constant(c), initializer(std::move(init)), sourceIndex(src) {}
};

inline VarDecl cloneVarDecl(const VarDecl& v) {
    VarDecl res;
    res.name = v.name;
    res.type = v.type;
    res.line = v.line;
    res.constant = v.constant;
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
    return res;
}
struct Signature {
    std::vector<Type> parameters;
    std::optional<Type> result;
    std::optional<Type> errorResult;
    std::vector<std::string> typeParams;
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
        if (eat(K::Null)) {
            if (eat(K::Task_Handle) || (at(K::Id) && (t().text == "Task_Handle" || t().text == "TaskHandle"))) {
                if (at(K::Id)) pop();
                base = Type::taskHandle(Type::nullType());
            } else if (eat(K::Block_Pointer) || (at(K::Id) && (t().text == "Block_Pointer" || t().text == "BlockPointer"))) {
                fail("The 'Null' prefix has been removed for non-producing block pointers; declare as 'Block_Pointer(...)' directly");
            } else {
                fail("'Null' is a pointer literal, not a declarable type; declare a typed Pointer instead");
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
            do {
                tupleTypes.push_back(parseTypeRef());
            } while (eat(K::Comma));
            expect(K::RParen, "expected ')' after tuple type");
            base = Type::tupleType(std::move(tupleTypes));
        } else if (at(K::Id)) {
            Token name = pop();
            if (name.text == "HMap") {
                expect(K::LBracket, "expected '[' after 'HMap'");
                Type k = parseTypeRef();
                expect(K::Colon, "expected ':' between key and value types in HMap[Key : Val]");
                Type v = parseTypeRef();
                expect(K::RBracket, "expected ']' after HMap types");
                base = Type::hmapType(std::move(k), std::move(v));
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
        while (!at(K::Eof) && !at(K::When) &&
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
                pop();
                s.kind = Stmt::Kind::Loop;
                if (eat(K::Until)) {
                    s.until = true;
                    s.expr = expression();
                    requireLineEnd("Loop until condition");
                } else if (eat(K::While)) {
                    s.whileLoop = true;
                    s.expr = expression();
                    requireLineEnd("Loop while condition");
                } else {
                    requireLineEnd("Loop");
                }
                s.body = statements(true);
                endPair("", "Loop");
            } else if (at(K::Match)) {
                Token matchTok = pop();
                s.kind = Stmt::Kind::Match;
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
                    expect(K::Colon, "expected ':' after When pattern");
                    requireLineEnd("When pattern");
                    mc.body = statementsForMatchCase();
                    s.matchCases.push_back(std::move(mc));
                    newlines();
                }
                if (eat(K::Else)) {
                    expect(K::Colon, "expected ':' after Else");
                    requireLineEnd("Else in Match");
                    s.elseBody = statementsForMatchCase();
                    newlines();
                }
                expect(K::End, "expected 'End Match'");
                expect(K::Match, "expected 'Match' after 'End'");
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
                pop();
                s.kind = Stmt::Kind::Loop;
                if (eat(K::Until)) {
                    s.until = true;
                    s.expr = expression();
                    requireLineEnd("Loop until condition");
                } else if (eat(K::While)) {
                    s.whileLoop = true;
                    s.expr = expression();
                    requireLineEnd("Loop while condition");
                } else {
                    requireLineEnd("Loop");
                }
                s.body = statements(true);
                endPair("", "Loop");
            } else if (at(K::Match)) {
                Token matchTok = pop();
                s.kind = Stmt::Kind::Match;
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
                    expect(K::Colon, "expected ':' after When pattern");
                    requireLineEnd("When pattern");
                    mc.body = statementsForMatchCase();
                    s.matchCases.push_back(std::move(mc));
                    newlines();
                }
                if (eat(K::Else)) {
                    expect(K::Colon, "expected ':' after Else");
                    requireLineEnd("Else in Match");
                    s.elseBody = statementsForMatchCase();
                    newlines();
                }
                expect(K::End, "expected 'End Match'");
                expect(K::Match, "expected 'Match' after 'End'");
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
        Token n = expect(K::Id, "expected identifier in declaration");
        expect(K::Colon, "expected ':' after '" + n.text + "'");
        bool constant = eat(K::Constant);
        Type typ = parseTypeRef();
        VarDecl value(n.text, std::move(typ), n.line, constant, nullptr, n.sourceIndex);
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
            if (!at(K::Newline) && !stopped())
                fail("expected newline after " + listName + " declaration");
            newlines();
        }
    }
    Function function() {
        Token b = expect(K::Block, "expected Block");
        Function f;
        f.line = b.line;
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

inline bool isVolatileRead(const std::string& name) {
    return name == "Vol_Read_8" || name == "Vol_Read_16" || name == "Vol_Read_32" || name == "Vol_Read_64";
}
inline bool isVolatileWrite(const std::string& name) {
    return name == "Vol_Write_8" || name == "Vol_Write_16" || name == "Vol_Write_32" || name == "Vol_Write_64";
}
inline bool isCpuIntrinsic(const std::string& name) {
    return name == "Nop" || name == "WaitForInterrupt" || name == "MemoryBarrier";
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
           isAIMLIntrinsic(name) || isMathIntrinsic(name) || isFileIntrinsic(name) ||
           name == "Append" || name == "Pop" || name == "To_String" || name == "__loom_task_cancel";
}

struct Flow {
    bool falls = true, breaks = false, skips = false, returns = false;
};
class Analyzer {
    std::unordered_map<const Expr*, Type> expressionTypes;
    std::unordered_map<std::string, Signature> signatures;
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
    std::unordered_set<std::string> currentFunctionLocals;
    std::unordered_set<std::string> currentFunctionConstants;
    std::unordered_set<std::string> currentBlockUses;
    std::optional<Type> currentFunctionErrorResult;
    std::shared_ptr<TargetProfile> targetProfile;
    bool baremetal = false;

public:
    explicit Analyzer(bool bm = false) : targetProfile(createTargetProfile(bm)), baremetal(bm) {}
    explicit Analyzer(std::shared_ptr<TargetProfile> tp) : targetProfile(tp ? tp : createTargetProfile(false)), baremetal(!targetProfile->supportsHeap()) {}

private:
    void checkAccess(const std::string& entityKind, const std::string& entityName,
                     const std::string& defModule, int line, std::size_t callerSourceIndex) {
        if (defModule.empty()) return;
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
        if (baremetal && t.kind == TypeKind::TaskHandle) {
            throw Error("line " + std::to_string(line) + ": Task_Handle is not supported on baremetal/RP2040; task concurrency is restricted to Loom Native");
        }
        if (baremetal && t.kind == TypeKind::Text) {
            throw Error("line " + std::to_string(line) + ": Text is not supported on baremetal/RP2040; dynamic strings are restricted to Loom Native");
        }
        if (baremetal && t.kind == TypeKind::Vector) {
            throw Error("line " + std::to_string(line) + ": Vector is not supported on baremetal/RP2040; dynamic data structures are restricted to Loom Native");
        }
        if (baremetal && t.kind == TypeKind::HMap) {
            throw Error("line " + std::to_string(line) + ": HMap is not supported on baremetal/RP2040; dynamic data structures are restricted to Loom Native");
        }
        if (baremetal && t.kind == TypeKind::FileHandle) {
            throw Error("line " + std::to_string(line) + ": File_Handle is not supported on baremetal/RP2040; file I/O is restricted to Loom Native");
        }
        if (baremetal && t.kind == TypeKind::BlockPointer) {
            throw Error("line " + std::to_string(line) + ": Block_Pointer is not supported on baremetal/RP2040; first-class block pointers are restricted to Loom Native");
        }
        if (t.kind == TypeKind::Record) {
            auto it = recordModule.find(t.name);
            if (it != recordModule.end()) checkAccess("record", t.name, it->second.first, line, sourceIdx);
        } else if (t.kind == TypeKind::Enum) {
            auto it = enumModule.find(t.name);
            if (it != enumModule.end()) checkAccess("enum", t.name, it->second.first, line, sourceIdx);
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
    Type infer(const Expr& e, const std::unordered_map<std::string, Type>& env, std::optional<Type> expected = {}) {
        Type result;
        switch (e.kind) {
            case Expr::Kind::Integer:
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
            case Expr::Kind::Address: {
                if (e.left->kind != Expr::Kind::Variable && e.left->kind != Expr::Kind::Member &&
                    e.left->kind != Expr::Kind::Deref && e.left->kind != Expr::Kind::Index)
                    throw Error("line " + std::to_string(e.line) + ": Address() requires an addressable variable, field, or element");
                Type inner = infer(*e.left, env);
                result = Type::pointer(inner);
                break;
            }
            case Expr::Kind::Deref: {
                Type inner = infer(*e.left, env);
                if (inner.kind != TypeKind::Pointer || !inner.pointeeType)
                    throw Error("line " + std::to_string(e.line) + ": Deref() requires a pointer type, found " + typeName(inner));
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
                } else if (container.kind == TypeKind::String) {
                    result = Type::unsignedInt(8);
                } else {
                    throw Error("line " + std::to_string(e.line) + ": indexing requires an Array, Slice, Pointer, Vector, or String, found " + typeName(container));
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
                if (baremetal) {
                    throw Error("line " + std::to_string(e.line) + ": HMap is not supported on baremetal/RP2040; dynamic data structures are restricted to Loom Native");
                }
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
                Type base = infer(*e.left, env);
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
                    auto sig = signatures.find(e.text);
                    if (sig != signatures.end()) {
                        Type ret = sig->second.result ? *sig->second.result : Type::nullType();
                        result = Type::blockPointer(std::move(ret), sig->second.parameters);
                        break;
                    }
                    throw Error("line " + std::to_string(e.line) + ": undeclared variable '" + e.text + "'");
                }
                if (!currentFunctionLocals.count(e.text) && globalModule.count(e.text))
                    checkAccess("global", e.text, globalModule.at(e.text).first, e.line, currentSourceIndex);
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
                    if (isRec && recType.kind == TypeKind::Vector) {
                        const_cast<Expr&>(e).qualifier = "";
                        if (e.text == "Append") {
                            if (e.arguments.size() != 1)
                                throw Error("line " + std::to_string(e.line) + ": Vector.Append expects 1 argument (Item), got " + std::to_string(e.arguments.size()));
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
                    checkAccess("module", e.qualifier, e.qualifier, e.line, currentSourceIndex);
                    auto fIt = funcModule.find(e.text);
                    if (fIt == funcModule.end() || lower(fIt->second.first) != lower(e.qualifier)) {
                        throw Error("line " + std::to_string(e.line) + ": unresolved block call '" +
                                    e.qualifier + "." + e.text + "'");
                    }
                } else {
                    auto fIt = funcModule.find(e.text);
                    if (fIt != funcModule.end()) {
                        const std::string& defMod = fIt->second.first;
                        std::string callerMod = fileModules.count(currentSourceIndex) ? fileModules.at(currentSourceIndex) : "";
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
                    if (baremetal)
                        throw Error("line " + std::to_string(e.line) + ": " + e.text + " is not supported on baremetal/RP2040; file I/O is restricted to Loom Native");
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
                        throw Error("line " + std::to_string(e.line) + ": Index expects 2 arguments (Vector, Item), got " + std::to_string(e.arguments.size()));
                    Type vecType = infer(*e.arguments[0], env);
                    if (vecType.kind != TypeKind::Vector)
                        throw Error("line " + std::to_string(e.line) + ": Index requires a Vector first argument, found " + typeName(vecType));
                    infer(*e.arguments[1], env, *vecType.pointeeType);
                    result = Type::unsignedInt(64);
                    break;
                }
                if (e.text == "Index_All") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Index_All expects 2 arguments (Vector, Item), got " + std::to_string(e.arguments.size()));
                    Type vecType = infer(*e.arguments[0], env);
                    if (vecType.kind != TypeKind::Vector)
                        throw Error("line " + std::to_string(e.line) + ": Index_All requires a Vector first argument, found " + typeName(vecType));
                    infer(*e.arguments[1], env, *vecType.pointeeType);
                    result = Type::vectorType(Type::unsignedInt(64), 0, false);
                    break;
                }
                if (e.text == "Contains") {
                    if (e.arguments.size() != 2)
                        throw Error("line " + std::to_string(e.line) + ": Contains expects 2 arguments (Vector, Item), got " + std::to_string(e.arguments.size()));
                    Type vecType = infer(*e.arguments[0], env);
                    if (vecType.kind != TypeKind::Vector)
                        throw Error("line " + std::to_string(e.line) + ": Contains requires a Vector first argument, found " + typeName(vecType));
                    infer(*e.arguments[1], env, *vecType.pointeeType);
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
                if (e.text == "__loom_task_cancel") {
                    if (baremetal)
                        throw Error("line " + std::to_string(e.line) + ": Cancel is not supported on baremetal/RP2040");
                    if (e.arguments.size() != 1)
                        throw Error("line " + std::to_string(e.line) + ": Cancel expects 1 task argument");
                    infer(*e.arguments[0], env);
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
                auto signature = signatures.find(e.text);
                if (signature == signatures.end())
                    throw Error("line " + std::to_string(e.line) + ": unresolved block call '" + e.text + "'");
                auto fIt = funcModule.find(e.text);
                if (fIt != funcModule.end())
                    checkAccess("block", e.text, fIt->second.first, e.line, currentSourceIndex);
                if (!signature->second.result)
                    throw Error("line " + std::to_string(e.line) + ": procedure '" + e.text + "' cannot be used as a value");
                if (e.arguments.size() != signature->second.parameters.size())
                    throw Error("line " + std::to_string(e.line) + ": block '" + e.text + "' expects " +
                                std::to_string(signature->second.parameters.size()) + " arguments, got " +
                                std::to_string(e.arguments.size()));
                for (std::size_t i = 0; i < e.arguments.size(); ++i)
                    infer(*e.arguments[i], env, signature->second.parameters[i]);
                result = *signature->second.result;
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
                if (baremetal)
                    throw Error("line " + std::to_string(e.line) + ": Allocate is not supported on baremetal/RP2040; dynamic memory allocation is restricted to Loom Native");
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
                if (baremetal)
                    throw Error("line " + std::to_string(e.line) + ": Resize is not supported on baremetal/RP2040; dynamic memory allocation is restricted to Loom Native");
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
                if (baremetal)
                    throw Error("line " + std::to_string(e.line) + ": Release is not supported on baremetal/RP2040; dynamic memory allocation is restricted to Loom Native");
                if (e.arguments.size() != 1)
                    throw Error("line " + std::to_string(e.line) + ": Release expects 1 argument (Pointer), got " + std::to_string(e.arguments.size()));
                Type ptrType = infer(*e.arguments[0], env);
                if (ptrType.kind != TypeKind::Pointer && ptrType.kind != TypeKind::NullType)
                    throw Error("line " + std::to_string(e.line) + ": Release requires a Pointer, found " + typeName(ptrType));
                result = Type::nullType();
                break;
            }
            case Expr::Kind::Spawn: {
                if (baremetal)
                    throw Error("line " + std::to_string(e.line) + ": Spawn is not supported on baremetal/RP2040; task concurrency is restricted to Loom Native");
                if (e.arguments.empty() || e.arguments[0]->kind != Expr::Kind::Call)
                    throw Error("line " + std::to_string(e.line) + ": Spawn requires a procedure call, e.g. 'Spawn Calculate(10)'");
                const auto& call = *e.arguments[0];
                auto sigIt = signatures.find(call.text);
                if (sigIt == signatures.end())
                    throw Error("line " + std::to_string(e.line) + ": unknown procedure '" + call.text + "' in Spawn");
                const Signature& sig = sigIt->second;
                if (call.arguments.size() != sig.parameters.size())
                    throw Error("line " + std::to_string(e.line) + ": " + call.text + " expects " +
                                std::to_string(sig.parameters.size()) + " arguments, got " + std::to_string(call.arguments.size()));
                for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                    infer(*call.arguments[i], env, sig.parameters[i]);
                }
                Type retType = sig.result ? *sig.result : Type::nullType();
                expressionTypes[&call] = retType;
                result = Type::taskHandle(retType);
                break;
            }
            case Expr::Kind::Join: {
                if (baremetal)
                    throw Error("line " + std::to_string(e.line) + ": Join is not supported on baremetal/RP2040; task concurrency is restricted to Loom Native");
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
    void checkCallStatement(Expr& callExpr, int line, const std::unordered_map<std::string, Type>& env) {
        if (callExpr.kind == Expr::Kind::Release || callExpr.kind == Expr::Kind::Join) {
            infer(callExpr, env);
            return;
        }
        if (callExpr.text == "Append" || callExpr.text == "Pop" || callExpr.text == "Close_File" ||
            callExpr.text == "Remove" ||
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
        if (!callExpr.qualifier.empty()) {
            checkAccess("module", callExpr.qualifier, callExpr.qualifier, line, currentSourceIndex);
            auto fIt = funcModule.find(callExpr.text);
            if (fIt == funcModule.end() || lower(fIt->second.first) != lower(callExpr.qualifier)) {
                throw Error("line " + std::to_string(line) + ": unresolved block call '" +
                            callExpr.qualifier + "." + callExpr.text + "'");
            }
        } else {
            auto fIt = funcModule.find(callExpr.text);
            if (fIt != funcModule.end()) {
                const std::string& defMod = fIt->second.first;
                std::string callerMod = fileModules.count(currentSourceIndex) ? fileModules.at(currentSourceIndex) : "";
                if (!defMod.empty() && lower(defMod) != lower(callerMod)) {
                    checkAccess("block", callExpr.text, defMod, line, currentSourceIndex);
                    bool allowed = false;
                    auto uIt = fileUses.find(currentSourceIndex);
                    if (uIt != fileUses.end() && uIt->second.count(lower(defMod))) {
                        allowed = true;
                    }
                    if (currentBlockUses.count(lower(defMod))) {
                        allowed = true;
                    }
                    if (!allowed) {
                        throw Error("line " + std::to_string(line) + ": cannot call block '" + callExpr.text +
                                    "' from module '" + defMod + "' unqualified; access it as '" +
                                    defMod + "." + callExpr.text + "(...)' or add 'use " + defMod + "'");
                    }
                }
            }
        }
        auto signature = signatures.find(callExpr.text);
        if (signature == signatures.end())
            throw Error("line " + std::to_string(line) + ": unresolved block call '" + callExpr.text + "'");
        auto fIt = funcModule.find(callExpr.text);
        if (fIt != funcModule.end())
            checkAccess("block", callExpr.text, fIt->second.first, line, currentSourceIndex);
        if (callExpr.arguments.size() != signature->second.parameters.size())
            throw Error("line " + std::to_string(line) + ": block '" + callExpr.text + "' expects " +
                        std::to_string(signature->second.parameters.size()) + " arguments, got " +
                        std::to_string(callExpr.arguments.size()));
        for (std::size_t i = 0; i < callExpr.arguments.size(); ++i)
            infer(*callExpr.arguments[i], env, signature->second.parameters[i]);
    }
    Flow sequence(const std::vector<Stmt>& ss, int loopDepth, const std::optional<Type>& result,
                  const std::unordered_map<std::string, Type>& env) {
        Flow total;
        bool active = true;
        for (const auto& s : ss) {
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
                auto i = env.find(s.name);
                if (i == env.end())
                    throw Error("line " + std::to_string(s.line) + ": assignment to undeclared variable '" + s.name +
                                "'");
                if (s.member.empty() && (globalConstants.count(s.name) || currentFunctionConstants.count(s.name)))
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
                    try {
                        infer(*s.expr, env, successType);
                    } catch (...) {
                        infer(*s.expr, env, *currentFunctionErrorResult);
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
                    if (firstType.kind != TypeKind::FileHandle) {
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
                    matchType.kind != TypeKind::Character && matchType.kind != TypeKind::Tuple && matchType.kind != TypeKind::Record)
                    throw Error("line " + std::to_string(s.line) + ": Match expression must be an integer, enum, boolean, character, tuple, or record, found " + typeName(matchType));
                std::unordered_set<std::string> seenPats;
                for (const auto& mc : s.matchCases) {
                    if (mc.isWildcard) {
                        continue;
                    } else if (mc.isRange) {
                        if (!matchType.integer() && matchType.kind != TypeKind::Character)
                            throw Error("line " + std::to_string(mc.line) + ": range pattern '..' requires an integer or character Match expression, found " + typeName(matchType));
                        Type patType = infer(*mc.pattern, env, matchType);
                        Type endType = infer(*mc.rangeEnd, env, matchType);
                        if (!patType.integer() || !endType.integer())
                            throw Error("line " + std::to_string(mc.line) + ": range pattern bounds must be integers");
                    } else if (matchType.kind == TypeKind::Tuple) {
                        Type patType = infer(*mc.pattern, env, matchType);
                        if (patType != matchType)
                            throw Error("line " + std::to_string(mc.line) + ": tuple pattern type does not match Match expression type " + typeName(matchType));
                    } else if (matchType.kind == TypeKind::Record) {
                        Type patType = infer(*mc.pattern, env, matchType);
                        if (patType != matchType)
                            throw Error("line " + std::to_string(mc.line) + ": record pattern type does not match Match expression type " + typeName(matchType));
                    } else {
                        Type patType = infer(*mc.pattern, env, matchType);
                        if (patType != matchType && !(patType.integer() && matchType.integer()))
                            throw Error("line " + std::to_string(mc.line) + ": pattern type " + typeName(patType) + " does not match Match expression type " + typeName(matchType));
                        std::string patKey = mc.pattern->text;
                        if (mc.pattern->kind == Expr::Kind::Integer) patKey = std::to_string(mc.pattern->magnitude);
                        if (!seenPats.insert(patKey).second)
                            throw Error("line " + std::to_string(mc.line) + ": duplicate pattern '" + patKey + "' in Match");
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
                case Stmt::Kind::Match: {
                    Flow combined;
                    bool hasWildcard = false;
                    for (const auto& mc : s.matchCases) {
                        if (mc.isWildcard) hasWildcard = true;
                    }
                    bool allBranchesReturn = !s.matchCases.empty() && (!s.elseBody.empty() || hasWildcard);
                    for (const auto& mc : s.matchCases) {
                        Flow mf = sequence(mc.body, loopDepth, result, env);
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
             const std::unordered_map<std::size_t, std::unordered_set<std::string>>& uses = {}) {
        fileModules = mods;
        fileImports = imps;
        fileUses = uses;
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
        globalTypes.clear();
        globalConstants.clear();
        enumValues.clear();
        enumVariantTypes.clear();
        recordFieldTypes.clear();
        recordFieldOrder.clear();
        fieldAssignmentTypes.clear();
        fieldAssignmentRecords.clear();
        recordModule.clear();
        enumModule.clear();
        globalModule.clear();
        funcModule.clear();
        for (const auto& r : records) recordModule[r.name] = { fileModules.count(r.sourceIndex) ? fileModules.at(r.sourceIndex) : "", r.sourceIndex };
        for (const auto& e : enums) enumModule[e.name] = { fileModules.count(e.sourceIndex) ? fileModules.at(e.sourceIndex) : "", e.sourceIndex };
        for (const auto& g : globals) globalModule[g.name] = { fileModules.count(g.sourceIndex) ? fileModules.at(g.sourceIndex) : "", g.sourceIndex };
        for (const auto& f : fs) funcModule[f.name] = { fileModules.count(f.sourceIndex) ? fileModules.at(f.sourceIndex) : "", f.sourceIndex };
        for (const auto& record : records) {
            currentSourceIndex = record.sourceIndex;
            auto& fields = recordFieldTypes[record.name];
            auto& order = recordFieldOrder[record.name];
            for (const auto& field : record.fields) {
                checkTypeAccess(field.type, field.line, record.sourceIndex);
                if (!fields.emplace(field.name, field.type).second)
                    throw Error("line " + std::to_string(field.line) + ": duplicate field '" + field.name + "'");
                order.push_back(field.name);
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
            if (!globalTypes.emplace(g.name, g.type).second)
                throw Error("line " + std::to_string(g.line) + ": duplicate global '" + g.name + "'");
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
        for (const auto& f : fs) {
            currentSourceIndex = f.sourceIndex;
            Signature sig;
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
            if (!signatures.emplace(f.name, std::move(sig)).second)
                throw Error("line " + std::to_string(f.line) + ": duplicate function '" + f.name + "'");
        }
        for (const auto& e : enums) {
            currentSourceIndex = e.sourceIndex;
            if (signatures.count(e.name) || recordFieldTypes.count(e.name))
                throw Error("line " + std::to_string(e.line) + ": enum '" + e.name + "' conflicts with another declaration");
        }
        for (const auto& g : globals) {
            currentSourceIndex = g.sourceIndex;
            if (signatures.count(g.name) || std::any_of(enums.begin(), enums.end(), [&](const EnumDecl& e) { return e.name == g.name; }) || recordFieldTypes.count(g.name))
                throw Error("line " + std::to_string(g.line) + ": global '" + g.name + "' conflicts with another declaration");
            if (g.initializer) {
                if (!constantInitializer(*g.initializer))
                    throw Error("line " + std::to_string(g.line) + ": global initializers must be literal constants; references and calls are not allowed");
                infer(*g.initializer, globalTypes, g.type);
            } else if (g.constant) {
                throw Error("line " + std::to_string(g.line) + ": Constant global requires an initializer");
            }
        }
        for (const auto& f : fs) {
            if (f.isExtern) continue;
            currentSourceIndex = f.sourceIndex;
            currentFunctionLocals.clear();
            currentFunctionConstants.clear();
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
            std::unordered_map<std::string, Type> env = globalTypes;
            for (const auto& p : f.params)
                if (globalTypes.count(p.name))
                    throw Error("line " + std::to_string(p.line) + ": parameter '" + p.name + "' shadows a global");
                else if (!env.emplace(p.name, p.type).second)
                    throw Error("line " + std::to_string(p.line) + ": duplicate parameter/local name '" + p.name + "'");
            for (const auto& d : f.locals) {
                checkTypeAccess(d.type, d.line, f.sourceIndex);
                if (globalTypes.count(d.name))
                    throw Error("line " + std::to_string(d.line) + ": local '" + d.name + "' shadows a global");
                if (env.count(d.name))
                    throw Error("line " + std::to_string(d.line) + ": duplicate local or parameter/local name '" +
                                d.name + "'");
                if (d.type.kind == TypeKind::Vector && !d.type.hasExplicitSize && !d.initializer) {
                    throw Error("line " + std::to_string(d.line) + ": Vector declaration requires an explicit element count 'T Vector[count]' or an initializer literal '= {...}'");
                }
                if (d.initializer) infer(*d.initializer, env, d.type);
                env.emplace(d.name, d.type);
            }
            Flow flow = sequence(f.body, 0, f.result, env);
            if (f.result && flow.falls)
                throw Error("line " + std::to_string(f.line) + ": value-producing block '" + f.name +
                            "' has a reachable path without Give");
        }
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
            const std::string target = stmt.name + (stmt.member.empty() ? "" : "." + stmt.member);
            out << "WRITE\t" << source << "\t" << caller << "\t" << target << "\t" << stmt.line << "\n";
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
        t.kind == TypeKind::TaskHandle || t.kind == TypeKind::BlockPointer || t.kind == TypeKind::FileHandle)
        return llvm::PointerType::getUnqual(c);
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
                                const std::string& customCpu = "", bool baremetal = false) {
        TargetConfig cfg;
        cfg.isAggressive = releaseMode;
        cfg.optLevel = releaseMode ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::None;

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
        os << "[6] Implicit Safe Multi-Threading (Auto-Parallelization):\n";
        if (autoParallelizations.empty()) {
            os << "    * No loops met data-parallel purity criteria for auto-threading\n";
        } else {
            for (const auto& item : autoParallelizations) {
                os << "    * " << item << "\n";
            }
        }
        os << "================================================================================\n";
    }
};

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
        for (const auto& f : functions) functionMap.emplace(f.name, &f);
        for (const auto& g : globals) globalMap.emplace(g.name, &g);
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
                const auto* f = functionMap.at(*it);
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

class Generator {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module;
    llvm::IRBuilder<> builder{context};
    llvm::Function* current = nullptr;
    const Function* currentFn = nullptr;
    llvm::Function* writeCharacterFunction = nullptr;
    bool reachable = false;
    std::unordered_map<std::string, llvm::Value*> storage;
    std::unordered_map<std::string, Type> types;
    std::unordered_map<std::string, llvm::Function*> functions;
    std::unordered_map<std::string, llvm::GlobalVariable*> globalStorage;
    std::unordered_map<std::string, llvm::StructType*> recordStorageTypes;
    unsigned stringCounter = 0;
    struct LoopTarget {
        llvm::BasicBlock* exit;
        llvm::BasicBlock* next;
        bool hasExit = false;
        std::size_t ensureCount = 0;
    };
    std::vector<LoopTarget> loops;
    std::vector<const Stmt*> activeEnsures;
    const Analyzer& analyzer;
    bool releaseMode = false;
    std::shared_ptr<TargetProfile> targetProfile;
    bool baremetal = false;
    OptReport* optReport = nullptr;
    std::unique_ptr<ConstEvaluator> constEvaluator;

    bool isParallelSafeLoop(const Stmt& s, std::string& inductionVar) const {
        if (s.kind != Stmt::Kind::Loop || !s.whileLoop || !s.expr) return false;
        if (s.expr->kind != Expr::Kind::Binary) return false;
        if (s.expr->text != "<" && s.expr->text != "<=") return false;
        if (!s.expr->left || s.expr->left->kind != Expr::Kind::Variable) return false;
        inductionVar = s.expr->left->text;
        if (s.body.size() < 2) return false;
        const auto& lastStmt = s.body.back();
        bool stepValid = false;
        if (lastStmt.kind == Stmt::Kind::Assign && lastStmt.name == inductionVar) {
            if (lastStmt.expr && lastStmt.expr->kind == Expr::Kind::Binary && lastStmt.expr->text == "+") {
                if (lastStmt.expr->left && lastStmt.expr->left->kind == Expr::Kind::Variable &&
                    lastStmt.expr->left->text == inductionVar) {
                    stepValid = true;
                }
            }
        }
        if (!stepValid) return false;
        for (std::size_t i = 0; i + 1 < s.body.size(); ++i) {
            const auto& st = s.body[i];
            if (st.kind == Stmt::Kind::Exit || st.kind == Stmt::Kind::Skip ||
                st.kind == Stmt::Kind::Give || st.kind == Stmt::Kind::Ensure ||
                st.kind == Stmt::Kind::Loop || st.kind == Stmt::Kind::DerefAssign ||
                st.kind == Stmt::Kind::Write) return false;
            if (st.kind == Stmt::Kind::Assign) {
                if (st.name == inductionVar) return false;
                if (st.target) {
                    if (st.target->kind == Expr::Kind::Index) {
                        if (!st.target->right || st.target->right->kind != Expr::Kind::Variable ||
                            st.target->right->text != inductionVar) {
                            return false;
                        }
                    } else {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    llvm::Type* loomType(Type t) { return llvmType(context, t, &recordStorageTypes); }
    bool terminated() const {
        auto* b = builder.GetInsertBlock();
        return !b || (!b->empty() && b->back().isTerminator());
    }
    llvm::Value* load(const std::string& name) {
        auto i = storage.find(name);
        if (i != storage.end()) {
            bool isVol = types.at(name).isVolatile;
            return builder.CreateLoad(loomType(types.at(name)), i->second, isVol, name);
        }
        auto g = globalStorage.find(name);
        if (g != globalStorage.end()) {
            bool isVol = analyzer.typeOfGlobal(name).isVolatile;
            return builder.CreateLoad(g->second->getValueType(), g->second, isVol, name);
        }
        throw Error("internal error: missing variable '" + name + "'");
    }
    llvm::Value* emitCast(llvm::Value* val, Type src, Type dst) {
        if (src == dst) return val;
        llvm::Type* dstLlvm = loomType(dst);
        if ((src.kind == TypeKind::Pointer || src.kind == TypeKind::NullType) &&
            (dst.kind == TypeKind::Pointer || dst.kind == TypeKind::NullType))
            return val;
        if (src.kind == TypeKind::Pointer && dst.integer())
            return builder.CreatePtrToInt(val, dstLlvm, "ptrtoint");
        if (src.integer() && dst.kind == TypeKind::Pointer)
            return builder.CreateIntToPtr(val, dstLlvm, "inttoptr");
        if (src.kind == TypeKind::Float && dst.kind == TypeKind::Float) {
            if (src.bits < dst.bits) return builder.CreateFPExt(val, dstLlvm, "fpext");
            if (src.bits > dst.bits) return builder.CreateFPTrunc(val, dstLlvm, "fptrunc");
            return val;
        }
        if (src.kind == TypeKind::Float && (dst.integer() || dst.kind == TypeKind::Enum || dst.kind == TypeKind::Character)) {
            if (dst.kind == TypeKind::Signed || (dst.kind == TypeKind::Enum && dst.enumSigned))
                return builder.CreateFPToSI(val, dstLlvm, "fptosi");
            else
                return builder.CreateFPToUI(val, dstLlvm, "fptoui");
        }
        if ((src.integer() || src.kind == TypeKind::Enum || src.kind == TypeKind::Character) && dst.kind == TypeKind::Float) {
            if (src.kind == TypeKind::Signed || (src.kind == TypeKind::Enum && src.enumSigned))
                return builder.CreateSIToFP(val, dstLlvm, "sitofp");
            else
                return builder.CreateUIToFP(val, dstLlvm, "uitofp");
        }
        if (src.kind == TypeKind::Boolean && (dst.integer() || dst.kind == TypeKind::Enum))
            return builder.CreateZExt(val, dstLlvm, "booltoint");
        if ((src.integer() || src.kind == TypeKind::Enum) && dst.kind == TypeKind::Boolean)
            return builder.CreateICmpNE(val, llvm::ConstantInt::get(val->getType(), 0), "inttobool");
        if ((src.integer() || src.kind == TypeKind::Enum || src.kind == TypeKind::Character) &&
            (dst.integer() || dst.kind == TypeKind::Enum || dst.kind == TypeKind::Character)) {
            unsigned srcBits = src.kind == TypeKind::Character ? 32 : src.bits;
            unsigned dstBits = dst.kind == TypeKind::Character ? 32 : dst.bits;
            if (srcBits < dstBits) {
                if (src.kind == TypeKind::Signed || (src.kind == TypeKind::Enum && src.enumSigned))
                    return builder.CreateSExt(val, dstLlvm, "sext");
                else
                    return builder.CreateZExt(val, dstLlvm, "zext");
            }
            if (srcBits > dstBits)
                return builder.CreateTrunc(val, dstLlvm, "trunc");
            return val;
        }
        throw Error("internal error: unsupported cast code generation");
    }
    llvm::Value* address(const Expr& e) {
        if (e.kind == Expr::Kind::Call) {
            auto* temp = builder.CreateAlloca(loomType(analyzer.typeOf(e)), nullptr, "call.res.addr");
            builder.CreateStore(emit(e), temp);
            return temp;
        }
        if (e.kind == Expr::Kind::ArrayLiteral) {
            auto* temp = builder.CreateAlloca(loomType(analyzer.typeOf(e)), nullptr, "arr.lit.addr");
            builder.CreateStore(emit(e), temp);
            return temp;
        }
        if (e.kind == Expr::Kind::RecordLiteral) {
            auto* temp = builder.CreateAlloca(loomType(analyzer.typeOf(e)), nullptr, "rec.lit.addr");
            builder.CreateStore(emit(e), temp);
            return temp;
        }
        if (e.kind == Expr::Kind::Deref) {
            return emit(*e.left);
        }
        if (e.kind == Expr::Kind::Variable) {
            auto local = storage.find(e.text);
            if (local != storage.end()) return local->second;
            auto global = globalStorage.find(e.text);
            if (global != globalStorage.end()) return global->second;
        }
        if (e.kind == Expr::Kind::Member) {
            Type record = analyzer.typeOf(*e.left);
            return builder.CreateStructGEP(loomType(record), address(*e.left), analyzer.fieldIndex(record.name, e.text), e.text + ".addr");
        }
        if (e.kind == Expr::Kind::Index) {
            Type targetType = analyzer.typeOf(*e.left);
            llvm::Value* idx = emit(*e.right);
            if (idx->getType() != builder.getInt64Ty())
                idx = builder.CreateZExtOrTrunc(idx, builder.getInt64Ty());
            llvm::Value* idx0 = builder.CreateSub(idx, builder.getInt64(1), "idx0");

            bool staticallyInBounds = false;
            if (e.right->kind == Expr::Kind::Integer) {
                if (targetType.kind == TypeKind::Array && e.right->magnitude >= 1 && e.right->magnitude <= targetType.arraySize) {
                    staticallyInBounds = true;
                    if (optReport && optReport->enabled) {
                        optReport->boundsEliminations.push_back(
                            "Static constant index " + std::to_string(e.right->magnitude) +
                            " in [1, " + std::to_string(targetType.arraySize) + "]: bounds check eliminated"
                        );
                    }
                }
            }

            if (!staticallyInBounds) {
                llvm::Value* lenVal = nullptr;
                if (targetType.kind == TypeKind::Array) {
                    lenVal = builder.getInt64(targetType.arraySize);
                } else if (targetType.kind == TypeKind::Slice) {
                    llvm::Value* sliceVal = emit(*e.left);
                    lenVal = builder.CreateExtractValue(sliceVal, 1, "slice.len");
                } else if (targetType.kind == TypeKind::String) {
                    llvm::Value* stringVal = emit(*e.left);
                    auto* strlenType = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                    auto strlenCallee = module->getOrInsertFunction("strlen", strlenType);
                    lenVal = builder.CreateCall(strlenCallee, {stringVal}, "string.len");
                }
                if (lenVal) {
                    llvm::Value* ge1 = builder.CreateICmpSGE(idx, builder.getInt64(1), "check.ge1");
                    llvm::Value* leLen = builder.CreateICmpSLE(idx, lenVal, "check.lelen");
                    llvm::Value* inBounds = builder.CreateAnd(ge1, leLen, "in.bounds");
                    auto* trapBB = llvm::BasicBlock::Create(context, "bounds.trap", current);
                    auto* contBB = llvm::BasicBlock::Create(context, "bounds.ok", current);
                    builder.CreateCondBr(inBounds, contBB, trapBB);
                    builder.SetInsertPoint(trapBB);
                    builder.CreateIntrinsic(builder.getVoidTy(), llvm::Intrinsic::trap, {});
                    builder.CreateUnreachable();
                    builder.SetInsertPoint(contBB);
                }
            }

            if (targetType.kind == TypeKind::Vector) {
                llvm::Value* vecAddr = address(*e.left);
                auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto getCallee = module->getOrInsertFunction("loom_vec_get", getFnTy);
                return builder.CreateCall(getCallee, {vecAddr, idx}, "vec.elem.addr");
            }
            if (targetType.kind == TypeKind::Array) {
                llvm::Value* baseAddr = address(*e.left);
                llvm::Value* zero = builder.getInt64(0);
                return builder.CreateInBoundsGEP(loomType(targetType), baseAddr, {zero, idx0}, "array.elem.addr");
            }
            if (targetType.kind == TypeKind::Slice) {
                llvm::Value* sliceVal = emit(*e.left);
                llvm::Value* dataPtr = builder.CreateExtractValue(sliceVal, 0, "slice.ptr");
                Type elemType = *targetType.pointeeType;
                return builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {idx0}, "slice.elem.addr");
            }
            if (targetType.kind == TypeKind::Pointer) {
                llvm::Value* dataPtr = emit(*e.left);
                Type elemType = *targetType.pointeeType;
                return builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {idx0}, "ptr.elem.addr");
            }
            if (targetType.kind == TypeKind::String) {
                llvm::Value* strPtr = emit(*e.left);
                return builder.CreateInBoundsGEP(builder.getInt8Ty(), strPtr, {idx0}, "string.elem.addr");
            }
        }
        throw Error("internal error: expression is not an addressable lvalue");
    }

    std::unordered_map<std::string, llvm::Function*> taskTrampolines;

    llvm::FunctionCallee getLoomAlloc() {
        auto* fnType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()},
            false
        );
        return module->getOrInsertFunction("loom_alloc", fnType);
    }
    llvm::FunctionCallee getLoomRealloc() {
        auto* fnType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()},
            false
        );
        return module->getOrInsertFunction("loom_realloc", fnType);
    }
    llvm::FunctionCallee getLoomFree() {
        auto* fnType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy()},
            false
        );
        return module->getOrInsertFunction("loom_free", fnType);
    }
    llvm::FunctionCallee getLoomTaskSpawn() {
        auto* fnType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty()},
            false
        );
        return module->getOrInsertFunction("loom_task_spawn", fnType);
    }
    llvm::FunctionCallee getLoomTaskJoin() {
        auto* fnType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy(), builder.getPtrTy()},
            false
        );
        return module->getOrInsertFunction("loom_task_join", fnType);
    }

    llvm::Function* getOrCreateTaskTrampoline(const std::string& targetName, const Signature& sig) {
        std::string trampName = "__loom_task_trampoline_" + targetName;
        if (auto* existing = module->getFunction(trampName)) return existing;

        auto* trampType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy(), builder.getPtrTy()},
            false
        );
        auto* trampFn = llvm::Function::Create(trampType, llvm::Function::InternalLinkage, trampName, *module);
        auto* prevBB = builder.GetInsertBlock();
        auto* bb = llvm::BasicBlock::Create(context, "entry", trampFn);
        llvm::IRBuilder<> b(bb);

        auto* argsPtr = trampFn->getArg(0);
        auto* resPtr = trampFn->getArg(1);

        auto fit = functions.find(targetName);
        if (fit == functions.end()) {
            throw Error("internal error: missing target function for task trampoline '" + targetName + "'");
        }
        llvm::Function* targetFunc = fit->second;

        std::vector<llvm::Value*> callArgs;
        if (!sig.parameters.empty()) {
            std::vector<llvm::Type*> fieldTypes;
            for (const auto& pt : sig.parameters) {
                fieldTypes.push_back(loomType(pt));
            }
            auto* argsStructType = llvm::StructType::get(context, fieldTypes);
            for (unsigned i = 0; i < sig.parameters.size(); ++i) {
                auto* fieldPtr = b.CreateStructGEP(argsStructType, argsPtr, i, "arg." + std::to_string(i));
                auto* argVal = b.CreateLoad(loomType(sig.parameters[i]), fieldPtr, "loaded.arg." + std::to_string(i));
                callArgs.push_back(argVal);
            }
        }

        llvm::Value* retVal = b.CreateCall(targetFunc, callArgs, targetFunc->getReturnType()->isVoidTy() ? "" : "task.call");
        if (!targetFunc->getReturnType()->isVoidTy() && sig.result && sig.result->kind != TypeKind::NullType) {
            b.CreateStore(retVal, resPtr);
        }
        b.CreateRetVoid();

        if (prevBB) builder.SetInsertPoint(prevBB);
        taskTrampolines[targetName] = trampFn;
        return trampFn;
    }

    llvm::Value* emit(const Expr& e) {
        if (e.kind == Expr::Kind::Allocate) {
            Type ptrType = analyzer.typeOf(e);
            Type elemType = *ptrType.pointeeType;
            llvm::Type* llvmElemType = loomType(elemType);
            const auto& dl = module->getDataLayout();
            uint64_t elemSize = dl.getTypeAllocSize(llvmElemType);
            uint64_t elemAlign = dl.getABITypeAlign(llvmElemType).value();
            if (elemAlign < 8) elemAlign = 8;

            llvm::Value* countVal = emit(*e.arguments[0]);
            if (countVal->getType() != builder.getInt64Ty()) {
                countVal = builder.CreateZExtOrTrunc(countVal, builder.getInt64Ty());
            }

            llvm::Value* mulRes = builder.CreateBinaryIntrinsic(llvm::Intrinsic::umul_with_overflow, countVal, builder.getInt64(elemSize), nullptr, "umul");
            llvm::Value* overflow = builder.CreateExtractValue(mulRes, 1, "mul_overflow");

            if (analyzer.typeOf(*e.arguments[0]).kind == TypeKind::Signed) {
                llvm::Value* isNeg = builder.CreateICmpSLT(countVal, builder.getInt64(0), "count.isneg");
                overflow = builder.CreateOr(overflow, isNeg, "alloc.overflow");
            }

            auto* curBB = builder.GetInsertBlock();
            auto* okBB = llvm::BasicBlock::Create(context, "alloc.ok", current);
            auto* doneBB = llvm::BasicBlock::Create(context, "alloc.done", current);

            builder.CreateCondBr(overflow, doneBB, okBB);

            builder.SetInsertPoint(okBB);
            llvm::Value* rawPtr = builder.CreateCall(getLoomAlloc(), {countVal, builder.getInt32(elemSize), builder.getInt32(elemAlign)}, "alloc_raw");
            builder.CreateBr(doneBB);
            auto* okEndBB = builder.GetInsertBlock();

            builder.SetInsertPoint(doneBB);
            auto* phi = builder.CreatePHI(builder.getPtrTy(), 2, "alloc.ptr");
            phi->addIncoming(llvm::ConstantPointerNull::get(builder.getPtrTy()), curBB);
            phi->addIncoming(rawPtr, okEndBB);
            return phi;
        }
        if (e.kind == Expr::Kind::Resize) {
            Type ptrType = analyzer.typeOf(e);
            Type elemType = *ptrType.pointeeType;
            llvm::Type* llvmElemType = loomType(elemType);
            const auto& dl = module->getDataLayout();
            uint64_t elemSize = dl.getTypeAllocSize(llvmElemType);
            uint64_t elemAlign = dl.getABITypeAlign(llvmElemType).value();
            if (elemAlign < 8) elemAlign = 8;

            llvm::Value* origPtr = emit(*e.arguments[0]);
            llvm::Value* countVal = emit(*e.arguments[1]);
            if (countVal->getType() != builder.getInt64Ty()) {
                countVal = builder.CreateZExtOrTrunc(countVal, builder.getInt64Ty());
            }

            return builder.CreateCall(getLoomRealloc(), {origPtr, countVal, builder.getInt32(elemSize), builder.getInt32(elemAlign)}, "realloc_ptr");
        }
        if (e.kind == Expr::Kind::Release) {
            llvm::Value* ptr = emit(*e.arguments[0]);
            builder.CreateCall(getLoomFree(), {ptr});
            return llvm::ConstantPointerNull::get(builder.getPtrTy());
        }
        if (e.kind == Expr::Kind::Spawn) {
            const auto& call = *e.arguments[0];
            const Signature* sig = analyzer.signatureOf(call.text);
            if (!sig) throw Error("internal error: missing signature for spawned procedure '" + call.text + "'");

            llvm::Function* tramp = getOrCreateTaskTrampoline(call.text, *sig);
            const auto& dl = module->getDataLayout();

            llvm::Value* argsPtr = nullptr;
            uint64_t argsSize = 0;
            if (!sig->parameters.empty()) {
                std::vector<llvm::Type*> fieldTypes;
                for (const auto& pt : sig->parameters) {
                    fieldTypes.push_back(loomType(pt));
                }
                auto* argsStructType = llvm::StructType::get(context, fieldTypes);
                argsSize = dl.getTypeAllocSize(argsStructType);
                argsPtr = builder.CreateAlloca(argsStructType, nullptr, "task.args");
                for (unsigned i = 0; i < call.arguments.size(); ++i) {
                    llvm::Value* argVal = emit(*call.arguments[i]);
                    auto* fieldPtr = builder.CreateStructGEP(argsStructType, argsPtr, i, "task.arg." + std::to_string(i));
                    builder.CreateStore(argVal, fieldPtr);
                }
            } else {
                argsPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());
            }

            uint64_t resSize = 0;
            if (sig->result && sig->result->kind != TypeKind::NullType) {
                llvm::Type* llvmResType = loomType(*sig->result);
                resSize = dl.getTypeAllocSize(llvmResType);
            }

            return builder.CreateCall(getLoomTaskSpawn(), {tramp, argsPtr, builder.getInt64(argsSize), builder.getInt64(resSize)}, "task_handle");
        }
        if (e.kind == Expr::Kind::Join) {
            llvm::Value* taskVal = emit(*e.arguments[0]);
            Type retType = analyzer.typeOf(e);
            if (e.arguments.size() == 2) {
                llvm::Value* timeoutVal = emit(*e.arguments[1]);
                if (timeoutVal->getType() != builder.getInt64Ty())
                    timeoutVal = builder.CreateZExtOrTrunc(timeoutVal, builder.getInt64Ty());
                auto* joinTimeoutFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto joinTimeoutCallee = module->getOrInsertFunction("loom_task_join_timeout", joinTimeoutFnTy);
                if (retType.kind == TypeKind::NullType) {
                    llvm::Value* nullPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());
                    return builder.CreateCall(joinTimeoutCallee, {taskVal, nullPtr, timeoutVal}, "join_timeout");
                } else {
                    llvm::Type* llvmResType = loomType(retType);
                    llvm::Value* resSlot = builder.CreateAlloca(llvmResType, nullptr, "task.res_slot");
                    builder.CreateCall(joinTimeoutCallee, {taskVal, resSlot, timeoutVal});
                    return builder.CreateLoad(llvmResType, resSlot, "task.result");
                }
            } else {
                if (retType.kind == TypeKind::NullType) {
                    builder.CreateCall(getLoomTaskJoin(), {taskVal, llvm::ConstantPointerNull::get(builder.getPtrTy())});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                } else {
                    llvm::Type* llvmResType = loomType(retType);
                    llvm::Value* resSlot = builder.CreateAlloca(llvmResType, nullptr, "task.res_slot");
                    builder.CreateCall(getLoomTaskJoin(), {taskVal, resSlot});
                    return builder.CreateLoad(llvmResType, resSlot, "task.result");
                }
            }
        }
        if (e.kind == Expr::Kind::RecordLiteral) {
            Type recType = analyzer.typeOf(e);
            llvm::Type* llvmRecType = loomType(recType);
            llvm::Value* recVal = llvm::UndefValue::get(llvmRecType);
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                unsigned fieldIdx = static_cast<unsigned>(analyzer.fieldIndex(recType.name, e.fieldNames[i]));
                llvm::Value* fieldVal = emit(*e.arguments[i]);
                recVal = builder.CreateInsertValue(recVal, fieldVal, fieldIdx, "rec.field");
            }
            return recVal;
        }
        if (e.kind == Expr::Kind::ArrayLiteral) {
            Type litType = analyzer.typeOf(e);
            if (litType.kind == TypeKind::Vector) {
                Type elemType = *litType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                const auto& layout = module->getDataLayout();
                uint64_t elemSize = layout.getTypeAllocSize(llvmElemType);
                uint64_t elemAlign = layout.getABITypeAlign(llvmElemType).value();
                uint64_t count = e.arguments.size();
                auto* tempSlot = builder.CreateAlloca(loomType(litType), nullptr, "vec.lit.slot");
                auto initFn = module->getOrInsertFunction("loom_vec_init",
                    llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false));
                builder.CreateCall(initFn, {tempSlot, builder.getInt64(count), builder.getInt32(elemSize), builder.getInt32(elemAlign)});
                auto getFn = module->getOrInsertFunction("loom_vec_get",
                    llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
                for (unsigned i = 0; i < count; ++i) {
                    llvm::Value* elemAddr = builder.CreateCall(getFn, {tempSlot, builder.getInt64(i + 1)}, "vec.lit.elem.addr");
                    llvm::Value* val = emit(*e.arguments[i]);
                    builder.CreateStore(val, elemAddr);
                }
                return builder.CreateLoad(loomType(litType), tempSlot, "vec.lit");
            }
            llvm::Type* llvmArrType = loomType(litType);
            llvm::Value* arrVal = llvm::UndefValue::get(llvmArrType);
            for (unsigned i = 0; i < e.arguments.size(); ++i) {
                llvm::Value* elemVal = emit(*e.arguments[i]);
                arrVal = builder.CreateInsertValue(arrVal, elemVal, i, "arr.elem");
            }
            return arrVal;
        }
        if (e.kind == Expr::Kind::TupleLiteral) {
            Type tupType = analyzer.typeOf(e);
            llvm::Value* res = llvm::UndefValue::get(loomType(tupType));
            for (size_t i = 0; i < e.arguments.size(); ++i) {
                llvm::Value* elemVal = emit(*e.arguments[i]);
                res = builder.CreateInsertValue(res, elemVal, static_cast<unsigned>(i));
            }
            return res;
        }
        if (e.kind == Expr::Kind::HMapLiteral) {
            Type mapType = analyzer.typeOf(e);
            Type keyType = *mapType.keyType;
            Type valType = *mapType.valueType;
            uint32_t keyKind = (keyType.kind == TypeKind::String) ? 1 : 0;
            const auto& layout = module->getDataLayout();
            uint32_t valSize = layout.getTypeAllocSize(loomType(valType));
            uint32_t valAlign = layout.getABITypeAlign(loomType(valType)).value();

            auto* createFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false);
            auto createCallee = module->getOrInsertFunction("loom_hmap_create", createFnTy);
            llvm::Value* mapPtr = builder.CreateCall(createCallee, {builder.getInt32(keyKind), builder.getInt32(valSize), builder.getInt32(valAlign)}, "hmap");

            for (size_t i = 0; i < e.arguments.size(); i += 2) {
                llvm::Value* kVal = emit(*e.arguments[i]);
                llvm::Value* vVal = emit(*e.arguments[i + 1]);
                llvm::Value* valSlot = builder.CreateAlloca(loomType(valType));
                builder.CreateStore(vVal, valSlot);
                if (keyKind == 1) {
                    auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto insCallee = module->getOrInsertFunction("loom_hmap_insert_str", insFnTy);
                    builder.CreateCall(insCallee, {mapPtr, kVal, valSlot});
                } else {
                    if (kVal->getType() != builder.getInt64Ty())
                        kVal = builder.CreateZExtOrTrunc(kVal, builder.getInt64Ty());
                    auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy()}, false);
                    auto insCallee = module->getOrInsertFunction("loom_hmap_insert_i64", insFnTy);
                    builder.CreateCall(insCallee, {mapPtr, kVal, valSlot});
                }
            }
            return mapPtr;
        }
        if (e.kind == Expr::Kind::Integer) {
            Type t = analyzer.typeOf(e);
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), e.magnitude);
        }
        if (e.kind == Expr::Kind::Boolean)
            return llvm::ConstantInt::get(loomType(Type::boolean()), e.boolean);
        if (e.kind == Expr::Kind::Floating) {
            Type t = analyzer.typeOf(e);
            return llvm::ConstantFP::get(loomType(t), e.floating);
        }
        if (e.kind == Expr::Kind::String) {
            auto* data = llvm::ConstantDataArray::getString(context, e.text, true);
            auto* global = new llvm::GlobalVariable(*module, data->getType(), true,
                llvm::GlobalValue::PrivateLinkage, data, ".loom.string." + std::to_string(++stringCounter));
            return builder.CreateInBoundsGEP(data->getType(), global,
                {builder.getInt32(0), builder.getInt32(0)}, "string");
        }
        if (e.kind == Expr::Kind::Character)
            return llvm::ConstantInt::get(loomType(Type::character()), decodeUtf8Scalar(e.text, e.line));
        if (e.kind == Expr::Kind::NullLiteral) {
            return llvm::ConstantPointerNull::get(builder.getPtrTy());
        }
        if (e.kind == Expr::Kind::Cast) {
            llvm::Value* val = emit(*e.left);
            Type srcType = analyzer.typeOf(*e.left);
            if (e.targetType.kind == TypeKind::Character && srcType.kind != TypeKind::Character) {
                llvm::Value* wide = emitCast(val, srcType, Type::unsignedInt(64));
                llvm::Value* inRange = builder.CreateICmpULE(wide, builder.getInt64(0x10FFFF), "character.range");
                llvm::Value* belowSurrogates = builder.CreateICmpULT(wide, builder.getInt64(0xD800), "character.low");
                llvm::Value* aboveSurrogates = builder.CreateICmpUGT(wide, builder.getInt64(0xDFFF), "character.high");
                llvm::Value* scalar = builder.CreateAnd(inRange, builder.CreateOr(belowSurrogates, aboveSurrogates), "character.scalar");
                auto* trapBB = llvm::BasicBlock::Create(context, "character.trap", current);
                auto* validBB = llvm::BasicBlock::Create(context, "character.valid", current);
                builder.CreateCondBr(scalar, validBB, trapBB);
                builder.SetInsertPoint(trapBB);
                builder.CreateIntrinsic(builder.getVoidTy(), llvm::Intrinsic::trap, {});
                builder.CreateUnreachable();
                builder.SetInsertPoint(validBB);
            }
            return emitCast(val, srcType, e.targetType);
        }
        if (e.kind == Expr::Kind::Index) {
            Type containerType = analyzer.typeOf(*e.left);
            if (containerType.kind == TypeKind::HMap) {
                llvm::Value* mapVal = emit(*e.left);
                llvm::Value* keyVal = emit(*e.right);
                Type keyType = *containerType.keyType;
                Type valType = *containerType.valueType;
                llvm::Value* resPtr = nullptr;
                if (keyType.kind == TypeKind::String) {
                    auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto getCallee = module->getOrInsertFunction("loom_hmap_get_str", getFnTy);
                    resPtr = builder.CreateCall(getCallee, {mapVal, keyVal}, "hmap.get");
                } else {
                    if (keyVal->getType() != builder.getInt64Ty())
                        keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                    auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                    auto getCallee = module->getOrInsertFunction("loom_hmap_get_i64", getFnTy);
                    resPtr = builder.CreateCall(getCallee, {mapVal, keyVal}, "hmap.get");
                }
                auto* isNull = builder.CreateICmpEQ(resPtr, llvm::ConstantPointerNull::get(builder.getPtrTy()), "hmap.check");
                auto* failBB = llvm::BasicBlock::Create(context, "hmap.fail", current);
                auto* contBB = llvm::BasicBlock::Create(context, "hmap.ok", current);
                builder.CreateCondBr(isNull, failBB, contBB);

                builder.SetInsertPoint(failBB);
                builder.CreateIntrinsic(builder.getVoidTy(), llvm::Intrinsic::trap, {});
                builder.CreateUnreachable();

                builder.SetInsertPoint(contBB);
                return builder.CreateLoad(loomType(valType), resPtr, "hmap.val");
            }
            if (containerType.kind == TypeKind::String) {
                llvm::Value* byteAddr = address(e);
                return builder.CreateLoad(builder.getInt8Ty(), byteAddr, "str.byte");
            }
            Type elemType = analyzer.typeOf(e);
            llvm::Value* elemAddr = address(e);
            return builder.CreateLoad(loomType(elemType), elemAddr, "indexed");
        }
        if (e.kind == Expr::Kind::SliceConstructor) {
            if (!e.right) {
                Type arrType = analyzer.typeOf(*e.left);
                llvm::Value* arrAddr = address(*e.left);
                llvm::Value* zero = builder.getInt64(0);
                llvm::Value* dataPtr = builder.CreateInBoundsGEP(loomType(arrType), arrAddr, {zero, zero}, "slice.data");
                llvm::Value* lenVal = builder.getInt64(arrType.arraySize);
                llvm::Value* sliceVal = llvm::UndefValue::get(loomType(analyzer.typeOf(e)));
                sliceVal = builder.CreateInsertValue(sliceVal, dataPtr, 0);
                sliceVal = builder.CreateInsertValue(sliceVal, lenVal, 1);
                return sliceVal;
            } else {
                llvm::Value* ptrVal = emit(*e.left);
                Type lengthType = analyzer.typeOf(*e.right);
                llvm::Value* lenVal = emit(*e.right);
                if (lengthType.kind == TypeKind::Signed) {
                    auto* nonNegative = builder.CreateICmpSGE(lenVal, llvm::ConstantInt::get(lenVal->getType(), 0), "slice.length.nonnegative");
                    auto* okBB = llvm::BasicBlock::Create(context, "slice.length.ok", current);
                    auto* trapBB = llvm::BasicBlock::Create(context, "slice.length.trap", current);
                    builder.CreateCondBr(nonNegative, okBB, trapBB);
                    builder.SetInsertPoint(trapBB);
                    builder.CreateIntrinsic(builder.getVoidTy(), llvm::Intrinsic::trap, {});
                    builder.CreateUnreachable();
                    builder.SetInsertPoint(okBB);
                    if (lenVal->getType() != builder.getInt64Ty())
                        lenVal = builder.CreateSExtOrTrunc(lenVal, builder.getInt64Ty());
                } else if (lenVal->getType() != builder.getInt64Ty()) {
                    lenVal = builder.CreateZExtOrTrunc(lenVal, builder.getInt64Ty());
                }
                auto* isNull = builder.CreateIsNull(ptrVal, "slice.pointer.null");
                auto* isEmpty = builder.CreateICmpEQ(lenVal, builder.getInt64(0), "slice.length.empty");
                auto* valid = builder.CreateOr(builder.CreateNot(isNull), isEmpty, "slice.pointer.valid");
                auto* okBB = llvm::BasicBlock::Create(context, "slice.valid", current);
                auto* trapBB = llvm::BasicBlock::Create(context, "slice.invalid", current);
                builder.CreateCondBr(valid, okBB, trapBB);
                builder.SetInsertPoint(trapBB);
                builder.CreateIntrinsic(builder.getVoidTy(), llvm::Intrinsic::trap, {});
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                llvm::Value* sliceVal = llvm::UndefValue::get(loomType(analyzer.typeOf(e)));
                sliceVal = builder.CreateInsertValue(sliceVal, ptrVal, 0);
                sliceVal = builder.CreateInsertValue(sliceVal, lenVal, 1);
                return sliceVal;
            }
        }
        if (e.kind == Expr::Kind::SubSlice) {
            Type containerType = analyzer.typeOf(*e.left);
            llvm::Value* startVal = emit(*e.arguments[0]);
            llvm::Value* endVal = emit(*e.arguments[1]);
            if (startVal->getType() != builder.getInt64Ty())
                startVal = builder.CreateZExtOrTrunc(startVal, builder.getInt64Ty());
            if (endVal->getType() != builder.getInt64Ty())
                endVal = builder.CreateZExtOrTrunc(endVal, builder.getInt64Ty());
            if (containerType.kind == TypeKind::Text) {
                llvm::Value* textSlot = address(*e.left);
                auto* subFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty()}, false);
                auto subCallee = module->getOrInsertFunction("loom_text_subslice", subFnTy);
                return builder.CreateCall(subCallee, {textSlot, startVal, endVal}, "text.subslice");
            }
            llvm::Value* startIdx0 = builder.CreateSub(startVal, builder.getInt64(1), "subslice.start0");
            llvm::Value* subLen = builder.CreateAdd(builder.CreateSub(endVal, startVal), builder.getInt64(1), "subslice.len");
            Type elemType = *containerType.pointeeType;
            llvm::Value* subPtr = nullptr;
            if (containerType.kind == TypeKind::Array) {
                llvm::Value* arrAddr = address(*e.left);
                llvm::Value* zero = builder.getInt64(0);
                subPtr = builder.CreateInBoundsGEP(loomType(containerType), arrAddr, {zero, startIdx0}, "subslice.ptr");
            } else if (containerType.kind == TypeKind::Pointer) {
                llvm::Value* dataPtr = emit(*e.left);
                subPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {startIdx0}, "subslice.ptr");
            } else if (containerType.kind == TypeKind::Vector) {
                llvm::Value* vecAddr = address(*e.left);
                llvm::Value* dataPtrPtr = builder.CreateStructGEP(loomType(containerType), vecAddr, 0);
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrPtr, "vec.dataptr");
                subPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {startIdx0}, "subslice.ptr");
            } else {
                llvm::Value* srcSlice = emit(*e.left);
                llvm::Value* dataPtr = builder.CreateExtractValue(srcSlice, 0, "slice.dataptr");
                subPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {startIdx0}, "subslice.ptr");
            }
            llvm::Value* sliceVal = llvm::UndefValue::get(loomType(analyzer.typeOf(e)));
            sliceVal = builder.CreateInsertValue(sliceVal, subPtr, 0);
            sliceVal = builder.CreateInsertValue(sliceVal, subLen, 1);
            return sliceVal;
        }
        if (e.kind == Expr::Kind::Try) {
            llvm::Value* unionVal = emit(*e.left);
            llvm::Value* errCode = builder.CreateExtractValue(unionVal, 0, "try.err");
            llvm::Value* isErr = builder.CreateICmpNE(errCode, llvm::ConstantInt::get(errCode->getType(), 0), "try.is_err");
            auto* okBB = llvm::BasicBlock::Create(context, "try.ok", current);
            auto* errBB = llvm::BasicBlock::Create(context, "try.err_exit", current);
            builder.CreateCondBr(isErr, errBB, okBB);

            builder.SetInsertPoint(errBB);
            for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it) {
                emitEnsureStmt(**it);
            }
            llvm::Value* errRet = llvm::UndefValue::get(current->getReturnType());
            errRet = builder.CreateInsertValue(errRet, errCode, 0);
            builder.CreateRet(errRet);

            builder.SetInsertPoint(okBB);
            return builder.CreateExtractValue(unionVal, 1, "try.val");
        }
        if (e.kind == Expr::Kind::EnumVariant) {
            const auto value = analyzer.enumValueOf(e);
            const std::uint64_t bits = value.negative ? std::uint64_t(0) - value.magnitude : value.magnitude;
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(value.type)), bits);
        }
        if (e.kind == Expr::Kind::Address) {
            return address(*e.left);
        }
        if (e.kind == Expr::Kind::Deref) {
            llvm::Value* ptr = emit(*e.left);
            Type pointee = *analyzer.typeOf(*e.left).pointeeType;
            return builder.CreateLoad(loomType(pointee), ptr, pointee.isVolatile, "deref");
        }
        if (e.kind == Expr::Kind::Variable) {
            auto fit = functions.find(e.text);
            if (fit != functions.end()) return fit->second;
            return load(e.text);
        }
        if (e.kind == Expr::Kind::Member) {
            Type baseType = analyzer.typeOf(*e.left);
            if (baseType.kind == TypeKind::Array && e.text == "Length") {
                return builder.getInt64(baseType.arraySize);
            }
            if (baseType.kind == TypeKind::Slice && e.text == "Pointer") {
                llvm::Value* sliceVal = emit(*e.left);
                return builder.CreateExtractValue(sliceVal, 0, "slice.ptr");
            }
            if (baseType.kind == TypeKind::Slice && e.text == "Length") {
                llvm::Value* sliceVal = emit(*e.left);
                return builder.CreateExtractValue(sliceVal, 1, "slice.len");
            }
            if (baseType.kind == TypeKind::Vector && e.text == "Length") {
                llvm::Value* vecAddr = address(*e.left);
                auto* lenPtr = builder.CreateStructGEP(loomType(baseType), vecAddr, 1);
                return builder.CreateLoad(builder.getInt64Ty(), lenPtr, "vec.len");
            }
            if (baseType.kind == TypeKind::Vector && e.text == "Capacity") {
                llvm::Value* vecAddr = address(*e.left);
                auto* capPtr = builder.CreateStructGEP(loomType(baseType), vecAddr, 2);
                return builder.CreateLoad(builder.getInt64Ty(), capPtr, "vec.cap");
            }
            if (baseType.kind == TypeKind::Text && e.text == "Length") {
                llvm::Value* txtAddr = address(*e.left);
                auto* lenPtr = builder.CreateStructGEP(loomType(baseType), txtAddr, 1);
                return builder.CreateLoad(builder.getInt64Ty(), lenPtr, "text.len");
            }
            if (baseType.kind == TypeKind::Text && e.text == "Capacity") {
                llvm::Value* txtAddr = address(*e.left);
                auto* capPtr = builder.CreateStructGEP(loomType(baseType), txtAddr, 2);
                return builder.CreateLoad(builder.getInt64Ty(), capPtr, "text.cap");
            }
            if (baseType.kind == TypeKind::String && e.text == "Length") {
                llvm::Value* strVal = emit(*e.left);
                auto* strlenType = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto strlenCallee = module->getOrInsertFunction("strlen", strlenType);
                return builder.CreateCall(strlenCallee, {strVal}, "str.len");
            }
            if (baseType.kind == TypeKind::HMap && e.text == "Length") {
                llvm::Value* mapPtr = emit(*e.left);
                auto* lenFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto lenCallee = module->getOrInsertFunction("loom_hmap_length", lenFnTy);
                return builder.CreateCall(lenCallee, {mapPtr}, "hmap.len");
            }
            if (e.left->kind == Expr::Kind::Variable || e.left->kind == Expr::Kind::Member || e.left->kind == Expr::Kind::Deref) {
                return builder.CreateLoad(loomType(analyzer.typeOf(e)), address(e), e.text);
            }
            Type record = analyzer.typeOf(*e.left);
            return builder.CreateExtractValue(emit(*e.left), analyzer.fieldIndex(record.name, e.text), e.text);
        }
        if (e.kind == Expr::Kind::Call) {
            if (constEvaluator && (releaseMode || (optReport && optReport->enabled))) {
                unsigned steps = 0;
                auto constVal = constEvaluator->evalExpr(e, {}, 0, steps);
                if (constVal.isKnown()) {
                    if (optReport && optReport->enabled) {
                        optReport->frontendFolds.push_back("Pure block call '" + e.text + "(...)' evaluated at compile time -> " + valToString(constVal));
                    }
                    if (constVal.kind == ConstKind::Integer) {
                        Type t = analyzer.typeOf(e);
                        const std::uint64_t bits = constVal.isNegative ? std::uint64_t(0) - constVal.intVal : constVal.intVal;
                        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
                    }
                    if (constVal.kind == ConstKind::Boolean) {
                        return llvm::ConstantInt::get(loomType(Type::boolean()), constVal.boolVal ? 1 : 0);
                    }
                    if (constVal.kind == ConstKind::Floating) {
                        Type t = analyzer.typeOf(e);
                        return llvm::ConstantFP::get(loomType(t), constVal.floatVal);
                    }
                    if (constVal.kind == ConstKind::Character) {
                        return llvm::ConstantInt::get(loomType(Type::character()), constVal.intVal);
                    }
                }
            }
            if (isVolatileRead(e.text)) {
                llvm::Value* addrVal = emit(*e.arguments[0]);
                llvm::Value* ptr = builder.CreateIntToPtr(addrVal, builder.getPtrTy(), "mmio.ptr");
                if (e.text == "Vol_Read_8") return builder.CreateLoad(builder.getInt8Ty(), ptr, /*isVolatile=*/true, "mmio.val8");
                if (e.text == "Vol_Read_16") return builder.CreateLoad(builder.getInt16Ty(), ptr, /*isVolatile=*/true, "mmio.val16");
                if (e.text == "Vol_Read_32") return builder.CreateLoad(builder.getInt32Ty(), ptr, /*isVolatile=*/true, "mmio.val32");
                if (e.text == "Vol_Read_64") return builder.CreateLoad(builder.getInt64Ty(), ptr, /*isVolatile=*/true, "mmio.val64");
            }
            if (isVolatileWrite(e.text)) {
                llvm::Value* addrVal = emit(*e.arguments[0]);
                llvm::Value* val = emit(*e.arguments[1]);
                llvm::Value* ptr = builder.CreateIntToPtr(addrVal, builder.getPtrTy(), "mmio.ptr");
                if (e.text == "Vol_Write_8") {
                    if (val->getType() != builder.getInt8Ty()) val = builder.CreateTruncOrBitCast(val, builder.getInt8Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
                if (e.text == "Vol_Write_16") {
                    if (val->getType() != builder.getInt16Ty()) val = builder.CreateTruncOrBitCast(val, builder.getInt16Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
                if (e.text == "Vol_Write_32") {
                    if (val->getType() != builder.getInt32Ty()) val = builder.CreateTruncOrBitCast(val, builder.getInt32Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
                if (e.text == "Vol_Write_64") {
                    if (val->getType() != builder.getInt64Ty()) val = builder.CreateZExtOrBitCast(val, builder.getInt64Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
            }
            if (isCpuIntrinsic(e.text)) {
                if (e.text == "Nop") {
                    auto* asmFn = llvm::InlineAsm::get(llvm::FunctionType::get(builder.getVoidTy(), false), "nop", "", true);
                    return builder.CreateCall(asmFn, {});
                }
                if (e.text == "WaitForInterrupt") {
                    auto* asmFn = llvm::InlineAsm::get(llvm::FunctionType::get(builder.getVoidTy(), false), "wfi", "", true);
                    return builder.CreateCall(asmFn, {});
                }
                if (e.text == "MemoryBarrier") {
                    auto* asmFn = llvm::InlineAsm::get(llvm::FunctionType::get(builder.getVoidTy(), false), "dmb sy", "", true);
                    return builder.CreateCall(asmFn, {});
                }
            }
            if (isAIMLIntrinsic(e.text)) {
                if (e.text == "Vector_Dot") {
                    llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
                    llvm::Value* bPtr = getFloatPointer(*e.arguments[1]);
                    llvm::Value* nVal = emit(*e.arguments[2]);
                    if (nVal->getType() != builder.getInt32Ty()) nVal = builder.CreateTruncOrBitCast(nVal, builder.getInt32Ty());
                    llvm::FunctionType* sdotType = llvm::FunctionType::get(
                        builder.getFloatTy(),
                        {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy(), builder.getInt32Ty()},
                        false
                    );
                    auto sdotFunc = module->getOrInsertFunction("cblas_sdot", sdotType);
                    return builder.CreateCall(sdotFunc, {nVal, aPtr, builder.getInt32(1), bPtr, builder.getInt32(1)}, "dot");
                }
                if (e.text == "MatMul") {
                    emitMatMul(e, 0);
                    return nullptr;
                }
                if (e.text == "MatMul_TransposeB") {
                    emitMatMul(e, 1);
                    return nullptr;
                }
                if (e.text == "MatMul_TransposeA") {
                    emitMatMul(e, 2);
                    return nullptr;
                }
                if (e.text == "MatMul_Add") {
                    emitMatMulAdd(e);
                    return nullptr;
                }
                if (e.text == "ReLU") {
                    emitReLU(e);
                    return nullptr;
                }
                if (e.text == "ReLU_Backward") {
                    emitReLUBackward(e);
                    return nullptr;
                }
                if (e.text == "GELU") {
                    emitGELU(e);
                    return nullptr;
                }
                if (e.text == "Softmax") {
                    emitSoftmax(e);
                    return nullptr;
                }
                if (e.text == "LayerNorm") {
                    emitLayerNorm(e);
                    return nullptr;
                }
                if (e.text == "Vector_Add" || e.text == "Vector_Sub" || e.text == "Vector_Mul" || e.text == "Vector_Scale") {
                    emitVectorOp(e, e.text);
                    return nullptr;
                }
                if (e.text == "Parallel_For") {
                    emitParallelFor(e);
                    return nullptr;
                }
            }
            if (functions.find(e.text) == functions.end() && storage.find(e.text) == storage.end() && globalStorage.find(e.text) == globalStorage.end() && isMathIntrinsic(e.text)) {
                if (e.text == "Sqrt") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, val);
                }
                if (e.text == "Sin") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::sin, val);
                }
                if (e.text == "Cos") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::cos, val);
                }
                if (e.text == "Exp") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::exp, val);
                }
                if (e.text == "Log") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::log, val);
                }
                if (e.text == "Floor") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::floor, val);
                }
                if (e.text == "Ceil") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, val);
                }
                if (e.text == "Round") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::round, val);
                }
                if (e.text == "Tan") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "tan" : "tanf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ASin") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "asin" : "asinf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ACos") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "acos" : "acosf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ATan") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "atan" : "atanf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ATan2") {
                    llvm::Value* y = emit(*e.arguments[0]);
                    llvm::Value* x = emit(*e.arguments[1]);
                    bool is64 = y->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(y->getType(), {y->getType(), x->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "atan2" : "atan2f", fnTy);
                    return builder.CreateCall(callee, {y, x});
                }
                if (e.text == "Abs") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    if (val->getType()->isFloatingPointTy()) {
                        return builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, val);
                    } else {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::abs, val, builder.getInt1(false));
                    }
                }
                if (e.text == "Min") {
                    llvm::Value* a = emit(*e.arguments[0]);
                    llvm::Value* b = emit(*e.arguments[1]);
                    Type t = analyzer.typeOf(*e.arguments[0]);
                    if (t.kind == TypeKind::Float) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::minnum, a, b);
                    } else if (t.kind == TypeKind::Signed) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::smin, a, b);
                    } else {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, a, b);
                    }
                }
                if (e.text == "Max") {
                    llvm::Value* a = emit(*e.arguments[0]);
                    llvm::Value* b = emit(*e.arguments[1]);
                    Type t = analyzer.typeOf(*e.arguments[0]);
                    if (t.kind == TypeKind::Float) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, a, b);
                    } else if (t.kind == TypeKind::Signed) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::smax, a, b);
                    } else {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::umax, a, b);
                    }
                }
                if (e.text == "Pow") {
                    llvm::Value* baseVal = emit(*e.arguments[0]);
                    llvm::Value* expVal = emit(*e.arguments[1]);
                    Type t = analyzer.typeOf(*e.arguments[0]);
                    if (t.kind == TypeKind::Float) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::pow, baseVal, expVal);
                    } else if (t.kind == TypeKind::Signed) {
                        auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                        auto callee = module->getOrInsertFunction("loom_ipow_i64", fnTy);
                        llvm::Value* b64 = builder.CreateSExtOrTrunc(baseVal, builder.getInt64Ty());
                        llvm::Value* e64 = builder.CreateSExtOrTrunc(expVal, builder.getInt64Ty());
                        llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                        return builder.CreateIntCast(res64, loomType(t), true);
                    } else {
                        auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                        auto callee = module->getOrInsertFunction("loom_ipow_u64", fnTy);
                        llvm::Value* b64 = builder.CreateZExtOrTrunc(baseVal, builder.getInt64Ty());
                        llvm::Value* e64 = builder.CreateZExtOrTrunc(expVal, builder.getInt64Ty());
                        llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                        return builder.CreateIntCast(res64, loomType(t), false);
                    }
                }
            }
            if (functions.find(e.text) == functions.end() && storage.find(e.text) == storage.end() && globalStorage.find(e.text) == globalStorage.end() && isFileIntrinsic(e.text)) {
                if (e.text == "Open_File") {
                    Type modeType = analyzer.typeOf(*e.arguments[1]);
                    llvm::Value* modeIntVal = nullptr;
                    if (modeType.kind == TypeKind::String) {
                        int m = 1;
                        if (e.arguments[1]->kind == Expr::Kind::String) {
                            const std::string& s = e.arguments[1]->text;
                            if (s == "w") m = 2;
                            else if (s == "a") m = 3;
                            else if (s == "r+") m = 4;
                            else if (s == "w+") m = 5;
                            else m = 1;
                        }
                        modeIntVal = builder.getInt32(m);
                    } else {
                        llvm::Value* rawMode = emit(*e.arguments[1]);
                        modeIntVal = builder.CreateIntCast(rawMode, builder.getInt32Ty(), false);
                    }
                    auto* openFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                    auto openCallee = module->getOrInsertFunction("loom_file_open", openFnTy);
                    auto* errSlot = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "file.err");
                    builder.CreateStore(builder.getInt32(0), errSlot);
                    llvm::Value* pathVal = emit(*e.arguments[0]);
                    return builder.CreateCall(openCallee, {pathVal, modeIntVal, errSlot});
                }
                if (e.text == "Read_All") {
                    auto* readAllFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto readAllCallee = module->getOrInsertFunction("loom_file_read_all", readAllFnTy);
                    auto* errSlot = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "file.err");
                    builder.CreateStore(builder.getInt32(0), errSlot);
                    llvm::Value* pathVal = emit(*e.arguments[0]);
                    return builder.CreateCall(readAllCallee, {pathVal, errSlot});
                }
                if (e.text == "Read_File") {
                    auto* readFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy()}, false);
                    auto readCallee = module->getOrInsertFunction("loom_file_read", readFnTy);
                    auto* errSlot = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "file.err");
                    builder.CreateStore(builder.getInt32(0), errSlot);
                    llvm::Value* handle = emit(*e.arguments[0]);
                    llvm::Value* buf = emit(*e.arguments[1]);
                    llvm::Value* cnt = emit(*e.arguments[2]);
                    return builder.CreateCall(readCallee, {handle, buf, builder.CreateZExtOrTrunc(cnt, builder.getInt64Ty()), errSlot});
                }
                if (e.text == "Close_File") {
                    auto* closeFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()}, false);
                    auto closeCallee = module->getOrInsertFunction("loom_file_close", closeFnTy);
                    llvm::Value* handle = emit(*e.arguments[0]);
                    builder.CreateCall(closeCallee, {handle});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                }
            }
            if (e.text == "Append" && (e.left || !e.arguments.empty())) {
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                auto* lenPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 1);
                auto* capPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 2);
                llvm::Value* curLen = builder.CreateLoad(builder.getInt64Ty(), lenPtr);
                llvm::Value* curCap = builder.CreateLoad(builder.getInt64Ty(), capPtr);
                llvm::Value* needsGrow = builder.CreateICmpEQ(curLen, curCap);
                llvm::Function* parentFn = builder.GetInsertBlock()->getParent();
                llvm::BasicBlock* growBB = llvm::BasicBlock::Create(context, "vec.grow", parentFn);
                llvm::BasicBlock* contBB = llvm::BasicBlock::Create(context, "vec.cont", parentFn);
                builder.CreateCondBr(needsGrow, growBB, contBB);
                builder.SetInsertPoint(growBB);
                auto* growFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()}, false);
                auto growCallee = module->getOrInsertFunction("loom_vec_grow", growFnTy);
                builder.CreateCall(growCallee, {vecSlot});
                builder.CreateBr(contBB);
                builder.SetInsertPoint(contBB);
                auto* dataPtrField = builder.CreateStructGEP(loomType(vecType), vecSlot, 0);
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrField);
                llvm::Value* freshLen = builder.CreateLoad(builder.getInt64Ty(), lenPtr);
                llvm::Value* elemSlot = builder.CreateInBoundsGEP(llvmElemType, dataPtr, {freshLen});
                const Expr* itemExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*itemExpr);
                builder.CreateStore(itemVal, elemSlot);
                builder.CreateStore(builder.CreateAdd(freshLen, builder.getInt64(1)), lenPtr);
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            if (e.text == "Pop" && (e.left || !e.arguments.empty())) {
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                auto* lenPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 1);
                llvm::Value* curLen = builder.CreateLoad(builder.getInt64Ty(), lenPtr);
                llvm::Value* newLen = builder.CreateSub(curLen, builder.getInt64(1));
                builder.CreateStore(newLen, lenPtr);
                auto* dataPtrField = builder.CreateStructGEP(loomType(vecType), vecSlot, 0);
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrField);
                llvm::Value* elemSlot = builder.CreateInBoundsGEP(llvmElemType, dataPtr, {newLen});
                return builder.CreateLoad(llvmElemType, elemSlot, "vec.pop");
            }
            if (e.text == "Index" && (e.left || !e.arguments.empty())) {
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*argExpr);
                auto* tempSlot = builder.CreateAlloca(llvmElemType, nullptr, "idx.target");
                builder.CreateStore(itemVal, tempSlot);
                auto* idxFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                auto idxCallee = module->getOrInsertFunction("loom_vec_index", idxFnTy);
                return builder.CreateCall(idxCallee, {vecSlot, tempSlot}, "vec.idx");
            }
            if (e.text == "Index_All" && (e.left || !e.arguments.empty())) {
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*argExpr);
                auto* tempSlot = builder.CreateAlloca(llvmElemType, nullptr, "idx.target");
                builder.CreateStore(itemVal, tempSlot);
                Type resVecType = Type::vectorType(Type::unsignedInt(64), 0, false);
                auto* resSlot = builder.CreateAlloca(loomType(resVecType), nullptr, "idx.all.res");
                auto* idxAllFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
                auto idxAllCallee = module->getOrInsertFunction("loom_vec_index_all", idxAllFnTy);
                builder.CreateCall(idxAllCallee, {resSlot, vecSlot, tempSlot});
                return builder.CreateLoad(loomType(resVecType), resSlot, "vec.idx_all");
            }
            if (e.text == "Contains" && (e.left || !e.arguments.empty())) {
                Type containerType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                if (containerType.kind == TypeKind::HMap) {
                    llvm::Value* mapVal = emit(e.left ? *e.left : *e.arguments[0]);
                    const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                    llvm::Value* keyVal = emit(*argExpr);
                    if (containerType.keyType->kind == TypeKind::String) {
                        auto* cFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                        auto cCallee = module->getOrInsertFunction("loom_hmap_contains_str", cFnTy);
                        llvm::Value* callRes = builder.CreateCall(cCallee, {mapVal, keyVal}, "hmap.contains");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
                    } else {
                        if (keyVal->getType() != builder.getInt64Ty())
                            keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                        auto* cFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                        auto cCallee = module->getOrInsertFunction("loom_hmap_contains_i64", cFnTy);
                        llvm::Value* callRes = builder.CreateCall(cCallee, {mapVal, keyVal}, "hmap.contains");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
                    }
                }
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*argExpr);
                auto* tempSlot = builder.CreateAlloca(llvmElemType, nullptr, "contains.target");
                builder.CreateStore(itemVal, tempSlot);
                auto* containsFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                auto containsCallee = module->getOrInsertFunction("loom_vec_contains", containsFnTy);
                llvm::Value* callRes = builder.CreateCall(containsCallee, {vecSlot, tempSlot}, "vec.contains");
                return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
            }
            if (e.text == "Length" && e.left && analyzer.typeOf(*e.left).kind == TypeKind::Vector) {
                llvm::Value* vecSlot = address(*e.left);
                Type vecType = analyzer.typeOf(*e.left);
                auto* lenPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 1);
                return builder.CreateLoad(builder.getInt64Ty(), lenPtr, "vec.len");
            }
            if (e.text == "Capacity" && e.left && analyzer.typeOf(*e.left).kind == TypeKind::Vector) {
                llvm::Value* vecSlot = address(*e.left);
                Type vecType = analyzer.typeOf(*e.left);
                auto* capPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 2);
                return builder.CreateLoad(builder.getInt64Ty(), capPtr, "vec.cap");
            }
            if (e.text == "Length" && e.left && analyzer.typeOf(*e.left).kind == TypeKind::HMap) {
                llvm::Value* mapVal = emit(*e.left);
                auto* lenFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto lenCallee = module->getOrInsertFunction("loom_hmap_length", lenFnTy);
                return builder.CreateCall(lenCallee, {mapVal}, "hmap.len");
            }
            if (e.text == "Remove" && (e.left || !e.arguments.empty())) {
                Type containerType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                if (containerType.kind == TypeKind::HMap) {
                    llvm::Value* mapVal = emit(e.left ? *e.left : *e.arguments[0]);
                    const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                    llvm::Value* keyVal = emit(*argExpr);
                    if (containerType.keyType->kind == TypeKind::String) {
                        auto* rFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                        auto rCallee = module->getOrInsertFunction("loom_hmap_remove_str", rFnTy);
                        llvm::Value* callRes = builder.CreateCall(rCallee, {mapVal, keyVal}, "hmap.remove");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "remove.bool");
                    } else {
                        if (keyVal->getType() != builder.getInt64Ty())
                            keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                        auto* rFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                        auto rCallee = module->getOrInsertFunction("loom_hmap_remove_i64", rFnTy);
                        llvm::Value* callRes = builder.CreateCall(rCallee, {mapVal, keyVal}, "hmap.remove");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "remove.bool");
                    }
                }
            }
            if (e.text == "To_String") {
                llvm::Value* textSlot = address(*e.arguments[0]);
                auto* toStrFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()}, false);
                auto toStrCallee = module->getOrInsertFunction("loom_text_to_string", toStrFnTy);
                return builder.CreateCall(toStrCallee, {textSlot}, "to_string");
            }
            if (e.text == "__loom_task_cancel") {
                llvm::Value* taskVal = emit(*e.arguments[0]);
                auto* cancelFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                auto cancelCallee = module->getOrInsertFunction("loom_task_cancel", cancelFnTy);
                builder.CreateCall(cancelCallee, {taskVal});
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            auto f = functions.find(e.text);
            if (f == functions.end()) {
                auto loc = storage.find(e.text);
                auto glob = globalStorage.find(e.text);
                if (loc != storage.end() || glob != globalStorage.end()) {
                    llvm::Value* varSlot = loc != storage.end() ? loc->second : glob->second;
                    llvm::Value* fnPtr = builder.CreateLoad(builder.getPtrTy(), varSlot, e.text + ".fnptr");
                    Type retType = analyzer.typeOf(e);
                    llvm::Type* retLlvmType = retType.kind == TypeKind::NullType ? builder.getVoidTy() : loomType(retType);
                    std::vector<llvm::Type*> paramLlvmTypes;
                    std::vector<llvm::Value*> args;
                    for (const auto& arg : e.arguments) {
                        args.push_back(emit(*arg));
                        paramLlvmTypes.push_back(loomType(analyzer.typeOf(*arg)));
                    }
                    auto* fnTy = llvm::FunctionType::get(retLlvmType, paramLlvmTypes, false);
                    return builder.CreateCall(fnTy, fnPtr, args, retLlvmType->isVoidTy() ? "" : "bp.call");
                }
                throw Error("internal error: missing function declaration '" + e.text + "'");
            }
            std::vector<llvm::Value*> args;
            for (const auto& arg : e.arguments) args.push_back(emit(*arg));
            return builder.CreateCall(f->second, args, f->second->getReturnType()->isVoidTy() ? "" : "call");
        }
        if (e.kind == Expr::Kind::Unary) {
            if (constEvaluator) {
                unsigned steps = 0;
                auto constVal = constEvaluator->evalExpr(e, {}, 0, steps);
                if (constVal.isKnown()) {
                    if (optReport && optReport->enabled) {
                        optReport->frontendFolds.push_back("Constant unary op '" + e.text + "' evaluated at compile time -> " + valToString(constVal));
                    }
                    if (constVal.kind == ConstKind::Integer) {
                        Type t = analyzer.typeOf(e);
                        const std::uint64_t bits = constVal.isNegative ? std::uint64_t(0) - constVal.intVal : constVal.intVal;
                        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
                    }
                    if (constVal.kind == ConstKind::Boolean) {
                        return llvm::ConstantInt::get(loomType(Type::boolean()), constVal.boolVal ? 1 : 0);
                    }
                    if (constVal.kind == ConstKind::Floating) {
                        Type t = analyzer.typeOf(e);
                        return llvm::ConstantFP::get(loomType(t), constVal.floatVal);
                    }
                }
            }
            auto* a = emit(*e.left);
            if (e.text == "-")
                return analyzer.typeOf(*e.left).kind == TypeKind::Float ? builder.CreateFNeg(a, "fneg") : builder.CreateNeg(a, "neg");
            if (e.text == "~")
                return builder.CreateNot(a, "bnot");
            return builder.CreateNot(a, "not");
        }
        if (constEvaluator) {
            unsigned steps = 0;
            auto constVal = constEvaluator->evalExpr(e, {}, 0, steps);
            if (constVal.isKnown()) {
                if (optReport && optReport->enabled) {
                    optReport->frontendFolds.push_back("Constant binary op '" + e.text + "' evaluated at compile time -> " + valToString(constVal));
                }
                if (constVal.kind == ConstKind::Integer) {
                    Type t = analyzer.typeOf(e);
                    const std::uint64_t bits = constVal.isNegative ? std::uint64_t(0) - constVal.intVal : constVal.intVal;
                    return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
                }
                if (constVal.kind == ConstKind::Boolean) {
                    return llvm::ConstantInt::get(loomType(Type::boolean()), constVal.boolVal ? 1 : 0);
                }
                if (constVal.kind == ConstKind::Floating) {
                    Type t = analyzer.typeOf(e);
                    return llvm::ConstantFP::get(loomType(t), constVal.floatVal);
                }
                if (constVal.kind == ConstKind::Character) {
                    return llvm::ConstantInt::get(loomType(Type::character()), constVal.intVal);
                }
            }
        }
        const std::string op = lower(e.text);
        if (op == "and") {
            llvm::Value* lhs = emit(*e.left);
            auto* lhsBB = builder.GetInsertBlock();
            auto* rhsBB = llvm::BasicBlock::Create(context, "and.rhs", current);
            auto* mergeBB = llvm::BasicBlock::Create(context, "and.merge", current);
            builder.CreateCondBr(lhs, rhsBB, mergeBB);

            builder.SetInsertPoint(rhsBB);
            llvm::Value* rhs = emit(*e.right);
            auto* rhsEndBB = builder.GetInsertBlock();
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            auto* phi = builder.CreatePHI(builder.getInt1Ty(), 2, "and.res");
            phi->addIncoming(builder.getInt1(false), lhsBB);
            phi->addIncoming(rhs, rhsEndBB);
            return phi;
        }
        if (op == "or") {
            llvm::Value* lhs = emit(*e.left);
            auto* lhsBB = builder.GetInsertBlock();
            auto* rhsBB = llvm::BasicBlock::Create(context, "or.rhs", current);
            auto* mergeBB = llvm::BasicBlock::Create(context, "or.merge", current);
            builder.CreateCondBr(lhs, mergeBB, rhsBB);

            builder.SetInsertPoint(rhsBB);
            llvm::Value* rhs = emit(*e.right);
            auto* rhsEndBB = builder.GetInsertBlock();
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            auto* phi = builder.CreatePHI(builder.getInt1Ty(), 2, "or.res");
            phi->addIncoming(builder.getInt1(true), lhsBB);
            phi->addIncoming(rhs, rhsEndBB);
            return phi;
        }
        auto* a = emit(*e.left);
        auto* b = emit(*e.right);
        Type t = analyzer.typeOf(*e.left);
        if (op == "pow") {
            if (t.kind == TypeKind::Float) {
                return builder.CreateBinaryIntrinsic(llvm::Intrinsic::pow, a, b);
            } else if (t.kind == TypeKind::Signed) {
                auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_ipow_i64", fnTy);
                llvm::Value* b64 = builder.CreateSExtOrTrunc(a, builder.getInt64Ty());
                llvm::Value* e64 = builder.CreateSExtOrTrunc(b, builder.getInt64Ty());
                llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                return builder.CreateIntCast(res64, loomType(t), true);
            } else {
                auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_ipow_u64", fnTy);
                llvm::Value* b64 = builder.CreateZExtOrTrunc(a, builder.getInt64Ty());
                llvm::Value* e64 = builder.CreateZExtOrTrunc(b, builder.getInt64Ty());
                llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                return builder.CreateIntCast(res64, loomType(t), false);
            }
        }
        if (op == "+") {
            if (t.kind == TypeKind::Text) {
                llvm::Value* textSlot = address(*e.left);
                Type rType = analyzer.typeOf(*e.right);
                if (rType.kind == TypeKind::String) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_str", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, b});
                } else if (rType.kind == TypeKind::Character) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt8Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_char", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateIntCast(b, builder.getInt8Ty(), false)});
                } else if (rType.kind == TypeKind::Signed) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_i64", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateSExtOrTrunc(b, builder.getInt64Ty())});
                } else if (rType.kind == TypeKind::Unsigned) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_u64", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateZExtOrTrunc(b, builder.getInt64Ty())});
                } else if (rType.kind == TypeKind::Float) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getDoubleTy()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_f64", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateFPExt(b, builder.getDoubleTy())});
                } else if (rType.kind == TypeKind::Boolean) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt1Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_bool", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, b});
                }
                return builder.CreateLoad(loomType(t), textSlot);
            }
        }
        if (t.kind == TypeKind::Float) {
            if (releaseMode) {
                llvm::FastMathFlags fmf;
                fmf.setFast();
                builder.setFastMathFlags(fmf);
            }
            if (op == "+") return builder.CreateFAdd(a, b, "fadd");
            if (op == "-") return builder.CreateFSub(a, b, "fsub");
            if (op == "*") return builder.CreateFMul(a, b, "fmul");
            if (op == "/") return builder.CreateFDiv(a, b, "fdiv");
            if (op == "==") return builder.CreateFCmpOEQ(a, b, "fcmp");
            if (op == "!=") return builder.CreateFCmpONE(a, b, "fcmp");
            if (op == "<") return builder.CreateFCmpOLT(a, b, "fcmp");
            if (op == "<=") return builder.CreateFCmpOLE(a, b, "fcmp");
            if (op == ">") return builder.CreateFCmpOGT(a, b, "fcmp");
            return builder.CreateFCmpOGE(a, b, "fcmp");
        }
        if (op == "+")
            return builder.CreateAdd(a, b, "add");
        if (op == "-")
            return builder.CreateSub(a, b, "sub");
        if (op == "*")
            return builder.CreateMul(a, b, "mul");
        if (op == "/")
            return t.kind == TypeKind::Unsigned ? builder.CreateUDiv(a, b, "divu") : builder.CreateSDiv(a, b, "divs");
        if (op == "mod" || op == "%")
            return t.kind == TypeKind::Unsigned ? builder.CreateURem(a, b, "urem") : builder.CreateSRem(a, b, "srem");
        if (op == "&")
            return builder.CreateAnd(a, b, "band");
        if (op == "|")
            return builder.CreateOr(a, b, "bor");
        if (op == "^")
            return builder.CreateXor(a, b, "bxor");
        if (op == "lsl") {
            if (b->getType() != a->getType())
                b = builder.CreateZExtOrTrunc(b, a->getType());
            return builder.CreateShl(a, b, "shl");
        }
        if (op == "lsr") {
            if (b->getType() != a->getType())
                b = builder.CreateZExtOrTrunc(b, a->getType());
            return t.kind == TypeKind::Unsigned ? builder.CreateLShr(a, b, "lshr") : builder.CreateAShr(a, b, "ashr");
        }
        if (op == "and")
            return builder.CreateAnd(a, b, "and");
        if (op == "or")
            return builder.CreateOr(a, b, "or");
        if (op == "==")
            return builder.CreateICmpEQ(a, b, "eq");
        if (op == "!=")
            return builder.CreateICmpNE(a, b, "ne");
        if (t.kind == TypeKind::Unsigned || t.kind == TypeKind::Character) {
            if (op == "<")
                return builder.CreateICmpULT(a, b, "lt");
            if (op == "<=")
                return builder.CreateICmpULE(a, b, "le");
            if (op == ">")
                return builder.CreateICmpUGT(a, b, "gt");
            return builder.CreateICmpUGE(a, b, "ge");
        }
        if (op == "<")
            return builder.CreateICmpSLT(a, b, "lt");
        if (op == "<=")
            return builder.CreateICmpSLE(a, b, "le");
        if (op == ">")
            return builder.CreateICmpSGT(a, b, "gt");
        return builder.CreateICmpSGE(a, b, "ge");
    }
    llvm::Value* getFloatPointer(const Expr& e) {
        Type t = analyzer.typeOf(e);
        if (t.kind == TypeKind::Pointer) {
            return emit(e);
        }
        if (t.kind == TypeKind::Slice) {
            llvm::Value* sl = emit(e);
            return builder.CreateExtractValue(sl, 0, "slice.ptr");
        }
        if (t.kind == TypeKind::Array) {
            llvm::Value* arrAddr = address(e);
            llvm::Value* indices[] = {builder.getInt64(0), builder.getInt64(0)};
            return builder.CreateInBoundsGEP(loomType(t), arrAddr, indices, "arr.ptr");
        }
        return emit(e);
    }

    llvm::Function* getOrCreateTrampoline() {
        const std::string name = "__loom_dispatch_trampoline";
        if (auto* existing = module->getFunction(name)) return existing;
        auto* fnType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy(), builder.getInt64Ty()},
            false
        );
        auto* fn = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, name, *module);
        auto* bb = llvm::BasicBlock::Create(context, "entry", fn);
        llvm::IRBuilder<> b(bb);
        auto* ctxArg = fn->getArg(0);
        auto* idxArg = fn->getArg(1);
        auto* idx1 = b.CreateAdd(idxArg, b.getInt64(1), "idx1");

        auto* workerType = llvm::FunctionType::get(b.getVoidTy(), {b.getInt64Ty()}, false);
        b.CreateCall(workerType, ctxArg, {idx1});
        b.CreateRetVoid();
        return fn;
    }

    void emitMatMul(const Expr& e, int transMode) {
        llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* bPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* cPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* mVal = emit(*e.arguments[3]);
        llvm::Value* nVal = emit(*e.arguments[4]);
        llvm::Value* kVal = emit(*e.arguments[5]);
        if (mVal->getType() != builder.getInt32Ty()) mVal = builder.CreateTruncOrBitCast(mVal, builder.getInt32Ty());
        if (nVal->getType() != builder.getInt32Ty()) nVal = builder.CreateTruncOrBitCast(nVal, builder.getInt32Ty());
        if (kVal->getType() != builder.getInt32Ty()) kVal = builder.CreateTruncOrBitCast(kVal, builder.getInt32Ty());

        int transA = (transMode == 2) ? 112 : 111;
        int transB = (transMode == 1) ? 112 : 111;
        llvm::Value* lda = (transMode == 2) ? mVal : kVal;
        llvm::Value* ldb = (transMode == 1) ? kVal : nVal;
        llvm::Value* ldc = nVal;

        llvm::FunctionType* sgemmType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty()
            },
            false
        );
        auto sgemmFunc = module->getOrInsertFunction("cblas_sgemm", sgemmType);
        builder.CreateCall(sgemmFunc, {
            builder.getInt32(101), builder.getInt32(transA), builder.getInt32(transB),
            mVal, nVal, kVal,
            llvm::ConstantFP::get(builder.getFloatTy(), 1.0f),
            aPtr, lda,
            bPtr, ldb,
            llvm::ConstantFP::get(builder.getFloatTy(), 0.0f),
            cPtr, ldc
        });
    }

    void emitMatMulAdd(const Expr& e) {
        llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* bPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* biasPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* cPtr = getFloatPointer(*e.arguments[3]);
        llvm::Value* mVal = emit(*e.arguments[4]);
        llvm::Value* nVal = emit(*e.arguments[5]);
        llvm::Value* kVal = emit(*e.arguments[6]);
        if (mVal->getType() != builder.getInt32Ty()) mVal = builder.CreateTruncOrBitCast(mVal, builder.getInt32Ty());
        if (nVal->getType() != builder.getInt32Ty()) nVal = builder.CreateTruncOrBitCast(nVal, builder.getInt32Ty());
        if (kVal->getType() != builder.getInt32Ty()) kVal = builder.CreateTruncOrBitCast(kVal, builder.getInt32Ty());

        llvm::FunctionType* sgemmType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty()
            },
            false
        );
        auto sgemmFunc = module->getOrInsertFunction("cblas_sgemm", sgemmType);
        builder.CreateCall(sgemmFunc, {
            builder.getInt32(101), builder.getInt32(111), builder.getInt32(111),
            mVal, nVal, kVal,
            llvm::ConstantFP::get(builder.getFloatTy(), 1.0f),
            aPtr, kVal,
            bPtr, nVal,
            llvm::ConstantFP::get(builder.getFloatTy(), 0.0f),
            cPtr, nVal
        });

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* headerM = llvm::BasicBlock::Create(context, "bias.m.header", currentFunc);
        auto* bodyM = llvm::BasicBlock::Create(context, "bias.m.body", currentFunc);
        auto* exitM = llvm::BasicBlock::Create(context, "bias.m.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "bias.i");
        builder.CreateStore(builder.getInt32(0), iAlloca);
        builder.CreateBr(headerM);

        builder.SetInsertPoint(headerM);
        auto* iVal = builder.CreateLoad(builder.getInt32Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, mVal), bodyM, exitM);

        builder.SetInsertPoint(bodyM);
        auto* headerN = llvm::BasicBlock::Create(context, "bias.n.header", currentFunc);
        auto* bodyN = llvm::BasicBlock::Create(context, "bias.n.body", currentFunc);
        auto* exitN = llvm::BasicBlock::Create(context, "bias.n.exit", currentFunc);

        auto* jAlloca = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "bias.j");
        builder.CreateStore(builder.getInt32(0), jAlloca);
        builder.CreateBr(headerN);

        builder.SetInsertPoint(headerN);
        auto* jVal = builder.CreateLoad(builder.getInt32Ty(), jAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(jVal, nVal), bodyN, exitN);

        builder.SetInsertPoint(bodyN);
        auto* rowOffset = builder.CreateMul(iVal, nVal);
        auto* elemIdx = builder.CreateAdd(rowOffset, jVal);
        auto* cElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), cPtr, {elemIdx});
        auto* biasElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), biasPtr, {jVal});
        auto* cOld = builder.CreateLoad(builder.getFloatTy(), cElemPtr);
        auto* bVal = builder.CreateLoad(builder.getFloatTy(), biasElemPtr);
        auto* cNew = builder.CreateFAdd(cOld, bVal);
        builder.CreateStore(cNew, cElemPtr);

        builder.CreateStore(builder.CreateAdd(jVal, builder.getInt32(1)), jAlloca);
        builder.CreateBr(headerN);

        builder.SetInsertPoint(exitN);
        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt32(1)), iAlloca);
        builder.CreateBr(headerM);

        builder.SetInsertPoint(exitM);
    }

    void emitReLU(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* nVal = emit(*e.arguments[1]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "relu.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "relu.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "relu.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "relu.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* elemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {iVal});
        auto* val = builder.CreateLoad(builder.getFloatTy(), elemPtr);
        auto* maxVal = builder.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, val, llvm::ConstantFP::get(builder.getFloatTy(), 0.0f));
        builder.CreateStore(maxVal, elemPtr);

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitReLUBackward(const Expr& e) {
        llvm::Value* gInPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* xPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* gOutPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* nVal = emit(*e.arguments[3]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "relubwd.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "relubwd.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "relubwd.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "relubwd.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* xElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {iVal});
        auto* ginElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), gInPtr, {iVal});
        auto* goutElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), gOutPtr, {iVal});
        auto* xVal = builder.CreateLoad(builder.getFloatTy(), xElemPtr);
        auto* ginVal = builder.CreateLoad(builder.getFloatTy(), ginElemPtr);
        auto* cond = builder.CreateFCmpOGT(xVal, llvm::ConstantFP::get(builder.getFloatTy(), 0.0f));
        auto* outVal = builder.CreateSelect(cond, ginVal, llvm::ConstantFP::get(builder.getFloatTy(), 0.0f));
        builder.CreateStore(outVal, goutElemPtr);

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitSoftmax(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* mVal = nullptr;
        llvm::Value* dVal = nullptr;
        if (e.arguments.size() == 2) {
            mVal = builder.getInt64(1);
            dVal = emit(*e.arguments[1]);
        } else {
            mVal = emit(*e.arguments[1]);
            dVal = emit(*e.arguments[2]);
        }
        if (mVal->getType() != builder.getInt64Ty()) mVal = builder.CreateZExtOrTrunc(mVal, builder.getInt64Ty());
        if (dVal->getType() != builder.getInt64Ty()) dVal = builder.CreateZExtOrTrunc(dVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto expFunc = module->getOrInsertFunction("expf", llvm::FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));

        auto* rHeader = llvm::BasicBlock::Create(context, "sm.rhead", currentFunc);
        auto* rBody = llvm::BasicBlock::Create(context, "sm.rbody", currentFunc);
        auto* rExit = llvm::BasicBlock::Create(context, "sm.rexit", currentFunc);

        auto* rAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.row");
        builder.CreateStore(builder.getInt64(0), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rHeader);
        auto* rVal = builder.CreateLoad(builder.getInt64Ty(), rAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(rVal, mVal), rBody, rExit);

        builder.SetInsertPoint(rBody);
        auto* rowOffset = builder.CreateMul(rVal, dVal);

        auto* maxAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "sm.max");
        auto* x0Ptr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {rowOffset});
        builder.CreateStore(builder.CreateLoad(builder.getFloatTy(), x0Ptr), maxAlloca);

        auto* h1 = llvm::BasicBlock::Create(context, "sm.h1", currentFunc);
        auto* b1 = llvm::BasicBlock::Create(context, "sm.b1", currentFunc);
        auto* e1 = llvm::BasicBlock::Create(context, "sm.e1", currentFunc);
        auto* i1 = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.i1");
        builder.CreateStore(builder.getInt64(1), i1);
        builder.CreateBr(h1);

        builder.SetInsertPoint(h1);
        auto* iv1 = builder.CreateLoad(builder.getInt64Ty(), i1);
        builder.CreateCondBr(builder.CreateICmpSLT(iv1, dVal), b1, e1);

        builder.SetInsertPoint(b1);
        auto* p1 = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, iv1)});
        auto* v1 = builder.CreateLoad(builder.getFloatTy(), p1);
        auto* curMax = builder.CreateLoad(builder.getFloatTy(), maxAlloca);
        auto* newMax = builder.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, curMax, v1);
        builder.CreateStore(newMax, maxAlloca);
        builder.CreateStore(builder.CreateAdd(iv1, builder.getInt64(1)), i1);
        builder.CreateBr(h1);

        builder.SetInsertPoint(e1);
        auto* maxVal = builder.CreateLoad(builder.getFloatTy(), maxAlloca);

        auto* sumAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "sm.sum");
        builder.CreateStore(llvm::ConstantFP::get(builder.getFloatTy(), 0.0f), sumAlloca);

        auto* h2 = llvm::BasicBlock::Create(context, "sm.h2", currentFunc);
        auto* b2 = llvm::BasicBlock::Create(context, "sm.b2", currentFunc);
        auto* e2 = llvm::BasicBlock::Create(context, "sm.e2", currentFunc);
        auto* i2 = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.i2");
        builder.CreateStore(builder.getInt64(0), i2);
        builder.CreateBr(h2);

        builder.SetInsertPoint(h2);
        auto* iv2 = builder.CreateLoad(builder.getInt64Ty(), i2);
        builder.CreateCondBr(builder.CreateICmpSLT(iv2, dVal), b2, e2);

        builder.SetInsertPoint(b2);
        auto* p2 = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, iv2)});
        auto* v2 = builder.CreateLoad(builder.getFloatTy(), p2);
        auto* diff = builder.CreateFSub(v2, maxVal);
        auto* expVal = builder.CreateCall(expFunc, {diff});
        builder.CreateStore(expVal, p2);
        auto* curSum = builder.CreateLoad(builder.getFloatTy(), sumAlloca);
        builder.CreateStore(builder.CreateFAdd(curSum, expVal), sumAlloca);
        builder.CreateStore(builder.CreateAdd(iv2, builder.getInt64(1)), i2);
        builder.CreateBr(h2);

        builder.SetInsertPoint(e2);
        auto* sumVal = builder.CreateLoad(builder.getFloatTy(), sumAlloca);
        auto* invSum = builder.CreateFDiv(llvm::ConstantFP::get(builder.getFloatTy(), 1.0f), sumVal);

        auto* h3 = llvm::BasicBlock::Create(context, "sm.h3", currentFunc);
        auto* b3 = llvm::BasicBlock::Create(context, "sm.b3", currentFunc);
        auto* e3 = llvm::BasicBlock::Create(context, "sm.e3", currentFunc);
        auto* i3 = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.i3");
        builder.CreateStore(builder.getInt64(0), i3);
        builder.CreateBr(h3);

        builder.SetInsertPoint(h3);
        auto* iv3 = builder.CreateLoad(builder.getInt64Ty(), i3);
        builder.CreateCondBr(builder.CreateICmpSLT(iv3, dVal), b3, e3);

        builder.SetInsertPoint(b3);
        auto* p3 = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, iv3)});
        auto* v3 = builder.CreateLoad(builder.getFloatTy(), p3);
        builder.CreateStore(builder.CreateFMul(v3, invSum), p3);
        builder.CreateStore(builder.CreateAdd(iv3, builder.getInt64(1)), i3);
        builder.CreateBr(h3);

        builder.SetInsertPoint(e3);
        builder.CreateStore(builder.CreateAdd(rVal, builder.getInt64(1)), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rExit);
    }

    void emitGELU(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* nVal = emit(*e.arguments[1]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto tanhFunc = module->getOrInsertFunction("tanhf", llvm::FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));

        auto* header = llvm::BasicBlock::Create(context, "gelu.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "gelu.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "gelu.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "gelu.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* elemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {iVal});
        auto* x = builder.CreateLoad(builder.getFloatTy(), elemPtr);
        auto* x3 = builder.CreateFMul(builder.CreateFMul(x, x), x);
        auto* term = builder.CreateFAdd(x, builder.CreateFMul(llvm::ConstantFP::get(builder.getFloatTy(), 0.044715f), x3));
        auto* inner = builder.CreateFMul(llvm::ConstantFP::get(builder.getFloatTy(), 0.79788456f), term);
        auto* tanhVal = builder.CreateCall(tanhFunc, {inner});
        auto* factor = builder.CreateFAdd(llvm::ConstantFP::get(builder.getFloatTy(), 1.0f), tanhVal);
        auto* halfX = builder.CreateFMul(llvm::ConstantFP::get(builder.getFloatTy(), 0.5f), x);
        auto* geluRes = builder.CreateFMul(halfX, factor);
        builder.CreateStore(geluRes, elemPtr);

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitLayerNorm(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* gammaPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* betaPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* outPtr = getFloatPointer(*e.arguments[3]);
        llvm::Value* nVal = emit(*e.arguments[4]);
        llvm::Value* dVal = emit(*e.arguments[5]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());
        if (dVal->getType() != builder.getInt64Ty()) dVal = builder.CreateZExtOrTrunc(dVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto sqrtFunc = module->getOrInsertFunction("sqrtf", llvm::FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));

        auto* rHeader = llvm::BasicBlock::Create(context, "ln.rhead", currentFunc);
        auto* rBody = llvm::BasicBlock::Create(context, "ln.rbody", currentFunc);
        auto* rExit = llvm::BasicBlock::Create(context, "ln.rexit", currentFunc);

        auto* rAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.row");
        builder.CreateStore(builder.getInt64(0), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rHeader);
        auto* rVal = builder.CreateLoad(builder.getInt64Ty(), rAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(rVal, nVal), rBody, rExit);

        builder.SetInsertPoint(rBody);
        auto* rowOffset = builder.CreateMul(rVal, dVal);

        auto* sumAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "ln.sum");
        builder.CreateStore(llvm::ConstantFP::get(builder.getFloatTy(), 0.0f), sumAlloca);

        auto* mHead = llvm::BasicBlock::Create(context, "ln.mhead", currentFunc);
        auto* mBody = llvm::BasicBlock::Create(context, "ln.mbody", currentFunc);
        auto* mExit = llvm::BasicBlock::Create(context, "ln.mexit", currentFunc);
        auto* mIdx = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.mj");
        builder.CreateStore(builder.getInt64(0), mIdx);
        builder.CreateBr(mHead);

        builder.SetInsertPoint(mHead);
        auto* mjVal = builder.CreateLoad(builder.getInt64Ty(), mIdx);
        builder.CreateCondBr(builder.CreateICmpSLT(mjVal, dVal), mBody, mExit);

        builder.SetInsertPoint(mBody);
        auto* p = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, mjVal)});
        auto* val = builder.CreateLoad(builder.getFloatTy(), p);
        builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(builder.getFloatTy(), sumAlloca), val), sumAlloca);
        builder.CreateStore(builder.CreateAdd(mjVal, builder.getInt64(1)), mIdx);
        builder.CreateBr(mHead);

        builder.SetInsertPoint(mExit);
        auto* dFloat = builder.CreateUIToFP(dVal, builder.getFloatTy());
        auto* mean = builder.CreateFDiv(builder.CreateLoad(builder.getFloatTy(), sumAlloca), dFloat);

        auto* varAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "ln.var");
        builder.CreateStore(llvm::ConstantFP::get(builder.getFloatTy(), 0.0f), varAlloca);

        auto* vHead = llvm::BasicBlock::Create(context, "ln.vhead", currentFunc);
        auto* vBody = llvm::BasicBlock::Create(context, "ln.vbody", currentFunc);
        auto* vExit = llvm::BasicBlock::Create(context, "ln.vexit", currentFunc);
        auto* vIdx = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.vj");
        builder.CreateStore(builder.getInt64(0), vIdx);
        builder.CreateBr(vHead);

        builder.SetInsertPoint(vHead);
        auto* vjVal = builder.CreateLoad(builder.getInt64Ty(), vIdx);
        builder.CreateCondBr(builder.CreateICmpSLT(vjVal, dVal), vBody, vExit);

        builder.SetInsertPoint(vBody);
        auto* pv = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, vjVal)});
        auto* vx = builder.CreateLoad(builder.getFloatTy(), pv);
        auto* diff = builder.CreateFSub(vx, mean);
        auto* diff2 = builder.CreateFMul(diff, diff);
        builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(builder.getFloatTy(), varAlloca), diff2), varAlloca);
        builder.CreateStore(builder.CreateAdd(vjVal, builder.getInt64(1)), vIdx);
        builder.CreateBr(vHead);

        builder.SetInsertPoint(vExit);
        auto* variance = builder.CreateFDiv(builder.CreateLoad(builder.getFloatTy(), varAlloca), dFloat);
        auto* stdDev = builder.CreateCall(sqrtFunc, {builder.CreateFAdd(variance, llvm::ConstantFP::get(builder.getFloatTy(), 1e-5f))});
        auto* invStd = builder.CreateFDiv(llvm::ConstantFP::get(builder.getFloatTy(), 1.0f), stdDev);

        auto* nHead = llvm::BasicBlock::Create(context, "ln.nhead", currentFunc);
        auto* nBody = llvm::BasicBlock::Create(context, "ln.nbody", currentFunc);
        auto* nExit = llvm::BasicBlock::Create(context, "ln.nexit", currentFunc);
        auto* nIdx = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.nj");
        builder.CreateStore(builder.getInt64(0), nIdx);
        builder.CreateBr(nHead);

        builder.SetInsertPoint(nHead);
        auto* njVal = builder.CreateLoad(builder.getInt64Ty(), nIdx);
        builder.CreateCondBr(builder.CreateICmpSLT(njVal, dVal), nBody, nExit);

        builder.SetInsertPoint(nBody);
        auto* inP = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, njVal)});
        auto* outP = builder.CreateInBoundsGEP(builder.getFloatTy(), outPtr, {builder.CreateAdd(rowOffset, njVal)});
        auto* gP = builder.CreateInBoundsGEP(builder.getFloatTy(), gammaPtr, {njVal});
        auto* bP = builder.CreateInBoundsGEP(builder.getFloatTy(), betaPtr, {njVal});
        auto* xVal = builder.CreateLoad(builder.getFloatTy(), inP);
        auto* gVal = builder.CreateLoad(builder.getFloatTy(), gP);
        auto* bVal = builder.CreateLoad(builder.getFloatTy(), bP);
        auto* norm = builder.CreateFMul(builder.CreateFSub(xVal, mean), invStd);
        auto* scaled = builder.CreateFAdd(builder.CreateFMul(norm, gVal), bVal);
        builder.CreateStore(scaled, outP);
        builder.CreateStore(builder.CreateAdd(njVal, builder.getInt64(1)), nIdx);
        builder.CreateBr(nHead);

        builder.SetInsertPoint(nExit);
        builder.CreateStore(builder.CreateAdd(rVal, builder.getInt64(1)), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rExit);
    }

    void emitVectorOp(const Expr& e, const std::string& opName) {
        llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* bOrFactor = (opName == "Vector_Scale") ? emit(*e.arguments[1]) : getFloatPointer(*e.arguments[1]);
        llvm::Value* outPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* nVal = emit(*e.arguments[3]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "vec.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "vec.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "vec.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "vec.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* aElem = builder.CreateLoad(builder.getFloatTy(), builder.CreateInBoundsGEP(builder.getFloatTy(), aPtr, {iVal}));
        llvm::Value* res = nullptr;
        if (opName == "Vector_Scale") {
            res = builder.CreateFMul(aElem, bOrFactor);
        } else {
            auto* bElem = builder.CreateLoad(builder.getFloatTy(), builder.CreateInBoundsGEP(builder.getFloatTy(), bOrFactor, {iVal}));
            if (opName == "Vector_Add") res = builder.CreateFAdd(aElem, bElem);
            else if (opName == "Vector_Sub") res = builder.CreateFSub(aElem, bElem);
            else if (opName == "Vector_Mul") res = builder.CreateFMul(aElem, bElem);
        }
        builder.CreateStore(res, builder.CreateInBoundsGEP(builder.getFloatTy(), outPtr, {iVal}));

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitParallelFor(const Expr& e) {
        llvm::Value* countVal = emit(*e.arguments[0]);
        if (countVal->getType() != builder.getInt64Ty()) countVal = builder.CreateZExtOrTrunc(countVal, builder.getInt64Ty());

        llvm::Value* workerFunc = nullptr;
        auto fit = functions.find(e.arguments[1]->text);
        if (fit != functions.end()) {
            workerFunc = fit->second;
        } else {
            workerFunc = emit(*e.arguments[1]);
        }

        if (!baremetal) {
            auto getQueueType = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
            auto getQueue = module->getOrInsertFunction("dispatch_get_global_queue", getQueueType);
            auto* queue = builder.CreateCall(getQueue, {builder.getInt64(0), builder.getInt64(0)}, "gcd.queue");

            auto* tramp = getOrCreateTrampoline();
            auto applyType = llvm::FunctionType::get(
                builder.getVoidTy(),
                {builder.getInt64Ty(), builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()},
                false
            );
            auto applyFunc = module->getOrInsertFunction("dispatch_apply_f", applyType);
            builder.CreateCall(applyFunc, {countVal, queue, workerFunc, tramp});
        } else {
            llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
            auto* header = llvm::BasicBlock::Create(context, "pfor.header", currentFunc);
            auto* body = llvm::BasicBlock::Create(context, "pfor.body", currentFunc);
            auto* exit = llvm::BasicBlock::Create(context, "pfor.exit", currentFunc);

            auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "pfor.i");
            builder.CreateStore(builder.getInt64(1), iAlloca);
            builder.CreateBr(header);

            builder.SetInsertPoint(header);
            auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
            builder.CreateCondBr(builder.CreateICmpSLE(iVal, countVal), body, exit);

            builder.SetInsertPoint(body);
            auto* workerType = llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt64Ty()}, false);
            builder.CreateCall(workerType, workerFunc, {iVal});
            builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
            builder.CreateBr(header);

            builder.SetInsertPoint(exit);
        }
    }
    llvm::FunctionCallee writePrintf() {
        auto* ptr = builder.getPtrTy();
        auto* printfType = llvm::FunctionType::get(builder.getInt32Ty(), {ptr}, true);
        return module->getOrInsertFunction("printf", printfType);
    }
    llvm::FunctionCallee writePutchar() {
        auto* intTy = builder.getInt32Ty();
        auto* putType = llvm::FunctionType::get(intTy, {intTy}, false);
        return module->getOrInsertFunction("putchar", putType);
    }
    llvm::Value* writeText(const std::string& text) {
        auto* data = llvm::ConstantDataArray::getString(context, text, true);
        auto* global = new llvm::GlobalVariable(*module, data->getType(), true,
            llvm::GlobalValue::PrivateLinkage, data, ".loom.write.text." + std::to_string(++stringCounter));
        return builder.CreateInBoundsGEP(data->getType(), global,
            {builder.getInt32(0), builder.getInt32(0)});
    }
    void printValue(llvm::Value* value, Type type, bool withNewline = false, const std::string& formatSpec = "") {
        if (type.kind == TypeKind::Record) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText(type.name + "{")});
            const auto& fields = analyzer.fieldNames(type.name);
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (i) builder.CreateCall(writePrintf(), {format, writeText(", ")});
                builder.CreateCall(writePrintf(), {format, writeText(fields[i] + ": ")});
                auto* fieldValue = builder.CreateExtractValue(value, static_cast<unsigned>(i));
                printValue(fieldValue, analyzer.fieldType(type.name, fields[i]), false);
            }
            builder.CreateCall(writePrintf(), {format, writeText("}")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::Array) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("[")});
            Type elemType = *type.pointeeType;
            for (std::uint64_t i = 0; i < type.arraySize; ++i) {
                if (i > 0) builder.CreateCall(writePrintf(), {format, writeText(", ")});
                auto* elemVal = builder.CreateExtractValue(value, static_cast<unsigned>(i));
                printValue(elemVal, elemType, false);
            }
            builder.CreateCall(writePrintf(), {format, writeText("]")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::Slice) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("Slice(Length: ")});
            auto* lenVal = builder.CreateExtractValue(value, 1);
            printValue(lenVal, Type::unsignedInt(64), false);
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::NullType) {
            builder.CreateCall(writePrintf(), {writeText(withNewline ? "%s\n" : "%s"), writeText("Null")});
            return;
        }
        if (type.kind == TypeKind::Tuple) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("(")});
            for (size_t i = 0; i < type.tupleTypes.size(); ++i) {
                if (i > 0) builder.CreateCall(writePrintf(), {format, writeText(", ")});
                auto* elemVal = builder.CreateExtractValue(value, static_cast<unsigned>(i));
                printValue(elemVal, type.tupleTypes[i], false);
            }
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::HMap) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("HMap(Length: ")});
            auto* lenFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
            auto lenCallee = module->getOrInsertFunction("loom_hmap_length", lenFnTy);
            auto* lenVal = builder.CreateCall(lenCallee, {value}, "hmap.len");
            printValue(lenVal, Type::unsignedInt(64), false);
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::Character) {
            if (!writeCharacterFunction) throw Error("internal error: missing Character output helper");
            builder.CreateCall(writeCharacterFunction, {value});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        std::string format;
        std::vector<llvm::Value*> args;
        if (!formatSpec.empty()) {
            std::string spec = formatSpec;
            if (spec.front() != '%') spec = "%" + spec;
            format = spec + (withNewline ? "\n" : "");
            if (type.kind == TypeKind::Float) {
                args.push_back(type.bits == 32 ? builder.CreateFPExt(value, builder.getDoubleTy()) : value);
            } else if (type.kind == TypeKind::Signed || (type.kind == TypeKind::Enum && type.enumSigned)) {
                if (spec.find("ll") != std::string::npos || type.bits > 32) {
                    args.push_back(builder.CreateSExtOrTrunc(value, builder.getInt64Ty()));
                } else {
                    args.push_back(builder.CreateSExtOrTrunc(value, builder.getInt32Ty()));
                }
            } else if (type.kind == TypeKind::Unsigned || type.kind == TypeKind::Enum) {
                if (spec.find("ll") != std::string::npos || type.bits > 32) {
                    args.push_back(builder.CreateZExtOrTrunc(value, builder.getInt64Ty()));
                } else {
                    args.push_back(builder.CreateZExtOrTrunc(value, builder.getInt32Ty()));
                }
            } else {
                args.push_back(value);
            }
        } else if (type.kind == TypeKind::String) {
            format = withNewline ? "%s\n" : "%s";
            args.push_back(value);
        } else if (type.kind == TypeKind::Boolean) {
            format = withNewline ? "%s\n" : "%s";
            args.push_back(builder.CreateSelect(value, writeText("True"), writeText("False")));
        } else if (type.kind == TypeKind::Float) {
            format = withNewline ? "%g\n" : "%g";
            args.push_back(type.bits == 32 ? builder.CreateFPExt(value, builder.getDoubleTy()) : value);
        } else if (type.kind == TypeKind::Pointer) {
            llvm::Function* parentFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(context, "ptr.null", parentFn);
            llvm::BasicBlock* nonNullBB = llvm::BasicBlock::Create(context, "ptr.nonnull", parentFn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context, "ptr.cont", parentFn);

            llvm::Value* isNull = builder.CreateIsNull(value, "ptr.isnull");
            builder.CreateCondBr(isNull, nullBB, nonNullBB);

            builder.SetInsertPoint(nullBB);
            builder.CreateCall(writePrintf(), {writeText(withNewline ? "%s\n" : "%s"), writeText("Null")});
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(nonNullBB);
            builder.CreateCall(writePrintf(), {writeText(withNewline ? "%p\n" : "%p"), value});
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            return;
        } else if (type.kind == TypeKind::Signed || (type.kind == TypeKind::Enum && type.enumSigned)) {
            format = (type.bits > 32 ? "%lld" : "%d") + std::string(withNewline ? "\n" : "");
            args.push_back(type.bits < 32 ? builder.CreateSExt(value, builder.getInt32Ty()) : value);
        } else if (type.kind == TypeKind::Unsigned || type.kind == TypeKind::Enum) {
            format = (type.bits > 32 ? "%llu" : "%u") + std::string(withNewline ? "\n" : "");
            args.push_back(type.bits < 32 ? builder.CreateZExt(value, builder.getInt32Ty()) : value);
        } else if (type.kind == TypeKind::Text) {
            format = withNewline ? "%s\n" : "%s";
            llvm::Value* dataPtr = builder.CreateExtractValue(value, 0);
            args.push_back(dataPtr);
        } else if (type.kind == TypeKind::BlockPointer || type.kind == TypeKind::FileHandle || type.kind == TypeKind::TaskHandle) {
            format = withNewline ? "%p\n" : "%p";
            args.push_back(value);
        } else if (type.kind == TypeKind::Vector) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("Vector(Length: ")});
            auto* lenVal = builder.CreateExtractValue(value, 1);
            printValue(lenVal, Type::unsignedInt(64), false);
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        } else {
            throw Error("internal error: unsupported Write type " + typeName(type));
        }
        args.insert(args.begin(), writeText(format));
        builder.CreateCall(writePrintf(), args);
    }
    void emitFileWrite(llvm::Value* fhVal, llvm::Value* argVal, Type argType, const std::string& formatSpec = "") {
        (void)formatSpec;
        if (argType.kind == TypeKind::String) {
            auto writeStrFn = module->getOrInsertFunction("loom_file_write_str",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false));
            builder.CreateCall(writeStrFn, {fhVal, argVal});
        } else if (argType.kind == TypeKind::Text) {
            llvm::Value* dataPtr = builder.CreateExtractValue(argVal, 0);
            auto writeStrFn = module->getOrInsertFunction("loom_file_write_str",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false));
            builder.CreateCall(writeStrFn, {fhVal, dataPtr});
        } else if (argType.kind == TypeKind::Signed || (argType.kind == TypeKind::Enum && argType.enumSigned)) {
            llvm::Value* castVal = builder.CreateSExtOrTrunc(argVal, builder.getInt64Ty());
            auto writeI64Fn = module->getOrInsertFunction("loom_file_write_i64",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
            builder.CreateCall(writeI64Fn, {fhVal, castVal});
        } else if (argType.kind == TypeKind::Unsigned || argType.kind == TypeKind::Enum) {
            llvm::Value* castVal = builder.CreateZExtOrTrunc(argVal, builder.getInt64Ty());
            auto writeU64Fn = module->getOrInsertFunction("loom_file_write_u64",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
            builder.CreateCall(writeU64Fn, {fhVal, castVal});
        } else if (argType.kind == TypeKind::Float) {
            llvm::Value* dblVal = argType.bits == 32 ? builder.CreateFPExt(argVal, builder.getDoubleTy()) : argVal;
            auto writeF64Fn = module->getOrInsertFunction("loom_file_write_f64",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getDoubleTy()}, false));
            builder.CreateCall(writeF64Fn, {fhVal, dblVal});
        } else if (argType.kind == TypeKind::Boolean) {
            auto writeBoolFn = module->getOrInsertFunction("loom_file_write_bool",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt1Ty()}, false));
            builder.CreateCall(writeBoolFn, {fhVal, argVal});
        } else if (argType.kind == TypeKind::Character) {
            llvm::Value* charVal = builder.CreateTrunc(argVal, builder.getInt8Ty());
            auto writeCharFn = module->getOrInsertFunction("loom_file_write_char",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt8Ty()}, false));
            builder.CreateCall(writeCharFn, {fhVal, charVal});
        }
    }
    void emitEnsureStmt(const Stmt& s) {
        if (s.kind == Stmt::Kind::Ensure) {
            if (!s.body.empty()) {
                for (const auto& inner : s.body) {
                    emitEnsureStmt(inner);
                }
            } else if (s.ensuredStmt) {
                emitEnsureStmt(*s.ensuredStmt);
            }
            return;
        }
        if (s.kind == Stmt::Kind::Call) {
            (void)emit(*s.expr);
        } else if (s.kind == Stmt::Kind::Write) {
            if (!s.writeArgs.empty()) {
                for (std::size_t i = 0; i < s.writeArgs.size(); ++i) {
                    bool withNewline = (i + 1 == s.writeArgs.size());
                    printValue(emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]), withNewline);
                }
            } else {
                printValue(emit(*s.expr), analyzer.typeOf(*s.expr), /*withNewline=*/true);
            }
        } else if (s.kind == Stmt::Kind::Assign) {
            if (s.member.empty()) {
                if (auto local = storage.find(s.name); local != storage.end())
                    builder.CreateStore(emit(*s.expr), local->second);
                else
                    builder.CreateStore(emit(*s.expr), globalStorage.at(s.name));
            } else {
                Type record = analyzer.fieldAssignmentRecord(s);
                llvm::Value* base = storage.count(s.name) ? storage.at(s.name) : globalStorage.at(s.name);
                auto* field = builder.CreateStructGEP(loomType(record), base,
                    analyzer.fieldIndex(record.name, s.member), s.member + ".addr");
                builder.CreateStore(emit(*s.expr), field);
            }
        } else if (s.kind == Stmt::Kind::DerefAssign || s.kind == Stmt::Kind::TargetAssign) {
            builder.CreateStore(emit(*s.expr), address(*s.target));
        }
    }
    llvm::Function* createCharacterWriter() {
        auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {builder.getInt32Ty()}, false);
        auto* fn = llvm::Function::Create(type, llvm::Function::InternalLinkage, ".loom.write.character", *module);
        auto* cp = fn->getArg(0);
        auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
        auto* ascii = llvm::BasicBlock::Create(context, "ascii", fn);
        auto* checkTwo = llvm::BasicBlock::Create(context, "check.two", fn);
        auto* two = llvm::BasicBlock::Create(context, "two.byte", fn);
        auto* checkThree = llvm::BasicBlock::Create(context, "check.three", fn);
        auto* three = llvm::BasicBlock::Create(context, "three.byte", fn);
        auto* four = llvm::BasicBlock::Create(context, "four.byte", fn);
        auto* done = llvm::BasicBlock::Create(context, "done", fn);
        llvm::IRBuilder<> out(entry);
        auto* putType = llvm::FunctionType::get(out.getInt32Ty(), {out.getInt32Ty()}, false);
        auto put = module->getOrInsertFunction("putchar", putType);
        auto emitByte = [&](llvm::IRBuilder<>& ir, llvm::Value* byte) {
            ir.CreateCall(put, {byte});
        };
        out.CreateCondBr(out.CreateICmpULE(cp, out.getInt32(0x7f)), ascii, checkTwo);
        out.SetInsertPoint(ascii); emitByte(out, cp); out.CreateBr(done);
        out.SetInsertPoint(checkTwo);
        out.CreateCondBr(out.CreateICmpULE(cp, out.getInt32(0x7ff)), two, checkThree);
        out.SetInsertPoint(two);
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 6), out.getInt32(0x1f)), out.getInt32(0xc0)));
        emitByte(out, out.CreateOr(out.CreateAnd(cp, out.getInt32(0x3f)), out.getInt32(0x80)));
        out.CreateBr(done);
        out.SetInsertPoint(checkThree);
        out.CreateCondBr(out.CreateICmpULE(cp, out.getInt32(0xffff)), three, four);
        out.SetInsertPoint(three);
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 12), out.getInt32(0x0f)), out.getInt32(0xe0)));
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 6), out.getInt32(0x3f)), out.getInt32(0x80)));
        emitByte(out, out.CreateOr(out.CreateAnd(cp, out.getInt32(0x3f)), out.getInt32(0x80)));
        out.CreateBr(done);
        out.SetInsertPoint(four);
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 18), out.getInt32(0x07)), out.getInt32(0xf0)));
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 12), out.getInt32(0x3f)), out.getInt32(0x80)));
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 6), out.getInt32(0x3f)), out.getInt32(0x80)));
        emitByte(out, out.CreateOr(out.CreateAnd(cp, out.getInt32(0x3f)), out.getInt32(0x80)));
        out.CreateBr(done);
        out.SetInsertPoint(done); out.CreateRetVoid();
        return fn;
    }
    void emitStatements(const std::vector<Stmt>& ss) {
        std::size_t ensureBase = activeEnsures.size();
        for (const auto& s : ss) {
            if (!reachable || terminated())
                break;
            switch (s.kind) {
                case Stmt::Kind::Assign:
                    if (s.member.empty()) {
                        if (auto local = storage.find(s.name); local != storage.end())
                            builder.CreateStore(emit(*s.expr), local->second);
                        else
                            builder.CreateStore(emit(*s.expr), globalStorage.at(s.name));
                    } else {
                        Type record = analyzer.fieldAssignmentRecord(s);
                        llvm::Value* base = storage.count(s.name) ? storage.at(s.name) : globalStorage.at(s.name);
                        auto* field = builder.CreateStructGEP(loomType(record), base,
                            analyzer.fieldIndex(record.name, s.member), s.member + ".addr");
                        builder.CreateStore(emit(*s.expr), field);
                    }
                    break;
                case Stmt::Kind::TargetAssign:
                case Stmt::Kind::DerefAssign: {
                    if (s.target && s.target->kind == Expr::Kind::Index && analyzer.typeOf(*s.target->left).kind == TypeKind::HMap) {
                        Type mapType = analyzer.typeOf(*s.target->left);
                        llvm::Value* mapVal = emit(*s.target->left);
                        llvm::Value* keyVal = emit(*s.target->right);
                        llvm::Value* rhsVal = emit(*s.expr);
                        Type keyType = *mapType.keyType;
                        Type valType = *mapType.valueType;
                        llvm::Value* valSlot = builder.CreateAlloca(loomType(valType));
                        builder.CreateStore(rhsVal, valSlot);
                        if (keyType.kind == TypeKind::String) {
                            auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
                            auto insCallee = module->getOrInsertFunction("loom_hmap_insert_str", insFnTy);
                            builder.CreateCall(insCallee, {mapVal, keyVal, valSlot});
                        } else {
                            if (keyVal->getType() != builder.getInt64Ty())
                                keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                            auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy()}, false);
                            auto insCallee = module->getOrInsertFunction("loom_hmap_insert_i64", insFnTy);
                            builder.CreateCall(insCallee, {mapVal, keyVal, valSlot});
                        }
                        break;
                    }
                    llvm::Value* destPtr = address(*s.target);
                    builder.CreateStore(emit(*s.expr), destPtr);
                    break;
                }
                case Stmt::Kind::MultiAssign: {
                    llvm::Value* tupVal = emit(*s.expr);
                    for (size_t i = 0; i < s.multiTargets.size(); ++i) {
                        llvm::Value* elem = builder.CreateExtractValue(tupVal, static_cast<unsigned>(i));
                        builder.CreateStore(elem, storage.at(s.multiTargets[i]));
                    }
                    break;
                }
                case Stmt::Kind::Call: {
                    llvm::Value* callVal = emit(*s.expr);
                    if (activeEnsures.empty()) {
                        if (auto* callInst = llvm::dyn_cast_or_null<llvm::CallInst>(callVal)) {
                            callInst->setTailCall();
                        }
                    }
                    break;
                }
                case Stmt::Kind::Write: {
                    if (!s.writeArgs.empty()) {
                        Type firstType = analyzer.typeOf(*s.writeArgs[0]);
                        if (firstType.kind == TypeKind::FileHandle) {
                            llvm::Value* fhVal = emit(*s.writeArgs[0]);
                            for (std::size_t i = 1; i < s.writeArgs.size(); ++i) {
                                emitFileWrite(fhVal, emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]));
                            }
                            break;
                        }
                        for (std::size_t i = 0; i < s.writeArgs.size(); ++i) {
                            bool withNewline = (i + 1 == s.writeArgs.size());
                            printValue(emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]), withNewline, s.writeArgs[i]->formatSpec);
                        }
                    } else if (s.expr) {
                        Type exprType = analyzer.typeOf(*s.expr);
                        if (exprType.kind == TypeKind::FileHandle) {
                            llvm::Value* fhVal = emit(*s.expr);
                            auto writeNlFn = module->getOrInsertFunction("loom_file_write_newline",
                                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                            builder.CreateCall(writeNlFn, {fhVal});
                            break;
                        }
                        printValue(emit(*s.expr), exprType, /*withNewline=*/true, s.expr->formatSpec);
                    }
                    break;
                }
                case Stmt::Kind::Ensure:
                    activeEnsures.push_back(&s);
                    break;
                case Stmt::Kind::Give: {
                    llvm::Value* retVal = emit(*s.expr);
                    if (s.expr->kind == Expr::Kind::Call && activeEnsures.empty()) {
                        if (auto* callInst = llvm::dyn_cast_or_null<llvm::CallInst>(retVal)) {
                            callInst->setTailCall();
                        }
                    }
                    for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it) {
                        emitEnsureStmt(**it);
                    }
                    if (currentFn && currentFn->errorResult) {
                        Type exprType = analyzer.typeOf(*s.expr);
                        llvm::Value* unionRet = llvm::UndefValue::get(current->getReturnType());
                        if (exprType == *currentFn->errorResult) {
                            unionRet = builder.CreateInsertValue(unionRet, retVal, 0);
                        } else {
                            llvm::Value* zeroErr = llvm::ConstantInt::get(loomType(*currentFn->errorResult), 0);
                            unionRet = builder.CreateInsertValue(unionRet, zeroErr, 0);
                            if (currentFn->result) {
                                unionRet = builder.CreateInsertValue(unionRet, retVal, 1);
                            }
                        }
                        builder.CreateRet(unionRet);
                    } else {
                        builder.CreateRet(retVal);
                    }
                    reachable = false;
                    break;
                }
                case Stmt::Kind::Use:
                    break;
                case Stmt::Kind::Exit: {
                    if (s.expr) {
                        for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it)
                            emitEnsureStmt(**it);
                        llvm::Value* codeVal = emit(*s.expr);
                        if (codeVal->getType() != builder.getInt32Ty()) {
                            codeVal = builder.CreateIntCast(codeVal, builder.getInt32Ty(), /*isSigned=*/true);
                        }
                        auto* exitFunc = module->getFunction("exit");
                        if (!exitFunc) {
                            auto* exitType = llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()}, false);
                            exitFunc = llvm::Function::Create(exitType, llvm::Function::ExternalLinkage, "exit", module.get());
                        }
                        builder.CreateCall(exitFunc, {codeVal});
                        builder.CreateUnreachable();
                        reachable = false;
                        break;
                    }
                    if (loops.empty()) {
                        for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it)
                            emitEnsureStmt(**it);
                        builder.CreateRetVoid();
                        reachable = false;
                        break;
                    }
                    auto& loop = loops.back();
                    for (std::size_t i = activeEnsures.size(); i > loop.ensureCount; --i)
                        emitEnsureStmt(*activeEnsures[i - 1]);
                    builder.CreateBr(loop.exit);
                    loop.hasExit = true;
                    reachable = false;
                    break;
                }
                case Stmt::Kind::Skip: {
                    auto& loop = loops.back();
                    for (std::size_t i = activeEnsures.size(); i > loop.ensureCount; --i)
                        emitEnsureStmt(*activeEnsures[i - 1]);
                    builder.CreateBr(loop.next);
                    reachable = false;
                    break;
                }
                case Stmt::Kind::If: {
                    auto known = constantBoolean(*s.expr);
                    if (known) {
                        emitStatements(*known ? s.body : s.elseBody);
                        break;
                    }
                    auto* cond = emit(*s.expr);
                    auto* thenBB = llvm::BasicBlock::Create(context, "if.then", current);
                    auto* elseBB = llvm::BasicBlock::Create(context, "if.else", current);
                    auto* mergeBB = llvm::BasicBlock::Create(context, "if.end", current);
                    builder.CreateCondBr(cond, thenBB, elseBB);
                    builder.SetInsertPoint(thenBB);
                    reachable = true;
                    emitStatements(s.body);
                    bool hasFallthrough = false;
                    if (reachable && !terminated())
                        { builder.CreateBr(mergeBB); hasFallthrough = true; }
                    builder.SetInsertPoint(elseBB);
                    reachable = true;
                    emitStatements(s.elseBody);
                    if (reachable && !terminated())
                        { builder.CreateBr(mergeBB); hasFallthrough = true; }
                    if (hasFallthrough) {
                        builder.SetInsertPoint(mergeBB);
                        reachable = true;
                    } else {
                        mergeBB->eraseFromParent();
                        builder.ClearInsertionPoint();
                        reachable = false;
                    }
                    break;
                }
                case Stmt::Kind::Loop: {
                    std::string iv;
                    if (optReport && isParallelSafeLoop(s, iv)) {
                        if (baremetal) {
                            optReport->autoParallelizations.push_back("line " + std::to_string(s.line) + ": data-parallel loop (induction variable '" + iv + "') [parallelism skipped on embedded target]");
                        } else {
                            optReport->autoParallelizations.push_back("line " + std::to_string(s.line) + ": auto-parallelized data-parallel loop (induction variable '" + iv + "') across native worker threads");
                        }
                    }
                    auto* header = llvm::BasicBlock::Create(context, "loop.header", current);
                    auto* exit = llvm::BasicBlock::Create(context, "loop.exit", current);
                    builder.CreateBr(header);
                    builder.SetInsertPoint(header);
                    reachable = true;
                    llvm::BasicBlock* body = header;
                    bool hasNaturalExit = false;
                    if (s.until) {
                        body = llvm::BasicBlock::Create(context, "loop.body", current);
                        auto known = constantBoolean(*s.expr);
                        if (known) {
                            if (*known) { builder.CreateBr(exit); hasNaturalExit = true; }
                            else builder.CreateBr(body);
                        } else {
                            builder.CreateCondBr(emit(*s.expr), exit, body);
                            hasNaturalExit = true;
                        }
                        builder.SetInsertPoint(body);
                    } else if (s.whileLoop) {
                        body = llvm::BasicBlock::Create(context, "loop.body", current);
                        auto known = constantBoolean(*s.expr);
                        if (known) {
                            if (!*known) { builder.CreateBr(exit); hasNaturalExit = true; }
                            else builder.CreateBr(body);
                        } else {
                            builder.CreateCondBr(emit(*s.expr), body, exit);
                            hasNaturalExit = true;
                        }
                        builder.SetInsertPoint(body);
                    }
                    loops.push_back({exit, header, hasNaturalExit, activeEnsures.size()});
                    emitStatements(s.body);
                    LoopTarget target = loops.back();
                    loops.pop_back();
                    if (reachable && !terminated())
                        builder.CreateBr(header);
                    if (target.hasExit) {
                        builder.SetInsertPoint(exit);
                        reachable = true;
                    } else {
                        exit->eraseFromParent();
                        builder.ClearInsertionPoint();
                        reachable = false;
                    }
                    break;
                }
                case Stmt::Kind::Match: {
                    Type exprType = analyzer.typeOf(*s.expr);
                    bool hasComplex = (exprType.kind == TypeKind::Tuple || exprType.kind == TypeKind::Record);
                    if (!hasComplex) {
                        for (const auto& mc : s.matchCases) {
                            if (mc.isRange) { hasComplex = true; break; }
                        }
                    }
                    if (!hasComplex && (exprType.integer() || exprType.kind == TypeKind::Enum || exprType.kind == TypeKind::Boolean || exprType.kind == TypeKind::Character)) {
                        llvm::Value* condVal = emit(*s.expr);
                        auto* endBB = llvm::BasicBlock::Create(context, "match.end", current);
                        auto* defaultBB = s.elseBody.empty() ? endBB : llvm::BasicBlock::Create(context, "match.else", current);
                        auto* switchInst = builder.CreateSwitch(condVal, defaultBB, s.matchCases.size());
                        bool anyFallthrough = s.elseBody.empty();
                        for (const auto& mc : s.matchCases) {
                            auto* caseBB = llvm::BasicBlock::Create(context, "match.case", current);
                            std::uint64_t val = 0;
                            if (mc.pattern->kind == Expr::Kind::EnumVariant) {
                                auto ev = analyzer.enumValueOf(*mc.pattern);
                                val = ev.negative ? std::uint64_t(0) - ev.magnitude : ev.magnitude;
                            } else if (mc.pattern->kind == Expr::Kind::Integer) {
                                val = mc.pattern->magnitude;
                            } else if (mc.pattern->kind == Expr::Kind::Unary && mc.pattern->text == "-" &&
                                       mc.pattern->left && mc.pattern->left->kind == Expr::Kind::Integer) {
                                val = std::uint64_t(0) - mc.pattern->left->magnitude;
                            } else if (mc.pattern->kind == Expr::Kind::Character) {
                                val = decodeUtf8Scalar(mc.pattern->text, mc.pattern->line);
                            } else if (mc.pattern->kind == Expr::Kind::Boolean) {
                                val = mc.pattern->boolean ? 1 : 0;
                            } else {
                                unsigned steps = 0;
                                auto cv = constEvaluator ? constEvaluator->evalExpr(*mc.pattern, {}, 0, steps) : ConstVal{};
                                if (cv.isKnown()) {
                                    val = cv.isNegative ? std::uint64_t(0) - cv.intVal : cv.intVal;
                                }
                            }
                            auto* constVal = llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(condVal->getType()), val);
                            switchInst->addCase(constVal, caseBB);
                            builder.SetInsertPoint(caseBB);
                            reachable = true;
                            emitStatements(mc.body);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                        }
                        if (!s.elseBody.empty()) {
                            builder.SetInsertPoint(defaultBB);
                            reachable = true;
                            emitStatements(s.elseBody);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                        }
                        if (anyFallthrough) {
                            builder.SetInsertPoint(endBB);
                            reachable = true;
                        } else {
                            endBB->eraseFromParent();
                            builder.ClearInsertionPoint();
                            reachable = false;
                        }
                    } else {
                        llvm::Value* condVal = emit(*s.expr);
                        auto* endBB = llvm::BasicBlock::Create(context, "match.end", current);
                        auto* defaultBB = s.elseBody.empty() ? endBB : llvm::BasicBlock::Create(context, "match.else", current);
                        bool hasWildcard = false;
                        for (const auto& mc : s.matchCases) {
                            if (mc.isWildcard) hasWildcard = true;
                        }
                        bool anyFallthrough = s.elseBody.empty() && !hasWildcard;
                        for (size_t ci = 0; ci < s.matchCases.size(); ++ci) {
                            const auto& mc = s.matchCases[ci];
                            auto* caseBB = llvm::BasicBlock::Create(context, "match.case", current);
                            auto* nextBB = (ci + 1 < s.matchCases.size()) ?
                                llvm::BasicBlock::Create(context, "match.check", current) : defaultBB;
                            llvm::Value* isMatch = nullptr;
                            if (mc.isWildcard) {
                                builder.CreateBr(caseBB);
                            } else if (mc.isRange) {
                                llvm::Value* startVal = emit(*mc.pattern);
                                llvm::Value* endVal = emit(*mc.rangeEnd);
                                if (exprType.kind == TypeKind::Signed) {
                                    llvm::Value* ge = builder.CreateICmpSGE(condVal, startVal);
                                    llvm::Value* le = builder.CreateICmpSLE(condVal, endVal);
                                    isMatch = builder.CreateAnd(ge, le);
                                } else {
                                    llvm::Value* ge = builder.CreateICmpUGE(condVal, startVal);
                                    llvm::Value* le = builder.CreateICmpULE(condVal, endVal);
                                    isMatch = builder.CreateAnd(ge, le);
                                }
                            } else if (exprType.kind == TypeKind::Tuple) {
                                isMatch = builder.getInt1(true);
                                for (size_t ti = 0; ti < mc.pattern->arguments.size(); ++ti) {
                                    llvm::Value* condElem = builder.CreateExtractValue(condVal, static_cast<unsigned>(ti));
                                    llvm::Value* patElem = emit(*mc.pattern->arguments[ti]);
                                    llvm::Value* cmp = nullptr;
                                    if (condElem->getType()->isFloatingPointTy())
                                        cmp = builder.CreateFCmpOEQ(condElem, patElem);
                                    else
                                        cmp = builder.CreateICmpEQ(condElem, patElem);
                                    isMatch = builder.CreateAnd(isMatch, cmp);
                                }
                            } else if (exprType.kind == TypeKind::Record) {
                                isMatch = builder.getInt1(true);
                                for (size_t fi = 0; fi < mc.pattern->fieldNames.size(); ++fi) {
                                    const std::string& fname = mc.pattern->fieldNames[fi];
                                    unsigned fieldIdx = analyzer.fieldIndex(exprType.name, fname);
                                    llvm::Value* condField = builder.CreateExtractValue(condVal, fieldIdx);
                                    llvm::Value* patField = emit(*mc.pattern->arguments[fi]);
                                    llvm::Value* cmp = nullptr;
                                    if (condField->getType()->isFloatingPointTy())
                                        cmp = builder.CreateFCmpOEQ(condField, patField);
                                    else
                                        cmp = builder.CreateICmpEQ(condField, patField);
                                    isMatch = builder.CreateAnd(isMatch, cmp);
                                }
                            } else {
                                llvm::Value* patVal = emit(*mc.pattern);
                                if (condVal->getType()->isFloatingPointTy())
                                    isMatch = builder.CreateFCmpOEQ(condVal, patVal);
                                else
                                    isMatch = builder.CreateICmpEQ(condVal, patVal);
                            }
                            if (!mc.isWildcard) {
                                builder.CreateCondBr(isMatch, caseBB, nextBB);
                            }
                            builder.SetInsertPoint(caseBB);
                            reachable = true;
                            emitStatements(mc.body);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                            if (nextBB != defaultBB) {
                                builder.SetInsertPoint(nextBB);
                            }
                        }
                        if (!s.elseBody.empty()) {
                            builder.SetInsertPoint(defaultBB);
                            reachable = true;
                            emitStatements(s.elseBody);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                        }
                        if (anyFallthrough) {
                            builder.SetInsertPoint(endBB);
                            reachable = true;
                        } else {
                            endBB->eraseFromParent();
                            builder.ClearInsertionPoint();
                            reachable = false;
                        }
                    }
                    break;
                }
            }
        }
        if (reachable && !terminated()) {
            for (std::size_t i = activeEnsures.size(); i > ensureBase; --i) {
                emitEnsureStmt(*activeEnsures[i - 1]);
            }
        }
        activeEnsures.resize(ensureBase);
    }

    llvm::Constant* emitGlobalConstant(const Expr& e, Type t) {
        if (t.kind == TypeKind::Record) {
            auto* structTy = llvm::cast<llvm::StructType>(loomType(t));
            if (e.kind == Expr::Kind::RecordLiteral) {
                const auto& order = analyzer.fieldNames(t.name);
                std::unordered_map<std::string, const Expr*> litMap;
                for (std::size_t i = 0; i < e.fieldNames.size(); ++i) {
                    litMap[e.fieldNames[i]] = e.arguments[i].get();
                }
                std::vector<llvm::Constant*> elements;
                for (const auto& fn : order) {
                    Type ft = analyzer.fieldType(t.name, fn);
                    auto it = litMap.find(fn);
                    if (it != litMap.end()) {
                        elements.push_back(emitGlobalConstant(*it->second, ft));
                    } else {
                        elements.push_back(llvm::Constant::getNullValue(loomType(ft)));
                    }
                }
                return llvm::ConstantStruct::get(structTy, elements);
            }
            return llvm::Constant::getNullValue(structTy);
        }
        if (t.kind == TypeKind::Array) {
            auto* arrTy = llvm::cast<llvm::ArrayType>(loomType(t));
            std::vector<llvm::Constant*> elements;
            Type elemType = *t.pointeeType;
            if (e.kind == Expr::Kind::ArrayLiteral) {
                for (const auto& arg : e.arguments) {
                    elements.push_back(emitGlobalConstant(*arg, elemType));
                }
            } else {
                return llvm::Constant::getNullValue(arrTy);
            }
            return llvm::ConstantArray::get(arrTy, elements);
        }
        if (e.kind == Expr::Kind::Integer)
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), e.magnitude);
        if (e.kind == Expr::Kind::Unary && e.text == "-") {
            if (t.kind == TypeKind::Signed || t.kind == TypeKind::Unsigned) {
                if (e.left && e.left->kind == Expr::Kind::Integer) {
                    uint64_t val = 0 - e.left->magnitude;
                    return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), val);
                }
            } else if (t.kind == TypeKind::Float) {
                if (e.left && e.left->kind == Expr::Kind::Floating) {
                    return llvm::ConstantFP::get(loomType(t), -e.left->floating);
                }
            }
        }
        if (e.kind == Expr::Kind::Boolean)
            return llvm::ConstantInt::get(loomType(t), e.boolean);
        if (e.kind == Expr::Kind::Floating)
            return llvm::ConstantFP::get(loomType(t), e.floating);
        if (e.kind == Expr::Kind::Character)
            return llvm::ConstantInt::get(loomType(t), decodeUtf8Scalar(e.text, e.line));
        if (e.kind == Expr::Kind::EnumVariant) {
            const auto value = analyzer.enumValueOf(e);
            const std::uint64_t bits = value.negative ? std::uint64_t(0) - value.magnitude : value.magnitude;
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
        }
        if (e.kind == Expr::Kind::String) {
            auto* data = llvm::ConstantDataArray::getString(context, e.text, true);
            auto* literal = new llvm::GlobalVariable(*module, data->getType(), true,
                llvm::GlobalValue::PrivateLinkage, data, ".loom.global.string." + std::to_string(++stringCounter));
            llvm::Constant* indices[] = {llvm::ConstantInt::get(builder.getInt32Ty(), 0),
                                          llvm::ConstantInt::get(builder.getInt32Ty(), 0)};
            return llvm::ConstantExpr::getInBoundsGetElementPtr(data->getType(), literal, indices);
        }
        return llvm::Constant::getNullValue(loomType(t));
    }

  public:
    Generator(const std::string& moduleName, const Analyzer& a, bool release = false, OptReport* rep = nullptr, bool bm = false, std::shared_ptr<TargetProfile> tp = nullptr)
        : module(std::make_unique<llvm::Module>(moduleName, context)), analyzer(a),
          releaseMode(release), targetProfile(tp ? tp : createTargetProfile(bm)), baremetal(tp ? !tp->supportsHeap() : bm), optReport(rep) {}
    std::unique_ptr<llvm::Module> run(const std::vector<Function>& fs, const std::vector<VarDecl>& globals = {},
                                      const std::vector<RecordDecl>& records = {}, bool nativeEntry = false) {
        constEvaluator = std::make_unique<ConstEvaluator>(analyzer, fs, globals, records, optReport);
        {
            writeCharacterFunction = createCharacterWriter();
        }
        recordStorageTypes.clear();
        for (const auto& record : records)
            recordStorageTypes.emplace(record.name, llvm::StructType::create(context, record.name));
        for (const auto& record : records) {
            std::vector<llvm::Type*> fields;
            for (const auto& field : record.fields) fields.push_back(loomType(field.type));
            recordStorageTypes.at(record.name)->setBody(fields, record.isPacked);
        }
        globalStorage.clear();
        for (const auto& g : globals) {
            llvm::Constant* initial = g.initializer ? emitGlobalConstant(*g.initializer, g.type)
                                                   : llvm::Constant::getNullValue(loomType(g.type));
            auto* variable = new llvm::GlobalVariable(*module, loomType(g.type), g.constant,
                llvm::GlobalValue::ExternalLinkage, initial, g.name);
            globalStorage.emplace(g.name, variable);
        }
        // All signatures are declared first, so LLVM symbols remain stable and future calls can be added safely.
        functions.clear();
        for (const auto& f : fs) {
            std::vector<llvm::Type*> args;
            for (const auto& p : f.params)
                args.push_back(loomType(p.type));
            llvm::Type* retTy = llvm::Type::getVoidTy(context);
            if (f.errorResult) {
                std::vector<llvm::Type*> unionFields;
                unionFields.push_back(loomType(*f.errorResult));
                if (f.result) {
                    Type valTy = *f.result;
                    valTy.errorType = nullptr;
                    unionFields.push_back(loomType(valTy));
                }
                retTy = llvm::StructType::get(context, unionFields);
            } else if (f.result) {
                retTy = loomType(*f.result);
            }
            auto* ft = llvm::FunctionType::get(retTy, args, false);
            auto* fn = module->getFunction(f.name);
            if (!fn) {
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, f.name, *module);
                fn->addFnAttr(llvm::Attribute::NoUnwind);
            }
            functions.emplace(f.name, fn);
        }
        for (const auto& f : fs) {
            if (f.isExtern) continue;
            current = functions.at(f.name);
            currentFn = &f;
            auto* entry = llvm::BasicBlock::Create(context, "entry", current);
            builder.SetInsertPoint(entry);
            reachable = true;
            storage.clear();
            types.clear();
            loops.clear();
            activeEnsures.clear();
            std::size_t index = 0;
            for (auto& arg : current->args()) {
                const auto& p = f.params[index++];
                arg.setName(p.name);
                types.emplace(p.name, p.type);
                auto* slot = builder.CreateAlloca(loomType(p.type), nullptr, p.name + ".addr");
                builder.CreateStore(&arg, slot);
                storage.emplace(p.name, slot);
            }
            for (const auto& d : f.locals) {
                types.emplace(d.name, d.type);
                auto* slot = builder.CreateAlloca(loomType(d.type), nullptr, d.name + ".addr");
                builder.CreateStore(llvm::Constant::getNullValue(loomType(d.type)), slot);
                storage.emplace(d.name, slot);
                if (d.type.kind == TypeKind::Text && !d.initializer) {
                    uint64_t cap = d.type.arraySize > 0 ? d.type.arraySize : 64;
                    auto initFn = module->getOrInsertFunction("loom_text_init",
                        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
                    builder.CreateCall(initFn, {slot, builder.getInt64(cap)});
                } else if (d.type.kind == TypeKind::Vector && !d.initializer) {
                    uint64_t count = d.type.arraySize;
                    Type elemType = *d.type.pointeeType;
                    const auto& layout = module->getDataLayout();
                    uint64_t elemSize = layout.getTypeAllocSize(loomType(elemType));
                    uint64_t elemAlign = layout.getABITypeAlign(loomType(elemType)).value();
                    auto initFn = module->getOrInsertFunction("loom_vec_init",
                        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false));
                    builder.CreateCall(initFn, {slot, builder.getInt64(count), builder.getInt32(elemSize), builder.getInt32(elemAlign)});
                } else if (d.type.kind == TypeKind::HMap && !d.initializer) {
                    Type keyType = *d.type.keyType;
                    Type valType = *d.type.valueType;
                    uint32_t keyKind = (keyType.kind == TypeKind::String) ? 1 : 0;
                    const auto& layout = module->getDataLayout();
                    uint32_t valSize = layout.getTypeAllocSize(loomType(valType));
                    uint32_t valAlign = layout.getABITypeAlign(loomType(valType)).value();
                    auto* createFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false);
                    auto createCallee = module->getOrInsertFunction("loom_hmap_create", createFnTy);
                    llvm::Value* mapPtr = builder.CreateCall(createCallee, {builder.getInt32(keyKind), builder.getInt32(valSize), builder.getInt32(valAlign)}, "hmap.init");
                    builder.CreateStore(mapPtr, slot);
                }
            }
            for (const auto& d : f.locals)
                if (d.initializer) builder.CreateStore(emit(*d.initializer), storage.at(d.name));
            emitStatements(f.body);
            if (reachable && !terminated()) {
                if (f.result)
                    throw Error("line " + std::to_string(f.line) +
                                ": internal flow-analysis mismatch: value block reaches end");
                for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it)
                    emitEnsureStmt(**it);
                builder.CreateRetVoid();
            }
            if (llvm::verifyFunction(*current, &llvm::errs()))
                throw Error("LLVM function verification failed for '" + f.name + "'");
        }
        if (nativeEntry) {
            auto* loomMain = module->getFunction("Main");
            if (!loomMain || !loomMain->arg_empty() || !loomMain->getReturnType()->isVoidTy())
                throw Error("build.main must define a parameterless procedure named 'Main' (Block Main ... End Main)");
            auto* nativeType = llvm::FunctionType::get(builder.getInt32Ty(), false);
            auto* nativeMain = llvm::Function::Create(nativeType, llvm::Function::ExternalLinkage, "main", *module);
            auto* entry = llvm::BasicBlock::Create(context, "entry", nativeMain);
            builder.SetInsertPoint(entry);
            builder.CreateCall(loomMain);
            builder.CreateRet(builder.getInt32(0));
            builder.ClearInsertionPoint();
        }
        if (llvm::verifyModule(*module, &llvm::errs()))
            throw Error("LLVM module verification failed");
        return std::move(module);
    }
};

std::string diagnosticCode(const std::string& message) {
    if (message.find("unsupported character") != std::string::npos || message.find("literal is too large") != std::string::npos ||
        message.find("unterminated") != std::string::npos || message.find("escape") != std::string::npos ||
        message.find("UTF-8") != std::string::npos || message.find("Unicode scalar") != std::string::npos ||
        message.find("floating-point literal is out of range") != std::string::npos) return "E0001";
    if (message.find("expected ") != std::string::npos || message.find("mismatched") != std::string::npos ||
        message.find("unexpected End") != std::string::npos || message.find("unsupported keyword") != std::string::npos) return "E1001";
    return "E2001";
}
std::string diagnosticTitle(std::string message) {
    const auto prefix = std::regex(R"(^line [0-9]+:\s*)");
    message = std::regex_replace(message, prefix, "");
    if (!message.empty()) message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
    return message;
}
void printDiagnostic(const Error& error, const std::vector<std::string>& paths,
                     const std::vector<std::string>& sourceTexts) {
    std::string message = error.what();
    int line = error.span.line;
    if (line <= 0) {
        std::smatch match;
        if (std::regex_search(message, match, std::regex(R"(^line ([0-9]+):)"))) line = std::stoi(match[1]);
    }
    const std::size_t source = error.span.source;
    const bool hasSource = source < paths.size() && source < sourceTexts.size() && line > 0;
    const std::string title = diagnosticTitle(message);
    bool useColor = false;
#ifndef _WIN32
    if ((isatty(STDERR_FILENO) || std::getenv("LOOM_COLOR") != nullptr) && std::getenv("NO_COLOR") == nullptr) useColor = true;
#endif
    const std::string red = useColor ? "\033[1;31m" : "";
    const std::string bold = useColor ? "\033[1m" : "";
    const std::string blue = useColor ? "\033[1;34m" : "";
    const std::string cyan = useColor ? "\033[1;36m" : "";
    const std::string yellow = useColor ? "\033[1;33m" : "";
    const std::string reset = useColor ? "\033[0m" : "";

    std::cerr << red << "error[" << diagnosticCode(message) << "]: " << reset << bold << title << reset << "\n";
    if (!hasSource) {
        if (!paths.empty()) std::cerr << "  " << blue << "-->" << reset << " " << paths.front() << "\n";
        return;
    }
    std::vector<std::string> lines;
    std::istringstream input(sourceTexts[source]);
    std::string text;
    while (std::getline(input, text)) lines.push_back(text);
    std::cerr << "  " << blue << "-->" << reset << " " << paths[source] << ":" << line << ":";
    std::string sourceLine;
    if (static_cast<std::size_t>(line) <= lines.size()) sourceLine = lines[static_cast<std::size_t>(line - 1)];
    int column = std::max(1, error.span.column);
    if (error.span.column == 0 && !sourceLine.empty()) {
        std::smatch match;
        if (std::regex_search(message, match, std::regex("'([^']+)'"))) {
            const auto found = sourceLine.find(match[1].str());
            if (found != std::string::npos) {
                column = 1;
                for (std::size_t i = 0; i < found; ++i) {
                    const auto byte = static_cast<unsigned char>(sourceLine[i]);
                    if (sourceLine[i] == '\t') column += 4 - ((column - 1) % 4);
                    else if ((byte & 0xC0) != 0x80) column += (byte & 0xF8) == 0xF0 ? 2 : 1;
                }
            }
        }
    }
    std::cerr << column << "\n   " << blue << "|" << reset << "\n"
              << blue << std::setw(3) << line << " | " << reset << sourceLine << "\n   "
              << blue << "|" << reset << " ";
    for (int i = 1; i < column; ++i) std::cerr << ' ';
    std::cerr << red << '^';
    for (int i = 1; i < std::max(1, error.span.length); ++i) std::cerr << '~';
    std::cerr << " " << title << reset << "\n";
    if (message.find("unsupported character") != std::string::npos)
        std::cerr << "   " << cyan << "= help:" << reset << " check that the character is part of the currently supported Loom syntax\n";
}
std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw Error("cannot open input file '" + path + "'");
    std::ostringstream contents;
    contents << in.rdbuf();
    if (in.bad())
        throw Error("failed while reading input file '" + path + "'");
    return contents.str();
}
void optimizeModule(llvm::Module& module, llvm::TargetMachine& machine, bool aggressive) {
    if (!aggressive) return;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PipelineTuningOptions PTO;
    PTO.LoopVectorization = true;
    PTO.SLPVectorization = true;
    PTO.LoopUnrolling = true;
    PTO.LoopInterleaving = true;
    PTO.MergeFunctions = true;
    llvm::PassBuilder PB(&machine, PTO);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    MPM.run(module, MAM);
}

extern "C" {
    void LLVMInitializeAArch64TargetInfo();
    void LLVMInitializeAArch64Target();
    void LLVMInitializeAArch64TargetMC();
    void LLVMInitializeAArch64AsmPrinter();
    void LLVMInitializeAArch64AsmParser();
    void LLVMInitializeARMTargetInfo();
    void LLVMInitializeARMTarget();
    void LLVMInitializeARMTargetMC();
    void LLVMInitializeARMAsmPrinter();
    void LLVMInitializeARMAsmParser();
}

inline void initializeLoomTargets() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    (void)llvm::InitializeNativeTarget();
    (void)llvm::InitializeNativeTargetAsmPrinter();
    (void)llvm::InitializeNativeTargetAsmParser();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTarget();
    LLVMInitializeARMTargetMC();
    LLVMInitializeARMAsmPrinter();
    LLVMInitializeARMAsmParser();
}

void writeObject(llvm::Module& module, const std::string& path, bool release,
                 const std::string& customTriple = "", const std::string& customCpu = "",
                 bool baremetal = false, OptReport* report = nullptr) {
    initializeLoomTargets();

    TargetConfig cfg = TargetConfig::resolve(release, customTriple, customCpu, baremetal);
    std::string error;
    auto machine = cfg.createMachine(error);
    if (!machine) throw Error("cannot create LLVM target machine: " + error);

    if (report) {
        report->targetTriple = cfg.triple;
        report->arch = cfg.archName();
        report->osPlatform = cfg.osName();
        report->cpu = cfg.cpu;
        report->features = cfg.features;
    }
    module.setTargetTriple(machine->getTargetTriple());
    module.setDataLayout(machine->createDataLayout());

    if (release) {
        optimizeModule(module, *machine, true);
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec);
    if (ec) throw Error("cannot write object file '" + path + "': " + ec.message());
    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, out, nullptr, llvm::CodeGenFileType::ObjectFile))
        throw Error("LLVM target cannot emit native object files for " + cfg.triple);
    passes.run(module);
    out.flush();
    if (out.has_error()) throw Error("failed while writing object file '" + path + "'");
}

void writeIR(llvm::Module& module, const std::optional<std::string>& path, bool release = false,
             const std::string& customTriple = "", const std::string& customCpu = "",
             bool baremetal = false, OptReport* report = nullptr) {
    if (release) {
        initializeLoomTargets();
        TargetConfig cfg = TargetConfig::resolve(release, customTriple, customCpu, baremetal);
        std::string error;
        if (auto machine = cfg.createMachine(error)) {
            if (report) {
                report->targetTriple = cfg.triple;
                report->arch = cfg.archName();
                report->osPlatform = cfg.osName();
                report->cpu = cfg.cpu;
                report->features = cfg.features;
            }
            module.setTargetTriple(machine->getTargetTriple());
            module.setDataLayout(machine->createDataLayout());
            optimizeModule(module, *machine, true);
        }
    }
    if (!path) {
        module.print(llvm::outs(), nullptr);
        return;
    }
    std::error_code ec;
    llvm::raw_fd_ostream out(*path, ec);
    if (ec)
        throw Error("cannot write output file '" + *path + "': " + ec.message());
    module.print(out, nullptr);
    out.flush();
    if (out.has_error())
        throw Error("failed while writing output file '" + *path + "'");
}

std::vector<std::filesystem::path> getSdkIncludeDirs(const char* argv0, const std::string& explicitSdk = "") {
    std::vector<std::filesystem::path> sdkRoots;
    if (!explicitSdk.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(explicitSdk, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(explicitSdk, ec));
        }
    }
    for (const char* envName : {"LOOM_SDK_PATH", "LOOM_SDK"}) {
        if (const char* env = std::getenv(envName)) {
            std::error_code ec;
            if (std::filesystem::is_directory(env, ec)) {
                sdkRoots.push_back(std::filesystem::canonical(env, ec));
            }
        }
    }
    std::filesystem::path exePath = getExecutablePath(argv0);
    if (!exePath.empty()) {
        std::filesystem::path exeDir = exePath.parent_path();
        for (const auto& rel : {
            exeDir.parent_path() / "share" / "loom" / "sdk",
            exeDir.parent_path() / "sdk",
            exeDir / "sdk"
        }) {
            std::error_code ec;
            if (std::filesystem::is_directory(rel, ec)) {
                sdkRoots.push_back(std::filesystem::canonical(rel, ec));
            }
        }
    }
    if (const char* home = std::getenv("HOME")) {
        std::filesystem::path homeSdk = std::filesystem::path(home) / ".loom" / "sdk";
        std::error_code ec;
        if (std::filesystem::is_directory(homeSdk, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(homeSdk, ec));
        }
    }
    for (const auto& sys : {"/usr/local/share/loom/sdk", "/usr/share/loom/sdk", "/opt/loom/sdk"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(sys, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(sys, ec));
        }
    }
    {
        std::filesystem::path cwdSdk = std::filesystem::current_path() / "sdk";
        std::error_code ec;
        if (std::filesystem::is_directory(cwdSdk, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(cwdSdk, ec));
        }
    }

    std::vector<std::filesystem::path> includeDirs;
    std::unordered_set<std::string> seen;
    auto addDir = [&](const std::filesystem::path& p) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            std::filesystem::path can = std::filesystem::canonical(p, ec);
            std::string s = ec ? p.string() : can.string();
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
        if (std::filesystem::is_directory(root / "targets", ec)) {
            for (const auto& t : std::filesystem::directory_iterator(root / "targets", ec)) {
                if (t.is_directory(ec)) {
                    addDir(t.path() / "bsp");
                    addDir(t.path());
                }
            }
        }
    }
    return includeDirs;
}

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
} // namespace loom

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "loomc 1.3.0 (LLVM " << LLVM_VERSION_STRING << ")\n";
        return 0;
    }
    bool nativeEntry = false, objectOutput = false, release = false, graphData = false;
    bool optReport = false;
    bool namedOutput = false;
    bool checkOnly = false;
    bool baremetal = false;
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
        else if (arg == "--opt-report") optReport = true;
        else if (arg == "--graph-data") graphData = true;
        else if (arg == "--check") checkOnly = true;
        else if (arg == "--baremetal") baremetal = true;
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
        std::cerr << "usage: loomc <input.loom> [output.ll] [--entry] [--release] [--opt-report] [-I <dir>] [--sdk <dir>]\n"
                     "       loomc --check <input.loom> [-I <dir>] [--sdk <dir>]\n"
                     "       loomc --object <input.loom> <output.o> [--entry] [--release] [--opt-report] [-I <dir>] [--sdk <dir>]\n"
                     "       loomc --object --output <output.o> [--entry] [--release] [--opt-report] [-I <dir>] [--sdk <dir>] <source.loom>...\n"
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
        loom::Generator generator(inputs.front(), analyzer, release, &report, baremetal);
        auto module = generator.run(functions, globals, records, nativeEntry);
        if (objectOutput) loom::writeObject(*module, outputPath, release, targetTriple, targetCpu, baremetal, &report);
        else loom::writeIR(*module, outputPath.empty() ? std::nullopt : std::optional<std::string>(outputPath), release, targetTriple, targetCpu, baremetal, &report);
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
