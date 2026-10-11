#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Regular expressions with the syntax and matching rules of ICU's regex
// engine, which Android's java.util.regex uses (com.android.icu.util.regex
// natives): Java-style syntax, UTF-16 text and indices, code point matching.
//
// Supported: literals and escapes (\t \n \x{..} \uhhhh \0ooo \cX \Q..\E...),
// classes ([..] with ranges, negation, nesting, && intersection, --
// difference, [:posix:], \p{..}), predefined classes (\d \s \w \h \v and
// negations, \R, \X approximated), anchors (^ $ \A \z \Z \b \B \G), groups
// (capturing, named, non-capturing, atomic, lookahead, bounded lookbehind),
// back references (\n, \k<name>), greedy, lazy and possessive quantifiers,
// inline flags (?imsxdwu-imsxdwu) and their scoped form.
//
// Unicode properties come from the C library's classification of the BMP
// (as java.lang.Character in AnyAndroid); supplementary code points are
// matched literally but have no properties. Case-insensitive matching uses
// simple case folding.
//
// Patterns come from untrusted code: compilation bounds the pattern size and
// nesting, and matching bounds the steps and the backtracking stack
// (ResourceError rather than a hang or an exhausted host).
namespace aa::regex {

// Flags of java.util.regex.Pattern, with ICU's identical values.
enum Flags : uint32_t {
  kUnixLines = 0x01,
  kCaseInsensitive = 0x02,
  kComments = 0x04,
  kMultiline = 0x08,
  kLiteral = 0x10,
  kDotAll = 0x20,
};

// A syntax error: ICU's error name (U_REGEX_RULE_SYNTAX...) and the index of
// the offending character in the pattern.
struct SyntaxError : std::runtime_error {
  SyntaxError(std::string name, int32_t at) : std::runtime_error(name), index(at) {}
  int32_t index;
};

// The step or stack budget of a match was exhausted.
struct ResourceError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Program;

class Pattern {
 public:
  static std::shared_ptr<const Pattern> compile(std::u16string_view pattern, uint32_t flags);
  ~Pattern();

  int32_t group_count() const;
  // Number of the group with that name, or -1.
  int32_t group_index(std::u16string_view name) const;
  const Program& program() const { return *program_; }

 private:
  Pattern() = default;
  std::unique_ptr<Program> program_;
};

// Matching state of one pattern over one input (ICU's RegexMatcher).
class Matcher {
 public:
  explicit Matcher(std::shared_ptr<const Pattern> pattern);

  // Sets the text and the region [start, end), and resets the matcher.
  void set_input(std::u16string text, int32_t start, int32_t end);
  void use_anchoring_bounds(bool b) { anchoring_bounds_ = b; }
  void use_transparent_bounds(bool b) { transparent_bounds_ = b; }

  // find(start): resets the region to the whole text, then searches from
  // `start`. find_next(): next match in the region after the previous one.
  bool find(int32_t start);
  bool find_next();
  bool matches();
  bool looking_at();

  int32_t group_count() const;
  // Start/end pairs of group 0..group_count() of the last match (-1: unset).
  const std::vector<int32_t>& groups() const { return groups_; }
  bool hit_end() const { return hit_end_; }
  bool require_end() const { return require_end_; }

  // Budgets of one match operation (tests lower them).
  void set_limits(uint64_t max_steps, size_t max_stack) {
    max_steps_ = max_steps;
    max_stack_ = max_stack;
  }

 private:
  enum class Mode { kFind, kMatches, kLookingAt };
  bool search(int32_t from, Mode mode);

  std::shared_ptr<const Pattern> pattern_;
  std::u16string text_;
  int32_t region_start_ = 0, region_end_ = 0;
  bool anchoring_bounds_ = true, transparent_bounds_ = false;
  std::vector<int32_t> groups_;
  int32_t last_end_ = -1;    // end of the previous match (\G, find_next), -1 before the first
  bool last_empty_ = false;  // previous match was empty
  bool exhausted_ = false;   // find_next() reached the end
  bool hit_end_ = false, require_end_ = false;
  uint64_t max_steps_ = 200'000'000;
  size_t max_stack_ = 2'000'000;  // frames of 20 bytes
};

}  // namespace aa::regex
