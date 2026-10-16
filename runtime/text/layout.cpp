// minikin's Layout.cpp, LayoutCore.cpp, LayoutSplitter.h, LayoutUtils.cpp,
// BidiUtils.cpp, ScriptUtils.cpp, FontFeatureUtils.cpp, Measurement.cpp and
// GraphemeBreak.cpp (frameworks/minikin), on HarfBuzz and the Skia-faithful
// faces of font_face.cpp. minikin's quirks are kept where they change
// results (script runs read from the start of the context, rectangles joined
// even when empty).
#include "text/layout.h"

#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <set>

#include "text/locale.h"
#include "text/unicode.h"

namespace aa::text {

namespace {

constexpr char32_t kLineFeed = 0x0A, kCarriageReturn = 0x0D, kHyphen = 0x2010, kHyphenMinus = 0x002D;

struct Range {
  size_t start = 0, end = 0;
  size_t length() const { return end - start; }
};

// U16StringPiece::codePointAt.
char32_t code_point_at(std::u16string_view s, size_t i) {
  char32_t c = s[i];
  if (is_lead_surrogate(char16_t(c)) && i + 1 < s.size() && is_trail_surrogate(s[i + 1])) {
    c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
  }
  return c;
}

float hb_to_float(hb_position_t v) { return std::scalbn(float(v), -8); }
hb_position_t float_to_hb(float v) { return hb_position_t(std::scalbn(v, 8)); }

// --- Word boundaries for layout pieces (LayoutUtils.cpp) ---

bool is_word_space(char16_t c) { return c == ' '; }

bool word_break_after(char16_t c) {
  if (c == ' ' || (0x2000 <= c && c <= 0x200A) || c == 0x3000) return true;
  return (0x2066 <= c && c <= 0x2069) || (0x202A <= c && c <= 0x202E) || c == 0x200E || c == 0x200F;
}

bool word_break_before(char16_t c) { return word_break_after(c) || (0x3400 <= c && c <= 0x9FFF); }

size_t prev_word_break(std::u16string_view s, size_t offset) {
  if (offset == 0) return 0;
  if (offset > s.size()) offset = s.size();
  if (word_break_before(s[offset - 1])) return offset - 1;
  for (size_t i = offset - 1; i > 0; i--) {
    if (word_break_before(s[i]) || word_break_after(s[i - 1])) return i;
  }
  return 0;
}

size_t next_word_break(std::u16string_view s, size_t offset) {
  if (offset >= s.size()) return s.size();
  if (word_break_after(s[offset])) return offset + 1;
  for (size_t i = offset + 1; i < s.size(); i++) {
    if (word_break_before(s[i])) return i;
  }
  return s.size();
}

// LayoutSplitter: (context, piece) pairs in layout order.
std::vector<std::pair<Range, Range>> split_pieces(std::u16string_view s, Range range, bool rtl) {
  std::vector<std::pair<Range, Range>> out;
  if (range.start == range.end) return out;
  Range context, piece;
  if (rtl) {
    size_t pos = range.end;
    context = {prev_word_break(s, pos), next_word_break(s, pos == 0 ? 0 : pos - 1)};
    piece = {std::max(context.start, range.start), pos};
    while (true) {
      out.emplace_back(context, piece);
      pos = piece.start;
      if (pos == range.start) break;
      context = {prev_word_break(s, pos), pos};
      piece = {std::max(context.start, range.start), pos};
    }
  } else {
    size_t pos = range.start;
    context = {prev_word_break(s, pos == range.end ? pos : pos + 1), next_word_break(s, pos)};
    piece = {pos, std::min(context.end, range.end)};
    while (true) {
      out.emplace_back(context, piece);
      pos = piece.end;
      if (pos == range.end) break;
      context = {pos, next_word_break(s, pos)};
      piece = {pos, std::min(context.end, range.end)};
    }
  }
  return out;
}

// --- Bidirectional runs (BidiUtils.cpp) ---

// Characters that can make a paragraph not purely left to right: strong
// right-to-left letters, Arabic letter mark and the bidi controls.
bool may_be_rtl(char32_t c) {
  return (0x0590 <= c && c <= 0x08FF) || (0xFB1D <= c && c <= 0xFDFF) || (0xFE70 <= c && c <= 0xFEFF) ||
         (0x10800 <= c && c <= 0x10FFF) || (0x1E800 <= c && c <= 0x1EFFF) || c == 0x200F || c == 0x202B ||
         c == 0x202E || c == 0x2067 || c == 0x2068;
}

struct BidiRunInfo {
  Range range;
  bool rtl = false;
  bool valid = true;
};

std::vector<BidiRunInfo> bidi_runs(std::u16string_view text, Range range, Bidi bidi) {
  bool rtl = uint8_t(bidi) & 1;
  if (uint8_t(bidi) & 4) return {{range, rtl}};  // override: a single run
  if (bidi == Bidi::kLtr || bidi == Bidi::kDefaultLtr) {
    bool simple = true;
    for (size_t i = 0; i < text.size() && simple;) {
      if (may_be_rtl(next_code_point(text.data(), &i, text.size()))) simple = false;
    }
    if (simple) return {{range, false}};  // what ICU gives: one left-to-right run
  }
  uint8_t level = bidi == Bidi::kLtr ? 0 : bidi == Bidi::kRtl ? 1 : bidi == Bidi::kDefaultLtr ? 0x7E : 0x7F;
  std::vector<BidiRun> runs;
  bool para_rtl = rtl;
  if (!unicode_services().bidi_runs(text, level, &runs, &para_rtl)) return {{range, rtl}};
  if (runs.empty()) return {{range, para_rtl}};
  if (runs.size() == 1) return {{range, runs[0].rtl}};
  std::vector<BidiRunInfo> out;
  for (const BidiRun& r : runs) {
    size_t s = std::max<size_t>(r.start, range.start), e = std::min<size_t>(r.start + r.length, range.end);
    if (e <= s) {
      out.push_back({{0, 0}, false, false});  // empty after clipping: skipped
    } else {
      out.push_back({{s, e}, r.rtl});
    }
  }
  return out;
}

// --- Script runs (ScriptUtils.cpp) ---

std::pair<size_t, hb_script_t> script_run(std::u16string_view text, Range range, size_t pos) {
  if (pos < range.start || pos >= range.end) return {range.end, HB_SCRIPT_UNKNOWN};
  auto decode = [&](size_t at) {
    char32_t c = next_code_point(text.data(), &at, range.end);
    return is_surrogate(c) ? kReplacementChar : c;
  };
  char32_t c = decode(pos);
  hb_script_t current = hb_script_t(script(c));
  size_t i;
  for (i = pos + utf16_length(c); i < range.end; i += utf16_length(c)) {
    c = decode(i);
    hb_script_t next = hb_script_t(script(c));
    if (current != next) {
      if (current == HB_SCRIPT_INHERITED || current == HB_SCRIPT_COMMON) {
        current = next;
      } else if (next == HB_SCRIPT_INHERITED || next == HB_SCRIPT_COMMON) {
        continue;
      } else {
        break;
      }
    }
  }
  return {i, current == HB_SCRIPT_INHERITED ? HB_SCRIPT_COMMON : current};
}

// --- Letter spacing and features ---

bool letter_spacing_script(hb_script_t s) {
  return !(s == HB_SCRIPT_ARABIC || s == HB_SCRIPT_NKO || s == HB_SCRIPT_PSALTER_PAHLAVI || s == HB_SCRIPT_MANDAIC ||
           s == HB_SCRIPT_MONGOLIAN || s == HB_SCRIPT_PHAGS_PA || s == HB_SCRIPT_DEVANAGARI ||
           s == HB_SCRIPT_BENGALI || s == HB_SCRIPT_GURMUKHI || s == HB_SCRIPT_MODI || s == HB_SCRIPT_SHARADA ||
           s == HB_SCRIPT_SYLOTI_NAGRI || s == HB_SCRIPT_TIRHUTA || s == HB_SCRIPT_OGHAM);
}

bool letter_spacing_code_point(char32_t c) { return letter_spacing_script(hb_script_t(script(c))); }

std::vector<hb_feature_t> features_for(const Paint& paint) {
  std::vector<hb_feature_t> features;
  if (std::fabs(paint.letter_spacing) > 0.03) {
    features.push_back({HB_TAG('l', 'i', 'g', 'a'), 0, 0, ~0u});
    features.push_back({HB_TAG('c', 'l', 'i', 'g'), 0, 0, ~0u});
  }
  bool chws = true;
  for (const FontFeature& f : paint.font_features) {
    if (f.tag == HB_TAG('c', 'h', 'w', 's') || (f.value && (f.tag == HB_TAG('h', 'a', 'l', 't') || f.tag == HB_TAG('p', 'a', 'l', 't')))) {
      chws = false;
    }
    features.push_back({f.tag, f.value, 0, ~0u});
  }
  if (chws) features.push_back({HB_TAG('c', 'h', 'w', 's'), 1, 0, ~0u});
  return features;
}

// --- HarfBuzz font functions: advances from the Skia-faithful face ---

struct SkiaArguments {
  std::shared_ptr<Face> face;
  FontSpec spec;
};

hb_position_t h_advance(hb_font_t*, void* data, hb_codepoint_t glyph, void*) {
  auto* args = static_cast<SkiaArguments*>(data);
  uint16_t g = uint16_t(glyph);
  float advance = 0;
  args->face->advances(args->spec, std::span<const uint16_t>(&g, 1), std::span<float>(&advance, 1));
  return hb_position_t(256 * advance + 0.5);
}

void h_advances(hb_font_t*, void* data, unsigned count, const hb_codepoint_t* first_glyph, unsigned glyph_stride,
                hb_position_t* first_advance, unsigned advance_stride, void*) {
  auto* args = static_cast<SkiaArguments*>(data);
  std::vector<uint16_t> glyphs(count);
  std::vector<float> advances(count);
  const auto* g = reinterpret_cast<const uint8_t*>(first_glyph);
  for (unsigned i = 0; i < count; ++i, g += glyph_stride) glyphs[i] = uint16_t(*reinterpret_cast<const hb_codepoint_t*>(g));
  args->face->advances(args->spec, glyphs, advances);
  auto* a = reinterpret_cast<uint8_t*>(first_advance);
  for (unsigned i = 0; i < count; ++i, a += advance_stride) *reinterpret_cast<hb_position_t*>(a) = float_to_hb(advances[i]);
}

hb_bool_t h_origin(hb_font_t*, void*, hb_codepoint_t, hb_position_t*, hb_position_t*, void*) { return true; }

hb_font_funcs_t* font_funcs(bool color_bitmaps) {
  static hb_font_funcs_t* funcs = [] {
    hb_font_funcs_t* f = hb_font_funcs_create();
    // HarfBuzz's own advances would ignore hinting.
    hb_font_funcs_set_glyph_h_advance_func(f, h_advance, nullptr, nullptr);
    hb_font_funcs_set_glyph_h_advances_func(f, h_advances, nullptr, nullptr);
    hb_font_funcs_set_glyph_h_origin_func(f, h_origin, nullptr, nullptr);
    hb_font_funcs_make_immutable(f);
    return f;
  }();
  static hb_font_funcs_t* emoji = [] {
    hb_font_funcs_t* f = hb_font_funcs_create();
    hb_font_funcs_set_glyph_h_origin_func(f, h_origin, nullptr, nullptr);
    hb_font_funcs_make_immutable(f);
    return f;
  }();
  return color_bitmaps ? emoji : funcs;
}

// --- Hyphens ---

std::u16string_view hyphen_string(StartHyphenEdit h) {
  switch (h) {
    case StartHyphenEdit::kInsertZwj: return u"‍";
    case StartHyphenEdit::kInsertHyphen: return u"‐";
    default: return {};
  }
}

std::u16string_view hyphen_string(EndHyphenEdit h) {
  switch (h) {
    case EndHyphenEdit::kReplaceWithHyphen:
    case EndHyphenEdit::kInsertHyphen: return u"‐";
    case EndHyphenEdit::kInsertArmenianHyphen: return u"֊";
    case EndHyphenEdit::kInsertMaqaf: return u"־";
    case EndHyphenEdit::kInsertUcasHyphen: return u"᐀";
    case EndHyphenEdit::kInsertZwjAndHyphen: return u"‍‐";
    default: return {};
  }
}

bool is_insertion(StartHyphenEdit h) { return h != StartHyphenEdit::kNoEdit; }
bool is_insertion(EndHyphenEdit h) { return uint8_t(h) >= uint8_t(EndHyphenEdit::kInsertHyphen); }
bool is_replacement(EndHyphenEdit h) { return h == EndHyphenEdit::kReplaceWithHyphen; }

hb_codepoint_t hyphen_char(hb_codepoint_t preferred, hb_font_t* font) {
  hb_codepoint_t glyph;
  if (preferred == 0x058A || preferred == 0x05BE || preferred == 0x1400) {
    if (hb_font_get_nominal_glyph(font, preferred, &glyph)) return preferred;
    preferred = kHyphen;
  }
  if (preferred == kHyphen && !hb_font_get_nominal_glyph(font, preferred, &glyph)) return kHyphenMinus;
  return preferred;
}

template <typename Edit>
void add_hyphen(hb_buffer_t* buffer, hb_font_t* font, Edit edit, uint32_t cluster) {
  for (char16_t c : hyphen_string(edit)) hb_buffer_add(buffer, hyphen_char(c, font), cluster);
}

// addToHbBuffer: the cluster value of the first code point added.
uint32_t add_to_buffer(hb_buffer_t* buffer, const char16_t* buf, size_t start, size_t count, size_t buf_size,
                       size_t run_start, size_t run_end, StartHyphenEdit in_start, EndHyphenEdit in_end,
                       hb_font_t* font) {
  StartHyphenEdit start_hyphen = run_start == 0 ? in_start : StartHyphenEdit::kNoEdit;
  EndHyphenEdit end_hyphen = run_end == count ? in_end : EndHyphenEdit::kNoEdit;
  if (is_insertion(start_hyphen)) add_hyphen(buffer, font, start_hyphen, 0);
  const char16_t* hb_text;
  size_t hb_text_length;
  unsigned item_offset;
  unsigned item_length = unsigned(run_end - run_start);
  bool end_insertion = is_insertion(end_hyphen), end_replacement = is_replacement(end_hyphen);
  if (end_replacement) item_length -= 1;
  if (start_hyphen == StartHyphenEdit::kNoEdit) {
    hb_text = buf;
    item_offset = unsigned(start + run_start);
  } else {
    hb_text = buf + start + run_start;
    item_offset = 0;
  }
  if (end_hyphen == EndHyphenEdit::kNoEdit) {
    hb_text_length = size_t((buf + buf_size) - hb_text);
  } else {
    hb_text_length = item_offset + item_length;
  }
  hb_buffer_add_utf16(buffer, reinterpret_cast<const uint16_t*>(hb_text), int(hb_text_length), item_offset,
                      int(item_length));
  unsigned n = 0;
  hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &n);
  if (end_insertion || end_replacement) {
    uint32_t cluster = n == 0 ? 0 : info[n - 1].cluster + uint32_t(end_replacement);
    add_hyphen(buffer, font, end_hyphen, cluster);
    info = hb_buffer_get_glyph_infos(buffer, nullptr);
  }
  return info[0].cluster;
}

// --- LayoutPiece (LayoutCore.cpp) ---

struct Point {
  float x = 0, y = 0;
};

struct LayoutPiece {
  std::vector<float> advances;  // per code unit of the piece
  std::vector<uint8_t> font_indices;
  std::vector<uint32_t> glyph_ids;
  std::vector<Point> points;
  std::vector<uint32_t> clusters;
  std::vector<FakedFont> fonts;
  float advance = 0;
  Extent extent;
  uint32_t cluster_count = 0;
  bool vertical = false;
};

LayoutPiece shape_piece(std::u16string_view text_buf, Range range, bool rtl, const Paint& paint,
                        StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen) {
  LayoutPiece p;
  const char16_t* buf = text_buf.data();
  const size_t start = range.start, count = range.length(), buf_size = text_buf.size();
  p.advances.assign(count, 0.0f);
  p.vertical = paint.vertical_text;
  if (!paint.font) return p;
  std::u16string_view substr = text_buf.substr(start, count);
  std::vector<FontCollection::Run> items =
      paint.font->itemize(substr, paint.font_style, paint.locale_list_id, paint.family_variant);
  std::vector<hb_feature_t> features = features_for(paint);
  hb_buffer_t* buffer = hb_buffer_create();
  std::vector<hb_font_t*> hb_fonts;
  std::vector<std::shared_ptr<Face>> faces;  // keep the faces of the fonts alive
  double size = paint.size, scale_x = paint.scale_x;
  float x = 0, y = 0;
  float* dir = paint.vertical_text ? &y : &x;
  std::set<uint32_t> cluster_set;

  for (int ix = rtl ? int(items.size()) - 1 : 0; rtl ? ix >= 0 : ix < int(items.size()); rtl ? --ix : ++ix) {
    const FontCollection::Run& run = items[size_t(ix)];
    FakedFont faked = paint.font->best_font(substr, run, paint.font_style, paint.font_variations);
    std::shared_ptr<Face> face = faked.face();
    if (!face) continue;  // a font FreeType rejects draws nothing
    size_t font_ix = p.fonts.size();
    for (size_t k = 0; k < faces.size(); ++k) {
      if (faces[k] == face) {
        font_ix = k;
        break;
      }
    }
    if (font_ix == p.fonts.size()) {
      p.fonts.push_back(faked);
      faces.push_back(face);
      // FakedFont::hbFont: the base font's HarfBuzz font with the axes that
      // differ from their default (and, as in minikin, only those).
      hb_font_t* base = faked.font->face()->hb_font();
      std::vector<hb_variation_t> vars;
      for (const Variation& v : faked.fakery.variations) {
        const Axis* a = faked.font->fvar_axis(v.tag);
        if (a && a->def != v.value) vars.push_back({v.tag, v.value});
      }
      hb_font_t* adjusted = faked.fakery.variations.empty() ? hb_font_reference(base) : hb_font_create_sub_font(base);
      if (!faked.fakery.variations.empty()) hb_font_set_variations(adjusted, vars.data(), unsigned(vars.size()));
      hb_font_t* font = hb_font_create_sub_font(adjusted);
      hb_font_destroy(adjusted);
      bool color = face->color_bitmaps();
      hb_font_set_funcs(font, font_funcs(color), new SkiaArguments{face, skia_font(paint, faked.fakery)},
                        [](void* d) { delete static_cast<SkiaArguments*>(d); });
      hb_fonts.push_back(font);
    }
    hb_font_t* hb_font = hb_fonts[font_ix];

    bool need_extent = false;
    for (int i = run.start; i < run.end; ++i) {
      if (buf[i] != kLineFeed && buf[i] != kCarriageReturn) {  // minikin reads the context buffer
        need_extent = true;
        break;
      }
    }
    if (need_extent) p.extent.extend(font_extent(faked, paint));

    hb_font_set_ppem(hb_font, unsigned(size * scale_x), unsigned(size));
    hb_font_set_scale(hb_font, float_to_hb(float(size * scale_x)), float_to_hb(float(size)));

    // As minikin, script runs are read from the start of the context.
    Range run_range{size_t(run.start), size_t(run.end)};
    for (size_t pos = run_range.start; pos < run_range.end;) {
      auto [run_end, script_code] = script_run(text_buf, run_range, pos);
      size_t script_start = pos, script_end = run_end;
      pos = run_end;

      double letter_space = 0, letter_space_half = 0;
      if (paint.letter_spacing != 0.0 && letter_spacing_script(script_code)) {
        letter_space = paint.letter_spacing * size * scale_x;
        letter_space_half = letter_space * 0.5;
      }
      hb_buffer_clear_contents(buffer);
      hb_buffer_set_script(buffer, script_code);
      hb_buffer_set_direction(buffer, paint.vertical_text ? HB_DIRECTION_TTB : rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
      const LocaleList& locales = locale_list(paint.locale_list_id);
      if (!locales.empty()) {
        hb_language_t language = locales.hb_language(0);
        for (size_t i = 0; i < locales.size(); ++i) {
          if (locales[i].supports_script(hb_script_to_iso15924_tag(script_code))) {
            language = locales.hb_language(i);
            break;
          }
        }
        hb_buffer_set_language(buffer, language);
      }
      uint32_t cluster_start = add_to_buffer(buffer, buf, start, count, buf_size, script_start, script_end,
                                             start_hyphen, end_hyphen, hb_font);
      hb_shape(hb_font, buffer, features.empty() ? nullptr : features.data(), unsigned(features.size()));
      unsigned glyphs = 0;
      hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &glyphs);
      hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
      const int64_t cluster_offset = int64_t(cluster_start) - int64_t(script_start);

      if (glyphs && letter_space != 0) {
        uint32_t adv_index = uint32_t(info[0].cluster - cluster_offset);
        if (!is_control(code_point_at(text_buf, adv_index + start))) {
          p.advances[adv_index] += float(letter_space_half);
          *dir += float(letter_space_half);
        }
      }
      for (unsigned i = 0; i < glyphs; i++) {
        const size_t base = size_t(int64_t(info[i].cluster) - cluster_offset);
        if (letter_space != 0 && i > 0 && info[i - 1].cluster != info[i].cluster) {
          const size_t prev_index = size_t(int64_t(info[i - 1].cluster) - cluster_offset);
          bool ctrl = is_control(code_point_at(text_buf, base + start));
          bool prev_ctrl = is_control(code_point_at(text_buf, prev_index + start));
          if (!prev_ctrl) p.advances[prev_index] += float(letter_space_half);
          if (!ctrl) p.advances[base] += float(letter_space_half);
          if (!ctrl && !prev_ctrl) {
            *dir += float(letter_space);
          } else if (!ctrl || !prev_ctrl) {
            *dir += float(letter_space_half);
          }
        }
        float xoff = hb_to_float(positions[i].x_offset);
        float yoff = -hb_to_float(positions[i].y_offset);
        xoff += yoff * paint.skew_x;
        p.font_indices.push_back(uint8_t(font_ix));
        p.glyph_ids.push_back(info[i].codepoint);
        p.points.push_back({x + xoff, y + yoff});
        float advance = paint.vertical_text ? -hb_to_float(positions[i].y_advance) : hb_to_float(positions[i].x_advance);
        p.clusters.push_back(uint32_t(base));
        cluster_set.insert(uint32_t(base));
        if (base < count) p.advances[base] += advance;
        *dir += advance;
      }
      if (glyphs && letter_space != 0) {
        uint32_t last_index = uint32_t(int64_t(info[glyphs - 1].cluster) - cluster_offset);
        if (!is_control(code_point_at(text_buf, last_index + start))) {
          p.advances[last_index] += float(letter_space_half);
          *dir += float(letter_space_half);
        }
      }
    }
  }
  for (hb_font_t* f : hb_fonts) hb_font_destroy(f);
  hb_buffer_destroy(buffer);
  p.advance = *dir;
  p.cluster_count = uint32_t(cluster_set.size());
  return p;
}

// LayoutPiece::calculateBounds.
TextRect piece_bounds(const LayoutPiece& p, const Paint& paint) {
  TextRect out;
  for (size_t i = 0; i < p.glyph_ids.size(); ++i) {
    const FakedFont& font = p.fonts[p.font_indices[i]];
    uint16_t g = uint16_t(p.glyph_ids[i]);
    Rect r;
    if (std::shared_ptr<Face> face = font.face()) {
      face->bounds(skia_font(paint, font.fakery), std::span<const uint16_t>(&g, 1), std::span<Rect>(&r, 1));
    }
    out.join(TextRect{r.left, r.top, r.right, r.bottom}, p.points[i].x, p.points[i].y);
  }
  return out;
}

// Layout::doLayoutRunCached over pieces; `on_piece` gets each piece, its
// offset in the run and the advance before it.
template <typename Fn>
float layout_run(std::u16string_view text, Range range, bool rtl, const Paint& paint, StartHyphenEdit start_hyphen,
                 EndHyphenEdit end_hyphen, Fn&& on_piece) {
  float advance = 0;
  for (const auto& [context, piece] : split_pieces(text, range, rtl)) {
    StartHyphenEdit sh = piece.start == range.start ? start_hyphen : StartHyphenEdit::kNoEdit;
    EndHyphenEdit eh = piece.end == range.end ? end_hyphen : EndHyphenEdit::kNoEdit;
    std::u16string_view ctx = text.substr(context.start, context.length());
    LayoutPiece lp = shape_piece(ctx, Range{piece.start - context.start, piece.end - context.start}, rtl, paint, sh, eh);
    float word_spacing = piece.length() == 1 && is_word_space(text[piece.start]) ? paint.word_spacing : 0;
    on_piece(lp, piece, advance, word_spacing);
    advance += lp.advance + word_spacing;
  }
  return advance;
}

float adjust_advance_edges(std::u16string_view text, Range range, const std::vector<BidiRunInfo>& runs,
                           const Paint& paint, RunFlag flag, float advance, float* advances) {
  const float spacing = paint.letter_spacing * paint.size * paint.scale_x;
  const float half = spacing * 0.5f;
  if (spacing == 0 || text.empty()) return advance;
  const size_t off = range.start;
  auto cp = [&](size_t i) { return code_point_at(text, i); };
  if (flag & kRunLeftEdge) {
    const BidiRunInfo& l = runs.front();
    if (l.rtl) {
      size_t last = size_t(-1);
      for (size_t i = l.range.start; i < l.range.end; ++i) {
        if (!is_control(cp(i)) && advances[i - off] != 0) last = i;
      }
      if (last != size_t(-1) && letter_spacing_code_point(cp(last))) {
        advances[last - off] -= half;
        advance -= half;
      }
    } else {
      for (size_t i = l.range.start; i < l.range.end; ++i) {
        if (!is_control(cp(i)) && advances[i - off] != 0) {
          if (letter_spacing_code_point(cp(i))) {
            advances[i - off] -= half;
            advance -= half;
          }
          break;
        }
      }
    }
  }
  if (flag & kRunRightEdge) {
    const BidiRunInfo& r = runs.back();
    if (r.rtl) {
      for (size_t i = r.range.start; i < r.range.end; ++i) {
        if (!is_control(cp(i)) && advances[i - off] != 0) {
          if (letter_spacing_code_point(cp(i))) {
            advances[i - off] -= half;
            advance -= half;
          }
          break;
        }
      }
    } else {
      size_t last = size_t(-1);
      for (size_t i = r.range.start; i < r.range.end; ++i) {
        if (!is_control(cp(i)) && advances[i - off] != 0) last = i;
      }
      if (last != size_t(-1) && letter_spacing_code_point(cp(last))) {
        advances[last - off] -= half;
        advance -= half;
      }
    }
  }
  return advance;
}

void adjust_bounds_edges(const Paint& paint, RunFlag flag, TextRect* bounds) {
  if (!bounds) return;
  const float half = paint.letter_spacing * paint.size * paint.scale_x * 0.5f;
  if (half == 0) return;
  if (flag & kRunLeftEdge) {
    if (paint.vertical_text) {
      bounds->top -= half, bounds->bottom -= half;
    } else {
      bounds->left -= half, bounds->right -= half;
    }
  }
  if (flag & kRunRightEdge) {
    if (paint.vertical_text) {
      bounds->bottom -= half;
    } else {
      bounds->right -= half;
    }
  }
}

}  // namespace

std::vector<FontFeature> parse_font_features(std::string_view settings) {
  std::vector<FontFeature> out;
  size_t start = 0;
  while (start <= settings.size()) {
    size_t comma = settings.find(',', start);
    std::string_view item = settings.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    start = comma == std::string_view::npos ? settings.size() + 1 : comma + 1;
    hb_feature_t f;
    if (hb_feature_from_string(item.data(), int(item.size()), &f) && f.start == 0 && f.end == ~0u) {
      out.push_back({f.tag, f.value});
    }
  }
  return out;
}

FontSpec skia_font(const Paint& paint, const FontFakery& fakery) {
  FontSpec s = paint.flags;
  s.size = paint.size;
  s.scale_x = paint.scale_x;
  s.skew_x = paint.skew_x;
  s.embolden = s.embolden || fakery.fake_bold;
  if (fakery.fake_italic) s.skew_x -= 0.25f;
  return s;
}

void Extent::extend(const Extent& e) {
  ascent = std::min(ascent, e.ascent);
  descent = std::max(descent, e.descent);
}

void TextRect::join(const TextRect& r, float dx, float dy) {
  if (empty()) {
    *this = {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
  } else {
    left = std::min(left, r.left + dx);
    top = std::min(top, r.top + dy);
    right = std::max(right, r.right + dx);
    bottom = std::max(bottom, r.bottom + dy);
  }
}

Extent font_extent(const FakedFont& font, const Paint& paint) {
  FontMetrics m;
  std::shared_ptr<Face> face = font.face();
  if (!face) return Extent{};
  face->metrics(skia_font(paint, font.fakery), &m);
  return Extent{m.ascent, m.descent};
}

Extent reference_extent_for_locale(const Paint& paint) {
  const FontCollection& fc = *paint.font;
  Extent result;
  for (size_t i = 0; i < fc.family_count(); ++i) {
    const FontFamily& family = *fc.family(i);
    if (!family.custom_fallback()) break;
    result.extend(font_extent(family.closest_match(paint.font_style, paint.font_variations), paint));
  }
  if (paint.locale_list_id == kInvalidLocaleListId) return result;
  FamilyVariant request = paint.family_variant == FamilyVariant::kDefault ? FamilyVariant::kCompact : paint.family_variant;
  const LocaleList& requested = locale_list(paint.locale_list_id);
  bool found = false;
  // filterFamilyByLocale: families of the first locale's script.
  auto filter = [&](auto&& callback) {
    if (requested.empty()) return;
    const Locale& locale = requested[0];
    for (size_t i = 0; i < fc.family_count(); ++i) {
      const FontFamily& family = *fc.family(i);
      uint32_t id = family.locale_list_id();
      if (id == kInvalidLocaleListId) continue;
      const LocaleList& fl = locale_list(id);
      for (size_t j = 0; j < fl.size(); ++j) {
        if (fl[j].same_script(locale)) {
          if (callback(family)) break;  // continue with the next family
          return;
        }
      }
    }
  };
  filter([&](const FontFamily& family) {
    FamilyVariant fv = family.variant() == FamilyVariant::kDefault ? FamilyVariant::kCompact : family.variant();
    if (fv != request) return true;
    result.extend(font_extent(family.closest_match(paint.font_style, paint.font_variations), paint));
    found = true;
    return false;
  });
  filter([&](const FontFamily& family) {
    result.extend(font_extent(family.closest_match(paint.font_style, paint.font_variations), paint));
    found = true;
    return false;
  });
  if (!found) {
    Extent e = font_extent(fc.family(0)->closest_match(paint.font_style, paint.font_variations), paint);
    result = e;
  }
  return result;
}

float measure_text(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint,
                   StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen, float* advances, TextRect* bounds,
                   uint32_t* cluster_count, RunFlag run_flag) {
  float advance = 0;
  if (cluster_count) *cluster_count = 0;
  std::vector<float> tmp;
  if (!advances) {
    tmp.assign(count, 0.0f);
    advances = tmp.data();
  }
  Range range{start, start + count};
  std::vector<BidiRunInfo> runs = bidi_runs(text, range, bidi);
  for (const BidiRunInfo& run : runs) {
    if (!run.valid) continue;
    float* run_advances = advances + (run.range.start - range.start);
    TextRect run_bounds;
    float run_advance = layout_run(text, run.range, run.rtl, paint, start_hyphen, end_hyphen,
                                   [&](const LayoutPiece& lp, Range piece, float before, float word_spacing) {
                                     float* a = run_advances + (piece.start - run.range.start);
                                     std::copy(lp.advances.begin(), lp.advances.end(), a);
                                     if (word_spacing != 0) a[0] += word_spacing;
                                     if (bounds) {
                                       TextRect b = piece_bounds(lp, paint);
                                       if (lp.vertical) {
                                         run_bounds.join(b, 0, before);
                                       } else {
                                         run_bounds.join(b, before, 0);
                                       }
                                     }
                                     if (cluster_count) *cluster_count += lp.cluster_count;
                                   });
    if (bounds) {
      if (paint.vertical_text) {
        bounds->join(run_bounds, 0, advance);
      } else {
        bounds->join(run_bounds, advance, 0);
      }
    }
    advance += run_advance;
  }
  adjust_bounds_edges(paint, run_flag, bounds);
  return adjust_advance_edges(text, range, runs, paint, run_flag, advance, advances);
}

Extent text_extent(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint) {
  Extent extent;
  for (const BidiRunInfo& run : bidi_runs(text, Range{start, start + count}, bidi)) {
    if (!run.valid) continue;
    for (const auto& [context, piece] : split_pieces(text, run.range, run.rtl)) {
      std::u16string_view ctx = text.substr(context.start, context.length());
      LayoutPiece lp = shape_piece(ctx, Range{piece.start - context.start, piece.end - context.start}, run.rtl, paint,
                                   StartHyphenEdit::kNoEdit, EndHyphenEdit::kNoEdit);
      extent.extend(lp.extent);
    }
  }
  return extent;
}

TextRect text_bounds(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint,
                     StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen) {
  TextRect out;
  float advance = 0;
  Range range{start, start + count};
  for (const BidiRunInfo& run : bidi_runs(text, range, bidi)) {
    if (!run.valid) continue;
    for (const auto& [context, piece] : split_pieces(text, run.range, run.rtl)) {
      StartHyphenEdit sh = piece.start == range.start ? start_hyphen : StartHyphenEdit::kNoEdit;
      EndHyphenEdit eh = piece.end == range.end ? end_hyphen : EndHyphenEdit::kNoEdit;
      std::u16string_view ctx = text.substr(context.start, context.length());
      LayoutPiece lp = shape_piece(ctx, Range{piece.start - context.start, piece.end - context.start}, run.rtl, paint,
                                   sh, eh);
      TextRect b = piece_bounds(lp, paint);
      if (lp.vertical) {
        out.join(b, 0, advance);
      } else {
        out.join(b, advance, 0);
      }
      advance += lp.advance;
      if (piece.length() == 1 && is_word_space(text[piece.start])) advance += paint.word_spacing;
    }
  }
  return out;
}

TextLayout do_layout(std::u16string_view text, size_t start, size_t count, Bidi bidi, const Paint& paint,
                     StartHyphenEdit start_hyphen, EndHyphenEdit end_hyphen, RunFlag run_flag) {
  TextLayout out;
  out.advances.assign(count, 0.0f);
  Range range{start, start + count};
  std::vector<BidiRunInfo> runs = bidi_runs(text, range, bidi);
  for (const BidiRunInfo& run : runs) {
    if (!run.valid) continue;
    layout_run(text, run.range, run.rtl, paint, start_hyphen, end_hyphen,
               [&](const LayoutPiece& lp, Range piece, float, float word_spacing) {
                 // Layout::appendLayout
                 float xa = lp.vertical ? 0 : out.advance, ya = lp.vertical ? out.advance : 0;
                 size_t dst = piece.start - start;
                 for (size_t i = 0; i < lp.glyph_ids.size(); ++i) {
                   out.glyphs.push_back({lp.fonts[lp.font_indices[i]], lp.glyph_ids[i], uint32_t(lp.clusters[i] + dst),
                                         xa + lp.points[i].x, ya + lp.points[i].y});
                 }
                 for (size_t i = 0; i < lp.advances.size(); ++i) {
                   out.advances[i + dst] = lp.advances[i] + (i == 0 ? word_spacing : 0.0f);
                 }
                 out.advance += lp.advance + word_spacing;
               });
  }
  // adjustGlyphLetterSpacingEdge
  const float half = paint.letter_spacing * paint.size * paint.scale_x * 0.5f;
  std::u16string_view sub = text.substr(start, count);
  auto shift = [&](LayoutGlyph& g) {
    if (paint.vertical_text) {
      g.y -= half;
    } else {
      g.x -= half;
    }
  };
  size_t n = out.glyphs.size();
  if (run_flag & kRunLeftEdge) {
    size_t i = 0;
    for (; i < n; ++i) {
      if (!is_control(code_point_at(sub, out.glyphs[i].cluster))) break;
    }
    for (; i < n; ++i) {
      if (!letter_spacing_code_point(code_point_at(sub, out.glyphs[i].cluster))) break;
      shift(out.glyphs[i]);
    }
  }
  if (run_flag & kRunRightEdge) {
    size_t i = 0;
    for (; i < n; ++i) {
      char32_t c = code_point_at(sub, out.glyphs[n - 1 - i].cluster);
      if (!is_control(c)) {
        if (!letter_spacing_code_point(c)) i = n;
        break;
      }
    }
    if (i < n) {
      for (size_t j = n - i; j < n; ++j) shift(out.glyphs[j]);
    }
  }
  out.advance = adjust_advance_edges(text, range, runs, paint, run_flag, out.advance, out.advances.data());
  return out;
}

// --- Measurement.cpp and GraphemeBreak.cpp ---

namespace {

bool ascii_or_bidi_control(char16_t c) {
  return c <= 0x001F || c == 0x061C || c == 0x200E || c == 0x200F || (0x202A <= c && c <= 0x202E) ||
         (0x2066 <= c && c <= 0x2069);
}

GraphemeBreakProp tailored_break(char32_t c) {
  if (c == 0x00AD || c == 0x061C || c == 0x180E || c == 0x200B || c == 0x200E || c == 0x200F ||
      (0x202A <= c && c <= 0x202E) || (c | 0xF) == 0x206F || c == 0xFEFF || (c | 0x7F) == 0xE007F) {
    return GraphemeBreakProp::kExtend;
  }
  if (c == 0x0E33) return GraphemeBreakProp::kOther;  // THAI CHARACTER SARA AM
  return unicode_services().grapheme_break(c);
}

bool pure_killer(char32_t c) {
  return c == 0x0E3A || c == 0x0E4E || c == 0x0F84 || c == 0x103A || c == 0x1714 || c == 0x1734 || c == 0x17D1 ||
         c == 0x1BAA || c == 0x1BF2 || c == 0x1BF3 || c == 0xA806 || c == 0xA953 || c == 0xABED || c == 0x11134 ||
         c == 0x112EA || c == 0x1172B;
}

bool extended_pictographic(char32_t c) {
  return unicode_services().has_property(c, EmojiProperty::kExtendedPictographic);
}

float run_advance_from(const float* advances, const char16_t* buf, size_t layout_start, size_t start, size_t count,
                       size_t offset) {
  float advance = 0.0f;
  size_t last_cluster = start;
  float cluster_width = 0.0f;
  for (size_t i = start; i < offset; i++) {
    float a = advances[i - layout_start];
    if (a != 0.0f) {
      advance += a;
      last_cluster = i;
      cluster_width = a;
    }
  }
  if (offset < start + count && !ascii_or_bidi_control(buf[offset]) && advances[offset - layout_start] == 0.0f) {
    // Inside a cluster: each grapheme cluster gets an equal share.
    size_t next_cluster;
    for (next_cluster = offset + 1; next_cluster < start + count; next_cluster++) {
      if (advances[next_cluster - layout_start] != 0.0f || ascii_or_bidi_control(buf[next_cluster])) break;
    }
    int graphemes = 0, graphemes_after = 0;
    for (size_t i = last_cluster; i < next_cluster; i++) {
      if (is_grapheme_break(advances + (start - layout_start), buf, start, count, i)) {
        graphemes++;
        if (i >= offset) graphemes_after++;
      }
    }
    if (graphemes > 0) advance -= cluster_width * float(graphemes_after) / float(graphemes);
  }
  return advance;
}

}  // namespace

bool is_grapheme_break(const float* advances, const char16_t* buf, size_t start, size_t count, size_t offset) {
  using G = GraphemeBreakProp;
  if (offset <= start || offset >= start + count) return true;
  if (is_trail_surrogate(buf[offset])) return !is_lead_surrogate(buf[offset - 1]);
  size_t back = offset, forward = offset;
  char32_t c1 = prev_code_point(buf, start, &back);
  char32_t c2 = next_code_point(buf, &forward, start + count);
  G p1 = tailored_break(c1), p2 = tailored_break(c2);
  if (p1 == G::kCR && p2 == G::kLF) return false;
  if (p1 == G::kControl || p1 == G::kCR || p1 == G::kLF) return true;
  if (p2 == G::kControl || p2 == G::kCR || p2 == G::kLF) return true;
  if (p1 == G::kL && (p2 == G::kL || p2 == G::kV || p2 == G::kLV || p2 == G::kLVT)) return false;
  if ((p1 == G::kLV || p1 == G::kV) && (p2 == G::kV || p2 == G::kT)) return false;
  if ((p1 == G::kLVT || p1 == G::kT) && p2 == G::kT) return false;
  bool c2_has_advance = advances != nullptr && advances[offset - start] != 0.0;
  if (c2_has_advance) return true;
  if (p2 == G::kExtend || p2 == G::kZwj || p2 == G::kSpacingMark || p1 == G::kPrepend) return false;
  if (back > start && p1 == G::kZwj && extended_pictographic(c2)) {
    size_t backback = back;
    char32_t c0 = prev_code_point(buf, start, &backback);
    G p0 = tailored_break(c0);
    while (p0 == G::kExtend && backback > start) {
      c0 = prev_code_point(buf, start, &backback);
      p0 = tailored_break(c0);
    }
    if (extended_pictographic(c0)) return false;
  }
  if (p1 == G::kRegionalIndicator && p2 == G::kRegionalIndicator) {
    if (advances != nullptr) return false;
    size_t barrier = back > start + 1000 ? back - 1000 : start;
    size_t backback = back;
    while (backback > barrier) {
      char32_t c0 = prev_code_point(buf, barrier, &backback);
      if (tailored_break(c0) != G::kRegionalIndicator) {
        backback += utf16_length(c0);
        break;
      }
    }
    return (offset - backback) % 4 == 0;
  }
  if (combining_class(c1) == 9 && !pure_killer(c1) && is_other_letter(c2)) return false;  // virama + letter
  return true;
}

size_t text_run_cursor(const float* advances, const char16_t* buf, size_t start, size_t count, size_t offset,
                       CursorOpt opt) {
  switch (opt) {
    case CursorOpt::kAfter:
      if (offset < start + count) offset++;
      [[fallthrough]];
    case CursorOpt::kAtOrAfter:
      while (!is_grapheme_break(advances, buf, start, count, offset)) offset++;
      break;
    case CursorOpt::kBefore:
      if (offset > start) offset--;
      [[fallthrough]];
    case CursorOpt::kAtOrBefore:
      while (!is_grapheme_break(advances, buf, start, count, offset)) offset--;
      break;
    case CursorOpt::kAt:
      if (!is_grapheme_break(advances, buf, start, count, offset)) offset = size_t(-1);
      break;
  }
  return offset;
}

float run_advance(const float* advances, const char16_t* buf, size_t start, size_t count, size_t offset) {
  return run_advance_from(advances, buf, start, start, count, offset);
}

void distribute_advances(float* advances, const char16_t* buf, size_t start, size_t count) {
  size_t cluster_start = start;
  while (cluster_start < start + count) {
    float cluster_advance = advances[cluster_start - start];
    size_t cluster_end;
    for (cluster_end = cluster_start + 1; cluster_end < start + count; cluster_end++) {
      if (advances[cluster_end - start] != 0.0f || ascii_or_bidi_control(buf[cluster_end])) break;
    }
    size_t graphemes = 0;
    for (size_t i = cluster_start; i < cluster_end; i++) {
      if (is_grapheme_break(advances, buf, start, count, i)) graphemes++;
    }
    if (graphemes > 1) {
      for (size_t i = cluster_start; i < cluster_end; ++i) {
        if (is_grapheme_break(advances, buf, start, count, i)) advances[i - start] = cluster_advance / float(graphemes);
      }
    }
    cluster_start = cluster_end;
  }
}

size_t offset_for_advance(const float* advances, const char16_t* buf, size_t start, size_t count, float advance) {
  float x = 0.0f, x_last_cluster_start = 0.0f, x_search_start = 0.0f;
  size_t last_cluster_start = start, search_start = start;
  size_t max = start + count;
  for (size_t i = start; i < max; i++) {
    if (is_grapheme_break(advances, buf, start, count, i)) {
      search_start = last_cluster_start;
      x_search_start = x_last_cluster_start;
    }
    float width = advances[i - start];
    if (width != 0.0f) {
      last_cluster_start = i;
      x_last_cluster_start = x;
      x += width;
      if (x > advance) break;
    }
  }
  size_t best = search_start;
  float best_dist = FLT_MAX;
  for (size_t i = search_start; i <= max; i++) {
    if (is_grapheme_break(advances, buf, start, count, i)) {
      float delta = run_advance_from(advances, buf, start, search_start, max - search_start, i) + x_search_start - advance;
      if (std::abs(delta) < best_dist) {
        best_dist = std::abs(delta);
        best = i;
      }
      if (delta >= 0.0f) break;
    }
  }
  return best;
}

}  // namespace aa::text
