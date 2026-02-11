#pragma once
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace hymo {
namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    Array a;
    Object o;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(int v) : type(Type::Number), n(static_cast<double>(v)) {}
    Value(double v) : type(Type::Number), n(v) {}
    Value(const char* v) : type(Type::String), s(v) {}
    Value(const std::string& v) : type(Type::String), s(v) {}
    Value(const Array& v) : type(Type::Array), a(v) {}
    Value(const Object& v) : type(Type::Object), o(v) {}

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    Value& operator[](const std::string& key) {
        if (type != Type::Object) {
            type = Type::Object;
            o.clear();
        }
        return o[key];
    }

    // For arrays, need careful overload handling or separate method
    void push_back(const Value& v) {
        if (type != Type::Array) {
            type = Type::Array;
            a.clear();
        }
        a.push_back(v);
    }

    bool as_bool() const { return b; }
    double as_number() const { return n; }
    std::string as_string() const { return s; }
    const Array& as_array() const { return a; }
    const Object& as_object() const { return o; }
};

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
        else if ((unsigned char)c < 0x20) {
            ss << "\\u" << std::setfill('0') << std::setw(4) << std::hex << (int)(unsigned char)c;
        } else
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
    case Type::Number: {
        std::string s = std::to_string(v.n);
        // Remove trailing zeros for integers represented as double
        s.erase(s.find_last_not_of('0') + 1, std::string::npos);
        if (s.back() == '.')
            s.pop_back();
        return s;
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

    void skip_whitespace() {
        while (pos < str.size() && std::isspace(str[pos]))
            pos++;
    }

    Value parse_value() {
        skip_whitespace();
        if (pos >= str.size())
            return Value();

        char c = str[pos];
        if (c == 'n') {
            pos += 4;
            return Value();
        }  // null
        if (c == 't') {
            pos += 4;
            return Value(true);
        }
        if (c == 'f') {
            pos += 5;
            return Value(false);
        }
        if (c == '"')
            return parse_string();
        if (c == '[')
            return parse_array();
        if (c == '{')
            return parse_object();
        if (c == '-' || std::isdigit(c))
            return parse_number();
        return Value();
    }

    Value parse_string() {
        std::string s;
        pos++;  // skip "
        while (pos < str.size()) {
            char c = str[pos++];
            if (c == '"')
                break;
            if (c == '\\') {
                if (pos >= str.size())
                    break;
                char next = str[pos++];
                if (next == 'n')
                    s += '\n';
                else if (next == 't')
                    s += '\t';
                else if (next == '"')
                    s += '"';
                else if (next == '\\')
                    s += '\\';
                else
                    s += next;
            } else {
                s += c;
            }
        }
        return Value(s);
    }

    Value parse_number() {
        size_t start = pos;
        while (pos < str.size() && (std::isdigit(str[pos]) || str[pos] == '-' || str[pos] == '.'))
            pos++;
        std::string num_str = str.substr(start, pos - start);
        return Value(std::stod(num_str));
    }

    Value parse_array() {
        Value v;
        v.type = Type::Array;
        pos++;  // skip [

        while (pos < str.size()) {
            skip_whitespace();
            if (str[pos] == ']') {
                pos++;
                break;
            }
            v.a.push_back(parse_value());
            skip_whitespace();
            if (str[pos] == ',')
                pos++;
        }
        return v;
    }

    Value parse_object() {
        Value v;
        v.type = Type::Object;
        pos++;  // skip {

        while (pos < str.size()) {
            skip_whitespace();
            if (str[pos] == '}') {
                pos++;
                break;
            }

            Value key = parse_string();
            skip_whitespace();
            if (str[pos] == ':')
                pos++;

            Value val = parse_value();
            v.o[key.s] = val;

            skip_whitespace();
            if (str[pos] == ',')
                pos++;
        }
        return v;
    }

public:
    Parser(const std::string& s) : str(s) {}
    Value parse() { return parse_value(); }
};

inline Value parse(const std::string& s) {
    return Parser(s).parse();
}

}  // namespace json
}  // namespace hymo
