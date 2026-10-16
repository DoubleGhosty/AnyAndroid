#pragma once

// Builds minimal TrueType fonts for the text engine tests: rectangular
// glyphs with chosen advances, vertical metrics and kerning pairs, so that
// no font file extracted from an Android image is needed.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aa::test {

struct TestGlyph {
  char32_t code = 0;  // 0: no code point (the .notdef glyph)
  uint16_t advance = 0;
  int16_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;  // empty when equal
};

struct TestFont {
  uint16_t upem = 1000;
  int16_t ascender = 800, descender = -200, line_gap = 0;  // hhea
  int16_t typo_ascender = 750, typo_descender = -250, typo_line_gap = 100;
  bool use_typo_metrics = false;  // OS/2 fsSelection bit 7
  bool italic = false;
  uint16_t weight = 400;
  int16_t x_height = 500, cap_height = 700;
  int16_t underline_position = -100, underline_thickness = 50;
  int16_t strikeout_size = 50, strikeout_position = 250;
  std::vector<TestGlyph> glyphs;  // glyph 0 first
  std::vector<std::pair<std::pair<uint16_t, uint16_t>, int16_t>> kerning;  // (left, right) glyphs

  std::vector<uint8_t> build() const;
};

namespace font_detail {

struct Writer {
  std::vector<uint8_t> b;
  void u8(uint32_t v) { b.push_back(uint8_t(v)); }
  void u16(uint32_t v) {
    u8(v >> 8);
    u8(v);
  }
  void i16(int32_t v) { u16(uint32_t(uint16_t(int16_t(v)))); }
  void u32(uint32_t v) {
    u16(v >> 16);
    u16(v);
  }
  void tag(const char* t) {
    for (int i = 0; i < 4; ++i) u8(uint8_t(t[i]));
  }
  void zeros(size_t n) { b.insert(b.end(), n, 0); }
  void pad4() {
    while (b.size() % 4) u8(0);
  }
};

inline void search_params(uint16_t n, uint16_t unit, uint16_t* range, uint16_t* selector, uint16_t* shift) {
  uint16_t p = 1, e = 0;
  while (uint32_t(p) * 2 <= n) {
    p = uint16_t(p * 2);
    ++e;
  }
  *range = uint16_t(p * unit);
  *selector = e;
  *shift = uint16_t(n * unit - *range);
}

}  // namespace font_detail

inline std::vector<uint8_t> TestFont::build() const {
  using font_detail::Writer;
  std::map<std::string, std::vector<uint8_t>> tables;
  const uint16_t n = uint16_t(glyphs.size());
  int16_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  uint16_t max_advance = 0;
  bool any = false;
  for (const TestGlyph& g : glyphs) {
    max_advance = std::max(max_advance, g.advance);
    if (g.x_min == g.x_max || g.y_min == g.y_max) continue;
    if (!any) {
      bx0 = g.x_min, by0 = g.y_min, bx1 = g.x_max, by1 = g.y_max;
      any = true;
    }
    bx0 = std::min(bx0, g.x_min), by0 = std::min(by0, g.y_min);
    bx1 = std::max(bx1, g.x_max), by1 = std::max(by1, g.y_max);
  }

  // glyf and loca (long offsets): one rectangle contour per glyph.
  Writer glyf, loca;
  for (const TestGlyph& g : glyphs) {
    loca.u32(uint32_t(glyf.b.size()));
    if (g.x_min == g.x_max || g.y_min == g.y_max) continue;
    glyf.i16(1);
    glyf.i16(g.x_min), glyf.i16(g.y_min), glyf.i16(g.x_max), glyf.i16(g.y_max);
    glyf.u16(3);  // endPtsOfContours
    glyf.u16(0);  // no instructions
    for (int i = 0; i < 4; ++i) glyf.u8(0x01);  // on curve, 16-bit deltas
    // Clockwise: (x0,y0) (x0,y1) (x1,y1) (x1,y0).
    int16_t xs[4] = {g.x_min, g.x_min, g.x_max, g.x_max}, ys[4] = {g.y_min, g.y_max, g.y_max, g.y_min};
    int16_t px = 0;
    for (int16_t x : xs) glyf.i16(x - px), px = x;
    int16_t py = 0;
    for (int16_t y : ys) glyf.i16(y - py), py = y;
    glyf.pad4();
  }
  loca.u32(uint32_t(glyf.b.size()));
  tables["glyf"] = glyf.b;
  tables["loca"] = loca.b;

  Writer head;
  head.u32(0x00010000), head.u32(0x00010000), head.u32(0), head.u32(0x5F0F3CF5);
  head.u16(0x000B), head.u16(upem);
  head.zeros(16);  // created, modified
  head.i16(bx0), head.i16(by0), head.i16(bx1), head.i16(by1);
  head.u16(italic ? 2 : 0), head.u16(8), head.i16(2), head.i16(1), head.i16(0);
  tables["head"] = head.b;

  Writer hhea;
  hhea.u32(0x00010000), hhea.i16(ascender), hhea.i16(descender), hhea.i16(line_gap);
  hhea.u16(max_advance), hhea.i16(bx0), hhea.i16(0), hhea.i16(bx1);
  hhea.i16(1), hhea.i16(0), hhea.i16(0), hhea.zeros(8), hhea.i16(0), hhea.u16(n);
  tables["hhea"] = hhea.b;

  Writer hmtx;
  for (const TestGlyph& g : glyphs) hmtx.u16(g.advance), hmtx.i16(g.x_min);
  tables["hmtx"] = hmtx.b;

  Writer maxp;
  maxp.u32(0x00010000), maxp.u16(n), maxp.u16(4), maxp.u16(1), maxp.u16(0), maxp.u16(0), maxp.u16(2);
  maxp.zeros(16);
  tables["maxp"] = maxp.b;

  Writer os2;
  os2.u16(4), os2.i16(max_advance / 2), os2.u16(weight), os2.u16(5), os2.u16(0);
  os2.zeros(16);  // sub/superscript
  os2.i16(strikeout_size), os2.i16(strikeout_position), os2.i16(0);
  os2.zeros(10 + 16);  // panose, unicode ranges
  os2.tag("TEST");
  os2.u16((italic ? 1 : 0) | (use_typo_metrics ? 0x80 : 0) | (italic ? 0 : 0x40));
  os2.u16(0x20), os2.u16(0xFFFF);
  os2.i16(typo_ascender), os2.i16(typo_descender), os2.i16(typo_line_gap);
  os2.u16(uint16_t(std::max<int>(0, by1))), os2.u16(uint16_t(std::max<int>(0, -by0)));
  os2.zeros(8);
  os2.i16(x_height), os2.i16(cap_height), os2.u16(0), os2.u16(0x20), os2.u16(2);
  tables["OS/2"] = os2.b;

  Writer post;
  post.u32(0x00030000), post.u32(0), post.i16(underline_position), post.i16(underline_thickness);
  post.zeros(20);
  tables["post"] = post.b;

  Writer name;
  const char16_t family[] = u"Test";
  name.u16(0), name.u16(1), name.u16(6 + 12);
  name.u16(3), name.u16(1), name.u16(0x409), name.u16(1), name.u16(8), name.u16(0);
  for (char16_t c : std::u16string(family)) name.u16(c);
  tables["name"] = name.b;

  // cmap: one format 4 subtable (3, 1), one segment per code point.
  std::vector<std::pair<uint16_t, uint16_t>> map;  // code, glyph
  for (uint16_t i = 0; i < n; ++i) {
    if (glyphs[i].code && glyphs[i].code <= 0xFFFE) map.emplace_back(uint16_t(glyphs[i].code), i);
  }
  std::sort(map.begin(), map.end());
  uint16_t segs = uint16_t(map.size() + 1);
  Writer sub;
  uint16_t range, selector, shift;
  font_detail::search_params(segs, 2, &range, &selector, &shift);
  sub.u16(4), sub.u16(uint16_t(16 + segs * 8)), sub.u16(0), sub.u16(uint16_t(segs * 2));
  sub.u16(range), sub.u16(selector), sub.u16(shift);
  for (auto& [c, g] : map) sub.u16(c);
  sub.u16(0xFFFF), sub.u16(0);
  for (auto& [c, g] : map) sub.u16(c);
  sub.u16(0xFFFF);
  for (auto& [c, g] : map) sub.u16(uint16_t(g - c));
  sub.u16(1);
  for (size_t i = 0; i < segs; ++i) sub.u16(0);
  Writer cmap;
  cmap.u16(0), cmap.u16(1), cmap.u16(3), cmap.u16(1), cmap.u32(12);
  cmap.b.insert(cmap.b.end(), sub.b.begin(), sub.b.end());
  tables["cmap"] = cmap.b;

  if (!kerning.empty()) {
    auto pairs = kerning;
    std::sort(pairs.begin(), pairs.end());
    Writer kern;
    uint16_t np = uint16_t(pairs.size());
    kern.u16(0), kern.u16(1);
    kern.u16(0), kern.u16(uint16_t(14 + 6 * np)), kern.u16(0x0001);
    font_detail::search_params(np, 6, &range, &selector, &shift);
    kern.u16(np), kern.u16(range), kern.u16(selector), kern.u16(shift);
    for (auto& [lr, v] : pairs) kern.u16(lr.first), kern.u16(lr.second), kern.i16(v);
    tables["kern"] = kern.b;
  }

  // Offset table and table records, sorted by tag.
  Writer out;
  uint16_t nt = uint16_t(tables.size());
  font_detail::search_params(nt, 16, &range, &selector, &shift);
  out.u32(0x00010000), out.u16(nt), out.u16(range), out.u16(selector), out.u16(shift);
  uint32_t offset = uint32_t(12 + 16 * nt);
  std::vector<uint8_t> body;
  for (auto& [t, data] : tables) {
    out.tag(t.c_str());
    out.u32(0);
    out.u32(offset + uint32_t(body.size()));
    out.u32(uint32_t(data.size()));
    body.insert(body.end(), data.begin(), data.end());
    while (body.size() % 4) body.push_back(0);
  }
  out.b.insert(out.b.end(), body.begin(), body.end());
  return out.b;
}

}  // namespace aa::test
