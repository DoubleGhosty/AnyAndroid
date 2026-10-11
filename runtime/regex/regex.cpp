#include "regex/regex.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "common/unicode.h"

namespace aa::regex {

namespace {

constexpr char32_t kMaxCodePoint = 0x10FFFF;
constexpr size_t kMaxPatternLength = 1u << 20;  // UTF-16 units
constexpr int kMaxNesting = 200;                // groups, classes and quantified atoms
constexpr size_t kMaxProgram = 1u << 20;        // instructions

// --- Code point sets: sorted, disjoint, non-adjacent ranges -------------------------------------

struct Range {
  char32_t lo, hi;
};
using Ranges = std::vector<Range>;

Ranges normalized(Ranges r) {
  std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
  Ranges out;
  for (const Range& x : r) {
    if (!out.empty() && x.lo <= out.back().hi + 1) {
      out.back().hi = std::max(out.back().hi, x.hi);
    } else {
      out.push_back(x);
    }
  }
  return out;
}

Ranges unite(const Ranges& a, const Ranges& b) {
  Ranges r = a;
  r.insert(r.end(), b.begin(), b.end());
  return normalized(std::move(r));
}

Ranges complement(const Ranges& a) {
  Ranges out;
  char32_t next = 0;
  for (const Range& x : a) {
    if (x.lo > next) out.push_back({next, x.lo - 1});
    next = x.hi + 1;
  }
  if (next <= kMaxCodePoint) out.push_back({next, kMaxCodePoint});
  return out;
}

Ranges intersect(const Ranges& a, const Ranges& b) { return complement(unite(complement(a), complement(b))); }
Ranges subtract(const Ranges& a, const Ranges& b) { return intersect(a, complement(b)); }

bool contains(const Ranges& r, char32_t c) {
  auto it = std::upper_bound(r.begin(), r.end(), c, [](char32_t v, const Range& x) { return v < x.lo; });
  return it != r.begin() && c <= std::prev(it)->hi;
}

// --- Character properties ------------------------------------------------------------------------
// The BMP is classified with the C library, as java.lang.Character is in
// AnyAndroid; the tables below fill what it does not tell apart.

bool is_upper(char32_t c) { return c <= 0xFFFF && std::iswupper(wint_t(c)); }
bool is_lower(char32_t c) { return c <= 0xFFFF && std::iswlower(wint_t(c)); }
bool is_alpha(char32_t c) { return c <= 0xFFFF && std::iswalpha(wint_t(c)); }

using unicode::fold;
using unicode::to_lower;
using unicode::to_upper;

// Decimal digits (Nd) of the main scripts.
const Ranges& nd_ranges() {
  static Ranges r = normalized(
      {{0x30, 0x39},     {0x660, 0x669},     {0x6F0, 0x6F9},    {0x7C0, 0x7C9},   {0x966, 0x96F},   {0x9E6, 0x9EF},
       {0xA66, 0xA6F},   {0xAE6, 0xAEF},     {0xB66, 0xB6F},    {0xBE6, 0xBEF},   {0xC66, 0xC6F},   {0xCE6, 0xCEF},
       {0xD66, 0xD6F},   {0xDE6, 0xDEF},     {0xE50, 0xE59},    {0xED0, 0xED9},   {0xF20, 0xF29},   {0x1040, 0x1049},
       {0x1090, 0x1099}, {0x17E0, 0x17E9},   {0x1810, 0x1819},  {0x1946, 0x194F}, {0x19D0, 0x19D9}, {0x1A80, 0x1A89},
       {0x1A90, 0x1A99}, {0x1B50, 0x1B59},   {0x1BB0, 0x1BB9},  {0x1C40, 0x1C49}, {0x1C50, 0x1C59}, {0xA620, 0xA629},
       {0xA8D0, 0xA8D9}, {0xA900, 0xA909},   {0xA9D0, 0xA9D9},  {0xA9F0, 0xA9F9}, {0xAA50, 0xAA59}, {0xABF0, 0xABF9},
       {0xFF10, 0xFF19}, {0x104A0, 0x104A9}, {0x1D7CE, 0x1D7FF}});
  return r;
}

// Combining marks (M) of the main blocks.
const Ranges& mark_ranges() {
  static Ranges r = normalized(
      {{0x300, 0x36F},   {0x483, 0x489},   {0x591, 0x5BD},   {0x5BF, 0x5BF},   {0x5C1, 0x5C2},   {0x5C4, 0x5C5},
       {0x5C7, 0x5C7},   {0x610, 0x61A},   {0x64B, 0x65F},   {0x670, 0x670},   {0x6D6, 0x6DC},   {0x6DF, 0x6E4},
       {0x6E7, 0x6E8},   {0x6EA, 0x6ED},   {0x900, 0x903},   {0x93A, 0x93C},   {0x93E, 0x94F},   {0x951, 0x957},
       {0x962, 0x963},   {0x981, 0x983},   {0xE31, 0xE31},   {0xE34, 0xE3A},   {0xE47, 0xE4E},   {0x1AB0, 0x1AFF},
       {0x1DC0, 0x1DFF}, {0x200C, 0x200D}, {0x20D0, 0x20FF}, {0x302A, 0x302F}, {0x3099, 0x309A}, {0xFE00, 0xFE0F},
       {0xFE20, 0xFE2F}});
  return r;
}

const Ranges& space_separator_ranges() {  // Zs
  static Ranges r = normalized({{0x20, 0x20},
                                {0xA0, 0xA0},
                                {0x1680, 0x1680},
                                {0x2000, 0x200A},
                                {0x202F, 0x202F},
                                {0x205F, 0x205F},
                                {0x3000, 0x3000}});
  return r;
}

const Ranges& white_space_ranges() {  // White_Space
  static Ranges r = unite(space_separator_ranges(), normalized({{0x9, 0xD}, {0x85, 0x85}, {0x2028, 0x2029}}));
  return r;
}

const Ranges& control_ranges() {  // Cc
  static Ranges r = normalized({{0x0, 0x1F}, {0x7F, 0x9F}});
  return r;
}

const Ranges& format_ranges() {  // Cf
  static Ranges r = normalized({{0xAD, 0xAD},
                                {0x600, 0x605},
                                {0x61C, 0x61C},
                                {0x6DD, 0x6DD},
                                {0x70F, 0x70F},
                                {0x180E, 0x180E},
                                {0x200B, 0x200F},
                                {0x202A, 0x202E},
                                {0x2060, 0x2064},
                                {0x2066, 0x206F},
                                {0xFEFF, 0xFEFF},
                                {0xFFF9, 0xFFFB},
                                {0xE0001, 0xE0001},
                                {0xE0020, 0xE007F}});
  return r;
}

const Ranges& titlecase_ranges() {  // Lt
  static Ranges r = normalized({{0x1C5, 0x1C5},
                                {0x1C8, 0x1C8},
                                {0x1CB, 0x1CB},
                                {0x1F2, 0x1F2},
                                {0x1F88, 0x1F8F},
                                {0x1F98, 0x1F9F},
                                {0x1FA8, 0x1FAF},
                                {0x1FBC, 0x1FBC},
                                {0x1FCC, 0x1FCC},
                                {0x1FFC, 0x1FFC}});
  return r;
}

// ASCII symbols (S) and the main symbol blocks; the rest of iswpunct is P.
const Ranges& symbol_ranges() {
  static Ranges r = normalized(
      {{'$', '$'},       {'+', '+'},       {'<', '>'},       {'^', '^'},       {'`', '`'},        {'|', '|'},
       {'~', '~'},       {0xA2, 0xA6},     {0xA8, 0xA9},     {0xAC, 0xAC},     {0xAE, 0xB1},      {0xB4, 0xB4},
       {0xB8, 0xB8},     {0xD7, 0xD7},     {0xF7, 0xF7},     {0x2C2, 0x2C5},   {0x2D2, 0x2DF},    {0x20A0, 0x20CF},
       {0x2100, 0x214F}, {0x2190, 0x23FF}, {0x2400, 0x24FF}, {0x2500, 0x27BF}, {0x27F0, 0x27FF},  {0x2900, 0x2BFF},
       {0xFB29, 0xFB29}, {0xFE62, 0xFE66}, {0xFF04, 0xFF04}, {0xFF0B, 0xFF0B}, {0xFF1C, 0xFF1E},  {0xFF3E, 0xFF3E},
       {0xFF40, 0xFF40}, {0xFF5C, 0xFF5C}, {0xFF5E, 0xFF5E}, {0xFFE0, 0xFFEE}, {0x1F000, 0x1FAFF}});
  return r;
}

Ranges from_predicate(bool (*pred)(char32_t)) {
  Ranges out;
  for (char32_t c = 0; c <= 0xFFFF; ++c) {
    if (!pred(c)) continue;
    if (!out.empty() && out.back().hi + 1 == c) {
      out.back().hi = c;
    } else {
      out.push_back({c, c});
    }
  }
  return out;
}

const Ranges& letters() {  // L
  static Ranges r = subtract(unite(from_predicate(is_alpha), titlecase_ranges()), unite(nd_ranges(), mark_ranges()));
  return r;
}
const Ranges& uppercase() {  // Lu
  static Ranges r = intersect(from_predicate(is_upper), letters());
  return r;
}
const Ranges& lowercase() {  // Ll
  static Ranges r = intersect(from_predicate(is_lower), letters());
  return r;
}
const Ranges& punctuation() {  // P
  static Ranges r = subtract(from_predicate([](char32_t c) { return c <= 0xFFFF && std::iswpunct(wint_t(c)) != 0; }),
                             unite(symbol_ranges(), unite(letters(), nd_ranges())));
  return r;
}
const Ranges& alphabetic() {  // Alphabetic
  static Ranges r = unite(letters(), normalized({{0x2160, 0x2188}, {0x3007, 0x3007}, {0x3021, 0x3029}}));
  return r;
}
const Ranges& word_chars() {  // \w: Alphabetic, M, Nd, Pc, ZWNJ/ZWJ
  static Ranges r = unite(unite(alphabetic(), mark_ranges()), unite(nd_ranges(), normalized({{'_', '_'},
                                                                                             {0x203F, 0x2040},
                                                                                             {0x2054, 0x2054},
                                                                                             {0xFE33, 0xFE34},
                                                                                             {0xFE4D, 0xFE4F},
                                                                                             {0xFF3F, 0xFF3F},
                                                                                             {0x200C, 0x200D}})));
  return r;
}

Ranges ranges_of(std::initializer_list<Range> r) { return normalized(Ranges(r)); }

// Unicode scripts and blocks recognised by \p{IsX} / \p{InX} (main ones).
const std::map<std::string, Ranges>& scripts() {
  static const std::map<std::string, Ranges> m = {
      {"latin", ranges_of({{0x41, 0x5A},
                           {0x61, 0x7A},
                           {0xAA, 0xAA},
                           {0xBA, 0xBA},
                           {0xC0, 0xD6},
                           {0xD8, 0xF6},
                           {0xF8, 0x24F},
                           {0x1E00, 0x1EFF},
                           {0x2C60, 0x2C7F},
                           {0xA720, 0xA7FF},
                           {0xFF21, 0xFF3A},
                           {0xFF41, 0xFF5A}})},
      {"greek", ranges_of({{0x370, 0x373},
                           {0x375, 0x377},
                           {0x37A, 0x37D},
                           {0x37F, 0x37F},
                           {0x384, 0x384},
                           {0x386, 0x386},
                           {0x388, 0x3E1},
                           {0x3F0, 0x3FF},
                           {0x1F00, 0x1FFE}})},
      {"cyrillic", ranges_of({{0x400, 0x52F}, {0x1C80, 0x1C88}, {0x2DE0, 0x2DFF}, {0xA640, 0xA69F}})},
      {"armenian", ranges_of({{0x531, 0x58A}, {0xFB13, 0xFB17}})},
      {"hebrew", ranges_of({{0x591, 0x5F4}, {0xFB1D, 0xFB4F}})},
      {"arabic", ranges_of({{0x600, 0x604},
                            {0x606, 0x60B},
                            {0x60D, 0x61A},
                            {0x61C, 0x61E},
                            {0x620, 0x63F},
                            {0x641, 0x64A},
                            {0x656, 0x66F},
                            {0x671, 0x6DC},
                            {0x6DE, 0x6FF},
                            {0x750, 0x77F},
                            {0x8A0, 0x8FF},
                            {0xFB50, 0xFDFF},
                            {0xFE70, 0xFEFC}})},
      {"devanagari", ranges_of({{0x900, 0x950}, {0x955, 0x963}, {0x966, 0x97F}, {0xA8E0, 0xA8FF}})},
      {"bengali", ranges_of({{0x980, 0x9FE}})},
      {"thai", ranges_of({{0xE01, 0xE3A}, {0xE40, 0xE5B}})},
      {"georgian", ranges_of({{0x10A0, 0x10FF}, {0x2D00, 0x2D2D}})},
      {"hangul", ranges_of({{0x1100, 0x11FF},
                            {0x302E, 0x302F},
                            {0x3131, 0x318E},
                            {0xA960, 0xA97C},
                            {0xAC00, 0xD7A3},
                            {0xD7B0, 0xD7FB},
                            {0xFFA0, 0xFFDC}})},
      {"hiragana", ranges_of({{0x3041, 0x3096}, {0x309D, 0x309F}})},
      {"katakana", ranges_of({{0x30A1, 0x30FA},
                              {0x30FD, 0x30FF},
                              {0x31F0, 0x31FF},
                              {0x32D0, 0x32FE},
                              {0x3300, 0x3357},
                              {0xFF66, 0xFF6F},
                              {0xFF71, 0xFF9D}})},
      {"han", ranges_of({{0x2E80, 0x2E99},
                         {0x2E9B, 0x2EF3},
                         {0x2F00, 0x2FD5},
                         {0x3005, 0x3005},
                         {0x3007, 0x3007},
                         {0x3021, 0x3029},
                         {0x3038, 0x303B},
                         {0x3400, 0x4DBF},
                         {0x4E00, 0x9FFF},
                         {0xF900, 0xFAFF},
                         {0x20000, 0x2FA1F}})},
  };
  return m;
}

const std::map<std::string, Ranges>& blocks() {
  static const std::map<std::string, Ranges> m = {
      {"basiclatin", ranges_of({{0x0, 0x7F}})},
      {"latin1supplement", ranges_of({{0x80, 0xFF}})},
      {"latinextendeda", ranges_of({{0x100, 0x17F}})},
      {"latinextendedb", ranges_of({{0x180, 0x24F}})},
      {"ipaextensions", ranges_of({{0x250, 0x2AF}})},
      {"spacingmodifierletters", ranges_of({{0x2B0, 0x2FF}})},
      {"combiningdiacriticalmarks", ranges_of({{0x300, 0x36F}})},
      {"greek", ranges_of({{0x370, 0x3FF}})},
      {"greekandcoptic", ranges_of({{0x370, 0x3FF}})},
      {"cyrillic", ranges_of({{0x400, 0x4FF}})},
      {"armenian", ranges_of({{0x530, 0x58F}})},
      {"hebrew", ranges_of({{0x590, 0x5FF}})},
      {"arabic", ranges_of({{0x600, 0x6FF}})},
      {"devanagari", ranges_of({{0x900, 0x97F}})},
      {"thai", ranges_of({{0xE00, 0xE7F}})},
      {"latinextendedadditional", ranges_of({{0x1E00, 0x1EFF}})},
      {"greekextended", ranges_of({{0x1F00, 0x1FFF}})},
      {"generalpunctuation", ranges_of({{0x2000, 0x206F}})},
      {"superscriptsandsubscripts", ranges_of({{0x2070, 0x209F}})},
      {"currencysymbols", ranges_of({{0x20A0, 0x20CF}})},
      {"letterlikesymbols", ranges_of({{0x2100, 0x214F}})},
      {"numberforms", ranges_of({{0x2150, 0x218F}})},
      {"arrows", ranges_of({{0x2190, 0x21FF}})},
      {"mathematicaloperators", ranges_of({{0x2200, 0x22FF}})},
      {"miscellaneoustechnical", ranges_of({{0x2300, 0x23FF}})},
      {"boxdrawing", ranges_of({{0x2500, 0x257F}})},
      {"geometricshapes", ranges_of({{0x25A0, 0x25FF}})},
      {"miscellaneoussymbols", ranges_of({{0x2600, 0x26FF}})},
      {"dingbats", ranges_of({{0x2700, 0x27BF}})},
      {"cjksymbolsandpunctuation", ranges_of({{0x3000, 0x303F}})},
      {"hiragana", ranges_of({{0x3040, 0x309F}})},
      {"katakana", ranges_of({{0x30A0, 0x30FF}})},
      {"cjkunifiedideographs", ranges_of({{0x4E00, 0x9FFF}})},
      {"hangulsyllables", ranges_of({{0xAC00, 0xD7AF}})},
      {"highsurrogates", ranges_of({{0xD800, 0xDB7F}})},
      {"lowsurrogates", ranges_of({{0xDC00, 0xDFFF}})},
      {"privateusearea", ranges_of({{0xE000, 0xF8FF}})},
      {"alphabeticpresentationforms", ranges_of({{0xFB00, 0xFB4F}})},
      {"arabicpresentationformsa", ranges_of({{0xFB50, 0xFDFF}})},
      {"halfwidthandfullwidthforms", ranges_of({{0xFF00, 0xFFEF}})},
      {"specials", ranges_of({{0xFFF0, 0xFFFF}})},
      {"emoticons", ranges_of({{0x1F600, 0x1F64F}})},
  };
  return m;
}

// ICU's loose matching of property names: case, spaces, '_' and '-' ignored.
std::string loose(std::u16string_view s) {
  std::string out;
  for (char16_t c : s) {
    if (c == ' ' || c == '_' || c == '-') continue;
    if (c > 0x7F) return "\x01";  // never a property name
    out.push_back(char(std::tolower(int(c))));
  }
  return out;
}

std::optional<Ranges> general_category(const std::string& n) {
  static const Ranges kEmpty;
  auto z = [] { return unite(space_separator_ranges(), ranges_of({{0x2028, 0x2029}})); };
  if (n == "l" || n == "letter") return letters();
  if (n == "lu" || n == "uppercaseletter") return uppercase();
  if (n == "ll" || n == "lowercaseletter") return lowercase();
  if (n == "lt" || n == "titlecaseletter") return titlecase_ranges();
  if (n == "lc" || n == "casedletter") return unite(unite(uppercase(), lowercase()), titlecase_ranges());
  if (n == "lm" || n == "modifierletter")
    return ranges_of({{0x2B0, 0x2C1},
                      {0x2C6, 0x2D1},
                      {0x2E0, 0x2E4},
                      {0x2EC, 0x2EC},
                      {0x2EE, 0x2EE},
                      {0x3005, 0x3005},
                      {0x309D, 0x309E},
                      {0x30FC, 0x30FE},
                      {0xFF70, 0xFF70}});
  if (n == "lo" || n == "otherletter") {
    return subtract(letters(), unite(unite(uppercase(), lowercase()), titlecase_ranges()));
  }
  if (n == "m" || n == "mark" || n == "combiningmark") return mark_ranges();
  if (n == "mn" || n == "nonspacingmark") return mark_ranges();
  if (n == "mc" || n == "spacingmark" || n == "me" || n == "enclosingmark") return kEmpty;
  if (n == "n" || n == "number") {
    return unite(nd_ranges(), ranges_of({{0xB2, 0xB3},
                                         {0xB9, 0xB9},
                                         {0xBC, 0xBE},
                                         {0x2070, 0x2070},
                                         {0x2074, 0x2079},
                                         {0x2080, 0x2089},
                                         {0x2150, 0x2189},
                                         {0x2460, 0x249B},
                                         {0x24EA, 0x24FF},
                                         {0x3007, 0x3007},
                                         {0x3021, 0x3029}}));
  }
  if (n == "nd" || n == "decimalnumber" || n == "digit") return nd_ranges();
  if (n == "nl" || n == "letternumber")
    return ranges_of({{0x16EE, 0x16F0}, {0x2160, 0x2182}, {0x2185, 0x2188}, {0x3007, 0x3007}, {0x3021, 0x3029}});
  if (n == "no" || n == "othernumber")
    return ranges_of({{0xB2, 0xB3},
                      {0xB9, 0xB9},
                      {0xBC, 0xBE},
                      {0x2070, 0x2070},
                      {0x2074, 0x2079},
                      {0x2080, 0x2089},
                      {0x2460, 0x249B},
                      {0x24EA, 0x24FF}});
  if (n == "p" || n == "punctuation" || n == "punct") return punctuation();
  if (n == "pc" || n == "connectorpunctuation")
    return ranges_of(
        {{'_', '_'}, {0x203F, 0x2040}, {0x2054, 0x2054}, {0xFE33, 0xFE34}, {0xFE4D, 0xFE4F}, {0xFF3F, 0xFF3F}});
  if (n == "pd" || n == "dashpunctuation")
    return ranges_of({{'-', '-'},
                      {0x58A, 0x58A},
                      {0x5BE, 0x5BE},
                      {0x2010, 0x2015},
                      {0x2E17, 0x2E17},
                      {0x301C, 0x301C},
                      {0x3030, 0x3030},
                      {0xFE58, 0xFE58},
                      {0xFE63, 0xFE63},
                      {0xFF0D, 0xFF0D}});
  if (n == "ps" || n == "openpunctuation")
    return ranges_of({{'(', '('},
                      {'[', '['},
                      {'{', '{'},
                      {0x2018, 0x2018},
                      {0x201A, 0x201A},
                      {0x201C, 0x201C},
                      {0x201E, 0x201E},
                      {0x2045, 0x2045},
                      {0x3008, 0x3008},
                      {0x300A, 0x300A},
                      {0x300C, 0x300C},
                      {0x300E, 0x300E},
                      {0x3010, 0x3010},
                      {0xFF08, 0xFF08},
                      {0xFF3B, 0xFF3B},
                      {0xFF5B, 0xFF5B}});
  if (n == "pe" || n == "closepunctuation")
    return ranges_of({{')', ')'},
                      {']', ']'},
                      {'}', '}'},
                      {0x2046, 0x2046},
                      {0x3009, 0x3009},
                      {0x300B, 0x300B},
                      {0x300D, 0x300D},
                      {0x300F, 0x300F},
                      {0x3011, 0x3011},
                      {0xFF09, 0xFF09},
                      {0xFF3D, 0xFF3D},
                      {0xFF5D, 0xFF5D}});
  if (n == "pi" || n == "initialpunctuation")
    return ranges_of({{0xAB, 0xAB}, {0x2018, 0x2018}, {0x201B, 0x201C}, {0x201F, 0x201F}, {0x2039, 0x2039}});
  if (n == "pf" || n == "finalpunctuation")
    return ranges_of({{0xBB, 0xBB}, {0x2019, 0x2019}, {0x201D, 0x201D}, {0x203A, 0x203A}});
  if (n == "po" || n == "otherpunctuation")
    return subtract(punctuation(), ranges_of({{'_', '_'},
                                              {'-', '-'},
                                              {'(', ')'},
                                              {'[', '['},
                                              {']', ']'},
                                              {'{', '{'},
                                              {'}', '}'},
                                              {0xAB, 0xAB},
                                              {0xBB, 0xBB},
                                              {0x2010, 0x201F},
                                              {0x2039, 0x203A}}));
  if (n == "s" || n == "symbol") return symbol_ranges();
  if (n == "sm" || n == "mathsymbol")
    return ranges_of({{'+', '+'},
                      {'<', '>'},
                      {'|', '|'},
                      {'~', '~'},
                      {0xAC, 0xAC},
                      {0xB1, 0xB1},
                      {0xD7, 0xD7},
                      {0xF7, 0xF7},
                      {0x2190, 0x2194},
                      {0x2200, 0x22FF},
                      {0x27C0, 0x27FF},
                      {0x2900, 0x2AFF},
                      {0xFF0B, 0xFF0B},
                      {0xFF1C, 0xFF1E},
                      {0xFF5C, 0xFF5C},
                      {0xFF5E, 0xFF5E}});
  if (n == "sc" || n == "currencysymbol")
    return ranges_of({{'$', '$'},
                      {0xA2, 0xA5},
                      {0x58F, 0x58F},
                      {0x60B, 0x60B},
                      {0x9F2, 0x9F3},
                      {0xE3F, 0xE3F},
                      {0x20A0, 0x20CF},
                      {0xFDFC, 0xFDFC},
                      {0xFE69, 0xFE69},
                      {0xFF04, 0xFF04},
                      {0xFFE0, 0xFFE1},
                      {0xFFE5, 0xFFE6}});
  if (n == "sk" || n == "modifiersymbol")
    return ranges_of({{'^', '^'},
                      {'`', '`'},
                      {0xA8, 0xA8},
                      {0xAF, 0xAF},
                      {0xB4, 0xB4},
                      {0xB8, 0xB8},
                      {0x2C2, 0x2C5},
                      {0x2D2, 0x2DF},
                      {0xFF3E, 0xFF3E},
                      {0xFF40, 0xFF40}});
  if (n == "so" || n == "othersymbol")
    return ranges_of({{0xA6, 0xA6},
                      {0xA9, 0xA9},
                      {0xAE, 0xAE},
                      {0xB0, 0xB0},
                      {0x2100, 0x214F},
                      {0x2195, 0x21FF},
                      {0x2300, 0x23FF},
                      {0x2400, 0x24FF},
                      {0x2500, 0x27BF},
                      {0x2B00, 0x2BFF},
                      {0x1F000, 0x1FAFF}});
  if (n == "z" || n == "separator") return z();
  if (n == "zs" || n == "spaceseparator") return space_separator_ranges();
  if (n == "zl" || n == "lineseparator") return ranges_of({{0x2028, 0x2028}});
  if (n == "zp" || n == "paragraphseparator") return ranges_of({{0x2029, 0x2029}});
  if (n == "c" || n == "other") {
    return unite(unite(control_ranges(), format_ranges()), ranges_of({{0xD800, 0xF8FF}, {0xF0000, 0x10FFFF}}));
  }
  if (n == "cc" || n == "control" || n == "cntrl") return control_ranges();
  if (n == "cf" || n == "format") return format_ranges();
  if (n == "co" || n == "privateuse") return ranges_of({{0xE000, 0xF8FF}, {0xF0000, 0xFFFFD}, {0x100000, 0x10FFFD}});
  if (n == "cs" || n == "surrogate") return ranges_of({{0xD800, 0xDFFF}});
  if (n == "cn" || n == "unassigned") return kEmpty;
  return std::nullopt;
}

std::optional<Ranges> java_property(const std::string& n) {
  auto ignorable = [] { return unite(ranges_of({{0x0, 0x8}, {0xE, 0x1B}, {0x7F, 0x9F}}), format_ranges()); };
  auto pc = [] { return *general_category("pc"); };
  auto nl = [] { return *general_category("nl"); };
  auto sc = [] { return *general_category("sc"); };
  if (n == "javaletter" || n == "javaalphabetic") return n == "javaletter" ? letters() : alphabetic();
  if (n == "javadigit") return nd_ranges();
  if (n == "javaletterordigit") return unite(letters(), nd_ranges());
  if (n == "javalowercase") return lowercase();
  if (n == "javauppercase") return uppercase();
  if (n == "javatitlecase") return titlecase_ranges();
  if (n == "javawhitespace") {
    return subtract(unite(white_space_ranges(), ranges_of({{0x1C, 0x1F}})),
                    ranges_of({{0x85, 0x85}, {0xA0, 0xA0}, {0x2007, 0x2007}, {0x202F, 0x202F}}));
  }
  if (n == "javaspacechar") return *general_category("z");
  if (n == "javaisocontrol") return control_ranges();
  if (n == "javadefined" || n == "javavalidcodepoint") return ranges_of({{0, kMaxCodePoint}});
  if (n == "javasupplementarycodepoint") return ranges_of({{0x10000, kMaxCodePoint}});
  if (n == "javaidentifierignorable") return ignorable();
  if (n == "javajavaidentifierstart") return unite(unite(letters(), sc()), unite(pc(), nl()));
  if (n == "javajavaidentifierpart") {
    return unite(unite(unite(letters(), sc()), unite(pc(), nl())),
                 unite(unite(nd_ranges(), mark_ranges()), ignorable()));
  }
  if (n == "javaunicodeidentifierstart") return unite(letters(), nl());
  if (n == "javaunicodeidentifierpart") {
    return unite(unite(unite(letters(), pc()), unite(nl(), nd_ranges())), unite(mark_ranges(), ignorable()));
  }
  if (n == "javamirrored")
    return ranges_of({{'(', ')'},
                      {'<', '<'},
                      {'>', '>'},
                      {'[', '['},
                      {']', ']'},
                      {'{', '{'},
                      {'}', '}'},
                      {0xAB, 0xAB},
                      {0xBB, 0xBB},
                      {0x2039, 0x203A},
                      {0x2045, 0x2046},
                      {0x3008, 0x3011}});
  if (n == "javaideographic") return scripts().at("han");
  return std::nullopt;
}

// POSIX-like and binary properties ([:alpha:], \p{Alpha}, \p{WhiteSpace}...),
// with ICU's (Unicode) meanings.
std::optional<Ranges> binary_property(const std::string& n) {
  if (n == "alpha" || n == "alphabetic") return alphabetic();
  if (n == "upper" || n == "uppercase") return uppercase();
  if (n == "lower" || n == "lowercase") return lowercase();
  if (n == "space" || n == "whitespace" || n == "wspace") return white_space_ranges();
  if (n == "blank") return unite(space_separator_ranges(), ranges_of({{'\t', '\t'}}));
  if (n == "xdigit" || n == "hexdigit" || n == "asciihexdigit") {
    return unite(nd_ranges(), ranges_of({{'A', 'F'}, {'a', 'f'}, {0xFF21, 0xFF26}, {0xFF41, 0xFF46}}));
  }
  if (n == "alnum") return unite(alphabetic(), nd_ranges());
  if (n == "word") return word_chars();
  if (n == "ascii") return ranges_of({{0, 0x7F}});
  if (n == "any") return ranges_of({{0, kMaxCodePoint}});
  if (n == "assigned") return ranges_of({{0, kMaxCodePoint}});
  if (n == "graph") {
    return subtract(ranges_of({{0, kMaxCodePoint}}),
                    unite(unite(white_space_ranges(), control_ranges()), ranges_of({{0xD800, 0xDFFF}})));
  }
  if (n == "print") {
    return unite(subtract(*binary_property("graph"), control_ranges()), *binary_property("blank"));
  }
  if (n == "noncharactercodepoint") return ranges_of({{0xFDD0, 0xFDEF}, {0xFFFE, 0xFFFF}});
  if (n == "ideographic") return scripts().at("han");
  if (n == "math") return *general_category("sm");
  return std::nullopt;
}

// \p{name}: general categories, binary/POSIX properties, java* names,
// scripts (Is.., script=..) and blocks (In.., block=..).
Ranges property(std::u16string_view name, int32_t at) {
  static std::mutex lock;
  static std::map<std::string, Ranges> cache;
  std::string n = loose(name);
  {
    std::lock_guard<std::mutex> guard(lock);
    if (auto it = cache.find(n); it != cache.end()) return it->second;
  }
  std::optional<Ranges> r;
  auto value_of = [&](const char* key) -> std::optional<std::string> {
    std::string k = std::string(key) + "=";
    if (n.compare(0, k.size(), k) == 0) return n.substr(k.size());
    return std::nullopt;
  };
  if (auto v = value_of("generalcategory"))
    r = general_category(*v);
  else if (auto v2 = value_of("gc"))
    r = general_category(*v2);
  else if (auto v3 = value_of("script"))
    r = scripts().count(*v3) ? std::optional<Ranges>(scripts().at(*v3)) : std::nullopt;
  else if (auto v4 = value_of("sc"))
    r = scripts().count(*v4) ? std::optional<Ranges>(scripts().at(*v4)) : std::nullopt;
  else if (auto v5 = value_of("block"))
    r = blocks().count(*v5) ? std::optional<Ranges>(blocks().at(*v5)) : std::nullopt;
  else if (auto v6 = value_of("blk"))
    r = blocks().count(*v6) ? std::optional<Ranges>(blocks().at(*v6)) : std::nullopt;
  if (!r) r = general_category(n);
  if (!r) r = binary_property(n);
  if (!r) r = java_property(n);
  if (!r && n.size() > 2 && n.compare(0, 2, "is") == 0) {
    std::string rest = n.substr(2);
    r = general_category(rest);
    if (!r) r = binary_property(rest);
    if (!r && scripts().count(rest)) r = scripts().at(rest);
  }
  if (!r && n.size() > 2 && n.compare(0, 2, "in") == 0 && blocks().count(n.substr(2))) r = blocks().at(n.substr(2));
  if (!r && scripts().count(n)) r = scripts().at(n);
  if (!r) throw SyntaxError("U_REGEX_PROPERTY_SYNTAX", at);
  std::lock_guard<std::mutex> guard(lock);
  cache.emplace(n, *r);
  return *r;
}

const Ranges& digit_set() { return nd_ranges(); }
const Ranges& space_set() { return white_space_ranges(); }
const Ranges& hspace_set() {
  static Ranges r = unite(space_separator_ranges(), ranges_of({{'\t', '\t'}, {0x180E, 0x180E}}));
  return r;
}
const Ranges& vspace_set() {
  static Ranges r = ranges_of({{0xA, 0xD}, {0x85, 0x85}, {0x2028, 0x2029}});
  return r;
}

bool is_line_terminator(char32_t c, bool unix_lines) {
  if (unix_lines) return c == '\n';
  return (c >= 0xA && c <= 0xD) || c == 0x85 || c == 0x2028 || c == 0x2029;
}

}  // namespace

// --- Program ----------------------------------------------------------------------------------------

enum class Op : uint8_t {
  kChar,      // ch (folded when kCaseInsensitive)
  kAny,       // f: kDotAll, kUnixLines
  kSet,       // a: set index
  kAssert,    // a: AssertKind
  kSave,      // a: slot
  kSplit,     // a: first, b: alternative
  kJmp,       // a: target
  kBackref,   // a: group
  kRepeat,    // single item at pc + 1; a: min, b: max (-1: none), c: greed; continues at pc + 2
  kLoopInit,  // a: register
  kLoopHead,  // a: register, b: min, c: max (-1: none), d: greed, e: exit
  kLoopTail,  // a: register, b: head
  kAtomic,    // a: sub-program, b: continuation
  kLook,      // a: sub-program, b: continuation, c: min length, d: max length; f: kBehind, kNegative
  kSucceed,   // end of a (sub-)program
};

enum : uint8_t { kFoldFlag = 1, kBehind = 2, kNegative = 4 };
enum Greed : int32_t { kGreedy = 0, kLazy = 1, kPossessive = 2 };
enum AssertKind : int32_t {
  kBol,
  kBolMultiline,
  kEol,
  kEolMultiline,
  kStartInput,
  kEndInput,
  kEndInputOrTerminator,
  kWordBoundary,
  kNotWordBoundary,
  kPreviousMatchEnd,
};

struct Inst {
  Op op;
  uint8_t f = 0;
  char32_t ch = 0;
  int32_t a = 0, b = 0, c = 0, d = 0, e = 0;
};

struct CharSet {
  Ranges ranges;
  bool negated = false;
};

struct Program {
  std::vector<Inst> code;
  std::vector<CharSet> sets;
  int32_t groups = 0;  // capturing groups, group 0 excluded
  int32_t loops = 0;   // loop registers
  std::vector<std::pair<std::u16string, int32_t>> names;
  bool unix_lines = false;  // for ^ $ \Z (fixed per program: ICU applies the flag globally)
};

namespace {

// --- Parser: pattern -> syntax tree ---------------------------------------------------------------

enum class Kind { kEmpty, kChar, kAny, kSet, kAssert, kGroup, kConcat, kAlt, kRepeat, kBackref, kLook, kAtomic };

struct Node {
  Kind kind;
  char32_t ch = 0;
  bool fold = false;   // kChar, kSet, kBackref: case-insensitive
  uint8_t any = 0;     // kAny flags
  int32_t value = -1;  // kSet: set index, kAssert: kind, kGroup: group (-1 none), kBackref: group
  int32_t min = 0, max = -1;
  Greed greed = kGreedy;
  bool behind = false, negative = false;
  std::vector<std::unique_ptr<Node>> kids;
  explicit Node(Kind k) : kind(k) {}
};
using NodePtr = std::unique_ptr<Node>;

NodePtr node(Kind k) { return std::make_unique<Node>(k); }

class Parser {
 public:
  Parser(std::u16string_view p, uint32_t flags, Program& prog) : p_(p), flags_(flags), prog_(prog) {}

  NodePtr parse() {
    if (p_.size() > kMaxPatternLength) throw SyntaxError("U_REGEX_PATTERN_TOO_BIG", 0);
    NodePtr root;
    if (flags_ & kLiteral) {
      root = node(Kind::kConcat);
      while (i_ < p_.size()) root->kids.push_back(char_node(next_cp()));
    } else {
      root = parse_alt(0);
      if (i_ < p_.size()) throw SyntaxError("U_REGEX_MISMATCHED_PAREN", int32_t(i_));
    }
    for (int32_t g : backrefs_) {
      if (g > prog_.groups) throw SyntaxError("U_REGEX_INVALID_BACK_REF", int32_t(i_));
    }
    return root;
  }

 private:
  bool at_end() const { return i_ >= p_.size(); }
  char16_t peek(size_t ahead = 0) const { return i_ + ahead < p_.size() ? p_[i_ + ahead] : 0; }
  bool eat(char16_t c) {
    if (!at_end() && p_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  char32_t next_cp() {
    char16_t c = p_[i_++];
    if (c >= 0xD800 && c <= 0xDBFF && i_ < p_.size() && p_[i_] >= 0xDC00 && p_[i_] <= 0xDFFF) {
      return 0x10000 + ((char32_t(c) - 0xD800) << 10) + (p_[i_++] - 0xDC00);
    }
    return c;
  }
  [[noreturn]] void error(const char* name) { throw SyntaxError(name, int32_t(std::min(i_, p_.size()))); }

  // Free-spacing mode: white space and #-comments are not part of the pattern.
  void skip_comments() {
    if (!(flags_ & kComments)) return;
    while (!at_end()) {
      char16_t c = peek();
      if (c == ' ' || (c >= 0x9 && c <= 0xD) || c == 0x85 || c == 0x2028 || c == 0x2029) {
        ++i_;
      } else if (c == '#') {
        while (!at_end() && !is_line_terminator(peek(), false)) ++i_;
      } else {
        break;
      }
    }
  }

  NodePtr char_node(char32_t c) {
    NodePtr n = node(Kind::kChar);
    n->fold = (flags_ & kCaseInsensitive) != 0;
    n->ch = n->fold ? fold(c) : c;
    return n;
  }

  NodePtr set_node(Ranges r, bool negated) {
    NodePtr n = node(Kind::kSet);
    n->fold = (flags_ & kCaseInsensitive) != 0;
    n->value = int32_t(prog_.sets.size());
    prog_.sets.push_back({std::move(r), negated});
    return n;
  }

  NodePtr parse_alt(int depth) {
    if (depth > kMaxNesting) error("U_REGEX_PATTERN_TOO_BIG");
    uint32_t saved = flags_;  // (?i) inside a group ends with it
    NodePtr alt = node(Kind::kAlt);
    alt->kids.push_back(parse_concat(depth));
    while (eat('|')) alt->kids.push_back(parse_concat(depth));
    flags_ = saved;
    if (alt->kids.size() == 1) return std::move(alt->kids[0]);
    return alt;
  }

  NodePtr parse_concat(int depth) {
    NodePtr seq = node(Kind::kConcat);
    for (;;) {
      skip_comments();
      if (at_end() || peek() == '|' || peek() == ')') break;
      NodePtr atom = parse_atom(depth);
      if (!atom) continue;  // flag group or comment
      if (atom->kind == Kind::kConcat) {
        // \Q...\E: a quantifier applies to its last character only.
        if (atom->kids.empty()) continue;
        for (size_t k = 0; k + 1 < atom->kids.size(); ++k) seq->kids.push_back(std::move(atom->kids[k]));
        atom = std::move(atom->kids.back());
      }
      atom = parse_quantifier(std::move(atom));
      seq->kids.push_back(std::move(atom));
    }
    if (seq->kids.size() == 1) return std::move(seq->kids[0]);
    return seq;
  }

  int32_t parse_number(int32_t& out) {
    size_t start = i_;
    int64_t v = 0;
    while (!at_end() && peek() >= '0' && peek() <= '9') {
      v = v * 10 + (peek() - '0');
      if (v > INT32_MAX) error("U_REGEX_NUMBER_TOO_BIG");
      ++i_;
    }
    out = int32_t(v);
    return int32_t(i_ - start);
  }

  // At most one quantifier per atom (a second one is a syntax error, as in
  // ICU and Java), with its lazy (?) or possessive (+) modifier.
  NodePtr parse_quantifier(NodePtr atom) {
    skip_comments();
    if (at_end()) return atom;
    int32_t min, max;
    char16_t c = peek();
    if (c == '*') {
      min = 0, max = -1, ++i_;
    } else if (c == '+') {
      min = 1, max = -1, ++i_;
    } else if (c == '?') {
      min = 0, max = 1, ++i_;
    } else if (c == '{') {
      ++i_;
      if (parse_number(min) == 0) error("U_REGEX_BAD_INTERVAL");
      max = min;
      if (eat(',') && parse_number(max) == 0) max = -1;
      if (!eat('}')) error("U_REGEX_BAD_INTERVAL");
      if (max >= 0 && max < min) error("U_REGEX_MAX_LT_MIN");
    } else {
      return atom;
    }
    NodePtr rep = node(Kind::kRepeat);
    rep->min = min;
    rep->max = max;
    if (eat('?')) {
      rep->greed = kLazy;
    } else if (eat('+')) {
      rep->greed = kPossessive;
    }
    rep->kids.push_back(std::move(atom));
    skip_comments();
    if (!at_end() && (peek() == '*' || peek() == '+' || peek() == '?' || peek() == '{')) error("U_REGEX_RULE_SYNTAX");
    return rep;
  }

  NodePtr parse_group(int depth) {
    // After '('.
    if (!eat('?')) {
      NodePtr g = node(Kind::kGroup);
      g->value = ++prog_.groups;
      g->kids.push_back(parse_alt(depth + 1));
      if (!eat(')')) error("U_REGEX_MISMATCHED_PAREN");
      return g;
    }
    if (eat('#')) {  // (?# comment )
      while (!at_end() && peek() != ')') ++i_;
      if (!eat(')')) error("U_REGEX_MISMATCHED_PAREN");
      return nullptr;
    }
    auto enclosed = [&](NodePtr n) {
      n->kids.push_back(parse_alt(depth + 1));
      if (!eat(')')) error("U_REGEX_MISMATCHED_PAREN");
      return n;
    };
    if (eat(':')) return enclosed(node(Kind::kGroup));
    if (eat('>')) return enclosed(node(Kind::kAtomic));
    if (eat('=') || eat('!')) {
      NodePtr n = node(Kind::kLook);
      n->negative = p_[i_ - 1] == '!';
      return enclosed(std::move(n));
    }
    if (peek() == '<' && (peek(1) == '=' || peek(1) == '!')) {
      i_ += 2;
      NodePtr n = node(Kind::kLook);
      n->behind = true;
      n->negative = p_[i_ - 1] == '!';
      return enclosed(std::move(n));
    }
    if (eat('<')) {  // (?<name>...)
      std::u16string name = parse_name();
      if (!eat('>')) error("U_REGEX_INVALID_CAPTURE_GROUP_NAME");
      for (const auto& [n, g] : prog_.names) {
        if (n == name) error("U_REGEX_INVALID_CAPTURE_GROUP_NAME");
      }
      NodePtr g = node(Kind::kGroup);
      g->value = ++prog_.groups;
      prog_.names.emplace_back(name, g->value);
      return enclosed(std::move(g));
    }
    // Flags: (?imsxdwuU-imsx) or (?flags:...).
    uint32_t on = 0, off = 0;
    bool negate = false;
    for (;;) {
      if (at_end()) error("U_REGEX_MISMATCHED_PAREN");
      char16_t c = p_[i_++];
      uint32_t bit = 0;
      switch (c) {
        case 'i':
          bit = kCaseInsensitive;
          break;
        case 'm':
          bit = kMultiline;
          break;
        case 's':
          bit = kDotAll;
          break;
        case 'x':
          bit = kComments;
          break;
        case 'd':
          bit = kUnixLines;
          break;
        case 'u':
        case 'w':
        case 'U':
          bit = 0;
          break;  // Unicode case, Unicode words: always
        case '-':
          if (negate) error("U_REGEX_RULE_SYNTAX");
          negate = true;
          continue;
        case ')':
          flags_ = (flags_ | on) & ~off;
          return nullptr;
        case ':': {
          uint32_t saved = flags_;
          flags_ = (flags_ | on) & ~off;
          NodePtr g = node(Kind::kGroup);
          g->kids.push_back(parse_alt(depth + 1));
          flags_ = saved;
          if (!eat(')')) error("U_REGEX_MISMATCHED_PAREN");
          return g;
        }
        default:
          --i_;
          error("U_REGEX_RULE_SYNTAX");
      }
      (negate ? off : on) |= bit;
    }
  }

  std::u16string parse_name() {
    std::u16string name;
    while (!at_end()) {
      char16_t c = peek();
      bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      bool digit = c >= '0' && c <= '9';
      if (!letter && !(digit && !name.empty())) break;
      name.push_back(c);
      ++i_;
    }
    if (name.empty()) error("U_REGEX_INVALID_CAPTURE_GROUP_NAME");
    return name;
  }

  NodePtr assertion(AssertKind k) {
    NodePtr n = node(Kind::kAssert);
    n->value = k;
    return n;
  }

  NodePtr parse_atom(int depth) {
    if (depth > kMaxNesting) error("U_REGEX_PATTERN_TOO_BIG");
    char16_t c = peek();
    switch (c) {
      case '(':
        ++i_;
        return parse_group(depth);
      case '[': {
        ++i_;
        bool negated = false;
        Ranges r = parse_class(depth + 1, negated);
        return set_node(std::move(r), negated);
      }
      case '.': {
        ++i_;
        NodePtr n = node(Kind::kAny);
        n->any = uint8_t(flags_ & (kDotAll | kUnixLines));
        return n;
      }
      case '^':
        ++i_;
        return assertion(flags_ & kMultiline ? kBolMultiline : kBol);
      case '$':
        ++i_;
        return assertion(flags_ & kMultiline ? kEolMultiline : kEol);
      case '\\':
        ++i_;
        return parse_escape();
      case '*':
      case '+':
      case '?':
      case '{':
        error("U_REGEX_RULE_SYNTAX");
      default:
        return char_node(next_cp());
    }
  }

  // Escape after '\' outside a class.
  NodePtr parse_escape() {
    if (at_end()) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
    char16_t c = peek();
    switch (c) {
      case 'b':
        ++i_;
        return assertion(kWordBoundary);
      case 'B':
        ++i_;
        return assertion(kNotWordBoundary);
      case 'A':
        ++i_;
        return assertion(kStartInput);
      case 'z':
        ++i_;
        return assertion(kEndInput);
      case 'Z':
        ++i_;
        return assertion(kEndInputOrTerminator);
      case 'G':
        ++i_;
        return assertion(kPreviousMatchEnd);
      case 'Q': {
        ++i_;
        NodePtr seq = node(Kind::kConcat);
        while (!at_end()) {
          if (peek() == '\\' && peek(1) == 'E') {
            i_ += 2;
            break;
          }
          seq->kids.push_back(char_node(next_cp()));
        }
        return seq;
      }
      case 'E':  // \E without \Q: ignored, as ICU
        ++i_;
        return nullptr;
      case 'R': {  // (?>\r\n|[\n\x0B\f\r\x85  ])
        ++i_;
        NodePtr crlf = node(Kind::kConcat);
        crlf->kids.push_back(raw_char('\r'));
        crlf->kids.push_back(raw_char('\n'));
        NodePtr alt = node(Kind::kAlt);
        alt->kids.push_back(std::move(crlf));
        alt->kids.push_back(raw_set(vspace_set()));
        NodePtr atomic = node(Kind::kAtomic);
        atomic->kids.push_back(std::move(alt));
        return atomic;
      }
      case 'X': {  // grapheme cluster, approximated: (?>\r\n|\P{M}\p{M}*)
        ++i_;
        NodePtr crlf = node(Kind::kConcat);
        crlf->kids.push_back(raw_char('\r'));
        crlf->kids.push_back(raw_char('\n'));
        NodePtr base = node(Kind::kConcat);
        NodePtr not_mark = raw_set(mark_ranges());
        prog_.sets[size_t(not_mark->value)].negated = true;
        base->kids.push_back(std::move(not_mark));
        NodePtr marks = node(Kind::kRepeat);
        marks->kids.push_back(raw_set(mark_ranges()));
        base->kids.push_back(std::move(marks));
        NodePtr alt = node(Kind::kAlt);
        alt->kids.push_back(std::move(crlf));
        alt->kids.push_back(std::move(base));
        NodePtr atomic = node(Kind::kAtomic);
        atomic->kids.push_back(std::move(alt));
        return atomic;
      }
      case 'k': {
        ++i_;
        if (!eat('<')) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
        std::u16string name = parse_name();
        if (!eat('>')) error("U_REGEX_INVALID_CAPTURE_GROUP_NAME");
        for (const auto& [n, g] : prog_.names) {
          if (n == name) return backref(g);
        }
        error("U_REGEX_INVALID_CAPTURE_GROUP_NAME");
      }
      default:
        break;
    }
    if (c >= '1' && c <= '9') {
      // As ICU: as many digits as keep the number within the groups seen so far.
      int32_t g = 0;
      while (!at_end() && peek() >= '0' && peek() <= '9') {
        int32_t next = g * 10 + (peek() - '0');
        if (g != 0 && next > prog_.groups) break;
        g = next;
        ++i_;
        if (g > prog_.groups) break;
      }
      backrefs_.push_back(g);
      return backref(g);
    }
    bool negated = false;
    if (std::optional<Ranges> r = class_escape(negated)) return set_node(std::move(*r), negated);
    return char_node(char_escape());
  }

  NodePtr backref(int32_t g) {
    NodePtr n = node(Kind::kBackref);
    n->value = g;
    n->fold = (flags_ & kCaseInsensitive) != 0;
    return n;
  }

  NodePtr raw_char(char32_t c) {
    NodePtr n = node(Kind::kChar);
    n->ch = c;
    return n;
  }
  NodePtr raw_set(const Ranges& r) {
    NodePtr n = node(Kind::kSet);
    n->value = int32_t(prog_.sets.size());
    prog_.sets.push_back({r, false});
    return n;
  }

  // \d \D \s \S \w \W \h \H \v \V \p{..} \P{..}: the set, or nullopt when the
  // escape is not a class (the cursor is then unchanged).
  std::optional<Ranges> class_escape(bool& negated) {
    char16_t c = peek();
    const Ranges* base = nullptr;
    switch (c) {
      case 'd':
      case 'D':
        base = &digit_set();
        break;
      case 's':
      case 'S':
        base = &space_set();
        break;
      case 'w':
      case 'W':
        base = &word_chars();
        break;
      case 'h':
      case 'H':
        base = &hspace_set();
        break;
      case 'v':
      case 'V':
        base = &vspace_set();
        break;
      case 'p':
      case 'P': {
        ++i_;
        negated = c == 'P';
        size_t at = i_;
        std::u16string_view name;
        if (eat('{')) {
          size_t close = p_.find(u'}', i_);
          if (close == std::u16string_view::npos) error("U_REGEX_PROPERTY_SYNTAX");
          name = p_.substr(i_, close - i_);
          i_ = close + 1;
        } else {
          if (at_end()) error("U_REGEX_PROPERTY_SYNTAX");
          name = p_.substr(i_, 1);
          ++i_;
        }
        if (!name.empty() && name[0] == '^') {
          negated = !negated;
          name.remove_prefix(1);
        }
        return property(name, int32_t(at));
      }
      default:
        return std::nullopt;
    }
    ++i_;
    negated = c >= 'A' && c <= 'Z';
    return *base;
  }

  int32_t hex_digit(char16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  char32_t hex(int digits) {
    char32_t v = 0;
    for (int k = 0; k < digits; ++k) {
      int32_t d = at_end() ? -1 : hex_digit(peek());
      if (d < 0) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
      v = v * 16 + char32_t(d);
      ++i_;
    }
    return v;
  }

  // A character escape (the '\' already read).
  char32_t char_escape() {
    if (at_end()) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
    char16_t c = peek();
    ++i_;
    switch (c) {
      case 't':
        return '\t';
      case 'n':
        return '\n';
      case 'r':
        return '\r';
      case 'f':
        return '\f';
      case 'a':
        return 0x7;
      case 'e':
        return 0x1B;
      case 'c': {
        if (at_end()) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
        return char32_t(p_[i_++]) ^ 0x40;
      }
      case '0': {  // \0ooo: up to three octal digits
        char32_t v = 0;
        int n = 0;
        while (n < 3 && !at_end() && peek() >= '0' && peek() <= '7') {
          char32_t next = v * 8 + char32_t(peek() - '0');
          if (next > 0xFF) break;
          v = next;
          ++i_;
          ++n;
        }
        if (n == 0) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
        return v;
      }
      case 'x': {
        if (eat('{')) {
          char32_t v = 0;
          int n = 0;
          while (!at_end() && hex_digit(peek()) >= 0) {
            v = v * 16 + char32_t(hex_digit(peek()));
            if (v > kMaxCodePoint) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
            ++i_;
            ++n;
          }
          if (n == 0 || !eat('}')) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
          return v;
        }
        return hex(2);
      }
      case 'u':
        return hex(4);
      case 'U': {
        char32_t v = hex(8);
        if (v > kMaxCodePoint) error("U_REGEX_BAD_ESCAPE_SEQUENCE");
        return v;
      }
      case 'N':
        error("U_REGEX_BAD_ESCAPE_SEQUENCE");  // \N{NAME}: character names are not available
      default:
        break;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      --i_;
      error("U_REGEX_BAD_ESCAPE_SEQUENCE");
    }
    --i_;
    return next_cp();
  }

  // A class, after '[': operands joined by && (intersection) and --
  // (difference), each the union of its items.
  Ranges parse_class(int depth, bool& negated) {
    if (depth > kMaxNesting) error("U_REGEX_PATTERN_TOO_BIG");
    negated = eat('^');
    Ranges acc = class_operand(depth);
    for (;;) {
      class_skip();
      if (peek() == '&' && peek(1) == '&') {
        i_ += 2;
        acc = intersect(acc, class_operand(depth));
      } else if (peek() == '-' && peek(1) == '-') {
        i_ += 2;
        acc = subtract(acc, class_operand(depth));
      } else {
        break;
      }
    }
    if (!eat(']')) error("U_REGEX_MISSING_CLOSE_BRACKET");
    return acc;
  }

  void class_skip() {
    if (!(flags_ & kComments)) return;
    while (!at_end() && (peek() == ' ' || (peek() >= 0x9 && peek() <= 0xD))) ++i_;
  }

  // One operand of a class: the union of its items (nested classes,
  // [:posix:], class escapes, characters and ranges) up to ']', '&&' or '--'.
  Ranges class_operand(int depth) {
    Ranges acc;
    // A ']' right after '[' or '[^' is a literal (as in Java and ICU).
    if (peek() == ']' && (p_[i_ - 1] == '[' || (p_[i_ - 1] == '^' && i_ >= 2 && p_[i_ - 2] == '['))) {
      ++i_;
      acc = unite(acc, range_from(']'));
    }
    for (;;) {
      class_skip();
      if (at_end()) error("U_REGEX_MISSING_CLOSE_BRACKET");
      char16_t c = peek();
      if (c == ']') break;
      if ((c == '&' && peek(1) == '&') || (c == '-' && peek(1) == '-')) break;
      if (c == '[') {
        if (peek(1) == ':') {
          size_t close = p_.find(u":]", i_ + 2);
          if (close == std::u16string_view::npos) error("U_REGEX_MISSING_CLOSE_BRACKET");
          std::u16string_view name = p_.substr(i_ + 2, close - i_ - 2);
          bool neg = !name.empty() && name[0] == '^';
          if (neg) name.remove_prefix(1);
          Ranges r = property(name, int32_t(i_));
          acc = unite(acc, neg ? complement(r) : r);
          i_ = close + 2;
          continue;
        }
        ++i_;
        bool neg = false;
        Ranges r = parse_class(depth + 1, neg);
        acc = unite(acc, neg ? complement(r) : r);
        continue;
      }
      if (c == '\\') {
        ++i_;
        if (peek() == 'Q') {  // \Q...\E inside a class: literal characters
          ++i_;
          while (!at_end() && !(peek() == '\\' && peek(1) == 'E')) {
            char32_t q = next_cp();
            acc = unite(acc, {{q, q}});
          }
          if (!at_end()) i_ += 2;
          continue;
        }
        bool neg = false;
        if (std::optional<Ranges> r = class_escape(neg)) {
          acc = unite(acc, neg ? complement(*r) : *r);
          continue;
        }
        char32_t lo = char_escape();
        acc = unite(acc, range_from(lo));
        continue;
      }
      char32_t lo = next_cp();
      acc = unite(acc, range_from(lo));
    }
    return acc;
  }

  // `lo` read: "lo-hi" or just "lo".
  Ranges range_from(char32_t lo) {
    if (peek() == '-' && peek(1) != ']' && peek(1) != '-' && peek(1) != 0 && peek(1) != '[') {
      ++i_;
      char32_t hi;
      if (peek() == '\\') {
        ++i_;
        hi = char_escape();
      } else {
        hi = next_cp();
      }
      if (hi < lo) error("U_REGEX_INVALID_RANGE");
      return {{lo, hi}};
    }
    return {{lo, lo}};
  }

  std::u16string_view p_;
  size_t i_ = 0;
  uint32_t flags_;
  Program& prog_;
  std::vector<int32_t> backrefs_;
};

// --- Compiler: syntax tree -> program -------------------------------------------------------------

struct Lengths {
  int64_t min = 0, max = 0;  // in UTF-16 units; max < 0: unbounded
};

Lengths lengths(const Node& n, const Program& prog) {
  switch (n.kind) {
    case Kind::kEmpty:
    case Kind::kAssert:
    case Kind::kLook:
      return {0, 0};
    case Kind::kChar:
      return {n.ch > 0xFFFF ? 2 : 1, n.ch > 0xFFFF ? 2 : 1};
    case Kind::kAny:
    case Kind::kSet:
      return {1, 2};
    case Kind::kBackref:
      return {0, -1};
    case Kind::kGroup:
    case Kind::kAtomic:
      return n.kids.empty() ? Lengths{} : lengths(*n.kids[0], prog);
    case Kind::kConcat: {
      Lengths t;
      for (const auto& k : n.kids) {
        Lengths l = lengths(*k, prog);
        t.min += l.min;
        t.max = (t.max < 0 || l.max < 0) ? -1 : t.max + l.max;
      }
      return t;
    }
    case Kind::kAlt: {
      Lengths t{INT64_MAX, 0};
      for (const auto& k : n.kids) {
        Lengths l = lengths(*k, prog);
        t.min = std::min(t.min, l.min);
        t.max = (t.max < 0 || l.max < 0) ? -1 : std::max(t.max, l.max);
      }
      return t;
    }
    case Kind::kRepeat: {
      Lengths l = lengths(*n.kids[0], prog);
      Lengths t;
      t.min = std::min<int64_t>(l.min * n.min, INT32_MAX);
      t.max = (n.max < 0 || l.max < 0) ? -1 : std::min<int64_t>(l.max * n.max, INT32_MAX);
      return t;
    }
  }
  return {};
}

class Compiler {
 public:
  explicit Compiler(Program& prog) : prog_(prog) {}

  void compile(const Node& root) {
    emit({Op::kSave, 0, 0, 0});
    gen(root);
    emit({Op::kSave, 0, 0, 1});
    emit({Op::kSucceed});
  }

 private:
  int32_t pc() const { return int32_t(prog_.code.size()); }
  int32_t emit(Inst in) {
    if (prog_.code.size() >= kMaxProgram) throw SyntaxError("U_REGEX_PATTERN_TOO_BIG", 0);
    prog_.code.push_back(in);
    return pc() - 1;
  }
  Inst& at(int32_t i) { return prog_.code[size_t(i)]; }

  static bool single_item(const Node& n) {
    return n.kind == Kind::kChar || n.kind == Kind::kAny || n.kind == Kind::kSet;
  }

  void gen_item(const Node& n) {
    switch (n.kind) {
      case Kind::kChar: {
        Inst in{Op::kChar};
        in.ch = n.ch;
        in.f = n.fold ? kFoldFlag : 0;
        emit(in);
        break;
      }
      case Kind::kAny: {
        Inst in{Op::kAny};
        in.f = n.any;
        emit(in);
        break;
      }
      case Kind::kSet: {
        Inst in{Op::kSet};
        in.a = n.value;
        in.f = n.fold ? kFoldFlag : 0;
        emit(in);
        break;
      }
      default:
        break;
    }
  }

  void gen(const Node& n) {
    switch (n.kind) {
      case Kind::kEmpty:
        break;
      case Kind::kChar:
      case Kind::kAny:
      case Kind::kSet:
        gen_item(n);
        break;
      case Kind::kAssert: {
        Inst in{Op::kAssert};
        in.a = n.value;
        emit(in);
        break;
      }
      case Kind::kGroup:
        if (n.value >= 0) {
          Inst s{Op::kSave};
          s.a = 2 * n.value;
          emit(s);
          gen(*n.kids[0]);
          s.a = 2 * n.value + 1;
          emit(s);
        } else {
          gen(*n.kids[0]);
        }
        break;
      case Kind::kConcat:
        for (const auto& k : n.kids) gen(*k);
        break;
      case Kind::kAlt: {
        std::vector<int32_t> jumps;
        for (size_t k = 0; k < n.kids.size(); ++k) {
          if (k + 1 < n.kids.size()) {
            int32_t split = emit({Op::kSplit});
            at(split).a = pc();
            gen(*n.kids[k]);
            jumps.push_back(emit({Op::kJmp}));
            at(split).b = pc();
          } else {
            gen(*n.kids[k]);
          }
        }
        for (int32_t j : jumps) at(j).a = pc();
        break;
      }
      case Kind::kBackref: {
        Inst in{Op::kBackref};
        in.a = n.value;
        in.f = n.fold ? kFoldFlag : 0;
        emit(in);
        break;
      }
      case Kind::kAtomic: {
        int32_t start = emit({Op::kAtomic});
        at(start).a = pc();
        gen(*n.kids[0]);
        emit({Op::kSucceed});
        at(start).b = pc();
        break;
      }
      case Kind::kLook: {
        int32_t start = emit({Op::kLook});
        uint8_t f = uint8_t((n.behind ? kBehind : 0) | (n.negative ? kNegative : 0));
        at(start).f = f;
        if (n.behind) {
          Lengths l = lengths(*n.kids[0], prog_);
          if (l.max < 0) throw SyntaxError("U_REGEX_LOOK_BEHIND_LIMIT", 0);
          at(start).c = int32_t(l.min);
          at(start).d = int32_t(l.max);
        }
        at(start).a = pc();
        gen(*n.kids[0]);
        emit({Op::kSucceed});
        at(start).b = pc();
        break;
      }
      case Kind::kRepeat:
        gen_repeat(n);
        break;
    }
  }

  void gen_repeat(const Node& n) {
    const Node& body = *n.kids[0];
    if (n.min == 1 && n.max == 1) {
      gen(body);
      return;
    }
    if (n.max == 0) return;
    if (single_item(body)) {
      Inst in{Op::kRepeat};
      in.a = n.min;
      in.b = n.max;
      in.c = n.greed;
      emit(in);
      gen_item(body);
      return;
    }
    if (n.greed == kPossessive) {
      Node greedy(Kind::kRepeat);
      greedy.min = n.min;
      greedy.max = n.max;
      int32_t start = emit({Op::kAtomic});
      at(start).a = pc();
      gen_loop(greedy, body);
      emit({Op::kSucceed});
      at(start).b = pc();
      return;
    }
    gen_loop(n, body);
  }

  // Counted loop with an empty-iteration guard:
  //   LoopInit r; H: LoopHead r (exit E); body; LoopTail r H; E:
  void gen_loop(const Node& n, const Node& body) {
    int32_t reg = prog_.loops++;
    Inst init{Op::kLoopInit};
    init.a = reg;
    emit(init);
    Inst head{Op::kLoopHead};
    head.a = reg;
    head.b = n.min;
    head.c = n.max;
    head.d = n.greed;
    int32_t h = emit(head);
    gen(body);
    Inst tail{Op::kLoopTail};
    tail.a = reg;
    tail.b = h;
    emit(tail);
    at(h).e = pc();
  }

  Program& prog_;
};

// --- Matching engine ---------------------------------------------------------------------------------

struct Frame {
  enum Kind : uint8_t { kBranch, kSlotUndo, kRegUndo, kRepeatGreedy, kRepeatLazy, kLazyLoop };
  Kind kind;
  int32_t pc;
  int32_t pos;
  int32_t a;
  int32_t b;
};

class Engine {
 public:
  Engine(const Program& prog, const std::u16string& text, int32_t region_start, int32_t region_end, bool anchoring,
         bool transparent, int32_t previous_end, uint64_t max_steps, size_t max_stack)
      : prog_(prog),
        text_(text),
        region_lo_(region_start),
        region_hi_(region_end),
        anchor_lo_(anchoring ? region_start : 0),
        anchor_hi_(anchoring ? region_end : int32_t(text.size())),
        look_lo_(transparent ? 0 : region_start),
        look_hi_(transparent ? int32_t(text.size()) : region_end),
        previous_end_(previous_end),
        max_steps_(max_steps),
        max_stack_(max_stack) {
    lo_ = region_lo_;
    hi_ = region_hi_;
  }

  // One attempt at `start`: the end of the match, or -1. Group offsets in slots().
  int32_t attempt(int32_t start, int32_t must_end) {
    slots_.assign(size_t(2 * (prog_.groups + 1)), -1);
    regs_.assign(size_t(2 * prog_.loops), 0);
    stack_.clear();
    require_end_ = false;
    return run(0, start, must_end);
  }

  const std::vector<int32_t>& slots() const { return slots_; }
  bool hit_end() const { return hit_end_; }
  bool require_end() const { return require_end_; }
  void note_hit_end() { hit_end_ = true; }

 private:
  // Code point at pos (pos < hi_), and the index after it.
  char32_t cp_at(int32_t pos, int32_t& next) const {
    char16_t c = text_[size_t(pos)];
    if (c >= 0xD800 && c <= 0xDBFF && pos + 1 < hi_) {
      char16_t d = text_[size_t(pos) + 1];
      if (d >= 0xDC00 && d <= 0xDFFF) {
        next = pos + 2;
        return 0x10000 + ((char32_t(c) - 0xD800) << 10) + (d - 0xDC00);
      }
    }
    next = pos + 1;
    return c;
  }
  // Code point before pos (pos > lo), and its start.
  char32_t cp_before(int32_t pos, int32_t lo, int32_t& start) const {
    char16_t c = text_[size_t(pos) - 1];
    if (c >= 0xDC00 && c <= 0xDFFF && pos - 2 >= lo) {
      char16_t h = text_[size_t(pos) - 2];
      if (h >= 0xD800 && h <= 0xDBFF) {
        start = pos - 2;
        return 0x10000 + ((char32_t(h) - 0xD800) << 10) + (c - 0xDC00);
      }
    }
    start = pos - 1;
    return c;
  }

  bool set_has(const Inst& in, char32_t c) const {
    const CharSet& s = prog_.sets[size_t(in.a)];
    bool found = contains(s.ranges, c);
    if (!found && (in.f & kFoldFlag)) {
      found = contains(s.ranges, to_lower(c)) || contains(s.ranges, to_upper(c)) || contains(s.ranges, fold(c));
    }
    return found != s.negated;
  }

  // Matches the single item at pos: the next position, or -1.
  int32_t step_item(const Inst& in, int32_t pos) {
    if (pos >= hi_) {
      hit_end_ = true;
      return -1;
    }
    int32_t next;
    char32_t c = cp_at(pos, next);
    switch (in.op) {
      case Op::kChar:
        return ((in.f & kFoldFlag) ? fold(c) : c) == in.ch ? next : -1;
      case Op::kAny:
        if (!(in.f & kDotAll) && is_line_terminator(c, in.f & kUnixLines)) return -1;
        return next;
      case Op::kSet:
        return set_has(in, c) ? next : -1;
      default:
        return -1;
    }
  }

  bool is_word_at(int32_t pos) const {
    if (pos < look_lo_ || pos >= look_hi_) return false;
    char16_t c = text_[size_t(pos)];
    char32_t cp = c;
    if (c >= 0xD800 && c <= 0xDBFF && pos + 1 < look_hi_) {
      char16_t d = text_[size_t(pos) + 1];
      if (d >= 0xDC00 && d <= 0xDFFF) cp = 0x10000 + ((char32_t(c) - 0xD800) << 10) + (d - 0xDC00);
    }
    return contains(word_chars(), cp);
  }
  bool is_word_before(int32_t pos) const {
    if (pos <= look_lo_ || pos > look_hi_) return false;
    int32_t start;
    return contains(word_chars(), cp_before(pos, look_lo_, start));
  }

  // pos is at a line terminator that ends the input ("\r\n" counting as one).
  bool final_terminator(int32_t pos) const {
    if (pos >= anchor_hi_) return false;
    char16_t c = text_[size_t(pos)];
    if (!is_line_terminator(c, prog_.unix_lines)) return false;
    if (prog_.unix_lines) return pos + 1 == anchor_hi_;
    if (c == '\r' && pos + 2 == anchor_hi_ && text_[size_t(pos) + 1] == '\n') return true;
    if (c == '\n' && pos > anchor_lo_ && text_[size_t(pos) - 1] == '\r') return false;  // not inside "\r\n"
    return pos + 1 == anchor_hi_;
  }

  bool check_assert(int32_t kind, int32_t pos) {
    bool unix = prog_.unix_lines;
    switch (kind) {
      case kBol:
      case kStartInput:
        return pos == anchor_lo_;
      case kBolMultiline: {
        if (pos >= anchor_hi_) {  // never at the end of the input, even an empty one
          hit_end_ = true;
          return false;
        }
        if (pos == anchor_lo_) return true;
        if (pos <= 0) return false;
        char16_t prev = text_[size_t(pos) - 1];
        if (!is_line_terminator(prev, unix)) return false;
        return !(prev == '\r' && text_[size_t(pos)] == '\n' && !unix);
      }
      case kEol:
      case kEndInputOrTerminator:
        if (pos >= anchor_hi_) {
          hit_end_ = require_end_ = true;
          return pos == anchor_hi_;
        }
        if (final_terminator(pos)) {
          hit_end_ = require_end_ = true;
          return true;
        }
        return false;
      case kEolMultiline: {
        if (pos >= anchor_hi_) {
          hit_end_ = require_end_ = true;
          return true;
        }
        char16_t c = text_[size_t(pos)];
        if (!is_line_terminator(c, unix)) return false;
        // Not between "\r" and "\n".
        return !(c == '\n' && pos > 0 && text_[size_t(pos) - 1] == '\r' && !unix);
      }
      case kEndInput:
        if (pos >= anchor_hi_) {
          hit_end_ = require_end_ = true;
          return true;
        }
        return false;
      case kWordBoundary:
      case kNotWordBoundary: {
        if (pos >= look_hi_) hit_end_ = require_end_ = true;
        bool boundary = is_word_before(pos) != is_word_at(pos);
        return boundary == (kind == kWordBoundary);
      }
      case kPreviousMatchEnd:
        return pos == (previous_end_ >= 0 ? previous_end_ : region_lo_);
      default:
        return false;
    }
  }

  void push(Frame f) {
    if (stack_.size() >= max_stack_) throw ResourceError("pile de retour de l'expression régulière épuisée");
    stack_.push_back(f);
  }
  void set_slot(int32_t slot, int32_t value) {
    push({Frame::kSlotUndo, 0, 0, slot, slots_[size_t(slot)]});
    slots_[size_t(slot)] = value;
  }
  void set_reg(int32_t reg, int32_t value) {
    push({Frame::kRegUndo, 0, 0, reg, regs_[size_t(reg)]});
    regs_[size_t(reg)] = value;
  }

  // Keeps only the undo records above `mark` (a sub-match's alternatives are dropped).
  void keep_undo(size_t mark) {
    size_t w = mark;
    for (size_t r = mark; r < stack_.size(); ++r) {
      if (stack_[r].kind == Frame::kSlotUndo || stack_[r].kind == Frame::kRegUndo) stack_[w++] = stack_[r];
    }
    stack_.resize(w);
  }
  // Undoes everything above `mark`.
  void unwind(size_t mark) {
    while (stack_.size() > mark) {
      const Frame& f = stack_.back();
      if (f.kind == Frame::kSlotUndo) slots_[size_t(f.a)] = f.b;
      if (f.kind == Frame::kRegUndo) regs_[size_t(f.a)] = f.b;
      stack_.pop_back();
    }
  }

  // Pops frames above `base` until one gives an alternative (pc, pos).
  bool backtrack(size_t base, int32_t& pc, int32_t& pos) {
    while (stack_.size() > base) {
      Frame f = stack_.back();
      stack_.pop_back();
      switch (f.kind) {
        case Frame::kSlotUndo:
          slots_[size_t(f.a)] = f.b;
          break;
        case Frame::kRegUndo:
          regs_[size_t(f.a)] = f.b;
          break;
        case Frame::kBranch:
          pc = f.pc;
          pos = f.pos;
          return true;
        case Frame::kRepeatGreedy: {
          // One item fewer: step back one code point.
          int32_t start;
          cp_before(f.pos, f.a, start);
          if (start > f.a) push({Frame::kRepeatGreedy, f.pc, start, f.a, 0});
          pc = f.pc;
          pos = start;
          return true;
        }
        case Frame::kRepeatLazy: {
          // One item more.
          const Inst& rep = prog_.code[size_t(f.b)];
          if (rep.b >= 0 && f.a >= rep.b) break;
          int32_t next = step_item(prog_.code[size_t(f.b) + 1], f.pos);
          if (next < 0) break;
          push({Frame::kRepeatLazy, f.pc, next, f.a + 1, f.b});
          pc = f.pc;
          pos = next;
          return true;
        }
        case Frame::kLazyLoop: {
          // Enter the body of the loop whose head is f.pc.
          set_reg(2 * prog_.code[size_t(f.pc)].a + 1, f.pos);
          pc = f.pc + 1;
          pos = f.pos;
          return true;
        }
      }
    }
    return false;
  }

  int32_t run(int32_t pc, int32_t pos, int32_t must_end) {
    size_t base = stack_.size();
    for (;;) {
      if (++steps_ > max_steps_) throw ResourceError("expression régulière trop coûteuse");
      const Inst& in = prog_.code[size_t(pc)];
      bool ok = true;
      switch (in.op) {
        case Op::kChar:
        case Op::kAny:
        case Op::kSet: {
          int32_t next = step_item(in, pos);
          if (next < 0) {
            ok = false;
          } else {
            pos = next;
            ++pc;
          }
          break;
        }
        case Op::kAssert:
          if (check_assert(in.a, pos)) {
            ++pc;
          } else {
            ok = false;
          }
          break;
        case Op::kSave:
          set_slot(in.a, pos);
          ++pc;
          break;
        case Op::kSplit:
          push({Frame::kBranch, in.b, pos, 0, 0});
          pc = in.a;
          break;
        case Op::kJmp:
          pc = in.a;
          break;
        case Op::kBackref: {
          int32_t s = in.a <= prog_.groups ? slots_[size_t(2 * in.a)] : -1;
          int32_t e = in.a <= prog_.groups ? slots_[size_t(2 * in.a + 1)] : -1;
          if (s < 0 || e < 0) {
            ok = false;
            break;
          }
          int32_t p = pos;
          for (int32_t k = s; k < e && ok;) {
            if (p >= hi_) {
              hit_end_ = true;
              ok = false;
              break;
            }
            int32_t kn, pn;
            char32_t want = cp_at(k, kn), got = cp_at(p, pn);
            if ((in.f & kFoldFlag) ? fold(want) != fold(got) : want != got) ok = false;
            k = kn;
            p = pn;
          }
          if (ok) {
            pos = p;
            ++pc;
          }
          break;
        }
        case Op::kRepeat: {
          const Inst& item = prog_.code[size_t(pc) + 1];
          int32_t count = 0, p = pos, min_pos = pos;
          if (in.c == kLazy) {
            for (; count < in.a; ++count) {
              int32_t next = step_item(item, p);
              if (next < 0) break;
              p = next;
            }
            if (count < in.a) {
              ok = false;
              break;
            }
            push({Frame::kRepeatLazy, pc + 2, p, count, pc});
            pos = p;
            pc += 2;
            break;
          }
          while (in.b < 0 || count < in.b) {
            int32_t next = step_item(item, p);
            if (next < 0) break;
            p = next;
            if (++count == in.a) min_pos = p;
          }
          if (count < in.a) {
            ok = false;
            break;
          }
          if (in.a == 0) min_pos = pos;
          if (in.c == kGreedy && p > min_pos) push({Frame::kRepeatGreedy, pc + 2, p, min_pos, 0});
          pos = p;
          pc += 2;
          break;
        }
        case Op::kLoopInit:
          set_reg(2 * in.a, 0);
          ++pc;
          break;
        case Op::kLoopHead: {
          int32_t count = regs_[size_t(2 * in.a)];
          if (count < in.b) {
            set_reg(2 * in.a + 1, pos);
            ++pc;
          } else if (in.c >= 0 && count >= in.c) {
            pc = in.e;
          } else if (in.d == kLazy) {
            push({Frame::kLazyLoop, pc, pos, 0, 0});
            pc = in.e;
          } else {
            push({Frame::kBranch, in.e, pos, 0, 0});
            set_reg(2 * in.a + 1, pos);
            ++pc;
          }
          break;
        }
        case Op::kLoopTail: {
          const Inst& head = prog_.code[size_t(in.b)];
          int32_t count = regs_[size_t(2 * in.a)] + 1;
          set_reg(2 * in.a, count);
          // An empty iteration ends the loop (it would repeat forever).
          pc = pos == regs_[size_t(2 * in.a + 1)] ? head.e : in.b;
          break;
        }
        case Op::kAtomic: {
          size_t mark = stack_.size();
          int32_t end = run(in.a, pos, -1);
          if (end < 0) {
            ok = false;
            break;
          }
          keep_undo(mark);
          pos = end;
          pc = in.b;
          break;
        }
        case Op::kLook: {
          size_t mark = stack_.size();
          int32_t saved_lo = lo_, saved_hi = hi_;
          lo_ = look_lo_;
          hi_ = look_hi_;
          bool found = false;
          if (!(in.f & kBehind)) {
            found = run(in.a, pos, -1) >= 0;
          } else {
            for (int32_t l = in.c; l <= in.d && !found; ++l) {
              int32_t s = pos - l;
              if (s < lo_) break;
              found = run(in.a, s, pos) >= 0;
            }
          }
          lo_ = saved_lo;
          hi_ = saved_hi;
          if (in.f & kNegative) {
            if (found) {
              unwind(mark);
              ok = false;
            } else {
              pc = in.b;
            }
          } else if (found) {
            keep_undo(mark);
            pc = in.b;
          } else {
            ok = false;
          }
          break;
        }
        case Op::kSucceed:
          if (must_end >= 0 && pos != must_end) {
            ok = false;
            break;
          }
          return pos;
      }
      if (!ok && !backtrack(base, pc, pos)) return -1;
    }
  }

  const Program& prog_;
  const std::u16string& text_;
  int32_t region_lo_, region_hi_;
  int32_t anchor_lo_, anchor_hi_;
  int32_t look_lo_, look_hi_;
  int32_t lo_ = 0, hi_ = 0;  // current reading bounds
  int32_t previous_end_;
  uint64_t max_steps_;
  size_t max_stack_;
  uint64_t steps_ = 0;
  std::vector<int32_t> slots_;
  std::vector<int32_t> regs_;
  std::vector<Frame> stack_;
  bool hit_end_ = false, require_end_ = false;
};

}  // namespace

// --- Pattern -------------------------------------------------------------------------------------------

Pattern::~Pattern() = default;

std::shared_ptr<const Pattern> Pattern::compile(std::u16string_view pattern, uint32_t flags) {
  auto prog = std::make_unique<Program>();
  prog->unix_lines = (flags & kUnixLines) != 0;
  Parser parser(pattern, flags, *prog);
  NodePtr root = parser.parse();
  Compiler(*prog).compile(*root);
  std::shared_ptr<Pattern> p(new Pattern());
  p->program_ = std::move(prog);
  return p;
}

int32_t Pattern::group_count() const { return program_->groups; }

int32_t Pattern::group_index(std::u16string_view name) const {
  for (const auto& [n, g] : program_->names) {
    if (n == name) return g;
  }
  return -1;
}

// --- Matcher --------------------------------------------------------------------------------------------

Matcher::Matcher(std::shared_ptr<const Pattern> pattern) : pattern_(std::move(pattern)) {
  groups_.assign(size_t(2 * (pattern_->group_count() + 1)), -1);
}

void Matcher::set_input(std::u16string text, int32_t start, int32_t end) {
  text_ = std::move(text);
  int32_t len = int32_t(text_.size());
  region_start_ = std::clamp(start, 0, len);
  region_end_ = std::clamp(end, region_start_, len);
  last_end_ = -1;
  last_empty_ = false;
  exhausted_ = false;
  hit_end_ = require_end_ = false;
  std::fill(groups_.begin(), groups_.end(), -1);
}

int32_t Matcher::group_count() const { return pattern_->group_count(); }

bool Matcher::search(int32_t from, Mode mode) {
  const Program& prog = pattern_->program();
  const Inst& first = prog.code[1];
  // Anchored at the start (^ without MULTILINE, \A): a single attempt, at the
  // start of the region. Later find() calls fail at once, leaving hitEnd as
  // the last match set it (ICU's START_START patterns).
  bool anchored = first.op == Op::kAssert && (first.a == kBol || first.a == kStartInput);
  int32_t anchor = anchoring_bounds_ ? region_start_ : 0;
  if (mode == Mode::kFind && anchored && from != anchor) {
    std::fill(groups_.begin(), groups_.end(), -1);
    return false;
  }
  Engine engine(prog, text_, region_start_, region_end_, anchoring_bounds_, transparent_bounds_, last_end_, max_steps_,
                max_stack_);
  std::fill(groups_.begin(), groups_.end(), -1);
  hit_end_ = require_end_ = false;
  auto accept = [&] {
    groups_ = engine.slots();
    require_end_ = engine.require_end();
    hit_end_ = engine.hit_end();
    return true;
  };
  if (mode != Mode::kFind || anchored) {
    int32_t end = engine.attempt(mode == Mode::kFind ? from : region_start_, mode == Mode::kMatches ? region_end_ : -1);
    hit_end_ = engine.hit_end();
    return end >= 0 && accept();
  }
  // A pattern starting with a case-sensitive BMP character: skip the
  // positions where it cannot start.
  bool literal = first.op == Op::kChar && !(first.f & kFoldFlag) && first.ch <= 0xFFFF;
  for (int32_t start = from; start <= region_end_;) {
    if (literal) {
      while (start < region_end_ && text_[size_t(start)] != char16_t(first.ch)) ++start;
      if (start >= region_end_) break;
    }
    if (engine.attempt(start, -1) >= 0) return accept();
    if (start >= region_end_) break;
    char16_t c = text_[size_t(start)];
    start += (c >= 0xD800 && c <= 0xDBFF && start + 1 < region_end_ && text_[size_t(start) + 1] >= 0xDC00 &&
              text_[size_t(start) + 1] <= 0xDFFF)
                 ? 2
                 : 1;
  }
  hit_end_ = true;  // the scan reached the end of the region
  return false;
}

bool Matcher::find(int32_t start) {
  region_start_ = 0;
  region_end_ = int32_t(text_.size());
  last_end_ = -1;
  exhausted_ = false;
  bool found = search(std::clamp(start, 0, region_end_), Mode::kFind);
  if (found) {
    last_end_ = groups_[1];
    last_empty_ = groups_[0] == groups_[1];
  } else {
    exhausted_ = true;
  }
  return found;
}

bool Matcher::find_next() {
  if (exhausted_) return false;
  int32_t from = region_start_;
  if (last_end_ >= 0) {
    from = last_end_;
    if (last_empty_) {
      if (from >= region_end_) {
        exhausted_ = true;
        hit_end_ = true;
        return false;
      }
      char16_t c = text_[size_t(from)];
      from += (c >= 0xD800 && c <= 0xDBFF && from + 1 < region_end_) ? 2 : 1;
    }
  }
  bool found = search(from, Mode::kFind);
  if (found) {
    last_end_ = groups_[1];
    last_empty_ = groups_[0] == groups_[1];
  } else {
    exhausted_ = true;
  }
  return found;
}

bool Matcher::matches() {
  bool found = search(region_start_, Mode::kMatches);
  if (found) {
    last_end_ = groups_[1];
    last_empty_ = groups_[0] == groups_[1];
  }
  return found;
}

bool Matcher::looking_at() {
  bool found = search(region_start_, Mode::kLookingAt);
  if (found) {
    last_end_ = groups_[1];
    last_empty_ = groups_[0] == groups_[1];
  }
  return found;
}

}  // namespace aa::regex
