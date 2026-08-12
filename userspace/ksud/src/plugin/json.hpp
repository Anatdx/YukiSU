#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ksud::plugin_json {

enum class Type { Null, Bool, Integer, Number, String, Array, Object };

struct Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double n = 0;
    std::string s;
    Array a;
    Object o;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(int v) : type(Type::Integer), i(v) {}
    Value(int64_t v) : type(Type::Integer), i(v) {}
    Value(double v) : type(Type::Number), n(v) {}
    Value(const char* v) : type(Type::String), s(v) {}
    Value(const std::string& v) : type(Type::String), s(v) {}
    Value(std::string&& v) : type(Type::String), s(std::move(v)) {}
    Value(const Array& v) : type(Type::Array), a(v) {}
    Value(Array&& v) : type(Type::Array), a(std::move(v)) {}
    Value(const Object& v) : type(Type::Object), o(v) {}
    Value(Object&& v) : type(Type::Object), o(std::move(v)) {}

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    Value& operator[](const std::string& key) {
        if (type != Type::Object) {
            type = Type::Object;
            o.clear();
        }
        return o[key];
    }

    // const lookup that doesn't insert; returns a Null Value if absent.
    const Value& at(const std::string& key) const {
        static const Value null_value;
        if (type != Type::Object)
            return null_value;
        auto it = o.find(key);
        return it == o.end() ? null_value : it->second;
    }
    bool contains(const std::string& key) const {
        return type == Type::Object && o.count(key) != 0;
    }

    void push_back(const Value& v) {
        if (type != Type::Array) {
            type = Type::Array;
            a.clear();
        }
        a.push_back(v);
    }

    bool as_bool() const { return b; }
    double as_number() const { return type == Type::Integer ? static_cast<double>(i) : n; }
    std::string as_string() const { return s; }
    const Array& as_array() const { return a; }
    const Object& as_object() const { return o; }
};

inline bool is_valid_utf8(const std::string& value) {
    for (size_t index = 0; index < value.size();) {
        const unsigned char first = static_cast<unsigned char>(value[index++]);
        if (first <= 0x7FU)
            continue;

        size_t continuation_count = 0;
        unsigned char second_minimum = 0x80U;
        unsigned char second_maximum = 0xBFU;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            if (first == 0xE0U)
                second_minimum = 0xA0U;
            else if (first == 0xEDU)
                second_maximum = 0x9FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            if (first == 0xF0U)
                second_minimum = 0x90U;
            else if (first == 0xF4U)
                second_maximum = 0x8FU;
        } else {
            return false;
        }
        if (value.size() - index < continuation_count)
            return false;
        const unsigned char second = static_cast<unsigned char>(value[index]);
        if (second < second_minimum || second > second_maximum)
            return false;
        for (size_t offset = 0; offset < continuation_count; ++offset) {
            if ((static_cast<unsigned char>(value[index++]) & 0xC0U) != 0x80U)
                return false;
        }
    }
    return true;
}

inline bool has_valid_utf8(const Value& value) {
    switch (value.type) {
    case Type::String:
        return is_valid_utf8(value.s);
    case Type::Array:
        return std::all_of(value.a.begin(), value.a.end(), has_valid_utf8);
    case Type::Object:
        return std::all_of(value.o.begin(), value.o.end(), [](const auto& entry) {
            return is_valid_utf8(entry.first) && has_valid_utf8(entry.second);
        });
    default:
        return true;
    }
}

inline std::string escape_string(const std::string& s) {
    std::ostringstream ss;
    ss << '"';
    for (char c : s) {
        if (c == '"')
            ss << "\\\"";
        else if (c == '\\')
            ss << "\\\\";
        else if (c == '\b')
            ss << "\\b";
        else if (c == '\f')
            ss << "\\f";
        else if (c == '\n')
            ss << "\\n";
        else if (c == '\r')
            ss << "\\r";
        else if (c == '\t')
            ss << "\\t";
        else if ((unsigned char)c < 0x20)
            ss << "\\u" << std::setfill('0') << std::setw(4) << std::hex << (int)(unsigned char)c;
        else
            ss << c;
    }
    ss << '"';
    return ss.str();
}

inline std::string dump(const Value& v, int indent = -1, int level = 0) {
    switch (v.type) {
    case Type::Null:
        return "null";
    case Type::Bool:
        return v.b ? "true" : "false";
    case Type::Integer:
        return std::to_string(v.i);
    case Type::Number: {
        if (!std::isfinite(v.n))
            return "null";
        std::ostringstream ss;
        ss << std::setprecision(std::numeric_limits<double>::max_digits10) << v.n;
        return ss.str();
    }
    case Type::String:
        return escape_string(v.s);
    case Type::Array: {
        if (v.a.empty())
            return "[]";
        std::ostringstream ss;
        ss << "[" << (indent >= 0 ? "\n" : "");
        for (size_t i = 0; i < v.a.size(); ++i) {
            if (indent >= 0)
                ss << std::string((level + 1) * indent, ' ');
            ss << dump(v.a[i], indent, level + 1);
            if (i < v.a.size() - 1)
                ss << "," << (indent >= 0 ? "\n" : " ");
        }
        if (indent >= 0)
            ss << "\n" << std::string(level * indent, ' ');
        ss << "]";
        return ss.str();
    }
    case Type::Object: {
        if (v.o.empty())
            return "{}";
        std::ostringstream ss;
        ss << "{" << (indent >= 0 ? "\n" : "");
        size_t i = 0;
        for (const auto& kv : v.o) {
            if (indent >= 0)
                ss << std::string((level + 1) * indent, ' ');
            ss << escape_string(kv.first) << ":" << (indent >= 0 ? " " : "");
            ss << dump(kv.second, indent, level + 1);
            if (i < v.o.size() - 1)
                ss << "," << (indent >= 0 ? "\n" : " ");
            i++;
        }
        if (indent >= 0)
            ss << "\n" << std::string(level * indent, ' ');
        ss << "}";
        return ss.str();
    }
    }
    return "";
}

class Parser {
    const std::string& str;
    size_t pos = 0;
    size_t nodes = 0;
    std::string error;
    static constexpr size_t kMaxDepth = 128;
    static constexpr size_t kMaxNodes = 100000;

    bool fail(const char* message) {
        if (error.empty()) {
            error = std::string("JSON parse error at byte ") + std::to_string(pos) + ": " + message;
        }
        return false;
    }

    void skip_whitespace() {
        while (pos < str.size() &&
               (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\n' || str[pos] == '\r')) {
            ++pos;
        }
    }

    bool consume(char expected) {
        if (pos < str.size() && str[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    bool consume_literal(const char* literal) {
        const size_t length = std::char_traits<char>::length(literal);
        if (str.compare(pos, length, literal) != 0)
            return fail("invalid literal");
        pos += length;
        return true;
    }

    bool parse_hex_quad(unsigned int* output) {
        if (str.size() - pos < 4)
            return fail("incomplete Unicode escape");
        unsigned int value = 0;
        for (size_t index = 0; index < 4; ++index) {
            const char character = str[pos++];
            value <<= 4U;
            if (character >= '0' && character <= '9')
                value |= static_cast<unsigned int>(character - '0');
            else if (character >= 'a' && character <= 'f')
                value |= static_cast<unsigned int>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
                value |= static_cast<unsigned int>(character - 'A' + 10);
            else
                return fail("invalid Unicode escape");
        }
        *output = value;
        return true;
    }

    static void append_utf8(std::string* output, unsigned int codepoint) {
        if (codepoint <= 0x7FU) {
            output->push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output->push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output->push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output->push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output->push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output->push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output->push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    bool append_raw_utf8(unsigned char first, std::string* output) {
        size_t continuation_count = 0;
        unsigned char second_minimum = 0x80U;
        unsigned char second_maximum = 0xBFU;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            if (first == 0xE0U)
                second_minimum = 0xA0U;
            else if (first == 0xEDU)
                second_maximum = 0x9FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            if (first == 0xF0U)
                second_minimum = 0x90U;
            else if (first == 0xF4U)
                second_maximum = 0x8FU;
        } else {
            return fail("invalid UTF-8 sequence");
        }
        if (str.size() - pos < continuation_count)
            return fail("incomplete UTF-8 sequence");
        const unsigned char second = static_cast<unsigned char>(str[pos]);
        if (second < second_minimum || second > second_maximum)
            return fail("invalid UTF-8 sequence");
        output->push_back(static_cast<char>(first));
        for (size_t index = 0; index < continuation_count; ++index) {
            const unsigned char next = static_cast<unsigned char>(str[pos++]);
            if ((next & 0xC0U) != 0x80U)
                return fail("invalid UTF-8 sequence");
            output->push_back(static_cast<char>(next));
        }
        return true;
    }

    bool parse_value(Value* output, size_t depth) {
        if (depth > kMaxDepth)
            return fail("nesting is too deep");
        if (++nodes > kMaxNodes)
            return fail("too many values");
        skip_whitespace();
        if (pos >= str.size())
            return fail("expected a value");
        const char c = str[pos];
        if (c == 'n') {
            if (!consume_literal("null"))
                return false;
            *output = Value();
            return true;
        }
        if (c == 't') {
            if (!consume_literal("true"))
                return false;
            *output = Value(true);
            return true;
        }
        if (c == 'f') {
            if (!consume_literal("false"))
                return false;
            *output = Value(false);
            return true;
        }
        if (c == '"') {
            if (!parse_string(&output->s))
                return false;
            output->type = Type::String;
            return true;
        }
        if (c == '[')
            return parse_array(output, depth);
        if (c == '{')
            return parse_object(output, depth);
        if (c == '-' || std::isdigit((unsigned char)c))
            return parse_number(output);
        return fail("unexpected character");
    }

    bool parse_string(std::string* output) {
        if (!consume('"'))
            return fail("expected a string");
        output->clear();
        while (pos < str.size()) {
            const unsigned char c = static_cast<unsigned char>(str[pos++]);
            if (c == '"')
                return true;
            if (c == '\\') {
                if (pos >= str.size())
                    return fail("incomplete escape sequence");
                const char next = str[pos++];
                if (next == '"' || next == '\\' || next == '/')
                    *output += next;
                else if (next == 'b')
                    *output += '\b';
                else if (next == 'f')
                    *output += '\f';
                else if (next == 'n')
                    *output += '\n';
                else if (next == 'r')
                    *output += '\r';
                else if (next == 't')
                    *output += '\t';
                else if (next == 'u') {
                    unsigned int codepoint = 0;
                    if (!parse_hex_quad(&codepoint))
                        return false;
                    if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                        if (!consume('\\') || !consume('u'))
                            return fail("missing low surrogate");
                        unsigned int low = 0;
                        if (!parse_hex_quad(&low))
                            return false;
                        if (low < 0xDC00U || low > 0xDFFFU)
                            return fail("invalid low surrogate");
                        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                    } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                        return fail("unexpected low surrogate");
                    }
                    append_utf8(output, codepoint);
                } else {
                    return fail("invalid escape sequence");
                }
            } else {
                if (c < 0x20U)
                    return fail("unescaped control character");
                if (c < 0x80U)
                    *output += static_cast<char>(c);
                else if (!append_raw_utf8(c, output))
                    return false;
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Value* output) {
        const size_t start = pos;
        bool integer_syntax = true;
        (void)consume('-');
        if (pos >= str.size())
            return fail("incomplete number");
        if (consume('0')) {
            if (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos])))
                return fail("leading zero in number");
        } else {
            if (!std::isdigit(static_cast<unsigned char>(str[pos])))
                return fail("invalid number");
            while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos])))
                ++pos;
        }
        if (consume('.')) {
            integer_syntax = false;
            if (pos >= str.size() || !std::isdigit(static_cast<unsigned char>(str[pos])))
                return fail("fraction has no digits");
            while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos])))
                ++pos;
        }
        if (pos < str.size() && (str[pos] == 'e' || str[pos] == 'E')) {
            integer_syntax = false;
            ++pos;
            if (pos < str.size() && (str[pos] == '+' || str[pos] == '-'))
                ++pos;
            if (pos >= str.size() || !std::isdigit(static_cast<unsigned char>(str[pos])))
                return fail("exponent has no digits");
            while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos])))
                ++pos;
        }

        const std::string token = str.substr(start, pos - start);
        if (integer_syntax) {
            int64_t integer = 0;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (result.ec == std::errc() && result.ptr == token.data() + token.size()) {
                *output = Value(integer);
                return true;
            }
            return fail("integer is out of range");
        }
        char* end = nullptr;
        errno = 0;
        const double number = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || end != token.c_str() + token.size() || !std::isfinite(number))
            return fail("number is out of range");
        *output = Value(number);
        return true;
    }

    bool parse_array(Value* output, size_t depth) {
        output->type = Type::Array;
        output->a.clear();
        ++pos;
        skip_whitespace();
        if (consume(']'))
            return true;
        while (true) {
            Value value;
            if (!parse_value(&value, depth + 1))
                return false;
            output->a.push_back(std::move(value));
            skip_whitespace();
            if (consume(']'))
                return true;
            if (!consume(','))
                return fail("expected ',' or ']' in array");
        }
    }

    bool parse_object(Value* output, size_t depth) {
        output->type = Type::Object;
        output->o.clear();
        ++pos;
        skip_whitespace();
        if (consume('}'))
            return true;
        while (true) {
            skip_whitespace();
            std::string key;
            if (!parse_string(&key))
                return false;
            skip_whitespace();
            if (!consume(':'))
                return fail("expected ':' after object key");
            Value value;
            if (!parse_value(&value, depth + 1))
                return false;
            if (!output->o.emplace(key, std::move(value)).second)
                return fail("duplicate object key");
            skip_whitespace();
            if (consume('}'))
                return true;
            if (!consume(','))
                return fail("expected ',' or '}' in object");
        }
    }

public:
    explicit Parser(const std::string& s) : str(s) {}
    std::optional<Value> parse(std::string* output_error) {
        Value value;
        if (!parse_value(&value, 0)) {
            if (output_error)
                *output_error = error;
            return std::nullopt;
        }
        skip_whitespace();
        if (pos != str.size()) {
            (void)fail("trailing data");
            if (output_error)
                *output_error = error;
            return std::nullopt;
        }
        return value;
    }
};

inline std::optional<Value> parse(const std::string& s, std::string* error = nullptr) {
    return Parser(s).parse(error);
}

}  // namespace ksud::plugin_json
