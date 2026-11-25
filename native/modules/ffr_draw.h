// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
// FINAL FANTASY RESONANCE companion: a small RGBA compositor for the module's pages.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ffr_assets.h"

namespace ffr {

struct Rgba {
    uint8_t r, g, b, a;
};
constexpr Rgba C(uint32_t argb) {
    return {uint8_t(argb >> 16), uint8_t(argb >> 8), uint8_t(argb), uint8_t(argb >> 24)};
}

class Font {
public:
    bool Load(std::vector<uint8_t> ttf);
    bool ok() const { return ok_; }
    // Width in px of UTF-8 text at pixel height px.
    float Measure(const std::string& s, float px) const;
    struct Impl;

private:
    std::vector<uint8_t> data_;
    std::shared_ptr<Impl> impl_;
    bool ok_ = false;
    friend class Canvas;
};

enum class Align { Left, Center, Right };

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(size_t(w) * h * 4, 0) {}
    int w() const { return w_; }
    int h() const { return h_; }
    const std::vector<uint8_t>& pixels() const { return px_; }

    void Clear(Rgba c);
    void CopyFrom(const std::vector<uint8_t>& px) { if (px.size() == px_.size()) px_ = px; }
    void Fill(int x, int y, int w, int h, Rgba c);
    void GradientV(int x, int y, int w, int h, Rgba top, Rgba bottom);
    void GradientH(int x, int y, int w, int h, Rgba left, Rgba right);
    void Frame(int x, int y, int w, int h, int t, Rgba c);
    void Disc(float cx, float cy, float r, Rgba c);
    void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, Rgba c);
    // Bilinear scaled blit of src (sx,sy,sw,sh) into (dx,dy,dw,dh), alpha-multiplied and tinted.
    void Blit(const Image& src, int dx, int dy, int dw, int dh, float alpha = 1.f, Rgba tint = {255, 255, 255, 255},
              int sx = 0, int sy = 0, int sw = -1, int sh = -1);
    // Fit src inside the box keeping its aspect ratio; returns the drawn rect.
    void BlitFit(const Image& src, int x, int y, int w, int h, float alpha = 1.f, bool cover = false, int* out = nullptr);
    // Square sprite centred on (cx, cy), size px, turned deg clockwise.
    void BlitRotated(const Image& src, float cx, float cy, float size, float deg, float alpha = 1.f);
    // Nine-slice blit: border b (source px) scaled by s.
    void NineSlice(const Image& src, int x, int y, int w, int h, int b, float s = 1.f, float alpha = 1.f, Rgba tint = {255, 255, 255, 255});
    // Text with its baseline at y. Returns the advance width. maxw > 0 ellipsizes.
    float Text(const Font& f, const std::string& s, float x, float y, float px, Rgba c, Align a = Align::Left, float maxw = 0,
               Rgba shadow = {0, 0, 0, 0});
    void SetClip(int x, int y, int w, int h);
    void ResetClip() { SetClip(0, 0, w_, h_); }
    // Blits fade out horizontally between x0 (opaque) and x1 (clear); x1 == x0 turns it off.
    void SetFadeH(int x0, int x1) { fx0_ = x0, fx1_ = x1; }

private:
    void Blend(int x, int y, Rgba c, float cov);
    int w_, h_;
    std::vector<uint8_t> px_;
    int cx0_ = 0, cy0_ = 0, cx1_ = 1 << 30, cy1_ = 1 << 30;
    int fx0_ = 0, fx1_ = 0;
};

} // namespace ffr
