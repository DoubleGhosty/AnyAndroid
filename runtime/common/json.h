#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aa {

std::string json_escape(std::string_view s);

// Minimal streaming JSON writer (reports, structured logs). Keys and string
// values are escaped; the caller is responsible for balanced begin/end calls.
class JsonWriter {
 public:
  explicit JsonWriter(bool pretty = true) : pretty_(pretty) {}

  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(std::string_view k);

  JsonWriter& value(std::string_view v);
  JsonWriter& value(const char* v) { return value(std::string_view(v)); }
  JsonWriter& value(const std::string& v) { return value(std::string_view(v)); }
  JsonWriter& value(bool v);
  JsonWriter& value(int64_t v);
  JsonWriter& value(uint64_t v);
  JsonWriter& value(int v) { return value(int64_t(v)); }
  JsonWriter& value(unsigned v) { return value(uint64_t(v)); }
  JsonWriter& null();

  template <typename T>
  JsonWriter& field(std::string_view k, const T& v) {
    key(k);
    return value(v);
  }

  const std::string& str() const { return out_; }

 private:
  void before_value();
  void newline();

  std::string out_;
  std::vector<bool> first_;  // one entry per open container
  bool pretty_;
  bool after_key_ = false;
};

}  // namespace aa
