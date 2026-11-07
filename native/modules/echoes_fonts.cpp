// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_fonts.h"
#include "acnh_bytes.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
namespace echoes {
namespace {
constexpr size_t PixelBudget = 16 * 1024 * 1024;
constexpr uint32_t AtlasWidth = 1024, PageHeight = 512, MaxDense = 65536;
bool Scalar(uint32_t cp) {
  return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}
bool Utf8(std::string_view text, std::vector<uint32_t> &result) {
  if (text.size() > 8192)
    return false;
  for (size_t i = 0; i < text.size();) {
    uint8_t c = uint8_t(text[i++]);
    uint32_t cp = c;
    size_t n = 0;
    uint32_t minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) {
      cp = c & 31;
      n = 1;
      minimum = 128;
    } else if (c >= 0xe0 && c <= 0xef) {
      cp = c & 15;
      n = 2;
      minimum = 2048;
    } else if (c >= 0xf0 && c <= 0xf4) {
      cp = c & 7;
      n = 3;
      minimum = 65536;
    } else if (c >= 128)
      return false;
    if (n > text.size() - i)
      return false;
    while (n--) {
      uint8_t b = uint8_t(text[i++]);
      if ((b & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (b & 63);
    }
    if (cp < minimum || !Scalar(cp) || cp < 32)
      return false;
    result.push_back(cp);
  }
  return true;
}
// A line may not start with closing punctuation or small kana, nor end with
// opening punctuation.
bool NoBreakBefore(uint32_t cp) {
  static constexpr std::u32string_view closing =
      U"、。，．・：；？！）」』】〕〉》〗〙｝］ー々ゝゞぁぃぅぇぉっゃゅょゎ"
      U"ァィゥェォッャュョヮヵヶ…‥!),.:;?]}";
  return closing.find(char32_t(cp)) != std::u32string_view::npos;
}
bool NoBreakAfter(uint32_t cp) {
  static constexpr std::u32string_view opening = U"（「『【〔〈《〖〘｛［([{";
  return opening.find(char32_t(cp)) != std::u32string_view::npos;
}
// Splits a space-free word where CJK text may wrap.
std::vector<std::string_view> Segments(std::string_view word) {
  std::vector<std::string_view> out;
  uint32_t previous = 0;
  size_t start = 0;
  for (size_t at = 0; at < word.size();) {
    const size_t begin = at;
    uint32_t cp = uint8_t(word[at++]);
    const size_t n = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    cp &= n ? 0x3fu >> n : 0x7fu;
    for (size_t i = 0; i < n && at < word.size(); ++i)
      cp = cp << 6 | (uint8_t(word[at++]) & 63);
    if (begin && (CjkCodepoint(previous) || CjkCodepoint(cp)) &&
        !NoBreakBefore(cp) && !NoBreakAfter(previous)) {
      out.push_back(word.substr(start, begin - start));
      start = begin;
    }
    previous = cp;
  }
  out.push_back(word.substr(start));
  return out;
}
} // namespace
bool CjkCodepoint(uint32_t cp) {
  return ((cp >= 0x2e80 && cp <= 0x9fff) && !(cp >= 0x3130 && cp <= 0x318f)) ||
         (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
         (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
         (cp >= 0x20000 && cp <= 0x3ffff);
}
std::vector<uint32_t> EnglishBodyCodepoints() {
  std::vector<uint32_t> cp;
  for (uint32_t i = 32; i <= 126; ++i)
    cp.push_back(i);
  cp.insert(cp.end(), {0x2018, 0x2019, 0x201c, 0x201d, 0x2013, 0x2014, 0x2026});
  return cp;
}
bool BodyAtlas(const NativeFonts &native, std::span<const uint32_t> required,
               FontAtlas &out, uint32_t cap_height) {
  out = {};
  if (!native.compatible || !native.locale_valid ||
      !native.body_font_supported || !native.standard_font_valid ||
      native.standard_font.empty() || cap_height < 22 || cap_height > 36 ||
      required.empty() || required.size() > 8192)
    return false;
  // The reader's checksum-validated owned snapshot is the trust boundary. The
  // caller must preserve these immutable bytes; arbitrary external TTFs are not
  // an input format for this helper (stb_truetype is not a hostile-file
  // parser). Chinese and Korean faces come first, then Standard, like the
  // native shared-font priority.
  const auto families = LocaleFontFamilies(native.locale_flag);
  if (native.locale_fonts.size() != families.size())
    return false;
  std::vector<const std::vector<uint8_t> *> faces;
  for (size_t i = 0; i < families.size(); ++i) {
    const auto &copy = native.locale_fonts[i];
    if (copy.family != families[i] ||
        !ValidateNativeFontBytes(copy.bytes, families[i]))
      return false;
    faces.push_back(&copy.bytes);
  }
  const auto &bytes = native.standard_font;
  if (bytes.size() > MaxNativeFontBytes ||
      !(native.fallback ? families.empty() && !native.standard_family.empty() &&
                              ValidateNativeFontBytes(bytes, native.standard_family)
                        : ValidateStandardFontBytes(bytes)))
    return false;
  faces.push_back(&bytes);
  std::vector<stbtt_fontinfo> infos(faces.size());
  for (size_t i = 0; i < faces.size(); ++i)
    if (!stbtt_InitFont(&infos[i], faces[i]->data(), 0))
      return false;
  auto &info = infos.back();
  std::set<uint32_t> cps(required.begin(), required.end());
  cps.insert(32);
  if (!Scalar(*cps.begin()) || *cps.rbegin() > 0x10ffff ||
      *cps.rbegin() - *cps.begin() + 1 > MaxDense)
    return false;
  int hx0{}, hy0{}, hx1{}, hy1{};
  if (!stbtt_GetCodepointBox(&info, 'H', &hx0, &hy0, &hx1, &hy1) || hy1 <= hy0)
    return false;
  const float scale = float(cap_height) / float(hy1 - hy0);
  // Standard's H fixes the em; the other faces share that em size.
  const float em = scale / stbtt_ScaleForMappingEmToPixels(&info, 1);
  std::vector<float> scales;
  for (auto &face : infos)
    scales.push_back(&face == &info ? scale
                                    : stbtt_ScaleForMappingEmToPixels(&face, em));
  struct Raster {
    uint32_t cp;
    size_t face;
    int x0, y0, w, h, advance;
    uint32_t x, y;
  };
  std::vector<Raster> glyphs;
  uint32_t x = 1, y = 1, shelf = 0, page = 0;
  for (uint32_t cp : cps) {
    size_t face = 0;
    while (face < infos.size() && !stbtt_FindGlyphIndex(&infos[face], int(cp)))
      ++face;
    if (!Scalar(cp) || face == infos.size())
      return false;
    const float s = scales[face];
    int x0{}, y0{}, x1{}, y1{}, advance{}, bearing{};
    stbtt_GetCodepointBitmapBox(&infos[face], int(cp), s, s, &x0, &y0, &x1,
                                &y1);
    stbtt_GetCodepointHMetrics(&infos[face], int(cp), &advance, &bearing);
    if (x1 < x0 || y1 < y0 || x1 - x0 > 1022 || y1 - y0 > 510 ||
        x0 < INT16_MIN || x0 > INT16_MAX || -y0 < INT16_MIN || -y0 > INT16_MAX)
      return false;
    uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
    if (x + w + 1 > AtlasWidth) {
      x = 1;
      y += shelf;
      shelf = 0;
    }
    if (y + h + 1 > PageHeight) {
      ++page;
      x = 1;
      y = 1;
      shelf = 0;
    }
    if ((size_t(page) + 1) * AtlasWidth * PageHeight * 4 > PixelBudget)
      return false;
    int ax = int(std::lround(float(advance) * s));
    if (ax < 0 || ax > UINT16_MAX)
      return false;
    glyphs.push_back(
        {cp, face, x0, y0, int(w), int(h), ax, x, page * PageHeight + y});
    x += w + 2;
    shelf = std::max(shelf, h + 2);
  }
  FontAtlas result;
  result.image = acnh::Image(int(AtlasWidth), int((page + 1) * PageHeight));
  result.first_codepoint = *cps.begin();
  result.line_height = cap_height;
  result.glyphs.resize(*cps.rbegin() - result.first_codepoint + 1);
  for (size_t i = 0; i < result.image.rgba.size(); i += 4)
    result.image.rgba[i] = result.image.rgba[i + 1] = result.image.rgba[i + 2] =
        255;
  for (const auto &g : glyphs) {
    std::vector<uint8_t> mask(size_t(g.w) * size_t(g.h));
    if (g.w && g.h)
      stbtt_MakeCodepointBitmap(&infos[g.face], mask.data(), g.w, g.h, g.w,
                                scales[g.face], scales[g.face], int(g.cp));
    for (int iy = 0; iy < g.h; ++iy)
      for (int ix = 0; ix < g.w; ++ix)
        result.image
            .rgba[((size_t(g.y) + size_t(iy)) * AtlasWidth + g.x + size_t(ix)) *
                      4 +
                  3] = mask[size_t(iy) * size_t(g.w) + size_t(ix)];
    result.glyphs[g.cp - result.first_codepoint] = {
        uint16_t(g.x), uint16_t(g.y),  uint16_t(g.w),      uint16_t(g.h),
        int16_t(g.x0), int16_t(-g.y0), uint16_t(g.advance)};
  }
  out = std::move(result);
  return true;
}
bool ParseHeading(std::span<const uint8_t> d, HeadingFont &out) {
  out = {};
  using namespace acnh::bytes;
  auto magic = [&](size_t at, const char *s) {
    return Fits(d, at, 8) && std::memcmp(d.data() + at, s, 4) == 0;
  };
  auto block = [&](uint32_t ptr, const char *s, size_t minimum, size_t &at,
                   size_t &end) {
    if (ptr < 8)
      return false;
    at = ptr - 8;
    if (!magic(at, s))
      return false;
    size_t n = Le32(d, at + 4);
    if (n < minimum || !Fits(d, at, n))
      return false;
    end = at + n;
    return true;
  };
  if (d.size() > PixelBudget || d.size() < 20 || !magic(0, "FFNT") ||
      Le16(d, 4) != 0xfeff || Le32(d, 12) != d.size())
    return false;
  size_t finf = Le16(d, 6);
  if (!magic(finf, "FINF") || !Fits(d, finf, 32) || Le32(d, finf + 4) != 32)
    return false;
  size_t t{}, te{};
  if (!block(Le32(d, finf + 20), "TGLP", 32, t, te))
    return false;
  uint32_t cellw = d[t + 8], cellh = d[t + 9], sheets = d[t + 10],
           sheet_size = Le32(d, t + 12), baseline = Le16(d, t + 16),
           cols = Le16(d, t + 20), rows = Le16(d, t + 22), sw = Le16(d, t + 24),
           sh = Le16(d, t + 26), data = Le32(d, t + 28);
  // Regional fonts hold their glyphs on several sheets (BNTX array layers);
  // glyph index / (cols * rows) selects the sheet. Sheets stack vertically.
  // Cells start one pixel in; the last column may end at the sheet edge.
  if (!cellw || !cellh || !sheets || !cols || !rows ||
      cols * rows * sheets > 65536 || !sw || !sh || sw > 4096 ||
      sh * sheets > 4096 || size_t(sw) * sh * sheets * 4 > PixelBudget ||
      data < t + 32 || data >= te || !sheet_size ||
      uint64_t(sheet_size) * sheets > te - data || baseline > cellh ||
      cols * (cellw + 1) > sw || rows * (cellh + 1) > sh ||
      acnh::BntxLayers(d.data() + data, te - data) != sheets)
    return false;
  struct Width {
    int8_t left;
    uint8_t ink, advance;
  };
  Width defaults{int8_t(d[finf + 16]), d[finf + 17], d[finf + 18]};
  std::map<uint16_t, Width> widths;
  std::set<uint32_t> seen;
  uint32_t ptr = Le32(d, finf + 24);
  for (size_t count = 0; ptr; ++count) {
    size_t a{}, end{};
    if (count == 64 || !seen.insert(ptr).second ||
        !block(ptr, "CWDH", 16, a, end))
      return false;
    uint32_t first = Le16(d, a + 8), last = Le16(d, a + 10);
    if (last < first || size_t(last - first + 1) * 3 > end - a - 16)
      return false;
    for (uint32_t i = first; i <= last; ++i) {
      size_t q = a + 16 + size_t(i - first) * 3;
      if (!widths.emplace(uint16_t(i), Width{int8_t(d[q]), d[q + 1], d[q + 2]})
               .second)
        return false;
    }
    ptr = Le32(d, a + 12);
  }
  std::map<uint32_t, uint16_t> cmap;
  seen.clear();
  ptr = Le32(d, finf + 28);
  auto add = [&](uint32_t cp, uint32_t index) {
    return index == 0xffff || (Scalar(cp) && index < cols * rows * sheets &&
                               cmap.emplace(cp, uint16_t(index)).second);
  };
  for (size_t count = 0; ptr; ++count) {
    size_t a{}, end{};
    if (count == 64 || !seen.insert(ptr).second ||
        !block(ptr, "CMAP", 24, a, end))
      return false;
    uint32_t begin = Le32(d, a + 8), last = Le32(d, a + 12);
    uint16_t method = Le16(d, a + 16);
    size_t q = a + 24;
    if (last < begin || last > 0x10ffff || last - begin >= 65536)
      return false;
    if (method == 0) {
      if (end - q < 2)
        return false;
      uint32_t base = Le16(d, q);
      for (uint32_t cp = begin; cp <= last; ++cp)
        if (!add(cp, base + cp - begin))
          return false;
    } else if (method == 1) {
      if (size_t(last - begin + 1) * 2 > end - q)
        return false;
      for (uint32_t cp = begin; cp <= last; ++cp, q += 2)
        if (!add(cp, Le16(d, q)))
          return false;
    } else if (method == 2) {
      if (end - q < 4)
        return false;
      size_t n = Le16(d, q);
      q += 4;
      if (n > (end - q) / 8)
        return false;
      for (size_t i = 0; i < n; ++i, q += 8) {
        uint32_t cp = Le32(d, q);
        if (cp < begin || cp > last || !add(cp, Le16(d, q + 4)))
          return false;
      }
    } else
      return false;
    ptr = Le32(d, a + 20);
  }
  if (cmap.empty() || cmap.size() > 65536)
    return false;
  HeadingFont result;
  result.baseline = baseline;
  result.line_feed = Le16(d, finf + 12);
  if (!result.line_feed || result.line_feed > 1024)
    return false;
  result.sheet = acnh::Image(int(sw), int(sh * sheets));
  for (uint32_t layer = 0; layer < sheets; ++layer) {
    acnh::Image sheet;
    if (!acnh::BntxDecodeLayer(d.data() + data, te - data, layer, sheet) ||
        sheet.w != int(sw) || sheet.h != int(sh))
      return false;
    // Nintendo font sheet rows are stored bottom-up; glyph coordinates are
    // top-down.
    for (uint32_t y = 0; y < sh; ++y)
      std::memcpy(result.sheet.At(0, int(layer * sh + y)),
                  sheet.rgba.data() + size_t(sh - 1 - y) * sw * 4, size_t(sw) * 4);
  }
  for (const auto &[cp, index] : cmap) {
    auto it = widths.find(index);
    Width w = it == widths.end() ? defaults : it->second;
    uint32_t cell = index % (cols * rows), gx = (cell % cols) * (cellw + 1) + 1,
             gy = index / (cols * rows) * sh + (cell / cols) * (cellh + 1) + 1;
    if (w.ink > cellw || gx + w.ink > sw || gy + cellh > sh * sheets)
      return false;
    result.glyphs[cp] = {uint16_t(gx),    uint16_t(gy), w.ink,
                         uint16_t(cellh), w.left,       int16_t(baseline),
                         w.advance};
  }
  out = std::move(result);
  return true;
}
template<class Lookup>
bool RenderLine(const acnh::Image& sheet, uint32_t baseline, uint32_t line_feed,
                   const Lookup& lookup, std::string_view text,
                   std::array<uint8_t, 4> color, uint32_t width_bound,
                   acnh::Image &out) {
  out = {};
  if (sheet.Empty() || sheet.w > 4096 || sheet.h > 4096 ||
      baseline > 1024 || !line_feed || line_feed > 1024 || !width_bound ||
      width_bound > 4096)
    return false;
  std::vector<uint32_t> cps;
  if (!Utf8(text, cps) || cps.empty())
    return false;
  int pen = 0, left = 0, right = 0, top = 0, bottom = int(line_feed);
  for (uint32_t cp : cps) {
    const auto* glyph = lookup(cp);
    if (!glyph) return false;
    const auto& g = *glyph;
    if (uint32_t(g.x) + g.w > uint32_t(sheet.w) ||
        uint32_t(g.y) + g.h > uint32_t(sheet.h))
      return false;
    left = std::min(left, pen + g.bearing_x);
    right = std::max(right, pen + g.bearing_x + g.w);
    top = std::min(top, int(baseline) - g.bearing_y);
    bottom = std::max(bottom, int(baseline) - g.bearing_y + g.h);
    pen += g.advance;
    if (pen > 4096)
      return false;
  }
  right = std::max(right, pen);
  if (right - left <= 0 || uint32_t(right - left) > width_bound ||
      bottom - top <= 0 ||
      size_t(right - left) * size_t(bottom - top) * 4 > PixelBudget)
    return false;
  acnh::Image result(right - left, bottom - top);
  pen = 0;
  for (uint32_t cp : cps) {
    const auto &g = *lookup(cp);
    int dx = pen + g.bearing_x - left, dy = int(baseline) - g.bearing_y - top;
    for (uint32_t y = 0; y < g.h; ++y)
      for (uint32_t x = 0; x < g.w; ++x) {
        size_t src = (size_t(g.y + y) * size_t(sheet.w) + g.x + x) * 4;
        auto *dst = result.At(dx + int(x), dy + int(y));
        uint32_t alpha = uint32_t(sheet.rgba[src + 3]) * color[3] / 255;
        uint32_t old = dst[3], combined = alpha + old * (255 - alpha) / 255;
        if (combined) {
          for (size_t c = 0; c < 3; ++c)
            dst[c] = color[c];
          dst[3] = uint8_t(combined);
        }
      }
    pen += g.advance;
  }
  out = std::move(result);
  return true;
}
bool RenderHeading(const HeadingFont& font, std::string_view text,
                   std::array<uint8_t,4> color, uint32_t width_bound, acnh::Image& out) {
  return RenderLine(font.sheet,font.baseline,font.line_feed,[&](uint32_t cp) {
    const auto at=font.glyphs.find(cp);
    return at==font.glyphs.end()?nullptr:&at->second;
  },text,color,width_bound,out);
}
bool RenderCompactHeading(const HeadingFont& font, std::string_view text,
                          std::array<uint8_t,4> color, uint32_t width_bound,
                          acnh::Image& out) {
  out={};
  if(!width_bound || width_bound>3072)return false;
  acnh::Image native;
  if(!RenderHeading(font,text,color,width_bound*4/3,native))return false;
  acnh::Image scaled((native.w*3+3)/4,(native.h*3+3)/4);
  for(int y=0;y<scaled.h;++y)for(int x=0;x<scaled.w;++x) {
    const double sx=std::clamp((double(x)+.5)*4/3-.5,0.,double(native.w-1));
    const double sy=std::clamp((double(y)+.5)*4/3-.5,0.,double(native.h-1));
    const int ix=static_cast<int>(sx),iy=static_cast<int>(sy);
    const int jx=std::min(ix+1,native.w-1),jy=std::min(iy+1,native.h-1);
    const double fx=sx-ix,fy=sy-iy;
    const double a=(1-fy)*((1-fx)*native.At(ix,iy)[3]+fx*native.At(jx,iy)[3])+
        fy*((1-fx)*native.At(ix,jy)[3]+fx*native.At(jx,jy)[3]);
    auto* p=scaled.At(x,y);std::copy_n(color.data(),3,p);p[3]=static_cast<uint8_t>(std::lround(a));
  }
  out=std::move(scaled);return true;
}
bool RenderBody(const FontAtlas& font, std::string_view text,
                std::array<uint8_t,4> color, uint32_t width_bound, acnh::Image& out) {
  return RenderLine(font.image,font.line_height,font.line_height,[&](uint32_t cp)
      -> const EdenDsmodFontGlyph* {
    if (cp<font.first_codepoint || cp-font.first_codepoint>=font.glyphs.size()) return nullptr;
    const auto& glyph=font.glyphs[cp-font.first_codepoint];
    return glyph.w || glyph.h || glyph.advance ? &glyph : nullptr;
  },text,color,width_bound,out);
}
bool RenderBodyParagraph(const FontAtlas& font, std::string_view text,
                         std::array<uint8_t,4> color, uint32_t width,
                         uint32_t height, acnh::Image& out) {
  out={};
  if (text.empty() || text.size()>8192 || !width || width>4096 || !height || height>1024 ||
      size_t(width)*height*4>PixelBudget) return false;
  acnh::Image result(static_cast<int>(width),static_cast<int>(height));
  std::string line;
  uint32_t y=0;
  const uint32_t pitch=font.line_height+9;
  if (pitch<font.line_height || !font.line_height) return false;
  const auto emit=[&]() {
    if (!line.empty()) {
      acnh::Image pixels;
      if (!RenderBody(font,line,color,width,pixels) || y>height ||
          static_cast<uint32_t>(pixels.h)>height-y) return false;
      for (int row=0;row<pixels.h;++row)
        std::memcpy(result.At(0,static_cast<int>(y)+row),pixels.At(0,row),
                    static_cast<size_t>(pixels.w)*4);
    }
    if (pitch>UINT32_MAX-y) return false;
    y+=pitch; line.clear(); return true;
  };
  for (size_t at=0;at<text.size();) {
    const char c=text[at];
    if (c=='\n') { if (!emit()) return false; ++at; continue; }
    if (c==' ' || c=='\r' || c=='\t') { ++at; continue; }
    const auto end=text.find_first_of(" \r\n\t",at);
    const auto word=text.substr(at,end==std::string_view::npos?text.size()-at:end-at);
    // CJK segments of one word join without a space.
    bool spaced=true;
    for (const auto segment:Segments(word)) {
      std::string candidate=line.empty()?std::string(segment):line+(spaced?" ":"")+std::string(segment);
      acnh::Image measure;
      if (!RenderBody(font,candidate,color,width,measure)) {
        if (line.empty() || !emit() || !RenderBody(font,segment,color,width,measure)) return false;
        line=segment;
      } else line=std::move(candidate);
      spaced=false;
    }
    at=end==std::string_view::npos?text.size():end;
  }
  if (!line.empty() && !emit()) return false;
  out=std::move(result); return true;
}
} // namespace echoes
