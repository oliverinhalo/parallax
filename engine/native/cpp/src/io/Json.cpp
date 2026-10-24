#include "parallax/io/Json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace parallax {
namespace {

const Json& nullJson() {
  static const Json instance;
  return instance;
}

const std::string& emptyString() {
  static const std::string instance;
  return instance;
}

void appendEscaped(const std::string& in, std::string* out) {
  out->push_back('"');
  for (char c : in) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
          out->append(buffer);
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

/// Format a double without a trailing ".0" for integral values: glTF consumers
/// are happier with `1` than `1.0` for counts and indices.
void appendNumber(double value, std::string* out) {
  if (!std::isfinite(value)) {
    // JSON cannot represent these at all; emit 0 rather than invalid output.
    out->append("0");
    return;
  }
  if (value == std::floor(value) && std::abs(value) < 1e15) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    out->append(buffer);
    return;
  }
  char buffer[40];
  // 9 significant digits round-trips a float exactly, which is all glTF carries.
  std::snprintf(buffer, sizeof(buffer), "%.9g", value);
  out->append(buffer);
}

struct Parser {
  const std::string& text;
  size_t pos = 0;
  std::string error;

  explicit Parser(const std::string& t) : text(t) {}

  bool fail(const char* message) {
    if (error.empty()) {
      std::ostringstream oss;
      oss << message << " at offset " << pos;
      error = oss.str();
    }
    return false;
  }

  void skipWhitespace() {
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos;
      } else {
        break;
      }
    }
  }

  bool parseValue(Json* out, int depth) {
    // Bound recursion so hostile or corrupt input cannot blow the stack.
    if (depth > 200) return fail("nesting too deep");
    skipWhitespace();
    if (pos >= text.size()) return fail("unexpected end of input");

    switch (text[pos]) {
      case 'n':
        if (text.compare(pos, 4, "null") != 0) return fail("expected null");
        pos += 4;
        *out = Json::null();
        return true;
      case 't':
        if (text.compare(pos, 4, "true") != 0) return fail("expected true");
        pos += 4;
        *out = Json::boolean(true);
        return true;
      case 'f':
        if (text.compare(pos, 5, "false") != 0) return fail("expected false");
        pos += 5;
        *out = Json::boolean(false);
        return true;
      case '"': {
        std::string value;
        if (!parseString(&value)) return false;
        *out = Json::string(std::move(value));
        return true;
      }
      case '[': return parseArray(out, depth);
      case '{': return parseObject(out, depth);
      default: return parseNumber(out);
    }
  }

  bool parseString(std::string* out) {
    if (pos >= text.size() || text[pos] != '"') return fail("expected '\"'");
    ++pos;
    out->clear();
    while (pos < text.size()) {
      const char c = text[pos++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos >= text.size()) return fail("truncated escape");
      const char esc = text[pos++];
      switch (esc) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
          if (pos + 4 > text.size()) return fail("truncated \\u escape");
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text[pos + i];
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code |= static_cast<unsigned>(h - 'A' + 10);
            } else {
              return fail("bad hex digit in \\u escape");
            }
          }
          pos += 4;
          // Encode as UTF-8. Surrogate pairs are not recombined: everything this
          // engine writes is ASCII, so the parser only has to survive them.
          if (code < 0x80) {
            out->push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out->push_back(static_cast<char>(0xE0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool parseNumber(Json* out) {
    const size_t start = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
    bool any_digits = false;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
      ++pos;
      any_digits = true;
    }
    if (pos < text.size() && text[pos] == '.') {
      ++pos;
      while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        ++pos;
        any_digits = true;
      }
    }
    if (!any_digits) return fail("expected a number");
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
      ++pos;
      if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
      if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
        return fail("malformed exponent");
      }
      while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    }
    *out = Json::number(std::strtod(text.substr(start, pos - start).c_str(), nullptr));
    return true;
  }

  bool parseArray(Json* out, int depth) {
    ++pos;  // consume '['
    Json result = Json::array();
    skipWhitespace();
    if (pos < text.size() && text[pos] == ']') {
      ++pos;
      *out = std::move(result);
      return true;
    }
    while (true) {
      Json element;
      if (!parseValue(&element, depth + 1)) return false;
      result.push(std::move(element));
      skipWhitespace();
      if (pos >= text.size()) return fail("unterminated array");
      if (text[pos] == ',') {
        ++pos;
        continue;
      }
      if (text[pos] == ']') {
        ++pos;
        *out = std::move(result);
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parseObject(Json* out, int depth) {
    ++pos;  // consume '{'
    Json result = Json::object();
    skipWhitespace();
    if (pos < text.size() && text[pos] == '}') {
      ++pos;
      *out = std::move(result);
      return true;
    }
    while (true) {
      skipWhitespace();
      std::string key;
      if (!parseString(&key)) return false;
      skipWhitespace();
      if (pos >= text.size() || text[pos] != ':') return fail("expected ':'");
      ++pos;
      Json value;
      if (!parseValue(&value, depth + 1)) return false;
      result.set(key, std::move(value));
      skipWhitespace();
      if (pos >= text.size()) return fail("unterminated object");
      if (text[pos] == ',') {
        ++pos;
        continue;
      }
      if (text[pos] == '}') {
        ++pos;
        *out = std::move(result);
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }
};

}  // namespace

Json Json::boolean(bool v) {
  Json j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}
Json Json::number(double v) {
  Json j;
  j.type_ = Type::Number;
  j.number_ = v;
  return j;
}
Json Json::integer(int64_t v) { return number(static_cast<double>(v)); }
Json Json::string(std::string v) {
  Json j;
  j.type_ = Type::String;
  j.string_ = std::move(v);
  return j;
}
Json Json::array() {
  Json j;
  j.type_ = Type::Array;
  return j;
}
Json Json::object() {
  Json j;
  j.type_ = Type::Object;
  return j;
}

bool Json::asBool(bool fallback) const { return type_ == Type::Bool ? bool_ : fallback; }
double Json::asDouble(double fallback) const {
  return type_ == Type::Number ? number_ : fallback;
}
int64_t Json::asInt(int64_t fallback) const {
  return type_ == Type::Number ? static_cast<int64_t>(std::llround(number_)) : fallback;
}
const std::string& Json::asString() const {
  return type_ == Type::String ? string_ : emptyString();
}

void Json::push(Json value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

size_t Json::size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

const Json& Json::at(size_t index) const {
  if (type_ != Type::Array || index >= array_.size()) return nullJson();
  return array_[index];
}

void Json::set(const std::string& key, Json value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  object_[key] = std::move(value);
}

bool Json::has(const std::string& key) const {
  return type_ == Type::Object && object_.find(key) != object_.end();
}

const Json& Json::get(const std::string& key) const {
  if (type_ != Type::Object) return nullJson();
  const auto it = object_.find(key);
  return it == object_.end() ? nullJson() : it->second;
}

void Json::dumpTo(std::string* out, int indent, int depth) const {
  const auto newline = [out, indent](int d) {
    if (indent <= 0) return;
    out->push_back('\n');
    out->append(static_cast<size_t>(indent) * d, ' ');
  };

  switch (type_) {
    case Type::Null: out->append("null"); return;
    case Type::Bool: out->append(bool_ ? "true" : "false"); return;
    case Type::Number: appendNumber(number_, out); return;
    case Type::String: appendEscaped(string_, out); return;
    case Type::Array: {
      if (array_.empty()) {
        out->append("[]");
        return;
      }
      out->push_back('[');
      for (size_t i = 0; i < array_.size(); ++i) {
        if (i) out->push_back(',');
        newline(depth + 1);
        array_[i].dumpTo(out, indent, depth + 1);
      }
      newline(depth);
      out->push_back(']');
      return;
    }
    case Type::Object: {
      if (object_.empty()) {
        out->append("{}");
        return;
      }
      out->push_back('{');
      bool first = true;
      for (const auto& entry : object_) {
        if (!first) out->push_back(',');
        first = false;
        newline(depth + 1);
        appendEscaped(entry.first, out);
        out->push_back(':');
        if (indent > 0) out->push_back(' ');
        entry.second.dumpTo(out, indent, depth + 1);
      }
      newline(depth);
      out->push_back('}');
      return;
    }
  }
}

std::string Json::dump(int indent) const {
  std::string out;
  dumpTo(&out, indent, 0);
  return out;
}

bool Json::parse(const std::string& text, Json* out, std::string* error) {
  Parser parser(text);
  Json result;
  if (!parser.parseValue(&result, 0)) {
    if (error) *error = parser.error;
    return false;
  }
  parser.skipWhitespace();
  if (parser.pos != text.size()) {
    if (error) *error = "trailing content after JSON value";
    return false;
  }
  *out = std::move(result);
  return true;
}

}  // namespace parallax
