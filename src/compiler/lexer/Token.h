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

namespace loom {
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
    In,
    Where,
    Default,
    Drop,
    FatArrow,
    Operator,
    Smt_Pointer,
    Channel,
    Mutex,
    Atomic,
    Async,
    Await,
    Lock,
    Move,
    Copy,
    Borrow,
    Ref,
    Mut,
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

} // namespace loom
