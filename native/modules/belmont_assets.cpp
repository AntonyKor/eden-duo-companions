// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_assets.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace belmont_assets {
namespace {
constexpr std::string_view LatinBundle =
    "fonts-latin_assets_all_304d9e1f03b4a2a6d44b17c96032676a.bundle";
std::int64_t I(const ce_unity::Value& v, std::string_view key) {
    const auto* x = v.Get(key);
    return x ? x->AsInt() : 0;
}
std::vector<std::uint8_t> Bytes(const ce_unity::Value* v) {
    if (!v)
        return {};
    if (v->kind == ce_unity::Value::Kind::Bytes)
        return {v->s.begin(), v->s.end()};
    std::vector<std::uint8_t> out;
    for (const auto& x : v->items)
        out.push_back(static_cast<std::uint8_t>(x.AsInt()));
    return out;
}
// Unity's Switch swizzle addresses 16-byte groups. Pixel and BC formats
// therefore have a different swizzle block width from their compression block
// width.
std::pair<unsigned, unsigned> SwizzleBlock(int format) {
    if (format == 1 || format == 63)
        return {16, 1};
    if (format == 3 || format == 4 || format == 5 || format == 14)
        return {4, 1};
    if (format == 10 || format == 26)
        return {8, 4};
    if (format == 12 || format == 25 || format == 27)
        return {4, 4};
    if (format >= 48 && format <= 59) {
        constexpr unsigned sizes[]{4, 5, 6, 8, 10, 12};
        const auto s = sizes[(format - 48) % 6];
        return {s, s};
    }
    return {};
}
bool ValidBundle(std::string_view b) {
    return !b.empty() && b.size() < 240 && b.ends_with(".bundle") &&
           b.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                               "XYZ0123456789_-.") == std::string_view::npos &&
           b.find("..") == std::string_view::npos;
}
} // namespace
Reader::Reader(ce_unity::RangeReader read, ce_unity::AstcDecoder decode)
    : unity(std::move(read), decode, std::string{ce_unity::DefaultAssetRoot}, 3),
      astc(std::move(decode)) {
    unity.SetUseBuiltinPins(false);
}
Reader::Reader(const EdenDsmodHostApi* host, ce_unity::AstcDecoder decode)
    : Reader(ce_unity::MakeHostRangeReader(host), std::move(decode)) {}
void Reader::ClearCache() {
    std::lock_guard lock{mutex};
    unity.ClearCache();
}
std::optional<Image> Reader::LoadImage(std::string_view key) {
    std::lock_guard lock{mutex};
    try {
        if (!key.starts_with("native:"))
            return {};
        key.remove_prefix(7);
        const auto split = key.find('#');
        if (split == std::string_view::npos || !ValidBundle(key.substr(0, split)))
            return {};
        const auto number = key.substr(split + 1);
        std::int64_t id{};
        const auto [end, err] = std::from_chars(number.data(), number.data() + number.size(), id);
        if (err != std::errc{} || end != number.data() + number.size())
            return {};
        return Texture(key.substr(0, split), id);
    } catch (...) {
        return {};
    }
}
std::optional<Image> Reader::Texture(std::string_view bundle, std::int64_t id) {
    const auto sf = unity.OpenSerialized(bundle);
    const auto* object = sf ? sf->Find(id) : nullptr;
    if (!object || object->class_id != 28 || sf->Platform() != 38 || sf->Version() != 22)
        return {};
    const auto value = sf->ReadObject(*object);
    if (!value)
        return {};
    const auto& v = *value;
    ce_unity::TextureInfo t;
    const auto width = I(v, "m_Width"), height = I(v, "m_Height");
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return {};
    t.width = static_cast<int>(width);
    t.height = static_cast<int>(height);
    t.format = static_cast<int>(I(v, "m_TextureFormat"));
    auto raw = Bytes(v.Get("image data"));
    if (const auto* stream = v.Get("m_StreamData"); raw.empty() && stream) {
        const auto* path = stream->Get("path");
        const auto offset = I(*stream, "offset"), size = I(*stream, "size");
        if (!path || offset < 0 || size <= 0 ||
            size > static_cast<std::int64_t>(ce_unity::MaxAlloc))
            return {};
        auto b = unity.OpenBundle(bundle);
        const auto* node = b ? b->FindNode(path->AsString()) : nullptr;
        if (!node || static_cast<std::uint64_t>(offset) > node->size ||
            static_cast<std::uint64_t>(size) > node->size - static_cast<std::uint64_t>(offset))
            return {};
        raw.resize(static_cast<std::size_t>(size));
        if (!b->Read(node->offset + static_cast<std::uint64_t>(offset), raw.data(), raw.size()))
            return {};
    }
    const auto blob = Bytes(v.Get("m_PlatformBlob"));
    if (blob.size() >= 12) {
        const auto shift = static_cast<unsigned>(blob[8]) | static_cast<unsigned>(blob[9]) << 8 |
                           static_cast<unsigned>(blob[10]) << 16 |
                           static_cast<unsigned>(blob[11]) << 24;
        if (shift > 5)
            return {};
        const auto [bw, bh] = SwizzleBlock(t.format);
        if (!bw || !bh)
            return {};
        const unsigned gobs = 1u << shift;
        const unsigned dw = (static_cast<unsigned>(t.width) + bw * 4 - 1) / (bw * 4) * bw * 4;
        const unsigned dh =
            (static_cast<unsigned>(t.height) + bh * 8 * gobs - 1) / (bh * 8 * gobs) * bh * 8 * gobs;
        const std::uint64_t needed = static_cast<std::uint64_t>(dw / bw) * (dh / bh) * 16;
        if (needed > ce_unity::MaxAlloc || needed > raw.size() ||
            static_cast<std::uint64_t>(dw) * dh * 4 > ce_unity::MaxAlloc)
            return {};
        auto linear = ce_unity::SwitchDeswizzle(raw, dw, dh, bw, bh, gobs);
        if (linear.size() != needed)
            return {};
        // Decode padded dimensions without shared-core's GOB>1 heuristic, then crop
        // BEFORE the final vertical flip (the padded rows are above the output).
        t.width = static_cast<int>(dw);
        t.height = static_cast<int>(dh);
        if (t.format == 3)
            t.format = 4;
        auto padded = ce_unity::DecodeTextureData(linear, t, astc, 0);
        if (!padded)
            return {};
        Image out{static_cast<unsigned>(width), static_cast<unsigned>(height), {}};
        out.rgba.resize(static_cast<std::size_t>(width * height * 4));
        for (std::int64_t y = 0; y < height; ++y)
            std::memcpy(out.rgba.data() + y * width * 4,
                        padded->rgba.data() + ((dh - height + y) * dw) * 4,
                        static_cast<std::size_t>(width) * 4);
        return out;
    }
    return ce_unity::DecodeTextureData(raw, t, astc, 0);
}
std::optional<Font> Reader::LoadFont(std::string_view name) {
    std::lock_guard lock{mutex};
    try {
        if (name != "LatinFont" && name != "NotoSans_Latin")
            return {};
        auto font = ce_unity::LoadTmpFont(unity, LatinBundle, name, false);
        if (!font || font->atlas.file_id != 0 || font->atlas.IsNull())
            return {};
        for (const auto& glyph : font->glyphs)
            if (glyph.atlas_index != 0)
                return {};
        auto image = Texture(LatinBundle, font->atlas.path_id);
        if (!image || image->width != static_cast<unsigned>(font->atlas_w) ||
            image->height != static_cast<unsigned>(font->atlas_h))
            return {};
        font->atlas_image = std::move(*image);
        return font;
    } catch (...) {
        return {};
    }
}
namespace {
std::optional<float> MaterialFloat(const ce_unity::Value& material, std::string_view name) {
    const auto* values = material.Path("m_SavedProperties.m_Floats");
    if (!values)
        return {};
    for (const auto& pair : values->items) {
        const auto* key = pair.Get("first");
        const auto* value = pair.Get("second");
        if (key && value && key->AsString() == name) {
            const float f = static_cast<float>(value->AsFloat());
            if (std::isfinite(f))
                return f;
        }
    }
    return {};
}
float SampleAlpha(const Image& image, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(image.width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(image.height - 1));
    const auto x0 = static_cast<unsigned>(x), y0 = static_cast<unsigned>(y);
    const auto x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    const auto alpha = [&](unsigned px, unsigned py) {
        return static_cast<float>(
                   image.rgba[(static_cast<std::size_t>(py) * image.width + px) * 4 + 3]) /
               255.0f;
    };
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    return std::lerp(std::lerp(alpha(x0, y0), alpha(x1, y0), fx),
                     std::lerp(alpha(x0, y1), alpha(x1, y1), fx), fy);
}
} // namespace
std::optional<HostFont> Reader::LoadHostFont(std::string_view name, std::uint32_t cap_height) {
    std::lock_guard lock{mutex};
    try {
        if (cap_height < 24 || cap_height > 96)
            return {};
        auto source = LoadFont(name);
        const auto object = unity.ReadObjectByName(LatinBundle, name, 114);
        const auto* material_ref = object ? object->Get("material") : nullptr;
        if (!source || !material_ref || I(*material_ref, "m_FileID") != 0 ||
            !std::isfinite(source->cap_line) || source->cap_line < 8 || source->cap_line > 256 ||
            source->scale != 1.0f)
            return {};
        const auto material = unity.ReadObject(LatinBundle, I(*material_ref, "m_PathID"));
        if (!material)
            return {};
        if (const auto* keywords = material->Get("m_ValidKeywords");
            keywords && !keywords->items.empty())
            return {}; // No outline/underlay/glow variants in the supported regular faces.
        const auto gradient = MaterialFloat(*material, "_GradientScale");
        const auto normal = MaterialFloat(*material, "_WeightNormal");
        const auto dilate = MaterialFloat(*material, "_FaceDilate");
        const auto ratio = MaterialFloat(*material, "_ScaleRatioA");
        const auto sharpness = MaterialFloat(*material, "_Sharpness");
        const auto sx = MaterialFloat(*material, "_ScaleX"),
                   sy = MaterialFloat(*material, "_ScaleY");
        const auto outline = MaterialFloat(*material, "_OutlineWidth");
        const auto softness = MaterialFloat(*material, "_OutlineSoftness");
        if (!gradient || !normal || !dilate || !ratio || !sharpness || !sx || !sy || !outline ||
            !softness || *gradient <= 0 || *gradient > 64 || std::abs(*normal) > 4 ||
            std::abs(*dilate) > 1 || *ratio < 0 || *ratio > 1 || *sharpness <= -1 ||
            *sharpness > 1 || *sx != 1 || *sy != 1 || *outline != 0 || *softness != 0)
            return {};
        // TMP's ordinary unlit face shader: weight=(normal/4+dilate)*ratio/2;
        // alpha=saturate((SDF - .5 + weight)*scale + .5). For an orthographic
        // 1:1 pixel grid, rsqrt(dot(pixelSize,pixelSize)) = sqrt(2).
        const float factor = static_cast<float>(cap_height) / source->cap_line;
        if (factor <= 0 || factor > 4)
            return {};
        const float weight = (*normal / 4.0f + *dilate) * *ratio * 0.5f;
        const float sdf_scale = std::sqrt(2.0f) * *gradient * factor * (*sharpness + 1.0f);
        HostFont out;
        out.source = std::move(*source);
        out.atlas.width = static_cast<unsigned>(
            std::ceil(static_cast<float>(out.source.atlas_image.width) * factor));
        out.atlas.height = static_cast<unsigned>(
            std::ceil(static_cast<float>(out.source.atlas_image.height) * factor));
        const auto size = static_cast<std::uint64_t>(out.atlas.width) * out.atlas.height * 4;
        if (!out.atlas.width || !out.atlas.height || size > ce_unity::MaxAlloc)
            return {};
        out.atlas.rgba.assign(static_cast<std::size_t>(size), 255);
        for (unsigned y = 0; y < out.atlas.height; ++y)
            for (unsigned x = 0; x < out.atlas.width; ++x) {
                const float sdf = SampleAlpha(out.source.atlas_image,
                                              (static_cast<float>(x) + .5f) / factor - .5f,
                                              (static_cast<float>(y) + .5f) / factor - .5f);
                const float coverage =
                    std::clamp((sdf - .5f + weight) * sdf_scale + .5f, 0.0f, 1.0f);
                out.atlas.rgba[(static_cast<std::size_t>(y) * out.atlas.width + x) * 4 + 3] =
                    static_cast<std::uint8_t>(std::lround(coverage * 255));
            }
        if (out.source.chars.empty())
            return {};
        const auto lo = out.source.chars.begin()->first, hi = out.source.chars.rbegin()->first;
        if (hi > 0xFFFF || hi - lo > 0x4000)
            return {};
        out.first_codepoint = lo;
        out.line_height = cap_height;
        out.glyphs.resize(hi - lo + 1);
        for (const auto& [cp, index] : out.source.chars) {
            if (index >= out.source.glyphs.size())
                return {};
            const auto& g = out.source.glyphs[index];
            auto& h = out.glyphs[cp - lo];
            if (!std::isfinite(g.bearing_x) || !std::isfinite(g.bearing_y) ||
                !std::isfinite(g.advance) || g.advance < 0 || g.advance > 4096 ||
                std::abs(g.bearing_x) > 4096 || std::abs(g.bearing_y) > 4096 || g.rect_x < 0 ||
                g.rect_y < 0 || g.rect_w < 0 || g.rect_h < 0 ||
                g.rect_x > out.source.atlas_w - g.rect_w ||
                g.rect_y > out.source.atlas_h - g.rect_h)
                return {};
            h.advance = static_cast<std::uint16_t>(std::lround(g.advance * factor));
            if (g.rect_w == 0 || g.rect_h == 0)
                continue;
            // Include one source texel around the glyph for the regular material's
            // dilation/AA fringe. TMP's large SDF generation padding stays outside.
            const int x = std::max(0, g.rect_x - 1);
            const int y = std::max(0, out.source.atlas_h - g.rect_y - g.rect_h - 1);
            const int right = std::min(out.source.atlas_w, g.rect_x + g.rect_w + 1);
            const int bottom = std::min(out.source.atlas_h, out.source.atlas_h - g.rect_y + 1);
            h.x = static_cast<std::uint16_t>(std::floor(static_cast<float>(x) * factor));
            h.y = static_cast<std::uint16_t>(std::floor(static_cast<float>(y) * factor));
            h.w = static_cast<std::uint16_t>(std::ceil(static_cast<float>(right) * factor) - h.x);
            h.h = static_cast<std::uint16_t>(std::ceil(static_cast<float>(bottom) * factor) - h.y);
            h.bearing_x = static_cast<std::int16_t>(
                std::lround(g.bearing_x * factor + (static_cast<float>(x) * factor -
                                                    static_cast<float>(g.rect_x) * factor)));
            h.bearing_y = static_cast<std::int16_t>(std::lround(
                g.bearing_y * factor +
                (static_cast<float>(out.source.atlas_h - g.rect_y - g.rect_h) * factor - h.y)));
        }
        return out;
    } catch (...) {
        return {};
    }
}
} // namespace belmont_assets
