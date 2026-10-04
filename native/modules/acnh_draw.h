// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Raster primitives for the ACNH UI art recipes: the C++ port of the helpers in
// research/acnh/tools/acnh_mock.py (which uses Pillow). Where Pillow's integer arithmetic is
// cheap to reproduce it is reproduced exactly (alpha_composite, paste-with-mask, the material
// tint's float32 rounding); resampling is Pillow's Lanczos-3 coefficient scheme on premultiplied
// alpha, so resized art differs from the mock only by small rounding (tests document the bound).
#pragma once

#include <string_view>

#include "acnh_types.h"

namespace acnh::draw {

/// "#rrggbb", "#rrggbbaa" or without '#'; six digits = opaque. Bad input = transparent black.
Rgba Color(std::string_view hex);
Rgba WithAlpha(Rgba c, int a);
/// Mix of two colours' RGB (acnh_mock.mix): c*(1-k) + d*k, truncated; alpha 255.
Rgba Mix(Rgba c, Rgba d, float k);

/// Material rule: out = black + (white - black) * tex, all four channels (float32 as the mock).
Image Tint(const Image& img, Rgba black, Rgba white);
/// Tint times a vertex colour given as 0..1 floats (acnh_panesheet.tint with vtx: the mean of the
/// pane's four vertex colours / 255).
Image TintVtx(const Image& img, Rgba black, Rgba white, const float vtx[4]);
/// ^s mask shortcut: black = colour @ 0, white = colour @ (alpha < 0 ? colour.a : alpha).
Image MaskTint(const Image& mask, Rgba colour, int alpha = -1);

Image Resize(const Image& img, int w, int h); ///< Lanczos-3, premultiplied
Image ResizeNearest(const Image& img, int w, int h);
/// Area-average resize for display-size icons: every output pixel is the exact coverage-weighted
/// mean of the source texels under it, computed in linear light with alpha weighting (the game's
/// icon textures are *_SRGB, so its GPU filters in linear light too; an area average is the
/// alias-free form of that minification at any ratio). An axis that grows uses Resize (Lanczos).
Image ResizeArea(const Image& img, int w, int h);
/// The display-size parameters of icon keys: s > 0 = fit inside s x s keeping aspect, centred on a
/// transparent s x s canvas; else w and/or h > 0 = exactly w x h (only one given: the other keeps
/// the aspect); s with w and/or h = that exact size centred on a transparent s x s box without
/// scaling to it (a fixed-size widget box for icons of varying size, e.g. the map's per-icon keys).
/// Returns img unchanged when no size applies or it already has that size.
Image SizeTo(const Image& img, double s, double w, double h);
Image Fit(const Image& img, double w, double h);              ///< keep aspect, fit inside w x h
Image Crop(const Image& img, int x0, int y0, int x1, int y1); ///< outside = transparent
Image CropAlpha(const Image& img, int threshold = 8);         ///< bbox of alpha > threshold
Image FlipH(const Image& img);
Image FlipV(const Image& img);
Image Rotate90(const Image& img); ///< counter-clockwise (Pillow ROTATE_90)
Image MirrorH(const Image& img);  ///< img + its horizontal mirror (texture = left half)
Image MirrorV(const Image& img);  ///< img + its vertical mirror (texture = top half)
Image Mirror4(const Image& img);  ///< texture = top-left quarter
/// wnd1 window: left cap scaled to height h, mirrored right cap, centre = cap's inner column.
Image Pill(const Image& cap, double w, double h);
/// 9-slice: `border` source px of each corner drawn as r px.
Image Nine(const Image& src, int border, double w, double h, double r);
/// Corner window (acnh_mock.win): corner texture's alpha at the 4 corners, flat colour elsewhere.
Image Win(const Image& corner, double w, double h, Rgba colour);
/// Tile `t` over w x h starting at the top-left (Pillow paste with t as its own mask).
Image Tile(int w, int h, const Image& t, int off_x = 0, int off_y = 0);
/// alpha = min(img alpha, mask alpha) (mask resized to img when sizes differ).
Image Masked(const Image& img, const Image& mask);
Image AlphaMask(const Image& img); ///< white, alpha = img alpha
Image Fade(const Image& img, double k);
Image ShadowOf(const Image& img, Rgba colour);
/// Pillow alpha_composite of `src` onto `dst` at (x, y) (clipped).
void Composite(Image& dst, const Image& src, int x, int y);
/// acnh_mock.paste: optional drop shadow (shadow colour, offset dy) then the image; x/y rounded.
void Paste(Image& dst, const Image& src, double x, double y, Rgba shadow = {0, 0, 0, 0},
           double dy = 6);
void PasteC(Image& dst, const Image& src, double cx, double cy, Rgba shadow = {0, 0, 0, 0},
            double dy = 6);
/// Solid fill of a rect (alpha-composited).
void FillRect(Image& dst, double x0, double y0, double x1, double y1, Rgba c);
/// Anti-aliased filled rounded rectangle (composited).
void FillRoundRect(Image& dst, double x0, double y0, double x1, double y1, double r, Rgba c);
/// Inverse-mapped affine draw with bilinear sampling (Pillow Image.transform AFFINE BILINEAR,
/// then alpha_composite). `inv` maps destination pixel centres to source coordinates:
/// sx = inv[0]*x + inv[1]*y + inv[2], sy = inv[3]*x + inv[4]*y + inv[5].
void AffineComposite(Image& dst, const Image& src, const double inv[6]);

} // namespace acnh::draw
