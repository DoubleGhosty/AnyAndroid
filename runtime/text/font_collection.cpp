// minikin's Font.cpp, FontFakery.cpp, FontFamily.cpp and FontCollection.cpp
// (frameworks/minikin), typeface_redesign_readonly path.
#include "text/font_collection.h"

#include <hb.h>

#include <algorithm>

#include "text/locale.h"
#include "text/unicode.h"

namespace aa::text {

namespace {

constexpr uint32_t kTagWght = tag('w', 'g', 'h', 't');
constexpr uint32_t kTagItal = tag('i', 't', 'a', 'l');
constexpr uint32_t kTagSlnt = tag('s', 'l', 'n', 't');

}  // namespace

VariationSettings sorted_variations(std::vector<Variation> v) {
  std::stable_sort(v.begin(), v.end(), [](const Variation& a, const Variation& b) { return a.tag < b.tag; });
  VariationSettings out;
  for (const Variation& x : v) {
    if (!out.empty() && out.back().tag == x.tag) {
      out.back() = x;
    } else {
      out.push_back(x);
    }
  }
  return out;
}

bool VariationLess::operator()(const VariationSettings& a, const VariationSettings& b) const {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](const Variation& x, const Variation& y) {
    return x.tag != y.tag ? x.tag < y.tag : x.value < y.value;
  });
}

// --- Font ------------------------------------------------------------------

Font::Font(std::shared_ptr<Face> face, FontStyle style, uint32_t locale_list_id)
    : face_(std::move(face)), style_(style), locale_list_id_(locale_list_id) {}

const Axis* Font::fvar_axis(uint32_t t) const {
  for (const Axis& a : face_->axes()) {
    if (a.tag == t) return &a;
  }
  return nullptr;
}

bool Font::axis_supported(uint32_t t) const { return fvar_axis(t) != nullptr; }

std::shared_ptr<Face> Font::adjusted(const VariationSettings& axes) const {
  if (axes.empty()) return face_;
  std::lock_guard<std::mutex> guard(lock_);
  if (auto it = adjusted_.find(axes); it != adjusted_.end()) return it->second;
  std::vector<Variation> filtered;
  for (const Variation& v : axes) {
    const Axis* a = fvar_axis(v.tag);
    if (a && a->def != v.value) filtered.push_back(v);
  }
  std::shared_ptr<Face> face = face_->with_variations(filtered);
  if (!face) face = face_;
  constexpr size_t kMaxAdjusted = 64;
  if (adjusted_.size() >= kMaxAdjusted) adjusted_.erase(adjusted_.begin());
  adjusted_.emplace(axes, face);
  return face;
}

// --- FontFakery merge --------------------------------------------------------

namespace {

// minikin's merge (FontFakery.cpp): the font's own axes, those asked for,
// and those that the requested style implies, merged by tag.
FontFakery merge(const Font& font, const VariationSettings& base, const VariationSettings& target,
                 FontStyle base_style, FontStyle target_style) {
  bool has_ital = font.axis_supported(kTagItal);
  bool has_slnt = font.axis_supported(kTagSlnt);
  bool has_wght = font.axis_supported(kTagWght);

  Variation style_vars[2];
  uint32_t style_count = 0;
  if (has_slnt) {
    style_vars[style_count++] = {kTagSlnt, target_style.italic ? -10.0f : 0.0f};
  } else if (has_ital) {
    style_vars[style_count++] = {kTagItal, target_style.italic ? 1.0f : 0.0f};
  }
  if (has_wght) style_vars[style_count++] = {kTagWght, float(target_style.weight)};

  constexpr uint32_t kEnd = 0xFFFFFFFF;
  bool fake_bold = false;
  uint32_t bi = 0, ti = 0, si = 0;
  VariationSettings adjusted;
  while (bi < base.size() || ti < target.size() || si < style_count) {
    uint32_t bt = bi < base.size() ? base[bi].tag : kEnd;
    uint32_t tt = ti < target.size() ? target[ti].tag : kEnd;
    uint32_t st = si < style_count ? style_vars[si].tag : kEnd;
    uint32_t t;
    float value;
    bool style_used = false;
    if (bt < tt) {
      if (st < bt) {
        t = st, value = style_vars[si].value, style_used = true, si++;
      } else if (st == bt) {
        t = st, value = base[bi].value, bi++, si++;
      } else {
        t = bt, value = base[bi].value, bi++;
      }
    } else if (tt < bt) {
      if (st < tt) {
        t = st, value = style_vars[si].value, style_used = true, si++;
      } else if (st == tt) {
        t = tt, value = target[ti].value, si++, ti++;
      } else {
        t = tt, value = target[ti].value, ti++;
      }
    } else {
      if (st < bt) {
        t = st, value = style_vars[si].value, style_used = true, si++;
      } else if (st == bt) {
        t = tt, value = target[ti].value, bi++, ti++, si++;
      } else {
        t = tt, value = target[ti].value, bi++, ti++;
      }
    }
    const Axis* axis = font.fvar_axis(t);
    if (!axis) continue;                              // unsupported axis
    if (style_used && value == axis->def) continue;   // the default, from the style
    float clamped = std::clamp(value, axis->min, axis->max);
    adjusted.push_back({t, clamped});
    if (t == kTagWght) {
      // Fake bold when the font stops 200 or more below the request.
      fake_bold = target_style.weight >= 600 && (float(target_style.weight) - clamped) >= 200;
    }
  }
  if (!has_wght) fake_bold = target_style.weight >= 600 && (target_style.weight - base_style.weight) >= 200;
  bool fake_italic = false;
  if (target_style.italic) fake_italic = (has_ital || has_slnt) ? false : !base_style.italic;
  return FontFakery{fake_bold, fake_italic, std::move(adjusted)};
}

int match_score(FontStyle a, FontStyle b) {
  if (a == b) return 0;
  int score = std::abs(a.weight / 100 - b.weight / 100);
  if (a.italic != b.italic) score += 2;
  return score;
}

}  // namespace

// --- FontFamily --------------------------------------------------------------

FontFamily::FontFamily(uint32_t locale_list_id, FamilyVariant variant, std::vector<std::shared_ptr<const Font>> fonts,
                       bool custom_fallback, bool default_fallback, VariationFamilyType variation_type)
    : fonts_(std::move(fonts)),
      locale_list_id_(locale_list_id),
      variant_(variant),
      custom_fallback_(custom_fallback),
      default_fallback_(default_fallback),
      variation_type_(variation_type),
      color_emoji_(locale_list(locale_list_id).emoji_style() == EmojiStyle::kEmoji) {
  if (fonts_.empty()) return;
  const std::shared_ptr<const Font>& font = closest_match(FontStyle{}).font;
  coverage_ = cmap_coverage(font->face()->table(tag('c', 'm', 'a', 'p')), &has_vs_table_);
}

FakedFont FontFamily::closest_match(FontStyle style, const VariationSettings& axes) const {
  size_t best = 0;
  int best_score = match_score(fonts_[0]->style(), style);
  for (size_t i = 1; i < fonts_.size(); ++i) {
    int score = match_score(fonts_[i]->style(), style);
    if (score < best_score) {
      best = i;
      best_score = score;
    }
  }
  const Font& font = *fonts_[best];
  if (axes.empty() && style == font.style()) return FakedFont{fonts_[best], FontFakery{}};
  return FakedFont{fonts_[best], merge(font, sorted_variations(font.face()->variations()), axes, font.style(), style)};
}

bool FontFamily::has_glyph(char32_t c, char32_t selector) const {
  if (selector == 0) return coverage_.contains(c);
  if (!has_vs_table_ || vs_index(selector) == kInvalidVsIndex) return false;
  return closest_match(FontStyle{}).font->face()->glyph(c, selector) != 0;
}

// --- FontCollection ------------------------------------------------------------

namespace {

constexpr uint32_t kUnsupportedFontScore = 0;
constexpr uint32_t kFirstFontScore = 0xFFFFFFFF;
constexpr char32_t kEmojiStyleVs = 0xFE0F;
constexpr char32_t kTextStyleVs = 0xFE0E;

// Characters that never need a glyph of their own: the current font run
// continues over them.
bool needs_no_font_support(char32_t c) {
  return c == 0x00AD || c == 0x034F || c == 0x061C || (0x200C <= c && c <= 0x200F) || (0x202A <= c && c <= 0x202E) ||
         (0x2066 <= c && c <= 0x2069) || c == 0xFEFF || is_variation_selector(c);
}

// Characters that stay in the current font when it has them.
bool sticky(char32_t c) {
  switch (c) {
    case '!': case ',': case '-': case '.': case ':': case ';': case '?':
    case 0x00A0: case 0x2010: case 0x2011: case 0x202F: case 0x2640: case 0x2642: case 0x2695:
      return true;
    default:
      return false;
  }
}

bool emoji_break(char32_t prev, char32_t c) {
  return !(is_emoji_modifier(c) || (is_regional_indicator(prev) && is_regional_indicator(c)) || is_keycap(c) ||
           is_tag_char(c) || c == kZwj || prev == kZwj);
}

std::vector<uint8_t> intersect(const std::vector<uint8_t>& l, const std::vector<uint8_t>& r) {
  if (l == r) return l;
  std::vector<uint8_t> out;
  size_t li = 0, ri = 0;
  while (li < l.size() && ri < r.size()) {
    if (l[li] < r[ri]) {
      li++;
    } else if (l[li] > r[ri]) {
      ri++;
    } else {
      out.push_back(l[li]);
      li++, ri++;
    }
  }
  return out;
}

uint32_t locale_score(uint32_t user_id, const FontFamily& family) {
  const LocaleList& user = locale_list(user_id);
  const LocaleList& font = locale_list(family.locale_list_id());
  size_t n = std::min(user.size(), kFontLocaleLimit);
  uint32_t score = 0;
  for (size_t i = 0; i < n; ++i) score = score * 5u + uint32_t(user[i].score_for(font));
  return score;
}

uint32_t variant_score(FamilyVariant variant, const FontFamily& family) {
  FamilyVariant fv = family.variant();
  if (fv == FamilyVariant::kDefault || fv == variant) return 1;
  if (variant == FamilyVariant::kDefault && fv == FamilyVariant::kCompact) return 1;
  return 0;
}

// getGlyphScore (lower is better). As minikin, the tag-sequence check reads
// the start of the whole text, not of the run.
uint32_t glyph_score(std::u16string_view text, uint32_t start, uint32_t end, hb_font_t* font) {
  hb_buffer_t* buffer = hb_buffer_create();
  hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
  hb_buffer_add_utf16(buffer, reinterpret_cast<const uint16_t*>(text.data()) + start, int(end - start), 0,
                      int(end - start));
  hb_buffer_guess_segment_properties(buffer);
  hb_shape(font, buffer, nullptr, 0);
  unsigned glyphs = 0;
  hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &glyphs);
  uint32_t score = glyphs;
  if (glyphs == 1) {
    constexpr uint32_t kTagSequenceFallbackPenalty = 0x10000;
    size_t i = 0;
    char32_t c = next_code_point(text.data(), &i, text.size());
    if (!is_surrogate(c) && i < text.size()) {
      char32_t next = next_code_point(text.data(), &i, text.size());
      if (is_tag_char(next)) {
        uint32_t composed = info[0].codepoint;
        hb_buffer_reset(buffer);
        hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
        uint32_t cp = c;
        hb_buffer_add_codepoints(buffer, &cp, 1, 0, 1);
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(font, buffer, nullptr, 0);
        info = hb_buffer_get_glyph_infos(buffer, &glyphs);
        score = glyphs;
        if (glyphs == 1 && info[0].codepoint == composed) score = glyphs + kTagSequenceFallbackPenalty;
      }
    }
  }
  hb_buffer_destroy(buffer);
  return score;
}

}  // namespace

FontCollection::FontCollection(std::vector<std::shared_ptr<const FontFamily>> families) {
  for (auto& f : families) {
    if (!f || f->font_count() == 0) continue;
    if (f->has_vs_table()) vs_families_.push_back(f.get());
    max_char_ = std::max(max_char_, f->coverage().length());
    families_.push_back(std::move(f));
  }
}

bool FontCollection::primary(const FontFamily& family) const {
  if (family.custom_fallback()) return true;
  if (families_[0]->custom_fallback()) return false;
  return family.default_fallback();
}

uint32_t FontCollection::coverage_score(char32_t c, char32_t vs, uint32_t locale_list_id,
                                        const FontFamily& family) const {
  bool has_vs_glyph = vs != 0 && family.has_glyph(c, vs);
  if (!has_vs_glyph && !family.coverage().contains(c)) return kUnsupportedFontScore;
  if ((vs == 0 || has_vs_glyph) && primary(family)) return kFirstFontScore;
  if (vs != 0 && has_vs_glyph) return 3;
  bool color_request;
  if (vs == kEmojiStyleVs) {
    color_request = true;
  } else if (vs == kTextStyleVs) {
    color_request = false;
  } else {
    switch (locale_list(locale_list_id).emoji_style()) {
      case EmojiStyle::kEmoji: color_request = true; break;
      case EmojiStyle::kText: color_request = false; break;
      default: return 1;
    }
  }
  return color_request == family.color_emoji() ? 2 : 1;
}

uint32_t FontCollection::family_score(char32_t c, char32_t vs, FamilyVariant variant, uint32_t locale_list_id,
                                      const FontFamily& family) const {
  uint32_t coverage = coverage_score(c, vs, locale_list_id, family);
  if (coverage == kFirstFontScore || coverage == kUnsupportedFontScore) return coverage;
  return coverage << 29 | locale_score(locale_list_id, family) << 1 | variant_score(variant, family);
}

std::vector<uint8_t> FontCollection::family_for_char(char32_t c, char32_t vs, uint32_t locale_list_id,
                                                     FamilyVariant variant) const {
  if (c >= max_char_) return {0};
  uint32_t best = kUnsupportedFontScore;
  std::vector<uint8_t> result;
  for (size_t i = 0; i < families_.size() && i < 256; ++i) {
    const FontFamily& f = *families_[i];
    if (vs == 0 && !f.coverage().contains(c)) continue;  // minikin's per-page family lists
    uint32_t score = family_score(c, vs, variant, locale_list_id, f);
    if (score == kFirstFontScore) return {uint8_t(i)};
    if (score != kUnsupportedFontScore && score >= best) {
      if (score > best) {
        result.clear();
        best = score;
      }
      result.push_back(uint8_t(i));
    }
  }
  if (result.empty()) {
    char32_t first = first_of_decomposition(c);
    if (first != c) return family_for_char(first, vs, locale_list_id, variant);
    return {0};
  }
  return result;
}

bool FontCollection::has_variation_selector(char32_t base, char32_t selector) const {
  if (!is_variation_selector(selector) || base >= max_char_) return false;
  for (const FontFamily* f : vs_families_) {
    if (f->has_glyph(base, selector)) return true;
  }
  if (selector == kTextStyleVs) {
    for (const auto& f : families_) {
      if (!f->color_emoji() && f->has_glyph(base, 0)) return true;
    }
  }
  return false;
}

std::vector<FontCollection::Run> FontCollection::itemize(std::u16string_view text, FontStyle, uint32_t locale_list_id,
                                                         FamilyVariant variant, uint32_t run_max) const {
  std::vector<Run> result;
  const char16_t* s = text.data();
  const size_t size = text.size();
  if (size == 0) return result;
  constexpr char32_t kEndOfString = 0xFFFFFFFF;
  std::vector<uint8_t> last;
  Run* run = nullptr;
  size_t read = 0, next_pos = 0;
  char32_t prev = 0;
  char32_t next = next_code_point(s, &read, size);
  if (is_surrogate(next)) next = kReplacementChar;
  do {
    const char32_t c = next;
    const size_t pos = next_pos;
    next_pos = read;
    if (read < size) {
      next = next_code_point(s, &read, size);
      if (is_surrogate(next)) next = kReplacementChar;
    } else {
      next = kEndOfString;
    }

    bool continue_run = false;
    if (needs_no_font_support(c)) {
      continue_run = true;
    } else if (!last.empty() && (sticky(c) || is_combining(c))) {
      const FontFamily& last_family = *families_[last[0]];
      if (last_family.color_emoji()) {
        for (uint8_t ix : last) continue_run |= families_[ix]->coverage().contains(c);
      } else {
        continue_run = last_family.coverage().contains(c);
      }
    }

    if (!continue_run) {
      std::vector<uint8_t> families =
          family_for_char(c, is_variation_selector(next) ? next : 0, locale_list_id, variant);
      bool break_run;
      if (pos == 0 || last.empty()) {
        break_run = true;
      } else if (families_[last[0]]->color_emoji()) {
        std::vector<uint8_t> common = intersect(families, last);
        if (common.empty()) {
          break_run = true;
        } else {
          break_run = emoji_break(prev, c);
          if (!break_run) {
            families = common;
            last = common;
            run->families = common;
          }
        }
      } else {
        break_run = families[0] != last[0];
      }

      if (break_run) {
        size_t start = pos;
        // A combining mark or emoji modifier whose font also has the previous
        // character takes that character into its run.
        if (pos != 0 && (is_combining(c) || (is_emoji_modifier(c) && is_emoji_base(prev)))) {
          for (uint8_t ix : families) {
            if (families_[ix]->coverage().contains(prev)) {
              size_t prev_length = utf16_length(prev);
              if (run) {
                run->end -= int(prev_length);
                if (run->start == run->end) result.pop_back();
              }
              start -= prev_length;
              break;
            }
          }
        }
        if (last.empty()) start = 0;  // characters without font needs go to the first run
        result.push_back(Run{families, int(start), 0});
        run = &result.back();
        last = run->families;
      }
    }
    prev = c;
    if (run) run->end = int(next_pos);
    if (result.size() >= 2 && run_max == result.size() - 2) break;
  } while (next != kEndOfString);

  if (last.empty()) result.push_back(Run{{0}, 0, int(size)});
  if (result.size() > run_max) result.resize(run_max);
  return result;
}

FakedFont FontCollection::best_font(std::u16string_view text, const Run& run, FontStyle style,
                                    const VariationSettings& axes) const {
  uint8_t best = run.families[0];
  if (families_[best]->color_emoji() && run.families.size() > 1) {
    uint32_t best_score = 0xFFFFFFFF;
    for (uint8_t ix : run.families) {
      hb_font_t* font = families_[ix]->font(0)->face()->hb_font();
      uint32_t score = glyph_score(text, uint32_t(run.start), uint32_t(run.end), font);
      if (score < best_score) {
        best = ix;
        best_score = score;
      }
    }
  }
  return families_[best]->closest_match(style, axes);
}

}  // namespace aa::text
