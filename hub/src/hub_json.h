// Minimal JSON value, parser, and serializer for hub persistence.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hub {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    Json() = default;
    Json(bool b) : type(Type::Bool), boolean(b) {}
    Json(int n) : type(Type::Number), number(n) {}
    Json(long long n) : type(Type::Number), number(static_cast<double>(n)) {}
    Json(double n) : type(Type::Number), number(n) {}
    Json(const char* s) : type(Type::String), string(s ? s : "") {}
    Json(std::string s) : type(Type::String), string(std::move(s)) {}

    static Json make_object() { Json j; j.type = Type::Object; return j; }
    static Json make_array() { Json j; j.type = Type::Array; return j; }

    bool is_null() const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }
    bool is_bool() const { return type == Type::Bool; }

    const Json* get(const std::string& key) const;
    Json* get(const std::string& key);
    Json& set(const std::string& key, Json value);
    void push(Json value) { type = Type::Array; array.push_back(std::move(value)); }

    std::string get_string(const std::string& key, const std::string& fallback = "") const;
    bool get_bool(const std::string& key, bool fallback = false) const;
    double get_number(const std::string& key, double fallback = 0.0) const;
    long long get_int(const std::string& key, long long fallback = 0) const;

    static bool parse(const std::string& text, Json& out, std::string& error);
    std::string dump(int indent = 2) const;
};

bool read_text_file(const std::string& path, std::string& out);
bool write_text_file_atomic(const std::string& path, const std::string& text, std::string& error);

}  // namespace hub
