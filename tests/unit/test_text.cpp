// Text engine (ADR-007): fonts measured as HWUI's Skia measures them, on
// fonts built by font_builder.h.
#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "font_builder.h"
#include "test.h"
#include "text/coverage.h"
#include "text/font_collection.h"
#include "text/font_face.h"
#include "text/layout.h"
#include "text/locale.h"

namespace aa::text {
std::ostream& operator<<(std::ostream& os, const Rect& r) {
  return os << "[" << r.left << ", " << r.top << ", " << r.right << ", " << r.bottom << "]";
}
std::ostream& operator<<(std::ostream& os, const TextRect& r) {
  return os << "[" << r.left << ", " << r.top << ", " << r.right << ", " << r.bottom << "]";
}
std::ostream& operator<<(std::ostream& os, const Extent& e) { return os << "(" << e.ascent << ", " << e.descent << ")"; }
}  // namespace aa::text

using namespace aa;

namespace {

// Glyphs: .notdef, space, 'A' (advance 550), 'V' (600), 'x' (500, 480 high).
test::TestFont sample_font() {
  test::TestFont f;
  f.glyphs = {
      {0, 500, 50, 0, 450, 700},
      {U' ', 250, 0, 0, 0, 0},
      {U'A', 550, 10, 0, 540, 700},
      {U'V', 600, 0, 0, 600, 700},
      {U'x', 500, 20, 0, 480, 480},
  };
  f.kerning = {{{2, 3}, -80}};
  return f;
}

std::shared_ptr<text::Face> open_font(const test::TestFont& font) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(font.build());
  text::FontData data{bytes, bytes->data(), bytes->size()};
  std::string error;
  auto face = text::Face::open(data, 0, {}, &error);
  if (!face) throw test::Failure("police refusée : " + error);
  return face;
}

text::FontSpec spec(float size, text::Hinting hinting = text::Hinting::kNormal) {
  text::FontSpec s;
  s.size = size;
  s.hinting = hinting;
  s.edging = text::Edging::kAntiAlias;
  return s;
}

// Linear advances go through FreeType's 16.16 arithmetic.
bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

float advance(const text::Face& face, const text::FontSpec& s, uint16_t glyph) {
  float a = 0;
  face.advances(s, std::span<const uint16_t>(&glyph, 1), std::span<float>(&a, 1));
  return a;
}

text::Rect bounds(const text::Face& face, const text::FontSpec& s, uint16_t glyph) {
  text::Rect r;
  face.bounds(s, std::span<const uint16_t>(&glyph, 1), std::span<text::Rect>(&r, 1));
  return r;
}

}  // namespace

TEST(text_face_reads_cmap_style_and_tables) {
  auto face = open_font(sample_font());
  CHECK_EQ(face->units_per_em(), 1000u);
  CHECK(face->scalable());
  CHECK(!face->color_bitmaps());
  CHECK_EQ(face->glyph(U'A'), 2u);
  CHECK_EQ(face->glyph(U'x'), 4u);
  CHECK_EQ(face->glyph(U'B'), 0u);
  int weight = 0;
  bool italic = true;
  CHECK(face->os2_style(&weight, &italic));
  CHECK_EQ(weight, 400);
  CHECK(!italic);
  CHECK_EQ(face->table(text::tag('h', 'e', 'a', 'd')).size(), size_t(54));
  CHECK(face->table(text::tag('G', 'P', 'O', 'S')).empty());
}

TEST(text_face_rejects_garbage) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(64, uint8_t(0x5A));
  std::string error;
  CHECK(!text::Face::open({bytes, bytes->data(), bytes->size()}, 0, {}, &error));
  CHECK(!error.empty());
  auto font = std::make_shared<std::vector<uint8_t>>(sample_font().build());
  CHECK(!text::Face::open({font, font->data(), font->size()}, 3, {}, &error));  // no such index
}

// SkScalerContext_FreeType::generateFontMetrics: hhea unless USE_TYPO_METRICS,
// scaled by the size, y downwards.
TEST(text_font_metrics_follow_skia) {
  auto face = open_font(sample_font());
  text::FontMetrics m;
  float spacing = face->metrics(spec(20), &m);
  CHECK_EQ(m.ascent, -800.0f / 1000.0f * 20.0f);
  CHECK_EQ(m.descent, 200.0f / 1000.0f * 20.0f);
  CHECK_EQ(m.leading, 0.0f);
  CHECK_EQ(spacing, m.descent - m.ascent + m.leading);
  CHECK_EQ(m.top, -700.0f / 1000.0f * 20.0f);  // head yMax
  CHECK_EQ(m.bottom, 0.0f);                     // head yMin
  CHECK_EQ(m.x_height, 500.0f / 1000.0f * 20.0f);
  CHECK_EQ(m.cap_height, 700.0f / 1000.0f * 20.0f);
  CHECK_EQ(m.underline_thickness, 50.0f / 1000.0f * 20.0f);
  // FreeType's underline_position is post's minus half the thickness.
  CHECK_EQ(m.underline_position, -float((-100 - 50 / 2) + 50 / 2) / 1000.0f * 20.0f);
  CHECK_EQ(m.strikeout_position, -250.0f / 1000.0f * 20.0f);
  CHECK(m.flags & text::FontMetrics::kUnderlinePositionValid);
  CHECK(!(m.flags & text::FontMetrics::kBoundsInvalid));

  test::TestFont typo = sample_font();
  typo.use_typo_metrics = true;
  auto face2 = open_font(typo);
  face2->metrics(spec(20), &m);
  CHECK_EQ(m.ascent, -750.0f / 1000.0f * 20.0f);
  CHECK_EQ(m.descent, 250.0f / 1000.0f * 20.0f);
  CHECK_EQ(m.leading, 100.0f / 1000.0f * 20.0f);
}

// Hinted (FT_LOAD_TARGET_NORMAL) advances land on whole pixels; without
// hinting, or with linear metrics, they are the linear ones.
TEST(text_advances_follow_skia_hinting) {
  auto face = open_font(sample_font());
  CHECK_EQ(advance(*face, spec(10), 2), 6.0f);  // 5.5 rounded by FreeType
  CHECK_EQ(advance(*face, spec(10, text::Hinting::kNone), 2), 5.5f);
  text::FontSpec linear = spec(10);
  linear.linear_metrics = true;
  CHECK_EQ(advance(*face, linear, 2), 5.5f);
  // Skewed text (fake italic) is never hinted.
  text::FontSpec skewed = spec(10);
  skewed.skew_x = -0.25f;
  CHECK_EQ(advance(*face, skewed, 2), 5.5f);
  // Horizontal scale goes into the FreeType size.
  text::FontSpec wide = spec(10, text::Hinting::kNone);
  wide.scale_x = 2;
  CHECK(near(advance(*face, wide, 2), 11.0f));
  // Aliased text is hinted for monochrome (FT_LOAD_TARGET_MONO).
  text::FontSpec bw = spec(10);
  bw.edging = text::Edging::kAlias;
  CHECK_EQ(advance(*face, bw, 2), 6.0f);
  CHECK_EQ(advance(*face, spec(10), 1), 3.0f);  // space: 2.5 rounded
}

// Beyond 256 pixels, Skia measures at 64 pixels without hinting and scales.
TEST(text_large_sizes_use_canonical_path_size) {
  auto face = open_font(sample_font());
  CHECK(near(advance(*face, spec(300), 2), 550.0f / 1000.0f * 300.0f));
  text::FontMetrics m;
  face->metrics(spec(300), &m);
  CHECK_EQ(m.ascent, (-800.0f / 1000.0f * 64.0f) * (300.0f / 64.0f));
}

// SkGlyph keeps rounded-out integer bounds.
TEST(text_glyph_bounds_are_rounded_out) {
  auto face = open_font(sample_font());
  text::Rect r = bounds(*face, spec(10, text::Hinting::kNone), 2);
  CHECK_EQ(r, (text::Rect{0, -7, 6, 0}));  // [0.1, 5.4] x [-7, 0]
  CHECK(bounds(*face, spec(10), 1).empty());  // space has no outline
  // Beyond 256 pixels the bounds are those at 64 pixels, rounded out, then
  // scaled: [0, 38.4] x [-44.8, 0] -> [0, 39] x [-45, 0], times 1000 / 64.
  r = bounds(*face, spec(1000, text::Hinting::kNone), 3);
  CHECK_EQ(r, (text::Rect{0, -45 * 15.625f, 39 * 15.625f, 0}));
}

TEST(text_metrics_are_zero_for_empty_size) {
  auto face = open_font(sample_font());
  CHECK_EQ(advance(*face, spec(0), 2), 0.0f);
  text::FontMetrics m;
  face->metrics(spec(0), &m);
  CHECK(std::isfinite(m.ascent));
}

// --- minikin layer: locales, coverage, collections, layout ---

namespace {

std::shared_ptr<const text::Font> make_font(const test::TestFont& tf) {
  return std::make_shared<const text::Font>(open_font(tf), text::FontStyle{}, text::kEmptyLocaleListId);
}

std::shared_ptr<const text::FontCollection> collection_of(std::vector<test::TestFont> fonts) {
  std::vector<std::shared_ptr<const text::FontFamily>> families;
  bool first = true;
  for (const auto& tf : fonts) {
    families.push_back(std::make_shared<const text::FontFamily>(
        text::kEmptyLocaleListId, text::FamilyVariant::kDefault, std::vector<std::shared_ptr<const text::Font>>{make_font(tf)},
        false, first, text::VariationFamilyType::kNone));
    first = false;
  }
  return std::make_shared<const text::FontCollection>(std::move(families));
}

test::TestFont only_b_font() {
  test::TestFont f;
  f.glyphs = {{0, 500, 0, 0, 0, 0}, {U'B', 700, 0, 0, 700, 700}};
  return f;
}

text::Paint paint_of(std::shared_ptr<const text::FontCollection> fc, float size) {
  text::Paint p;
  p.font = std::move(fc);
  p.size = size;
  p.flags.edging = text::Edging::kAntiAlias;
  return p;
}

}  // namespace

TEST(text_locale_parsing_follows_minikin) {
  text::Locale ja("ja-Jpan-JP");
  CHECK(ja.supported());
  CHECK_EQ(ja.str(), std::string("ja-Jpan-JP"));
  CHECK(ja.supports_script(text::tag('H', 'i', 'r', 'a')));
  CHECK(ja.supports_script(text::tag('H', 'a', 'n', 'i')));
  CHECK(!ja.supports_script(text::tag('L', 'a', 't', 'n')));
  CHECK(text::Locale("und-Zsye").emoji_style() == text::EmojiStyle::kEmoji);
  CHECK(text::Locale("en-US-u-em-text").emoji_style() == text::EmojiStyle::kText);
  CHECK_EQ(text::Locale("de-1901").str(), std::string("de-1901"));
  CHECK(!text::Locale("1234").supported());
  uint32_t a = text::locale_list_id("en-US,fr");
  CHECK_EQ(text::locale_list_id("en-US,fr"), a);
  CHECK_EQ(text::locale_list(a).size(), size_t(2));
  CHECK_EQ(text::locale_list_id(""), text::kEmptyLocaleListId);
  CHECK(text::locale_list(text::kEmptyLocaleListId).empty());
}

TEST(text_cmap_coverage_reads_format4) {
  auto face = open_font(sample_font());
  bool vs = true;
  text::Coverage c = text::cmap_coverage(face->table(text::tag('c', 'm', 'a', 'p')), &vs);
  CHECK(!vs);
  CHECK(c.contains(U'A'));
  CHECK(c.contains(U' '));
  CHECK(!c.contains(U'B'));
  CHECK_EQ(c.next(U'B'), uint32_t(U'V'));
  CHECK_EQ(c.length(), uint32_t(U'x') + 1);
  CHECK_EQ(c.next(U'y'), text::Coverage::kNotFound);
}

TEST(text_itemize_splits_by_coverage) {
  auto fc = collection_of({sample_font(), only_b_font()});
  auto runs = fc->itemize(u"AxB A", text::FontStyle{}, text::kEmptyLocaleListId, text::FamilyVariant::kDefault);
  CHECK_EQ(runs.size(), size_t(3));
  CHECK_EQ(runs[0].families[0], uint8_t(0));
  CHECK_EQ(runs[0].end, 2);
  CHECK_EQ(runs[1].families[0], uint8_t(1));
  CHECK_EQ(runs[1].start, 2);
  CHECK_EQ(runs[2].start, 3);
  // Unsupported characters go to the first family.
  runs = fc->itemize(u"一", text::FontStyle{}, text::kEmptyLocaleListId, text::FamilyVariant::kDefault);
  CHECK_EQ(runs[0].families[0], uint8_t(0));
}

TEST(text_fake_bold_and_italic_follow_minikin) {
  auto font = make_font(sample_font());
  text::FontFamily family(text::kEmptyLocaleListId, text::FamilyVariant::kDefault, {font}, false, true,
                          text::VariationFamilyType::kNone);
  text::FakedFont f = family.closest_match(text::FontStyle{700, true});
  CHECK(f.fakery.fake_bold);
  CHECK(f.fakery.fake_italic);
  f = family.closest_match(text::FontStyle{500, false});
  CHECK(!f.fakery.fake_bold);
}

TEST(text_measure_applies_kerning_hinted_advances_and_spacing) {
  auto fc = collection_of({sample_font()});
  text::Paint p = paint_of(fc, 10);
  std::u16string s = u"AV";
  float adv[2];
  float total = text::measure_text(s, 0, 2, text::Bidi::kForceLtr, p, text::StartHyphenEdit::kNoEdit,
                                   text::EndHyphenEdit::kNoEdit, adv, nullptr, nullptr, text::kRunNone);
  // 'A' and 'V' are hinted to 6 px; the kern pair takes -80/1000 em, that
  // HarfBuzz shares between the two glyphs for a legacy 'kern' table.
  CHECK(std::fabs(total - (12.0f - 0.8f)) < 0.01f);
  CHECK(std::fabs(adv[0] - (6.0f - 0.4f)) < 0.01f);
  CHECK(std::fabs(total - adv[0] - adv[1]) < 1e-4f);

  p.letter_spacing = 0.1f;  // 1 px at 10 px: half before, half after each letter
  std::u16string aa = u"AA";
  total = text::measure_text(aa, 0, 2, text::Bidi::kForceLtr, p, text::StartHyphenEdit::kNoEdit,
                             text::EndHyphenEdit::kNoEdit, adv, nullptr, nullptr, text::kRunNone);
  CHECK(near(total, 14.0f));
  CHECK(near(adv[0], 7.0f));
  total = text::measure_text(aa, 0, 2, text::Bidi::kForceLtr, p, text::StartHyphenEdit::kNoEdit,
                             text::EndHyphenEdit::kNoEdit, adv, nullptr, nullptr, text::kRunWholeLine);
  CHECK(near(total, 13.0f));  // no spacing at the line's edges

  p.letter_spacing = 0;
  p.word_spacing = 2;
  std::u16string words = u"A A";
  total = text::measure_text(words, 0, 3, text::Bidi::kDefaultLtr, p, text::StartHyphenEdit::kNoEdit,
                             text::EndHyphenEdit::kNoEdit, nullptr, nullptr, nullptr, text::kRunNone);
  CHECK(near(total, 6.0f + 3.0f + 2.0f + 6.0f));
}

TEST(text_measure_bounds_and_extent) {
  auto fc = collection_of({sample_font()});
  text::Paint p = paint_of(fc, 10);
  p.flags.hinting = text::Hinting::kNone;
  std::u16string s = u"Ax";
  text::TextRect b;
  text::measure_text(s, 0, 2, text::Bidi::kForceLtr, p, text::StartHyphenEdit::kNoEdit, text::EndHyphenEdit::kNoEdit,
                     nullptr, &b, nullptr, text::kRunNone);
  // A: [0, 6] x [-7, 0]; x at 5.5: [5, 10] (0.2..4.8 rounded out) x [-5, 0].
  CHECK_EQ(b, (text::TextRect{0, -7, 5.5f + 5, 0}));
  text::Extent e = text::text_extent(s, 0, 2, text::Bidi::kForceLtr, p);
  CHECK_EQ(e, (text::Extent{-8, 2}));
}

TEST(text_cursor_and_offsets_follow_minikin) {
  std::u16string s = u"ab";
  float adv[2] = {5, 5};
  CHECK_EQ(text::run_advance(adv, s.data(), 0, 2, 1), 5.0f);
  CHECK_EQ(text::offset_for_advance(adv, s.data(), 0, 2, 7.6f), size_t(2));
  CHECK_EQ(text::offset_for_advance(adv, s.data(), 0, 2, 2.4f), size_t(0));
  // A ligature: the second letter has no advance, each grapheme gets half.
  float lig[2] = {10, 0};
  CHECK_EQ(text::run_advance(lig, s.data(), 0, 2, 1), 5.0f);
  std::u16string emoji = u"a\U0001F600b";
  float e[4] = {5, 10, 0, 5};
  CHECK(!text::is_grapheme_break(e, emoji.data(), 0, 4, 2));  // inside a surrogate pair
  CHECK_EQ(text::text_run_cursor(e, emoji.data(), 0, 4, 2, text::CursorOpt::kAtOrAfter), size_t(3));
  CHECK_EQ(text::text_run_cursor(e, emoji.data(), 0, 4, 2, text::CursorOpt::kAt), size_t(-1));
}
