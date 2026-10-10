#include "common/json.h"

#include <cstdio>

namespace aa {

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

void JsonWriter::newline() {
  if (!pretty_) return;
  out_.push_back('\n');
  out_.append(first_.size() * 2, ' ');
}

void JsonWriter::before_value() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!first_.empty()) {
    if (!first_.back()) out_.push_back(',');
    first_.back() = false;
    newline();
  }
}

JsonWriter& JsonWriter::begin_object() {
  before_value();
  out_.push_back('{');
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  bool empty = first_.back();
  first_.pop_back();
  if (!empty) newline();
  out_.push_back('}');
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  before_value();
  out_.push_back('[');
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  bool empty = first_.back();
  first_.pop_back();
  if (!empty) newline();
  out_.push_back(']');
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) {
  before_value();
  out_.push_back('"');
  out_ += json_escape(k);
  out_ += pretty_ ? "\": " : "\":";
  after_key_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view v) {
  before_value();
  out_.push_back('"');
  out_ += json_escape(v);
  out_.push_back('"');
  return *this;
}

JsonWriter& JsonWriter::value(bool v) {
  before_value();
  out_ += v ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::value(int64_t v) {
  before_value();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter& JsonWriter::value(uint64_t v) {
  before_value();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter& JsonWriter::null() {
  before_value();
  out_ += "null";
  return *this;
}

}  // namespace aa
