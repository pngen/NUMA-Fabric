#pragma once
// ============================================================================
// NUMA Fabric - small deterministic JSON value model for CLI / explanation
// output and round-trip testing. Not used for the binary persistence format
// (that uses its own versioned, CRC-32 checksummed encoding).
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace numafabric {

class JsonError final : public std::exception {
public:
    explicit JsonError(std::string m) : message_(std::move(m)) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

class Json;
class JsonObject;
class JsonArray;

using JsonString = std::string;
using JsonBool   = bool;
using JsonNull   = std::nullptr_t;
using JsonNumber = double;

class Json {
public:
    using Object = std::map<std::string, Json>;
    using Array  = std::vector<Json>;
    using Value  = std::variant<JsonNull, JsonBool, JsonNumber, JsonString, Array, Object>;

    Json() : value_(JsonNull{}) {}
    Json(JsonNull) : value_(JsonNull{}) {}
    Json(JsonBool b) : value_(b) {}
    Json(int i) : value_(static_cast<double>(i)) {}
    Json(std::uint64_t u) : value_(static_cast<double>(u)) {}
    Json(double d) : value_(d) {}
    Json(const char* s) : value_(JsonString(s)) {}
    Json(JsonString s) : value_(std::move(s)) {}
    Json(Array a) : value_(std::move(a)) {}
    Json(Object o) : value_(std::move(o)) {}

    static Json null() { return Json(JsonNull{}); }
    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    bool is_null()   const { return std::holds_alternative<JsonNull>(value_); }
    bool is_bool()   const { return std::holds_alternative<JsonBool>(value_); }
    bool is_number() const { return std::holds_alternative<JsonNumber>(value_); }
    bool is_string() const { return std::holds_alternative<JsonString>(value_); }
    bool is_array()  const { return std::holds_alternative<Array>(value_); }
    bool is_object() const { return std::holds_alternative<Object>(value_); }

    const JsonString& as_string() const { return std::get<JsonString>(value_); }
    JsonBool as_bool()   const { return std::get<JsonBool>(value_); }
    double    as_number() const { return std::get<JsonNumber>(value_); }
    const Array&  as_array()  const { return std::get<Array>(value_); }
    const Object& as_object() const { return std::get<Object>(value_); }

    Object& object_ref() { return std::get<Object>(value_); }
    Array&  array_ref()  { return std::get<Array>(value_); }

    static Json object_from(std::initializer_list<std::pair<std::string, Json>> init) {
        Json o = object();
        for (auto& [k, v] : init) { o.object_ref()[k] = v; }
        return o;
    }

    // Writer
    void write(std::ostream& os, bool pretty = false, int indent = 0) const;
    std::string dump(bool pretty = false) const;

    // Parser
    static Json parse(std::string_view text);

private:
    Value value_;
};

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
namespace detail {
inline void write_string(std::ostream& os, std::string_view s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    os << buf;
                } else { os << c; }
        }
    }
    os << '"';
}
} // namespace detail

inline void Json::write(std::ostream& os, bool pretty, int indent) const {
    const auto nl = [&]() { if (pretty) { os << '\n'; for (int i = 0; i < indent; ++i) os << "  "; } };
    if (is_null()) { os << "null"; }
    else if (is_bool()) { os << (as_bool() ? "true" : "false"); }
    else if (is_number()) {
        const double d = as_number();
        if (d == static_cast<double>(static_cast<std::int64_t>(d))) {
            os << static_cast<std::int64_t>(d);
        } else {
            os << d;
        }
    }
    else if (is_string()) { detail::write_string(os, as_string()); }
    else if (is_array()) {
        os << '[';
        const auto& a = as_array();
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) os << ',';
            nl();
            a[i].write(os, pretty, indent + 1);
        }
        os << ']';
    }
    else if (is_object()) {
        os << '{';
        const auto& o = as_object();
        std::size_t i = 0;
        for (const auto& [k, v] : o) {
            if (i++) os << ',';
            nl();
            detail::write_string(os, k);
            os << ':';
            if (pretty) os << ' ';
            v.write(os, pretty, indent + 1);
        }
        os << '}';
    }
}

inline std::string Json::dump(bool pretty) const {
    std::ostringstream os;
    write(os, pretty);
    return os.str();
}

// ---------------------------------------------------------------------------
// parser (strict: rejects trailing garbage, unknown tokens, malformed numbers)
// ---------------------------------------------------------------------------
namespace detail {
struct Parser {
    std::string_view s;
    std::size_t i = 0;

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eof() { ws(); return i >= s.size(); }
    char peek() { ws(); return i < s.size() ? s[i] : '\0'; }
    char get() { ws(); if (i >= s.size()) throw JsonError("unexpected end of JSON"); return s[i++]; }
    bool consume(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    Json parse_value() {
        ws();
        const char c = peek();
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't') { expect("true"); return Json(true); }
        if (c == 'f') { expect("false"); return Json(false); }
        if (c == 'n') { expect("null"); return Json::null(); }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        throw JsonError("unexpected token in JSON");
    }
    void expect(std::string_view lit) {
        for (char c : lit) { if (get() != c) throw JsonError("malformed literal"); }
    }
    Json parse_string() {
        get(); // opening quote
        std::string out;
        while (true) {
            if (i >= s.size()) throw JsonError("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) throw JsonError("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) throw JsonError("bad unicode escape");
                        const auto hex = [&](char h) -> int {
                            if (h >= '0' && h <= '9') return h - '0';
                            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                            throw JsonError("bad unicode escape");
                        };
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) cp = cp * 16 + hex(s[i++]);
                        // UTF-8 encode (BMP only, sufficient for our ASCII-ish data)
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: throw JsonError("bad escape char");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                throw JsonError("control char in string");
            } else {
                out += c;
            }
        }
        return Json(JsonString(out));
    }
    Json parse_number() {
        ws();
        std::size_t start = i;
        consume('-');
        if (consume('0')) { /* leading zero ok only if not followed by digit */ if (i < s.size() && s[i] >= '0' && s[i] <= '9') throw JsonError("leading zero in number"); }
        else { if (!(i < s.size() && s[i] >= '1' && s[i] <= '9')) throw JsonError("bad number"); while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
        if (i < s.size() && s[i] == '.') { ++i; if (!(i < s.size() && s[i] >= '0' && s[i] <= '9')) throw JsonError("bad fraction"); while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i; if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!(i < s.size() && s[i] >= '0' && s[i] <= '9')) throw JsonError("bad exponent");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        std::string num(s.substr(start, i - start));
        try { return Json(std::stod(num)); } catch (...) { throw JsonError("bad number value"); }
    }
    Json parse_array() {
        get(); // [
        Json a = Json::array();
        if (consume(']')) return a;
        while (true) {
            a.array_ref().push_back(parse_value());
            if (consume(']')) break;
            if (!consume(',')) throw JsonError("expected ',' in array");
        }
        return a;
    }
    Json parse_object() {
        get(); // {
        Json o = Json::object();
        if (consume('}')) return o;
        while (true) {
            ws();
            if (peek() != '"') throw JsonError("expected string key");
            JsonString key = parse_string().as_string();
            if (!consume(':')) throw JsonError("expected ':'");
            o.object_ref()[key] = parse_value();
            if (consume('}')) break;
            if (!consume(',')) throw JsonError("expected ',' in object");
        }
        return o;
    }
};
} // namespace detail

inline Json Json::parse(std::string_view text) {
    detail::Parser p{text};
    Json v = p.parse_value();
    if (!p.eof()) throw JsonError("trailing garbage after JSON value");
    return v;
}

} // namespace numafabric
