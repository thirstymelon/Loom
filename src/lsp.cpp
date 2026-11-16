#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <regex>
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;

namespace loom {
namespace lsp {

struct Position {
    int line = 0;
    int character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Document {
    std::string uri;
    std::string text;
    int version = 0;
};

static std::unordered_map<std::string, Document> documents;

static const std::unordered_map<std::string, std::string> KEYWORD_DOCS = {
    {"module", "**Module**: Defines the module namespace for a source file. Every Loom file must start with `Module <Name>`."},
    {"block", "**Block**: Defines a callable procedure or function.\n\nSyntax:\n```loom\nBlock Name Takes Param : Type Produces RetType\nImplements\n    ...\nEnd Name\n```"},
    {"takes", "**Takes**: Introduces the formal parameter list of a Block."},
    {"produces", "**Produces**: Declares the return type of a Block (supports `Produces T or ErrorType`)."},
    {"declares", "**Declares**: Introduces local variable declarations in a Block."},
    {"implements", "**Implements**: Introduces the statement body of a Block."},
    {"end", "**End**: Closes a Block, If, Loop, or Match construct."},
    {"ensure", "**Ensure**: Guaranteed cleanup block. Executes automatically when exiting the block scope, even during early exit or return.\n\n```loom\nEnsure {\n    Release_Resource()\n}\n```"},
    {"give", "**Give**: Returns a value from a value-producing Block."},
    {"exit", "**Exit**: Exits the nearest enclosing Loop immediately."},
    {"skip", "**Skip**: Skips to the next iteration of the nearest enclosing Loop."},
    {"if", "**If**: Conditional branch.\n\n```loom\nIf Condition\n    ...\nElse\n    ...\nEnd If\n```"},
    {"else", "**Else**: Alternative branch for an If statement."},
    {"loop", "**Loop**: Begins a loop construct (infinite, `Loop while <cond>`, or `Loop until <cond>`)."},
    {"match", "**Match**: Pattern matching over enums, integers, or booleans.\n\n```loom\nMatch Value\n    When Pattern:\n        ...\nEnd Match\n```"},
    {"when", "**When**: A case arm in a Match construct."},
    {"use", "**use**: Imports an external module's blocks and types unqualified into the current scope."},
    {"as", "**as**: Type cast expression (`expr as TargetType`). Performs safe conversions and explicit bit-width casts."},
    {"try", "**try**: Error propagation expression (`try expr`). Unwraps success value or early-returns with error."},
    {"or", "**or**: Error union type specification (`Produces T or ErrorType`)."},
    {"packed", "**Packed**: Specifies packed struct layout with no padding between record fields."},
    {"align", "**Align**: Specifies custom memory alignment for records."},
    {"volatile", "**volatile**: Type qualifier for memory-mapped I/O operations with strict load/store semantics."},
    {"spawn", "**Spawn**: Concurrently spawns an asynchronous task.\n\n```loom\nhandle : Task_Handle = Spawn Worker(arg)\n```"},
    {"join", "**Join**: Waits for asynchronous task completion and returns the result.\n\n```loom\nres = Join handle\n```"},
    {"to_string", "**To_String**: Converts expression or Text buffer to a String.\n\n```loom\nstr : String = To_String(buf)\n```"},
    {"allocate", "**Allocate**: Dynamically allocates heap memory with size and alignment.\n\n```loom\nptr : Signed_32 Pointer = Allocate(4, 4)\n```"},
    {"resize", "**Resize**: Reallocates a dynamically allocated heap block.\n\n```loom\nptr = Resize(ptr, 8, 4)\n```"},
    {"release", "**Release**: Deallocates a dynamically allocated heap block.\n\n```loom\nRelease(ptr)\n```"}
};

static const std::unordered_map<std::string, std::string> TYPE_DOCS = {
    {"signed_8", "**Signed_8**: 8-bit signed integer (-128 to 127)."},
    {"signed_16", "**Signed_16**: 16-bit signed integer (-32,768 to 32,767)."},
    {"signed_32", "**Signed_32**: 32-bit signed integer."},
    {"signed_64", "**Signed_64**: 64-bit signed integer."},
    {"unsigned_8", "**Unsigned_8**: 8-bit unsigned integer (0 to 255)."},
    {"unsigned_16", "**Unsigned_16**: 16-bit unsigned integer (0 to 65,535)."},
    {"unsigned_32", "**Unsigned_32**: 32-bit unsigned integer."},
    {"unsigned_64", "**Unsigned_64**: 64-bit unsigned integer."},
    {"float_32", "**Float_32**: 32-bit IEEE-754 single-precision float."},
    {"float_64", "**Float_64**: 64-bit IEEE-754 double-precision float."},
    {"boolean", "**Boolean**: Boolean type (`True` or `False`)."},
    {"character", "**Character**: 32-bit Unicode scalar character literal."},
    {"string", "**String**: Null-terminated UTF-8 byte string."},
    {"pointer", "**Pointer**: Memory pointer to a typed location (`T Pointer`)."},
    {"slice", "**Slice**: Non-owning view of a contiguous sequence (`T Slice` or `Slice(ptr, len)`)."},
    {"array", "**Array**: Fixed-size contiguous array (`Array[T, N]`)."},
    {"hmap", "**HMap**: Open-addressing hash map with integer or string keys (`HMap[KeyType : ValType]`)."},
    {"vector", "**Vector**: Growable dynamic typed array (`Type Vector[initial_size]`)."},
    {"text", "**Text**: Growable dynamic string buffer / string builder (`Text` or `Text[capacity]`)."},
    {"block_pointer", "**Block_Pointer**: First-class function pointer type (`[RetType] Block_Pointer(ParamTypes)`)."},
    {"task_handle", "**Task_Handle**: Asynchronous spawned task handle."}
};

static std::string escapeJson(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\b') o << "\\b";
        else if (c == '\f') o << "\\f";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (static_cast<unsigned char>(c) <= 0x1f) {
            o << "\\u" << std::hex << (int)c;
        } else {
            o << c;
        }
    }
    return o.str();
}

static void sendResponse(const std::string& json) {
    std::cout << "Content-Length: " << json.size() << "\r\n\r\n" << json;
    std::cout.flush();
}

static std::string readMessage() {
    std::string line;
    std::size_t contentLength = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") {
            break;
        }
        if (line.rfind("Content-Length: ", 0) == 0) {
            contentLength = std::stoul(line.substr(16));
        }
    }
    if (contentLength == 0) return {};
    std::string body(contentLength, '\0');
    std::cin.read(&body[0], contentLength);
    return body;
}

static std::string unescapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == 'n') { out.push_back('\n'); i++; }
            else if (next == 'r') { out.push_back('\r'); i++; }
            else if (next == 't') { out.push_back('\t'); i++; }
            else if (next == '"') { out.push_back('"'); i++; }
            else if (next == '\\') { out.push_back('\\'); i++; }
            else { out.push_back(next); i++; }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string getJsonStringField(const std::string& json, const std::string& field) {
    std::string key = "\"" + field + "\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    auto endPos = json.find('"', pos + 1);
    while (endPos != std::string::npos && json[endPos - 1] == '\\') {
        endPos = json.find('"', endPos + 1);
    }
    if (endPos == std::string::npos) return {};
    return unescapeJson(json.substr(pos + 1, endPos - pos - 1));
}

static int getJsonIntField(const std::string& json, const std::string& field, int def = 0) {
    std::string key = "\"" + field + "\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::size_t endPos = pos;
    while (endPos < json.size() && (std::isdigit(json[endPos]) || json[endPos] == '-')) endPos++;
    try {
        return std::stoi(json.substr(pos, endPos - pos));
    } catch (...) {
        return def;
    }
}

static std::string getWordAt(const std::string& text, int line, int character) {
    std::istringstream stream(text);
    std::string currentLine;
    int curLine = 0;
    while (std::getline(stream, currentLine)) {
        if (curLine == line) {
            if (character >= (int)currentLine.size()) return {};
            int start = character;
            while (start > 0 && (std::isalnum(currentLine[start - 1]) || currentLine[start - 1] == '_')) {
                start--;
            }
            int end = character;
            while (end < (int)currentLine.size() && (std::isalnum(currentLine[end]) || currentLine[end] == '_')) {
                end++;
            }
            return currentLine.substr(start, end - start);
        }
        curLine++;
    }
    return {};
}

static bool findDefinition(const std::string& text, const std::string& word, int& defLine, int& defCol) {
    if (word.empty()) return false;
    std::istringstream stream(text);
    std::string line;
    int curLine = 0;
    std::regex blockRegex(R"(\bBlock\s+)" + word + R"(\b)");
    std::regex recordRegex(R"(\bRecord\s+)" + word + R"(\b)");
    std::regex enumRegex(R"(\bEnum\s+)" + word + R"(\b)");
    std::regex varRegex(R"(\b)" + word + R"(\s*:)");

    while (std::getline(stream, line)) {
        std::smatch m;
        if (std::regex_search(line, m, blockRegex) ||
            std::regex_search(line, m, recordRegex) ||
            std::regex_search(line, m, enumRegex)) {
            defLine = curLine;
            auto pos = line.find(word);
            defCol = (pos != std::string::npos) ? (int)pos : 0;
            return true;
        }
        curLine++;
    }

    stream.clear();
    stream.seekg(0);
    curLine = 0;
    while (std::getline(stream, line)) {
        std::smatch m;
        if (std::regex_search(line, m, varRegex)) {
            defLine = curLine;
            auto pos = line.find(word);
            defCol = (pos != std::string::npos) ? (int)pos : 0;
            return true;
        }
        curLine++;
    }
    return false;
}

static void publishDiagnostics(const std::string& uri, const std::string& text) {
    // Write text to a temporary file and run loomc --check to gather diagnostic errors
    char tempPath[] = "/tmp/loom_lsp_check_XXXXXX.loom";
    int fd = mkstemps(tempPath, 5);
    if (fd < 0) return;
    FILE* f = fdopen(fd, "w");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);

    std::string cmd = "loomc --check " + std::string(tempPath) + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    std::string output;
    if (pipe) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        pclose(pipe);
    }
    std::remove(tempPath);

    std::vector<std::string> diagItems;
    std::istringstream stream(output);
    std::string l;
    std::regex errRegex(R"(line\s+(\d+)(?::(\d+))?:\s*(?:(error|warning|note):\s*)?(.*))", std::regex::icase);
    std::regex modernRegex(R"(error\[([A-Z0-9]+)\]:\s*(.*))", std::regex::icase);
    std::regex locRegex(R"(-->\s+.*:(\d+):(\d+))");

    int pendingLine = -1;
    int pendingCol = 0;
    std::string pendingMsg;

    while (std::getline(stream, l)) {
        std::smatch m;
        if (std::regex_search(l, m, modernRegex)) {
            pendingMsg = m[2].str();
        } else if (std::regex_search(l, m, locRegex)) {
            pendingLine = std::max(0, std::stoi(m[1].str()) - 1);
            pendingCol = std::max(0, std::stoi(m[2].str()) - 1);
            if (!pendingMsg.empty()) {
                std::ostringstream d;
                d << "{\"range\":{\"start\":{\"line\":" << pendingLine << ",\"character\":" << pendingCol
                  << "},\"end\":{\"line\":" << pendingLine << ",\"character\":" << (pendingCol + 5)
                  << "}},\"severity\":1,\"message\":\"" << escapeJson(pendingMsg) << "\"}";
                diagItems.push_back(d.str());
                pendingMsg.clear();
            }
        } else if (std::regex_search(l, m, errRegex)) {
            int lineNum = std::max(0, std::stoi(m[1].str()) - 1);
            int colNum = m[2].matched ? std::max(0, std::stoi(m[2].str()) - 1) : 0;
            std::string msg = m[4].str();
            std::ostringstream d;
            d << "{\"range\":{\"start\":{\"line\":" << lineNum << ",\"character\":" << colNum
              << "},\"end\":{\"line\":" << lineNum << ",\"character\":" << (colNum + 5)
              << "}},\"severity\":1,\"message\":\"" << escapeJson(msg) << "\"}";
            diagItems.push_back(d.str());
        }
    }

    std::ostringstream res;
    res << "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\""
        << escapeJson(uri) << "\",\"diagnostics\":[";
    for (std::size_t i = 0; i < diagItems.size(); ++i) {
        res << diagItems[i];
        if (i + 1 < diagItems.size()) res << ",";
    }
    res << "]}}";
    sendResponse(res.str());
}

} // namespace lsp
} // namespace loom

int main() {
    while (std::cin.good()) {
        std::string msg = loom::lsp::readMessage();
        if (msg.empty()) continue;

        std::string method = loom::lsp::getJsonStringField(msg, "method");
        int id = loom::lsp::getJsonIntField(msg, "id", -1);

        if (method == "initialize") {
            std::ostringstream res;
            res << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":{"
                << "\"capabilities\":{"
                << "\"textDocumentSync\":1,"
                << "\"hoverProvider\":true,"
                << "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\",\"[\"]},"
                << "\"definitionProvider\":true"
                << "}}}";
            loom::lsp::sendResponse(res.str());
        } else if (method == "initialized") {
            // Notification; no response needed
        } else if (method == "textDocument/didOpen") {
            auto uri = loom::lsp::getJsonStringField(msg, "uri");
            auto text = loom::lsp::getJsonStringField(msg, "text");
            loom::lsp::documents[uri] = {uri, text, 1};
            loom::lsp::publishDiagnostics(uri, text);
        } else if (method == "textDocument/didChange") {
            auto uri = loom::lsp::getJsonStringField(msg, "uri");
            auto text = loom::lsp::getJsonStringField(msg, "text");
            if (!text.empty()) {
                loom::lsp::documents[uri] = {uri, text, 1};
                loom::lsp::publishDiagnostics(uri, text);
            }
        } else if (method == "textDocument/hover") {
            auto uri = loom::lsp::getJsonStringField(msg, "uri");
            auto posPos = msg.find("\"position\"");
            int line = 0, character = 0;
            if (posPos != std::string::npos) {
                line = loom::lsp::getJsonIntField(msg.substr(posPos), "line", 0);
                character = loom::lsp::getJsonIntField(msg.substr(posPos), "character", 0);
            }
            std::string word;
            if (loom::lsp::documents.count(uri)) {
                word = loom::lsp::getWordAt(loom::lsp::documents[uri].text, line, character);
            }
            std::string lowerWord = word;
            std::transform(lowerWord.begin(), lowerWord.end(), lowerWord.begin(), ::tolower);

            std::string hoverText;
            if (loom::lsp::KEYWORD_DOCS.count(lowerWord)) {
                hoverText = loom::lsp::KEYWORD_DOCS.at(lowerWord);
            } else if (loom::lsp::TYPE_DOCS.count(lowerWord)) {
                hoverText = loom::lsp::TYPE_DOCS.at(lowerWord);
            }

            std::ostringstream res;
            res << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":";
            if (!hoverText.empty()) {
                res << "{\"contents\":{\"kind\":\"markdown\",\"value\":\"" << loom::lsp::escapeJson(hoverText) << "\"}}";
            } else {
                res << "null";
            }
            res << "}";
            loom::lsp::sendResponse(res.str());
        } else if (method == "textDocument/completion") {
            auto uri = loom::lsp::getJsonStringField(msg, "uri");
            auto posPos = msg.find("\"position\"");
            int line = 0, character = 0;
            if (posPos != std::string::npos) {
                line = loom::lsp::getJsonIntField(msg.substr(posPos), "line", 0);
                character = loom::lsp::getJsonIntField(msg.substr(posPos), "character", 0);
            }
            std::string prefixLine;
            if (loom::lsp::documents.count(uri)) {
                std::istringstream stream(loom::lsp::documents[uri].text);
                std::string curLine;
                int cl = 0;
                while (std::getline(stream, curLine)) {
                    if (cl == line) {
                        if (character <= (int)curLine.size()) prefixLine = curLine.substr(0, character);
                        else prefixLine = curLine;
                        break;
                    }
                    cl++;
                }
            }
            std::ostringstream res;
            res << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":[";
            bool first = true;
            auto addCompletion = [&](const std::string& label, int kind, const std::string& detail) {
                if (!first) res << ",";
                first = false;
                res << "{\"label\":\"" << label << "\",\"kind\":" << kind << ",\"detail\":\"" << loom::lsp::escapeJson(detail) << "\"}";
            };

            auto trimmed = prefixLine;
            while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back())) trimmed.pop_back();
            if (!trimmed.empty() && trimmed.back() == '.') {
                addCompletion("Length", 10, "collection element count or string length");
                addCompletion("Capacity", 10, "allocated buffer capacity");
                addCompletion("Append", 2, "Append(elem): appends an element to Vector");
                addCompletion("Pop", 2, "Pop(): removes and returns last element from Vector");
                addCompletion("Contains", 2, "Contains(key/elem): checks presence in Vector or HMap");
                addCompletion("Remove", 2, "Remove(key): removes key from HMap");
                addCompletion("Index", 2, "Index(elem): returns 1-based index of first match (or 0)");
                addCompletion("Index_All", 2, "Index_All(elem): returns Vector of all matching 1-based indices");
            } else if (trimmed.find("use ") != std::string::npos || trimmed.find("Import ") != std::string::npos) {
                addCompletion("Math", 9, "built-in math module");
                addCompletion("System", 9, "system intrinsics and memory");
                addCompletion("IO", 9, "file and console I/O");
            } else {
                for (const auto& [kw, doc] : loom::lsp::KEYWORD_DOCS) addCompletion(kw, 14, "keyword");
                for (const auto& [ty, doc] : loom::lsp::TYPE_DOCS) addCompletion(ty, 7, "type");
                addCompletion("Block", 14, "procedure/function declaration");
                addCompletion("Takes", 14, "parameter list");
                addCompletion("Produces", 14, "return type");
                addCompletion("Declares", 14, "local variable declarations");
                addCompletion("Implements", 14, "executable body");
                addCompletion("Ensure", 14, "guaranteed cleanup block");
                addCompletion("as", 14, "type casting expression");
                addCompletion("try", 14, "error unwrapping expression");
                addCompletion("Packed Record", 14, "packed structure layout");
                addCompletion("Spawn", 14, "concurrency spawn");
                addCompletion("Join", 14, "concurrency join");
                addCompletion("Print", 3, "Print(msg): standard output with newline");
                addCompletion("Write", 3, "Write(msg): standard output without newline");
                addCompletion("To_String", 3, "To_String(val): converts value to String");
            }
            res << "]}";
            loom::lsp::sendResponse(res.str());
        } else if (method == "textDocument/definition") {
            auto uri = loom::lsp::getJsonStringField(msg, "uri");
            auto posPos = msg.find("\"position\"");
            int line = 0, character = 0;
            if (posPos != std::string::npos) {
                line = loom::lsp::getJsonIntField(msg.substr(posPos), "line", 0);
                character = loom::lsp::getJsonIntField(msg.substr(posPos), "character", 0);
            }
            std::string word;
            if (loom::lsp::documents.count(uri)) {
                word = loom::lsp::getWordAt(loom::lsp::documents[uri].text, line, character);
            }
            int defLine = 0, defCol = 0;
            bool found = false;
            if (!word.empty() && loom::lsp::documents.count(uri)) {
                found = loom::lsp::findDefinition(loom::lsp::documents[uri].text, word, defLine, defCol);
            }
            std::ostringstream res;
            res << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":";
            if (found) {
                res << "{\"uri\":\"" << loom::lsp::escapeJson(uri) << "\",\"range\":{"
                    << "\"start\":{\"line\":" << defLine << ",\"character\":" << defCol << "},"
                    << "\"end\":{\"line\":" << defLine << ",\"character\":" << (defCol + (int)word.size()) << "}"
                    << "}}";
            } else {
                res << "null";
            }
            res << "}";
            loom::lsp::sendResponse(res.str());
        } else if (method == "shutdown") {
            std::ostringstream res;
            res << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":null}";
            loom::lsp::sendResponse(res.str());
        } else if (method == "exit") {
            return 0;
        } else if (id >= 0) {
            std::ostringstream res;
            res << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"result\":null}";
            loom::lsp::sendResponse(res.str());
        }
    }
    return 0;
}
