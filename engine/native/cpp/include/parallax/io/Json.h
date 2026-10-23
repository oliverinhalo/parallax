// Minimal JSON document model: parse, build and serialise.
//
// Needed because glTF is JSON and Parallax writes it without a third-party
// dependency. Having a parser as well as a writer is what lets the exporter
// tests read a produced .glb back and check its accessors, buffer views and
// byte lengths against the mesh they came from, instead of merely asserting
// that a file appeared on disk.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace parallax {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  static Json null() { return Json(); }
  static Json boolean(bool v);
  static Json number(double v);
  static Json integer(int64_t v);
  static Json string(std::string v);
  static Json array();
  static Json object();

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  bool asBool(bool fallback = false) const;
  double asDouble(double fallback = 0.0) const;
  int64_t asInt(int64_t fallback = 0) const;
  const std::string& asString() const;

  /// Array access. `push` appends; `at` returns a null Json when out of range.
  void push(Json value);
  size_t size() const;
  const Json& at(size_t index) const;

  /// Object access. `set` inserts or replaces; `get` returns null when absent.
  void set(const std::string& key, Json value);
  bool has(const std::string& key) const;
  const Json& get(const std::string& key) const;
  const std::map<std::string, Json>& members() const { return object_; }

  /// Serialise. `indent` of 0 produces the compact form glTF should use.
  std::string dump(int indent = 0) const;

  /// Parse. Returns false and leaves `out` untouched on malformed input;
  /// `error` receives a human-readable reason with a byte offset.
  static bool parse(const std::string& text, Json* out, std::string* error = nullptr);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Json> array_;
  std::map<std::string, Json> object_;

  void dumpTo(std::string* out, int indent, int depth) const;
};

}  // namespace parallax
