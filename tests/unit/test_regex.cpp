// ICU-compatible regular expressions (regex/regex.h), the engine behind
// java.util.regex in AnyAndroid. Expected results were checked against
// java.util.regex, except where ICU (Android's engine) differs from it.
#include <string>
#include <vector>

#include "common/unicode.h"
#include "regex/regex.h"
#include "test.h"

using namespace aa;
using regex::Matcher;
using regex::Pattern;

namespace {

// Every find() of the pattern: "[start,end,g1start,g1end...]" separated by spaces.
std::string finds(std::u16string_view pattern, std::u16string input, uint32_t flags = 0) {
  Matcher m(Pattern::compile(pattern, flags));
  int32_t len = int32_t(input.size());
  m.set_input(std::move(input), 0, len);
  std::string out;
  for (int n = 0; n < 50 && m.find_next(); ++n) {
    out += out.empty() ? "[" : " [";
    for (size_t g = 0; g < m.groups().size(); ++g) out += (g ? "," : "") + std::to_string(m.groups()[g]);
    out += "]";
  }
  return out;
}

bool matches(std::u16string_view pattern, std::u16string input, uint32_t flags = 0) {
  Matcher m(Pattern::compile(pattern, flags));
  int32_t len = int32_t(input.size());
  m.set_input(std::move(input), 0, len);
  return m.matches();
}

std::string syntax_error(std::u16string_view pattern) {
  try {
    Pattern::compile(pattern, 0);
  } catch (const regex::SyntaxError& e) {
    return std::string(e.what()) + "@" + std::to_string(e.index);
  }
  return "";
}

}  // namespace

TEST(regex_literals_classes_and_quantifiers) {
  CHECK_EQ(finds(u"abc", u"xabcxabc"), std::string("[1,4] [5,8]"));
  CHECK_EQ(finds(u"a.c", u"abc a\nc a c"), std::string("[0,3]"));
  CHECK_EQ(finds(u"a.c", u"a\nc", regex::kDotAll), std::string("[0,3]"));
  CHECK_EQ(finds(u"a{2,3}", u"aaaaaaa"), std::string("[0,3] [3,6]"));
  CHECK_EQ(finds(u"a+?", u"aaa"), std::string("[0,1] [1,2] [2,3]"));
  CHECK_EQ(finds(u"a*?b", u"aaab"), std::string("[0,4]"));
  CHECK_EQ(finds(u"x*", u"axxb"), std::string("[0,0] [1,3] [3,3] [4,4]"));
  CHECK_EQ(finds(u"(a|ab)(c|bcd)(d*)", u"abcd"), std::string("[0,4,0,1,1,4,4,4]"));
  CHECK_EQ(finds(u"(\\d+)-(\\d+)", u"12-345 and 6-7"), std::string("[0,6,0,2,3,6] [11,14,11,12,13,14]"));
  CHECK_EQ(finds(u"[a-z&&[^aeiou]]+", u"hello world"), std::string("[0,1] [2,4] [6,7] [8,11]"));
  CHECK_EQ(finds(u"[\\w&&[^\\d]]+", u"ab12cd"), std::string("[0,2] [4,6]"));
  CHECK_EQ(finds(u"[\\p{L}--[a-z]]", u"aBcÉ"), std::string("[1,2] [3,4]"));  // ICU difference
  CHECK_EQ(finds(u"[]a]", u"]"), std::string("[0,1]"));
  CHECK_EQ(finds(u"[[:alpha:]]+", u"ab1c"), std::string("[0,2] [3,4]"));  // POSIX names (ICU)
  CHECK_EQ(finds(u"\\Qa.b\\E+", u"a.bbb a.b"), std::string("[0,5] [6,9]"));
  CHECK_EQ(finds(u"\\x41\\u0042\\x{43}\\0101", u"ABCA"), std::string("[0,4]"));
  CHECK_EQ(finds(u"a # comment\n b", u"ab", regex::kComments), std::string("[0,2]"));
  CHECK_EQ(finds(u"(a|(b))+", u"ba"), std::string("[0,2,1,2,0,1]"));
  CHECK_EQ(finds(u"(\\w)(?:\\1)*", u"aaabbc"), std::string("[0,3,0,1] [3,5,3,4] [5,6,5,6]"));
  CHECK_EQ(finds(u"<([a-z]+)>.*?</\\1>", u"<b>x</b><i>y</i>"), std::string("[0,8,1,2] [8,16,9,10]"));
  CHECK_EQ(finds(u"a{1000}", std::u16string(1000, u'a')), std::string("[0,1000]"));
}

TEST(regex_anchors_lines_and_boundaries) {
  CHECK(matches(u"^abc$", u"abc"));
  CHECK_EQ(finds(u"^abc$", u"abc\r\n"), std::string("[0,3]"));
  CHECK_EQ(finds(u"^abc", u"abc\nabc", regex::kMultiline), std::string("[0,3] [4,7]"));
  CHECK_EQ(finds(u"$", u"a\nb\n", regex::kMultiline), std::string("[1,1] [3,3] [4,4]"));
  CHECK_EQ(finds(u"$", u"a\r\nb", regex::kMultiline), std::string("[1,1] [4,4]"));
  CHECK_EQ(finds(u"^", u"a\nb\n", regex::kMultiline), std::string("[0,0] [2,2]"));
  CHECK_EQ(finds(u"^$", u"", regex::kMultiline), std::string(""));  // never at the end, as Java and ICU
  CHECK_EQ(finds(u"ab\\Z", u"ab\n"), std::string("[0,2]"));
  CHECK_EQ(finds(u"ab\\z", u"ab\n"), std::string(""));
  CHECK_EQ(finds(u"\\bfoo\\b", u"foo bar foobar barfoo foo"), std::string("[0,3] [22,25]"));
  CHECK_EQ(finds(u"\\G\\w", u"ab cd"), std::string("[0,1] [1,2]"));
  CHECK_EQ(finds(u"\\R", u"a\r\nb\nc"), std::string("[1,3] [4,5]"));
}

TEST(regex_groups_lookaround_and_backtracking_control) {
  CHECK_EQ(finds(u"(?<=\\$)\\d+", u"cost $42 or 7"), std::string("[6,8]"));
  CHECK_EQ(finds(u"(?<!\\$)\\b\\d+", u"cost $42 or 7"), std::string("[12,13]"));
  CHECK_EQ(finds(u"(?<=a.{0,2})z", u"az axz axxz axxxz"), std::string("[1,2] [5,6] [10,11]"));
  CHECK_EQ(finds(u"(?=(a+))a*b\\1", u"baaabac"), std::string("[3,6,3,4]"));
  CHECK_EQ(finds(u"(\\d{3})(?=(\\d{3})+$)", u"1234567"), std::string("[1,4,1,4,4,7]"));
  CHECK_EQ(finds(u"(?>a+)a", u"aaaa"), std::string(""));
  CHECK_EQ(finds(u"a++b", u"aaab"), std::string("[0,4]"));
  CHECK_EQ(finds(u"(?<year>\\d{4})-(?<m>\\d\\d)", u"on 2024-05-01"), std::string("[3,10,3,7,8,10]"));
  CHECK_EQ(finds(u"(?<x>a)\\k<x>", u"aa ab"), std::string("[0,2,0,1]"));
  auto p = Pattern::compile(u"(?<year>\\d{4})-(?<m>\\d\\d)", 0);
  CHECK_EQ(p->group_count(), 2);
  CHECK_EQ(p->group_index(u"m"), 2);
  CHECK_EQ(p->group_index(u"day"), -1);
  // Exponential backtracking stays within the default budget.
  CHECK_EQ(finds(u"(a+)+$", u"aaaaaaaaaaaaaaaaaab"), std::string(""));
}

TEST(regex_case_insensitive_and_unicode) {
  CHECK_EQ(finds(u"(?i)hello", u"HeLLo"), std::string("[0,5]"));
  CHECK_EQ(finds(u"[a-c]+", u"ABCabc", regex::kCaseInsensitive), std::string("[0,6]"));
  CHECK_EQ(finds(u"(?i)[^a-c]+", u"ABCdef"), std::string("[3,6]"));
  CHECK_EQ(finds(u"(?i)(a)\\1", u"aA"), std::string("[0,2,0,1]"));
  CHECK_EQ(finds(u"(?i)é", u"É"), std::string("[0,1]"));
  CHECK_EQ(finds(u"\\p{Lu}", u"éÉ", regex::kCaseInsensitive), std::string("[0,1] [1,2]"));
  CHECK_EQ(finds(u"\\p{Lu}\\p{Ll}+", u"Hello World"), std::string("[0,5] [6,11]"));
  CHECK_EQ(finds(u"\\p{IsLatin}+", u"abc αβγ"), std::string("[0,3]"));
  CHECK_EQ(finds(u"\\p{InGreek}+", u"abc αβγ"), std::string("[4,7]"));
  CHECK_EQ(finds(u"\\d+", u"٣٤٥ and 678"), std::string("[0,3] [8,11]"));
  CHECK_EQ(finds(u"[\\u4e00-\\u9fff]+", u"中文abc"), std::string("[0,2]"));
  // Supplementary code points: one '.' and one class item each.
  CHECK_EQ(finds(u".", u"\U0001F600x"), std::string("[0,2] [2,3]"));
  CHECK_EQ(finds(u"[\U0001F600-\U0001F64F]", u"\U0001F603"), std::string("[0,2]"));
  CHECK_EQ(unicode::to_upper(0xE9), char32_t(0xC9));
  CHECK_EQ(unicode::to_lower(0x391), char32_t(0x3B1));  // Greek
}

TEST(regex_regions_bounds_and_end_flags) {
  Matcher m(Pattern::compile(u"^\\w+$", 0));
  m.set_input(u"ab cd ef", 3, 5);  // region "cd"
  CHECK(m.matches());
  CHECK_EQ(m.groups()[0], 3);
  m.set_input(u"ab cd ef", 3, 5);
  m.use_anchoring_bounds(false);
  CHECK(!m.looking_at());  // ^ is the start of the input
  // Lookahead past the region only with transparent bounds.
  Matcher look(Pattern::compile(u"cd(?= )", 0));
  look.set_input(u"ab cd ef", 3, 5);
  CHECK(!look.find_next());
  look.set_input(u"ab cd ef", 3, 5);
  look.use_transparent_bounds(true);
  CHECK(look.find_next());
  // hitEnd / requireEnd.
  Matcher e(Pattern::compile(u"a+$", 0));
  e.set_input(u"baa", 0, 3);
  CHECK(e.find_next());
  CHECK(e.hit_end());
  CHECK(e.require_end());
  Matcher f(Pattern::compile(u"ab", 0));
  f.set_input(u"abx", 0, 3);
  CHECK(f.looking_at());
  CHECK(!f.hit_end());
  // find(start) restarts on the whole input.
  Matcher g(Pattern::compile(u"\\d", 0));
  g.set_input(u"1a2b3", 0, 2);
  CHECK(g.find(3));
  CHECK_EQ(g.groups()[0], 4);
}

TEST(regex_syntax_errors_and_budgets) {
  CHECK_EQ(syntax_error(u"*a"), std::string("U_REGEX_RULE_SYNTAX@0"));
  CHECK_EQ(syntax_error(u"(ab"), std::string("U_REGEX_MISMATCHED_PAREN@3"));
  CHECK_EQ(syntax_error(u"ab)"), std::string("U_REGEX_MISMATCHED_PAREN@2"));
  CHECK_EQ(syntax_error(u"[ab"), std::string("U_REGEX_MISSING_CLOSE_BRACKET@3"));
  CHECK_EQ(syntax_error(u"a{3,2}"), std::string("U_REGEX_MAX_LT_MIN@6"));
  CHECK_EQ(syntax_error(u"a**"), std::string("U_REGEX_RULE_SYNTAX@2"));
  CHECK_EQ(syntax_error(u"\\q"), std::string("U_REGEX_BAD_ESCAPE_SEQUENCE@1"));
  CHECK_EQ(syntax_error(u"\\p{NoSuchProperty}"), std::string("U_REGEX_PROPERTY_SYNTAX@2"));
  CHECK_EQ(syntax_error(u"(?<=a+)b").substr(0, 25), std::string("U_REGEX_LOOK_BEHIND_LIMIT"));
  CHECK_EQ(syntax_error(u"\\2(a)").substr(0, 24), std::string("U_REGEX_INVALID_BACK_REF"));
  CHECK_EQ(syntax_error(u"(?<n>a)(?<n>b)").substr(0, 34), std::string("U_REGEX_INVALID_CAPTURE_GROUP_NAME"));
  // Untrusted patterns: nesting is bounded, and so are a match's steps and stack.
  CHECK_EQ(syntax_error(std::u16string(5000, u'(') + std::u16string(5000, u')')).substr(0, 23),
           std::string("U_REGEX_PATTERN_TOO_BIG"));
  Matcher slow(Pattern::compile(u"(a+)+$", 0));
  slow.set_input(std::u16string(40, u'a') + u"b", 0, 41);
  slow.set_limits(100000, 1000000);
  bool limited = false;
  try {
    slow.find_next();
  } catch (const regex::ResourceError&) {
    limited = true;
  }
  CHECK(limited);
}
