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

enum class TypeKind { Invalid, Boolean, Signed, Unsigned, Float, String, Character, Enum, Record, Pointer, IntegerLiteral, Array, Slice, NullType };
struct Type {
    TypeKind kind = TypeKind::Invalid;
    unsigned bits = 0;
    std::string name;
    bool enumSigned = false;
    unsigned arraySize = 0;
    std::shared_ptr<Type> pointeeType = nullptr;

    Type() = default;
    Type(TypeKind k, unsigned b = 0, std::string n = "", bool es = false)
        : kind(k), bits(b), name(std::move(n)), enumSigned(es), arraySize(0), pointeeType(nullptr) {}

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
    static Type sliceType(Type elem) {
        Type t{TypeKind::Slice, 0};
        t.pointeeType = std::make_shared<Type>(std::move(elem));
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
        if (kind == TypeKind::NullType && o.kind == TypeKind::Pointer) return true;
        if (kind == TypeKind::Pointer && o.kind == TypeKind::NullType) return true;
        if (kind != o.kind || bits != o.bits || name != o.name || enumSigned != o.enumSigned)
            return false;
        if (kind == TypeKind::Pointer || kind == TypeKind::Slice) {
            if (!pointeeType || !o.pointeeType) return pointeeType == o.pointeeType;
            return *pointeeType == *o.pointeeType;
        }
        if (kind == TypeKind::Array) {
            if (arraySize != o.arraySize) return false;
            if (!pointeeType || !o.pointeeType) return pointeeType == o.pointeeType;
            return *pointeeType == *o.pointeeType;
        }
        return true;
    }
    bool operator!=(const Type& o) const {
        return !(*this == o);
    }
};
std::string typeName(Type t) {
    if (t.kind == TypeKind::Boolean)
        return "Boolean";
    if (t.kind == TypeKind::Signed)
        return "Signed_" + std::to_string(t.bits);
    if (t.kind == TypeKind::Unsigned)
        return "Unsigned_" + std::to_string(t.bits);
    if (t.kind == TypeKind::Float)
        return "Float_" + std::to_string(t.bits);
    if (t.kind == TypeKind::String) return "String";
    if (t.kind == TypeKind::Character) return "Character";
    if (t.kind == TypeKind::Enum) return t.name;
    if (t.kind == TypeKind::Record) return t.name;
    if (t.kind == TypeKind::Pointer)
        return typeName(*t.pointeeType) + " Pointer";
    if (t.kind == TypeKind::Slice)
        return typeName(*t.pointeeType) + " Slice";
    if (t.kind == TypeKind::Array)
        return typeName(*t.pointeeType) + " Array[" + std::to_string(t.arraySize) + "]";
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
        case TypeKind::Enum: return t.bits;
        case TypeKind::Array: return t.pointeeType ? typeBitSize(*t.pointeeType) * t.arraySize : 0;
        case TypeKind::Slice: return 128;
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
Type parseType(const std::string& s, int line) {
    if (s == "Boolean")
        return Type::boolean();
    if (s == "Float_32") return Type::floating(32);
    if (s == "Float_64") return Type::floating(64);
    if (s == "String") return Type::string();
    if (s == "Character") return Type::character();
    if (s == "Integer" || s == "Int") return Type::signedInt(64);
    if (s == "Null") return Type::nullType();
    for (unsigned bits : {8u, 16u, 32u, 64u}) {
        if (s == "Signed_" + std::to_string(bits))
            return Type::signedInt(bits);
        if (s == "Unsigned_" + std::to_string(bits))
            return Type::unsignedInt(bits);
    }
    throw Error("line " + std::to_string(line) + ": unsupported type '" + s +
                "' (supported: Signed_8/16/32/64, Unsigned_8/16/32/64, Float_32, Float_64, Boolean, String, Character, Pointer[T])");
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
                    emit(K::Dot, ".", atLine);
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
    enum class Kind { Integer, Floating, Boolean, String, Character, EnumVariant, Member, Variable, Call, Unary, Binary, Address, Deref, NullLiteral, Cast, Index, SliceConstructor, ArrayLiteral, RecordLiteral } kind;
    std::string text;
    std::string qualifier;
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
    std::vector<Stmt> body;
    int line = 1;
};

struct Stmt {
    enum class Kind { Assign, Call, Give, Write, Exit, Skip, If, Loop, DerefAssign, Ensure, TargetAssign, Match, Use } kind;
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
};

inline std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    auto c = std::make_unique<Expr>();
    c->kind = e.kind;
    c->text = e.text;
    c->qualifier = e.qualifier;
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
struct EnumVariant { std::string name; std::uint64_t magnitude; bool negative; int line; };
struct EnumDecl { std::string name; Type underlying; std::vector<EnumVariant> variants; int line; std::size_t sourceIndex = 0; };
struct EnumValue { bool negative; std::uint64_t magnitude; Type type; };
struct RecordField { std::string name; Type type; int line; };
struct RecordDecl { std::string name; std::vector<RecordField> fields; int line; std::size_t sourceIndex = 0; };
struct VarDecl {
    std::string name;
    Type type;
    int line = 1;
    bool constant = false;
    std::unique_ptr<Expr> initializer;
    std::size_t sourceIndex = 0;

    VarDecl() = default;
    VarDecl(std::string n, Type t, int l, bool c = false, std::unique_ptr<Expr> init = nullptr, std::size_t src = 0)
        : name(std::move(n)), type(std::move(t)), line(l), constant(c), initializer(std::move(init)), sourceIndex(src) {}
};
struct Function {
    std::string name;
    int line;
    std::vector<VarDecl> params, locals;
    std::optional<Type> result;
    std::vector<Stmt> body;
    std::size_t sourceIndex = 0;
    bool isExtern = false;
    bool isConcise = false;
};
struct Signature {
    std::vector<Type> parameters;
    std::optional<Type> result;
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
            default:
                return 0;
        }
    }
    Type resolveType(const Token& token) const {
        auto known = enumTypes.find(token.text);
        if (known != enumTypes.end()) return known->second;
        auto record = recordTypes.find(token.text);
        return record == recordTypes.end() ? parseType(token.text, token.line) : record->second;
    }
    Type parseTypeRef() {
        Type base;
        if (eat(K::Null)) {
            fail("'Null' is a pointer literal, not a declarable type; declare a typed Pointer instead");
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
        } else if (at(K::Id)) {
            Token name = pop();
            if (name.text == "Pointer") {
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
            } else {
                base = resolveType(name);
            }
        } else {
            fail("expected type name");
        }

        while (true) {
            if (eat(K::Pointer) || (at(K::Id) && t().text == "Pointer")) {
                if (at(K::Id)) pop();
                base = Type::pointer(std::move(base));
            } else if (eat(K::Slice) || (at(K::Id) && t().text == "Slice")) {
                if (at(K::Id)) pop();
                base = Type::sliceType(std::move(base));
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
        } else if (x.kind == K::Id) {
            if (x.text == "Call" && at(K::Id)) {
                fail("the 'Call' keyword has been removed from Loom; call functions directly as '" + t().text + "(...)'");
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
        } else if (x.kind == K::LParen) {
            base = expression();
            expect(K::RParen, "expected ')' to close expression");
        } else if (x.kind == K::Minus || x.kind == K::Not || x.kind == K::Tilde) {
            return make(Expr::Kind::Unary, x, primary());
        } else if (x.kind == K::LBrace) {
            auto lit = make(Expr::Kind::ArrayLiteral, x);
            newlines();
            if (!at(K::RBrace)) {
                do {
                    newlines();
                    if (at(K::RBrace)) break;
                    lit->arguments.push_back(expression());
                    newlines();
                } while (eat(K::Comma));
            }
            newlines();
            expect(K::RBrace, "expected '}' after array elements");
            base = std::move(lit);
        } else if (x.kind == K::Unsupported) {
            throw Error("line " + std::to_string(x.line) + ": unsupported keyword '" + x.text + "'");
        } else {
            throw Error("line " + std::to_string(x.line) + ": expected expression");
        }

        while (at(K::Dot) || at(K::LBracket)) {
            if (eat(K::Dot)) {
                Token field = expect(K::Id, "expected field after '.'");
                base = make(Expr::Kind::Member, field, std::move(base));
            } else if (eat(K::LBracket)) {
                auto idx = expression();
                expect(K::RBracket, "expected ']' after index");
                auto indexed = make(Expr::Kind::Index, x);
                indexed->left = std::move(base);
                indexed->right = std::move(idx);
                base = std::move(indexed);
            }
        }
        return base;
    }
    std::unique_ptr<Expr> expression(int minPrec = 1) {
        auto lhs = primary();
        while (precedence(t().kind) >= minPrec) {
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
                        (std::isalpha(static_cast<unsigned char>(inner[start])) || inner[start] == '_')) {
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
                        (std::isalpha(static_cast<unsigned char>(exprCode[start])) || exprCode[start] == '_')) {
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
                        out.push_back(subParser.expression());
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
        if (eat(K::Give)) {
            s.kind = Stmt::Kind::Give;
            s.expr = expression();
        } else if (eat(K::Write)) {
            parseWriteStatement(s, "Write");
        } else if (at(K::Id) && t().text == "Write_UART" && pos + 1 < ts.size() && ts[pos + 1].kind == K::LParen) {
            Token t_uart = pop();
            parseWriteStatement(s, t_uart.text);
        } else if (eat(K::Exit)) {
            s.kind = Stmt::Kind::Exit;
        } else if (eat(K::Skip)) {
            s.kind = Stmt::Kind::Skip;
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
        newlines();
        while (!at(K::Eof) && !at(K::When) &&
               !(at(K::Else) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Colon) &&
               !(at(K::End) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Match)) {
            Token x = t();
            Stmt s;
            s.line = x.line;
            if (eat(K::Ensure)) {
                s.kind = Stmt::Kind::Ensure;
                expect(K::LBrace, "expected '{' after Ensure");
                newlines();
                if (at(K::RBrace)) fail("Ensure requires one cleanup statement");
                if (at(K::Give) || at(K::Exit) || at(K::Skip))
                    fail("control-flow statements are not valid inside Ensure");
                auto inner = parseSingleStmt();
                newlines();
                expect(K::RBrace, "expected '}' after Ensure cleanup statement");
                s.ensuredStmt = std::make_shared<Stmt>(std::move(inner));
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
                    mc.pattern = expression();
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
                s.kind = Stmt::Kind::Ensure;
                expect(K::LBrace, "expected '{' after Ensure");
                newlines();
                if (at(K::RBrace)) fail("Ensure requires one cleanup statement");
                if (at(K::Give) || at(K::Exit) || at(K::Skip))
                    fail("control-flow statements are not valid inside Ensure");
                auto inner = parseSingleStmt();
                newlines();
                expect(K::RBrace, "expected '}' after Ensure cleanup statement");
                s.ensuredStmt = std::make_shared<Stmt>(std::move(inner));
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
                    mc.pattern = expression();
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
        if (at(K::LParen))
            fail("parenthesized Takes syntax is not part of Loom; use multiline 'Takes' declarations");
        newlines();
        if (eat(K::Takes))
            parseList(f.params, {K::Produces, K::Declares, K::Implements}, "parameter");
        newlines();
        if (eat(K::Produces)) {
            newlines();
            if (at(K::LParen)) {
                f.isConcise = true;
                expect(K::LParen, "expected '(' in concise Produces");
                auto retExpr = expression();
                expect(K::RParen, "expected ')' after concise Produces expression");
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
                return f;
            } else {
                f.result = parseTypeRef();
                newlines();
            }
        }
        if (eat(K::Declares))
            parseList(f.locals, {K::Implements}, "local");
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
    void record() {
        Token start = expect(K::Record, "expected Record declaration");
        Token name = expect(K::Id, "expected record name");
        bool isDuplicateRecord = std::any_of(recordDeclarations.begin(), recordDeclarations.end(), [&](const RecordDecl& d) {
            return d.name == name.text;
        });
        if (isDuplicateRecord)
            throw Error("line " + std::to_string(name.line) + ": duplicate type '" + name.text + "'");
        RecordDecl decl{name.text, {}, start.line, start.sourceIndex};
        requireLineEnd("record header");
        newlines();
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
        expect(K::End, "expected record ending");
        Token close = expect(K::Id, "expected record name after End");
        if (close.text != name.text) throw Error("line " + std::to_string(close.line) + ": mismatched record ending; expected 'End " + name.text + "'");
        if (decl.fields.empty()) throw Error("line " + std::to_string(start.line) + ": Record must declare at least one field");
        recordTypes.insert_or_assign(name.text, Type::recordType(name.text));
        recordDeclarations.push_back(std::move(decl));
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
            if (at(K::Record)) { record(); continue; }
            if (at(K::Block) && pos + 1 < ts.size() && ts[pos + 1].kind == K::Id &&
                ts[pos + 1].text == "Global") {
                globalBlock();
                continue;
            }
            fs.push_back(function());
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
inline bool isBuiltinIntrinsic(const std::string& name) {
    return isVolatileRead(name) || isVolatileWrite(name) || isCpuIntrinsic(name);
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
        if (t.kind == TypeKind::Record) {
            auto it = recordModule.find(t.name);
            if (it != recordModule.end()) checkAccess("record", t.name, it->second.first, line, sourceIdx);
        } else if (t.kind == TypeKind::Enum) {
            auto it = enumModule.find(t.name);
            if (it != enumModule.end()) checkAccess("enum", t.name, it->second.first, line, sourceIdx);
        } else if (t.kind == TypeKind::Pointer || t.kind == TypeKind::Slice || t.kind == TypeKind::Array) {
            if (t.pointeeType) checkTypeAccess(*t.pointeeType, line, sourceIdx);
        }
    }

    static bool constantInitializer(const Expr& e) {
        switch (e.kind) {
            case Expr::Kind::Integer: case Expr::Kind::Floating: case Expr::Kind::Boolean:
            case Expr::Kind::String: case Expr::Kind::Character: case Expr::Kind::EnumVariant: return true;
            case Expr::Kind::Unary:
                return (e.text == "-" || e.text == "not" || e.text == "~") && e.left && constantInitializer(*e.left);
            case Expr::Kind::ArrayLiteral: {
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
                infer(*e.left, env);
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
                } else if (container.kind == TypeKind::String) {
                    result = Type::unsignedInt(8);
                } else {
                    throw Error("line " + std::to_string(e.line) + ": indexing requires an Array, Slice, or String, found " + typeName(container));
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
            case Expr::Kind::Member: {
                Type base = infer(*e.left, env);
                if (base.kind == TypeKind::Slice && e.text == "Length") {
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
                if (i == env.end())
                    throw Error("line " + std::to_string(e.line) + ": undeclared variable '" + e.text + "'");
                if (!currentFunctionLocals.count(e.text) && globalModule.count(e.text))
                    checkAccess("global", e.text, globalModule.at(e.text).first, e.line, currentSourceIndex);
                result = i->second;
                break;
            }
            case Expr::Kind::Call: {
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
        }
        if (expected && result != *expected) {
            if (result.kind == TypeKind::NullType && expected->kind == TypeKind::Pointer) {
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
        if (isVolatileRead(callExpr.text)) {
            throw Error("line " + std::to_string(line) + ": value-producing block '" + callExpr.text + "' cannot be called as a statement");
        }
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
        if (signature->second.result)
            throw Error("line " + std::to_string(line) + ": value-producing block '" + callExpr.text + "' cannot be called as a statement");
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
                }
                Type targetType = infer(*s.target, env);
                infer(*s.expr, env, targetType);
            } else if (s.kind == Stmt::Kind::Ensure) {
                if (s.ensuredStmt) {
                    const Stmt& inner = *s.ensuredStmt;
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
                }
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
                if (!result)
                    throw Error("line " + std::to_string(s.line) + ": Give is only valid in a value-producing block");
                infer(*s.expr, env, *result);
            } else if (s.kind == Stmt::Kind::Write) {
                if (!s.writeArgs.empty()) {
                    for (const auto& arg : s.writeArgs) {
                        Type value = infer(*arg, env, arg->kind == Expr::Kind::Floating ?
                                           std::optional<Type>(Type::floating(64)) : std::nullopt);
                        if (value.kind == TypeKind::Record || value.kind == TypeKind::Array || value.kind == TypeKind::Slice) {
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
                    if (value.kind == TypeKind::Record || value.kind == TypeKind::Array || value.kind == TypeKind::Slice) {
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
                if (loopDepth == 0) {
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
            } else if (s.kind == Stmt::Kind::Match) {
                Type matchType = infer(*s.expr, env);
                if (!matchType.integer() && matchType.kind != TypeKind::Enum && matchType.kind != TypeKind::Boolean && matchType.kind != TypeKind::Character)
                    throw Error("line " + std::to_string(s.line) + ": Match expression must be an integer, enum, boolean, or character, found " + typeName(matchType));
                std::unordered_set<std::string> seenPats;
                for (const auto& mc : s.matchCases) {
                    Type patType = infer(*mc.pattern, env, matchType);
                    if (patType != matchType && !(patType.integer() && matchType.integer()))
                        throw Error("line " + std::to_string(mc.line) + ": pattern type " + typeName(patType) + " does not match Match expression type " + typeName(matchType));
                    std::string patKey = mc.pattern->text;
                    if (mc.pattern->kind == Expr::Kind::Integer) patKey = std::to_string(mc.pattern->magnitude);
                    if (!seenPats.insert(patKey).second)
                        throw Error("line " + std::to_string(mc.line) + ": duplicate pattern '" + patKey + "' in Match");
                }
            }
            Flow one;
            switch (s.kind) {
                case Stmt::Kind::Assign:
                case Stmt::Kind::DerefAssign:
                case Stmt::Kind::TargetAssign:
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
                    if (loopDepth == 0) {
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
                    bool allBranchesReturn = !s.matchCases.empty() && !s.elseBody.empty();
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
};

std::string referenceName(const Expr& expr) {
    if (expr.kind == Expr::Kind::Variable) return expr.text;
    if (expr.kind == Expr::Kind::Member && expr.left) return referenceName(*expr.left) + "." + expr.text;
    if (expr.kind == Expr::Kind::Deref && expr.left) return referenceName(*expr.left);
    if (expr.kind == Expr::Kind::Index && expr.left) return referenceName(*expr.left);
    return {};
}
void collectGraphExpr(const Expr& expr, std::size_t source, const std::string& caller, std::ostream& out) {
    if (expr.kind == Expr::Kind::Call && !isBuiltinIntrinsic(expr.text))
        out << "CALL\t" << source << "\t" << caller << "\t" << expr.text << "\t" << expr.line << "\n";
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
    if (t.kind == TypeKind::Boolean)
        return llvm::Type::getInt1Ty(c);
    if (t.integer() || t.kind == TypeKind::Enum)
        return llvm::IntegerType::get(c, t.bits);
    if (t.kind == TypeKind::Float)
        return t.bits == 32 ? llvm::Type::getFloatTy(c) : llvm::Type::getDoubleTy(c);
    if (t.kind == TypeKind::Character)
        return llvm::Type::getInt32Ty(c);
    if (t.kind == TypeKind::String || t.kind == TypeKind::Pointer || t.kind == TypeKind::NullType)
        return llvm::PointerType::getUnqual(c);
    if (t.kind == TypeKind::Array)
        return llvm::ArrayType::get(llvmType(c, *t.pointeeType, records), t.arraySize);
    if (t.kind == TypeKind::Slice)
        return llvm::StructType::get(c, { llvm::PointerType::getUnqual(c), llvm::Type::getInt64Ty(c) });
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
        if (tcKind == "native") {
            bool isEmbedded = baremetal || t.isThumb() ||
                              customTriple.find("thumb") != std::string::npos ||
                              customTriple.find("none-eabi") != std::string::npos ||
                              customCpu == "cortex-m0plus";
            if (isEmbedded) {
                throw Error("target '" + (customTriple.empty() ? "thumbv6m-none-eabi" : customTriple) +
                            "' is not supported by the Loom native desktop toolchain.\n"
                            "  Please install and switch to the 'rp2040' embedded toolchain:\n"
                            "    loom use rp2040");
            }
        } else if (tcKind == "rp2040") {
            bool isEmbedded = baremetal || t.isThumb() ||
                              customTriple.find("thumb") != std::string::npos ||
                              customTriple.find("none-eabi") != std::string::npos ||
                              customCpu == "cortex-m0plus";
            if (!isEmbedded) {
                throw Error("target '" + cfg.triple +
                            "' is not supported by the Loom 'rp2040' embedded toolchain.\n"
                            "  Please install and switch to the 'native' desktop toolchain:\n"
                            "    loom use native");
            }
        }

        if (!customCpu.empty()) {
            cfg.cpu = customCpu;
            cfg.features = "";
        } else if (baremetal && (t.isARM() || t.isThumb())) {
            cfg.cpu = "cortex-m0plus";
            cfg.features = "";
        } else if (releaseMode && customTriple.empty()) {
            cfg.cpu = llvm::sys::getHostCPUName().str();
            auto featureMap = llvm::sys::getHostCPUFeatures();
            llvm::SubtargetFeatures feat;
            for (const auto& f : featureMap) feat.AddFeature(f.first(), f.second);
            cfg.features = feat.getString();
        } else {
            cfg.cpu = "generic";
            cfg.features = "";
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
    OptReport* optReport = nullptr;
    std::unique_ptr<ConstEvaluator> constEvaluator;
    llvm::Type* loomType(Type t) { return llvmType(context, t, &recordStorageTypes); }
    bool terminated() const {
        auto* b = builder.GetInsertBlock();
        return !b || (!b->empty() && b->back().isTerminator());
    }
    llvm::Value* load(const std::string& name) {
        auto i = storage.find(name);
        if (i != storage.end()) return builder.CreateLoad(loomType(types.at(name)), i->second, name);
        auto g = globalStorage.find(name);
        if (g != globalStorage.end()) return builder.CreateLoad(g->second->getValueType(), g->second, name);
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
            if (targetType.kind == TypeKind::String) {
                llvm::Value* strPtr = emit(*e.left);
                return builder.CreateInBoundsGEP(builder.getInt8Ty(), strPtr, {idx0}, "string.elem.addr");
            }
        }
        throw Error("internal error: expression is not an addressable lvalue");
    }
    llvm::Value* emit(const Expr& e) {
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
            llvm::Type* llvmArrType = loomType(litType);
            llvm::Value* arrVal = llvm::UndefValue::get(llvmArrType);
            for (unsigned i = 0; i < e.arguments.size(); ++i) {
                llvm::Value* elemVal = emit(*e.arguments[i]);
                arrVal = builder.CreateInsertValue(arrVal, elemVal, i, "arr.elem");
            }
            return arrVal;
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
            return builder.CreateLoad(loomType(pointee), ptr, "deref");
        }
        if (e.kind == Expr::Kind::Variable)
            return load(e.text);
        if (e.kind == Expr::Kind::Member) {
            Type baseType = analyzer.typeOf(*e.left);
            if (baseType.kind == TypeKind::Array && e.text == "Length") {
                return builder.getInt64(baseType.arraySize);
            }
            if (baseType.kind == TypeKind::Slice && e.text == "Length") {
                llvm::Value* sliceVal = emit(*e.left);
                return builder.CreateExtractValue(sliceVal, 1, "slice.len");
            }
            if (baseType.kind == TypeKind::String && e.text == "Length") {
                llvm::Value* strVal = emit(*e.left);
                auto* strlenType = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto strlenCallee = module->getOrInsertFunction("strlen", strlenType);
                return builder.CreateCall(strlenCallee, {strVal}, "str.len");
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
            auto f = functions.find(e.text);
            if (f == functions.end()) throw Error("internal error: missing function declaration '" + e.text + "'");
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
        if (t.kind == TypeKind::Float) {
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
    void printValue(llvm::Value* value, Type type, bool withNewline = false) {
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
        if (type.kind == TypeKind::Character) {
            if (!writeCharacterFunction) throw Error("internal error: missing Character output helper");
            builder.CreateCall(writeCharacterFunction, {value});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        std::string format;
        std::vector<llvm::Value*> args;
        if (type.kind == TypeKind::String) {
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
        } else {
            throw Error("internal error: unsupported Write type " + typeName(type));
        }
        args.insert(args.begin(), writeText(format));
        builder.CreateCall(writePrintf(), args);
    }
    void emitEnsureStmt(const Stmt& s) {
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
                    llvm::Value* destPtr = address(*s.target);
                    builder.CreateStore(emit(*s.expr), destPtr);
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
                        for (std::size_t i = 0; i < s.writeArgs.size(); ++i) {
                            bool withNewline = (i + 1 == s.writeArgs.size());
                            printValue(emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]), withNewline);
                        }
                    } else {
                        printValue(emit(*s.expr), analyzer.typeOf(*s.expr), /*withNewline=*/true);
                    }
                    break;
                }
                case Stmt::Kind::Ensure:
                    activeEnsures.push_back(s.ensuredStmt.get());
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
                    builder.CreateRet(retVal);
                    reachable = false;
                    break;
                }
                case Stmt::Kind::Use:
                    break;
                case Stmt::Kind::Exit: {
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
    Generator(const std::string& moduleName, const Analyzer& a, bool release = false, OptReport* rep = nullptr)
        : module(std::make_unique<llvm::Module>(moduleName, context)), analyzer(a),
          releaseMode(release), optReport(rep) {}
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
            recordStorageTypes.at(record.name)->setBody(fields, false);
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
            auto* ft = llvm::FunctionType::get(f.result ? loomType(*f.result) : llvm::Type::getVoidTy(context),
                                               args, false);
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
    llvm::PassBuilder PB(&machine);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    MPM.run(module, MAM);
}

extern "C" {
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
} // namespace loom

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "loomc 1.1.0 (LLVM " << LLVM_VERSION_STRING << ")\n";
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
        loom::Analyzer analyzer;
        analyzer.run(functions, globals, enums, records, parser.modules(), parser.imports(), parser.uses());
        if (checkOnly) {
            return 0;
        }
        if (graphData) {
            loom::writeGraphData(functions, globals, outputPath);
            return 0;
        }
        loom::OptReport report;
        report.enabled = optReport;
        loom::Generator generator(inputs.front(), analyzer, release, &report);
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
