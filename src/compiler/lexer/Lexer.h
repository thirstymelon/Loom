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

#include "Token.h"

namespace loom {
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
                                                                 {"in", K::In},
                                                                 {"In", K::In},
                                                                 {"where", K::Where},
                                                                 {"Where", K::Where},
                                                                 {"default", K::Default},
                                                                 {"Default", K::Default},
                                                                 {"Drop", K::Drop},
                                                                 {"drop", K::Drop},
                                                                 {"pow", K::Pow},
                                                                 {"Mod", K::Mod},
                                                                 {"Operator", K::Operator},
                                                                 {"operator", K::Operator},
                                                                 {"Async", K::Async},
                                                                 {"async", K::Async},
                                                                 {"Await", K::Await},
                                                                 {"await", K::Await},
                                                                 {"Lock", K::Lock},
                                                                 {"lock", K::Lock},
                                                                 {"Move", K::Move},
                                                                 {"move", K::Move},
                                                                 {"Copy", K::Copy},
                                                                 {"copy", K::Copy},
                                                                 {"Borrow", K::Borrow},
                                                                 {"borrow", K::Borrow},
                                                                 {"Ref", K::Ref},
                                                                 {"ref", K::Ref},
                                                                 {"Mut", K::Mut},
                                                                 {"mut", K::Mut}};
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
                            case 'e': value += '\x1b'; break;
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
                            case 'e': value += '\x1b'; break;
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
                    } else if (peek() == '>') {
                        take();
                        emit(K::FatArrow, "=>", atLine);
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


} // namespace loom
