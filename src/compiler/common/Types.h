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

enum class TypeKind { Invalid, Boolean, Signed, Unsigned, Float, String, Character, Enum, Record, Pointer, IntegerLiteral, Array, Slice, NullType, TaskHandle, BlockPointer, Text, Vector, FileHandle, Tuple, HMap, SmartPointer, Channel, Mutex, Atomic, Future };
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
    static Type smartPointer(Type pointee) {
        Type t{TypeKind::SmartPointer, 64};
        t.pointeeType = std::make_shared<Type>(std::move(pointee));
        return t;
    }
    static Type channelType(Type elem) {
        Type t{TypeKind::Channel, 64};
        t.pointeeType = std::make_shared<Type>(std::move(elem));
        return t;
    }
    static Type mutexType() {
        Type t{TypeKind::Mutex, 64};
        t.name = "Mutex";
        return t;
    }
    static Type atomicType(Type inner) {
        Type t{TypeKind::Atomic, inner.bits};
        t.pointeeType = std::make_shared<Type>(std::move(inner));
        return t;
    }
    static Type futureType(Type res) {
        Type t{TypeKind::Future, 64};
        t.pointeeType = std::make_shared<Type>(std::move(res));
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
        if (kind == TypeKind::Pointer || kind == TypeKind::Slice || kind == TypeKind::TaskHandle ||
            kind == TypeKind::SmartPointer || kind == TypeKind::Channel || kind == TypeKind::Atomic || kind == TypeKind::Future) {
            if (!pointeeType || !o.pointeeType) return pointeeType == o.pointeeType;
            return *pointeeType == *o.pointeeType;
        }
        if (kind == TypeKind::Mutex) {
            return o.kind == TypeKind::Mutex;
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
        return prefix + (t.pointeeType && t.pointeeType->kind != TypeKind::NullType ? typeName(*t.pointeeType) : "No_Type") + " Task_Handle";
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
    if (t.kind == TypeKind::SmartPointer) {
        return prefix + (t.pointeeType ? typeName(*t.pointeeType) : "Unknown") + " Smt_Pointer";
    }
    if (t.kind == TypeKind::Channel) {
        return prefix + (t.pointeeType ? typeName(*t.pointeeType) : "Unknown") + " Channel";
    }
    if (t.kind == TypeKind::Mutex) {
        return prefix + "Mutex";
    }
    if (t.kind == TypeKind::Atomic) {
        return prefix + "Atomic[" + (t.pointeeType ? typeName(*t.pointeeType) : "Unknown") + "]";
    }
    if (t.kind == TypeKind::Future) {
        return prefix + "Future[" + (t.pointeeType ? typeName(*t.pointeeType) : "Unknown") + "]";
    }
    if (t.kind == TypeKind::NullType)
        return "No_Type";
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
        case TypeKind::SmartPointer: return 64;
        case TypeKind::Channel: return 64;
        case TypeKind::Mutex: return 64;
        case TypeKind::Atomic: return t.bits;
        case TypeKind::Future: return 64;
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
    if (s == "No_Type" || s == "NoType") return Type::nullType();
    if (s == "Null")
        throw Error("line " + std::to_string(line) + ": 'Null' is a pointer literal, not a declarable type");
    if (s == "Text" || s == "String_Builder") return Type::textType(64);
    if (s == "File_Handle" || s == "File") return Type::fileHandle();
    for (unsigned bits : {8u, 16u, 32u, 64u}) {
        if (s == "Signed_" + std::to_string(bits) || s == "Integer_" + std::to_string(bits))
            return Type::signedInt(bits);
        if (s == "Unsigned_" + std::to_string(bits))
            return Type::unsignedInt(bits);
    }
    throw Error("line " + std::to_string(line) + ": unsupported type '" + s +
                "' (supported: Signed_8/16/32/64, Unsigned_8/16/32/64, Float_32, Float_64, Boolean, String, Character, Text, Pointer[T])");
}

// Keywords are matched strictly case-sensitively; all keywords start with a capital letter.

} // namespace loom
