/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lua-facing Skia canvas. This is deliberately a Cairo-shaped drawing API:
 * widgets keep receiving a mutable context with paths, transforms, clipping,
 * source colours, fill/stroke and save/restore. Unlike Cairo, every operation
 * targets the Skia surface acquired from the Vulkan swapchain for this frame.
 */

#include "render/skia/skia_lua.h"

#include "render/skia/skia_backend.h"

extern "C" {
#include <lauxlib.h>
}

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/svg/SkSVGCanvas.h"
#include "include/encode/SkPngEncoder.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/core/SkTextBlob.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

#ifdef AWESOME_SKIA_HAS_SVG
#include "modules/svg/include/SkSVGDOM.h"
#endif

#include <fontconfig/fontconfig.h>
#include <pango/pango.h>
#include <pango/pangofc-font.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <utility>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

struct awesome_skia_image
{
    std::atomic_uint references {1};
    sk_sp<SkImage> image;
};

namespace {

constexpr const char *k_frame_type = "awesome.skia.frame";
constexpr const char *k_pattern_type = "awesome.skia.pattern";
constexpr const char *k_image_type = "awesome.skia.image";
constexpr const char *k_surface_type = "awesome.skia.surface";

std::unordered_map<std::string, sk_sp<SkImage>> encoded_images;

/* SkSVGDOM reports a zero container size for valid icon SVGs that only carry
 * a viewBox. Cairo/RSVG inferred that size, so retain that useful behaviour
 * for the compatibility API. */
bool svg_intrinsic_size(const char *path, float *width, float *height)
{
    sk_sp<SkData> data = SkData::MakeFromFileName(path);
    if (!data)
        return false;

    const std::string xml(static_cast<const char *>(data->data()), data->size());
    const char *view_box = std::strstr(xml.c_str(), "viewBox");
    if (view_box)
    {
        const char *value = std::strchr(view_box, '=');
        if (value)
        {
            ++value;
            while (*value == ' ' || *value == '\t' || *value == '\'' || *value == '\"')
                ++value;
            float x, y, w, h;
            if (std::sscanf(value, "%f%f%f%f", &x, &y, &w, &h) == 4 && w > 0 && h > 0)
            {
                *width = w;
                *height = h;
                return true;
            }
        }
    }

    return false;
}

std::string css_value(const char *stylesheet, const char *property)
{
    if (!stylesheet)
        return {};
    const std::string css(stylesheet);
    const size_t property_pos = css.find(property);
    if (property_pos == std::string::npos)
        return {};
    const size_t colon = css.find(':', property_pos + std::strlen(property));
    if (colon == std::string::npos)
        return {};
    size_t begin = colon + 1;
    while (begin < css.size() && std::isspace(static_cast<unsigned char>(css[begin])))
        ++begin;
    size_t end = begin;
    while (end < css.size() && css[end] != ';' && css[end] != '}')
        ++end;
    while (end > begin && std::isspace(static_cast<unsigned char>(css[end - 1])))
        --end;
    return css.substr(begin, end - begin);
}

void override_svg_attribute(std::string *svg, const char *attribute, const std::string &value)
{
    if (value.empty())
        return;
    const std::string key = std::string(attribute) + "=";
    size_t position = 0;
    while ((position = svg->find(key, position)) != std::string::npos)
    {
        const size_t quote_pos = position + key.size();
        if (quote_pos >= svg->size() || ((*svg)[quote_pos] != '\'' && (*svg)[quote_pos] != '\"'))
        {
            position = quote_pos;
            continue;
        }
        const char quote = (*svg)[quote_pos];
        const size_t end = svg->find(quote, quote_pos + 1);
        if (end == std::string::npos)
            break;
        svg->replace(quote_pos + 1, end - quote_pos - 1, value);
        position = quote_pos + value.size() + 1;
    }

    const size_t svg_start = svg->find("<svg");
    const size_t svg_end = svg_start == std::string::npos ? std::string::npos : svg->find('>', svg_start);
    if (svg_end != std::string::npos)
        svg->insert(svg_end, " " + std::string(attribute) + "=\"" + value + "\"");
}

std::string styled_svg(const char *path, const char *stylesheet)
{
    sk_sp<SkData> data = SkData::MakeFromFileName(path);
    if (!data)
        return {};
    std::string svg(static_cast<const char *>(data->data()), data->size());
    override_svg_attribute(&svg, "fill", css_value(stylesheet, "fill"));
    override_svg_attribute(&svg, "stroke", css_value(stylesheet, "stroke"));
    return svg;
}

struct canvas_state
{
    SkPaint paint;
};

struct lua_skia_frame
{
    /* Must stay first: the small C fallback registered from luaa.c has this
     * layout too, which keeps GC safe during an error while modules load. */
    awesome_skia_frame_t *frame = nullptr;
    /* Set instead of `frame` for an offscreen canvas created by
     * skia.new_image_surface(); such a canvas has no swapchain/present cycle
     * and is finished with :snapshot() instead of :present(). */
    sk_sp<SkSurface> offscreen;
    /* A vector export canvas owned by lua_skia_surface. */
    SkCanvas *external_canvas = nullptr;
    SkPathBuilder path;
    canvas_state state;
    std::vector<canvas_state> states;
    SkPoint current = {0, 0};
    /* Cairo's new_sub_path() means "the next arc starts a fresh sub-path
     * rather than connecting from the current point". Skia expresses that as
     * the forceMoveTo argument to arcTo(), so record it until the arc lands. */
    bool force_move_to = false;
    /* Cairo transforms path points into device space as they are added, so a
     * path built inside save()/transform()/restore() keeps that transform
     * once filled outside it (gears.shape.transform relies on this). Skia
     * builds the path untransformed and applies the canvas matrix at draw
     * time, which would drop it. Capture the matrix in force when the path
     * starts and draw with that instead. */
    SkMatrix path_matrix;
    bool has_path_matrix = false;
    /* push_group()/pop_group(): each entry is an offscreen layer that
     * subsequent drawing redirects into, along with the CTM in force when it
     * was pushed (see pattern's local_matrix for why that matrix matters). */
    struct group_layer_t
    {
        sk_sp<SkSurface> surface;
        SkMatrix push_matrix;
    };
    std::vector<group_layer_t> group_stack;
};

/* A decoded or rendered image ready to be drawn with frame:draw_image(). */
struct lua_skia_image
{
    sk_sp<SkImage> image;
};

/* A drawing target, mirroring cairo_surface_t. One of three things backs it:
 *
 *   raster    an ImageSurface the caller allocated and may draw into
 *   renderer  a drawin's Vulkan swapchain; Context() acquires a frame
 *   image     a decoded file, usable as a source but not drawn into
 *
 * Keeping all three behind one type is what lets the Lua side hold a single
 * code path: Context(surface) works the same for a wibar, an offscreen
 * buffer, and a loaded PNG, exactly as cairo.Context() does. */
struct lua_skia_surface
{
    sk_sp<SkSurface> raster;
    sk_sp<SkImage> image;
    std::unique_ptr<SkFILEWStream> svg_stream;
    std::unique_ptr<SkCanvas> svg_canvas;
    awesome_skia_renderer_t *renderer = nullptr;
    int width = 0;
    int height = 0;
};

enum class pattern_kind
{
    solid,
    linear,
    radial,
    image,
};

struct lua_skia_pattern
{
    pattern_kind kind = pattern_kind::solid;
    SkColor color = SK_ColorBLACK;
    SkPoint start = {0, 0};
    SkPoint end = {0, 0};
    SkScalar start_radius = 0;
    SkScalar end_radius = 0;
    SkTileMode tile_mode = SkTileMode::kClamp;
    std::vector<SkColor4f> colors;
    std::vector<SkScalar> offsets;
    sk_sp<SkImage> image;
    sk_sp<SkShader> shader;
    /* Set when this pattern came from pop_group(): the inverse of the CTM in
     * force when the group was pushed, so drawing the pattern back under the
     * (unchanged) current CTM lands on the same device pixels it was
     * rendered to, matching cairo_pop_group's coordinate-space contract. */
    SkMatrix local_matrix = SkMatrix::I();
    bool has_local_matrix = false;
};

lua_skia_pattern *test_pattern(lua_State *L, int index)
{
    if (!lua_isuserdata(L, index) || !lua_getmetatable(L, index))
        return nullptr;
    luaL_getmetatable(L, k_pattern_type);
    const bool matches = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return matches ? static_cast<lua_skia_pattern *>(lua_touserdata(L, index)) : nullptr;
}

lua_skia_frame *check_frame(lua_State *L, int index)
{
    auto *frame = static_cast<lua_skia_frame *>(luaL_checkudata(L, index, k_frame_type));
    if (frame->offscreen || frame->external_canvas)
        return frame;
    if (!frame->frame || !awesome_skia_frame_canvas(frame->frame))
        luaL_error(L, "Skia frame has already been presented");
    return frame;
}

int skia_is_canvas(lua_State *L)
{
    lua_pushboolean(L, luaL_testudata(L, 1, k_frame_type) != nullptr);
    return 1;
}

SkCanvas *canvas(lua_skia_frame *frame)
{
    if (!frame->group_stack.empty())
        return frame->group_stack.back().surface->getCanvas();
    if (frame->external_canvas)
        return frame->external_canvas;
    if (frame->offscreen)
        return frame->offscreen->getCanvas();
    return awesome_skia_frame_canvas(frame->frame);
}

lua_skia_image *test_image(lua_State *L, int index)
{
    if (!lua_isuserdata(L, index) || !lua_getmetatable(L, index))
        return nullptr;
    luaL_getmetatable(L, k_image_type);
    const bool matches = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return matches ? static_cast<lua_skia_image *>(lua_touserdata(L, index)) : nullptr;
}

int skia_is_image(lua_State *L)
{
    lua_pushboolean(L, test_image(L, 1) != nullptr);
    return 1;
}

lua_skia_surface *test_surface(lua_State *L, int index)
{
    if (!lua_isuserdata(L, index) || !lua_getmetatable(L, index))
        return nullptr;
    luaL_getmetatable(L, k_surface_type);
    const bool matches = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return matches ? static_cast<lua_skia_surface *>(lua_touserdata(L, index)) : nullptr;
}

lua_skia_surface *push_surface(lua_State *L)
{
    void *storage = lua_newuserdata(L, sizeof(lua_skia_surface));
    auto *surface = new (storage) lua_skia_surface;
    luaL_getmetatable(L, k_surface_type);
    lua_setmetatable(L, -2);
    return surface;
}

/* The pixels of a surface, for use as a drawing source. A raster surface is
 * snapshotted at the moment it is read, matching cairo's behaviour of
 * sourcing whatever has been drawn so far. */
sk_sp<SkImage> surface_source(lua_skia_surface *surface)
{
    if (!surface)
        return nullptr;
    if (surface->image)
        return surface->image;
    if (surface->raster)
        return surface->raster->makeImageSnapshot();
    return nullptr;
}

SkColorType color_type_from_format(lua_State *L, int index)
{
    /* cairo.Format values; A8/A1 have no direct N32 equivalent so they use
     * an alpha-only type, which is what callers actually want them for. */
    const lua_Integer format = luaL_optinteger(L, index, 0);
    switch (format)
    {
    case 2:  return kRGB_888x_SkColorType;   /* RGB24 */
    case 3:  return kAlpha_8_SkColorType;    /* A8    */
    case 4:  return kAlpha_8_SkColorType;    /* A1    */
    default: return kN32_SkColorType;        /* ARGB32 */
    }
}

sk_sp<SkFontMgr> font_manager()
{
    static sk_sp<SkFontMgr> manager =
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    return manager;
}

sk_sp<SkImage> load_encoded_image(const char *path)
{
    const auto cached = encoded_images.find(path);
    if (cached != encoded_images.end())
        return cached->second;

    sk_sp<SkData> data = SkData::MakeFromFileName(path);
    if (!data)
        return nullptr;
    sk_sp<SkImage> image = SkImages::DeferredFromEncodedData(std::move(data));
    if (image)
        encoded_images.emplace(path, image);
    return image;
}

uint8_t component(double value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

SkColor rgba(double red, double green, double blue, double alpha)
{
    return SkColorSetARGB(component(alpha), component(red), component(green), component(blue));
}

const char *pattern_kind_name(pattern_kind kind)
{
    switch (kind)
    {
    case pattern_kind::solid:
        return "SOLID";
    case pattern_kind::linear:
        return "LINEAR";
    case pattern_kind::radial:
        return "RADIAL";
    case pattern_kind::image:
        return "SURFACE";
    }
    return "UNKNOWN";
}

void refresh_gradient(lua_skia_pattern *pattern)
{
    pattern->shader.reset();
    if (pattern->kind == pattern_kind::image)
    {
        if (pattern->image)
            pattern->shader = pattern->image->makeShader(
                pattern->tile_mode, pattern->tile_mode,
                SkSamplingOptions(SkFilterMode::kLinear),
                pattern->has_local_matrix ? &pattern->local_matrix : nullptr);
        return;
    }
    if (pattern->kind == pattern_kind::solid || pattern->colors.empty())
        return;

    std::vector<SkColor4f> colors = pattern->colors;
    std::vector<SkScalar> offsets = pattern->offsets;
    if (colors.size() == 1)
    {
        colors.push_back(colors.front());
        offsets = {0, 1};
    }

    const SkGradient gradient(
        SkGradient::Colors(SkSpan<const SkColor4f>(colors.data(), colors.size()),
                           SkSpan<const float>(offsets.data(), offsets.size()), pattern->tile_mode),
        {});
    if (pattern->kind == pattern_kind::linear)
    {
        const SkPoint points[] = {pattern->start, pattern->end};
        pattern->shader = SkShaders::LinearGradient(points, gradient);
    }
    else
    {
        pattern->shader = SkShaders::TwoPointConicalGradient(
            pattern->start, pattern->start_radius, pattern->end, pattern->end_radius,
            gradient);
    }
}

lua_skia_pattern *new_pattern(lua_State *L)
{
    void *storage = lua_newuserdata(L, sizeof(lua_skia_pattern));
    auto *pattern = new (storage) lua_skia_pattern;
    luaL_getmetatable(L, k_pattern_type);
    lua_setmetatable(L, -2);
    return pattern;
}

bool hex_digit(char character, uint8_t *result)
{
    if (character >= '0' && character <= '9')
        *result = static_cast<uint8_t>(character - '0');
    else if (character >= 'a' && character <= 'f')
        *result = static_cast<uint8_t>(character - 'a' + 10);
    else if (character >= 'A' && character <= 'F')
        *result = static_cast<uint8_t>(character - 'A' + 10);
    else
        return false;
    return true;
}

bool parse_hex_color(const char *value, size_t size, SkColor *result)
{
    if (!value || size < 4 || value[0] != '#')
        return false;

    if (size == 4 || size == 5)
    {
        uint8_t r, g, b, a = 15;
        if (!hex_digit(value[1], &r) || !hex_digit(value[2], &g) ||
            !hex_digit(value[3], &b) || (size == 5 && !hex_digit(value[4], &a)))
            return false;
        *result = SkColorSetARGB(a * 17, r * 17, g * 17, b * 17);
        return true;
    }
    if (size != 7 && size != 9)
        return false;

    std::array<uint8_t, 4> bytes = {0, 0, 0, 255};
    for (size_t index = 0; index < (size - 1) / 2; ++index)
    {
        uint8_t high, low;
        if (!hex_digit(value[1 + index * 2], &high) ||
            !hex_digit(value[2 + index * 2], &low))
            return false;
        bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    *result = SkColorSetARGB(bytes[3], bytes[0], bytes[1], bytes[2]);
    return true;
}

SkColor check_color(lua_State *L, int index)
{
    if (lua_isnumber(L, index))
    {
        const lua_Integer raw = luaL_checkinteger(L, index);
        if (raw < 0 || static_cast<uint64_t>(raw) > UINT32_MAX)
            luaL_argerror(L, index, "expected 0xRRGGBBAA");
        const uint32_t color = static_cast<uint32_t>(raw);
        return SkColorSetARGB(color & 0xff, (color >> 24) & 0xff,
                              (color >> 16) & 0xff, (color >> 8) & 0xff);
    }
    size_t size = 0;
    const char *string = luaL_checklstring(L, index, &size);
    SkColor color;
    if (!parse_hex_color(string, size, &color))
        luaL_argerror(L, index, "expected #rgb, #rgba, #rrggbb, or #rrggbbaa");
    return color;
}

/* Cairo's Lua binding hands operators through as the numeric
 * cairo_operator_t value (e.g. cairo.Operator.CLEAR == 0), not a string, so
 * that is the primary form here; a string fallback stays for direct
 * skia-only callers. */
SkBlendMode blend_mode_from_operator(lua_State *L, int index)
{
    if (lua_isnumber(L, index))
    {
        switch (static_cast<int>(lua_tointeger(L, index)))
        {
        case 0: return SkBlendMode::kClear;  // CAIRO_OPERATOR_CLEAR
        case 1: return SkBlendMode::kSrc;    // CAIRO_OPERATOR_SOURCE
        case 3: return SkBlendMode::kSrcIn;  // CAIRO_OPERATOR_IN
        default: return SkBlendMode::kSrcOver; // CAIRO_OPERATOR_OVER and others
        }
    }
    const char *op = lua_tostring(L, index);
    if (op && (std::strcmp(op, "SOURCE") == 0 || std::strcmp(op, "source") == 0))
        return SkBlendMode::kSrc;
    if (op && (std::strcmp(op, "CLEAR") == 0 || std::strcmp(op, "clear") == 0))
        return SkBlendMode::kClear;
    if (op && (std::strcmp(op, "IN") == 0 || std::strcmp(op, "in") == 0))
        return SkBlendMode::kSrcIn;
    return SkBlendMode::kSrcOver;
}

void reset_path(lua_skia_frame *frame)
{
    const SkPathFillType fill_type = frame->path.fillType();
    frame->path = SkPathBuilder(fill_type);
    frame->has_path_matrix = false;
}

/* Pin the path to the transform in force when its first point is added. */
void note_path_start(lua_skia_frame *frame)
{
    if (frame->has_path_matrix)
        return;
    frame->path_matrix = canvas(frame)->getTotalMatrix();
    frame->has_path_matrix = true;
}

/* Draw the accumulated path under the matrix it was built with, rather than
 * whatever transform happens to be current at fill/stroke time. */
void draw_path(lua_skia_frame *frame, const SkPath &path, const SkPaint &paint)
{
    SkCanvas *target = canvas(frame);
    SkAutoCanvasRestore restore(target, true);
    if (frame->has_path_matrix)
        target->setMatrix(frame->path_matrix);
    target->drawPath(path, paint);
}

int frame_begin(lua_State *L)
{
    auto *renderer = static_cast<awesome_skia_renderer_t *>(lua_touserdata(L, 1));
    if (!renderer)
        return luaL_argerror(L, 1, "expected drawable.skia_renderer");

    char error[256] = {0};
    awesome_skia_frame_t *native = awesome_skia_renderer_begin_frame(renderer, error, sizeof(error));
    if (!native)
        return luaL_error(L, "could not begin Skia frame: %s", error);

    auto *frame = static_cast<lua_skia_frame *>(lua_newuserdata(L, sizeof(lua_skia_frame)));
    new (frame) lua_skia_frame();
    frame->frame = native;
    frame->state.paint.setAntiAlias(true);
    frame->states.push_back(frame->state);
    luaL_getmetatable(L, k_frame_type);
    lua_setmetatable(L, -2);
    return 1;
}

/* skia.Context(surface) -- cairo.Context. Works for every surface kind: a
 * swapchain surface acquires a frame, a raster surface draws in place. */
int skia_context(lua_State *L)
{
    const int base = lua_istable(L, 1) ? 1 : 0;
    lua_skia_surface *surface = test_surface(L, base + 1);
    if (!surface)
        return luaL_argerror(L, base + 1, "expected a skia surface");

    awesome_skia_frame_t *native = nullptr;
    if (surface->renderer)
    {
        char error[256] = {0};
        native = awesome_skia_renderer_begin_frame(surface->renderer, error, sizeof(error));
        if (!native)
            return luaL_error(L, "could not begin Skia frame: %s", error);
    }
    else if (!surface->raster && !surface->svg_canvas)
    {
        return luaL_error(L, "cannot draw into a read-only image surface");
    }

    auto *context = static_cast<lua_skia_frame *>(lua_newuserdata(L, sizeof(lua_skia_frame)));
    new (context) lua_skia_frame();
    context->frame = native;
    context->offscreen = surface->raster;
    context->external_canvas = surface->svg_canvas.get();
    context->state.paint.setAntiAlias(true);
    context->states.push_back(context->state);
    luaL_getmetatable(L, k_frame_type);
    lua_setmetatable(L, -2);
    return 1;
}

/* cr:set_source_surface(surface, x, y) */
int frame_set_source_surface(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    sk_sp<SkImage> image;
    if (lua_skia_surface *surface = test_surface(L, 2))
        image = surface_source(surface);
    else if (lua_skia_image *stored = test_image(L, 2))
        image = stored->image;
    if (!image)
        return luaL_argerror(L, 2, "expected a surface with pixels");

    const SkScalar x = static_cast<SkScalar>(luaL_optnumber(L, 3, 0));
    const SkScalar y = static_cast<SkScalar>(luaL_optnumber(L, 4, 0));
    frame->state.paint.setColor(SK_ColorBLACK);
    frame->state.paint.setShader(image->makeShader(
        SkTileMode::kDecal, SkTileMode::kDecal,
        SkSamplingOptions(SkFilterMode::kLinear),
        SkMatrix::Translate(x, y)));
    return 0;
}

int frame_gc(lua_State *L)
{
    auto *frame = static_cast<lua_skia_frame *>(luaL_checkudata(L, 1, k_frame_type));
    if (frame->frame)
    {
        char ignored[256] = {0};
        awesome_skia_renderer_end_frame(frame->frame, ignored, sizeof(ignored));
        frame->frame = nullptr;
    }
    frame->~lua_skia_frame();
    return 0;
}

int frame_present(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (frame->offscreen)
        return luaL_error(L, "call :snapshot() on an offscreen image surface instead of :present()");
    char error[256] = {0};
    awesome_skia_frame_t *native = frame->frame;
    frame->frame = nullptr;
    if (!awesome_skia_renderer_end_frame(native, error, sizeof(error)))
        return luaL_error(L, "could not present Skia frame: %s", error);
    return 0;
}

int frame_clear(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->clear(check_color(L, 2));
    return 0;
}

int frame_save(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->save();
    frame->states.push_back(frame->state);
    return 0;
}

int frame_restore(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (frame->states.size() <= 1)
        return luaL_error(L, "Skia canvas restore without matching save");
    canvas(frame)->restore();
    /* save() pushed a snapshot of the then-current state, so restore() takes
     * that snapshot back off the stack. Reading states.back() *after*
     * popping would instead revert to the state one save() too far out,
     * silently dropping the source colour set before the save(). */
    frame->state = frame->states.back();
    frame->states.pop_back();
    return 0;
}

int frame_translate(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->translate(luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    return 0;
}

int frame_rotate(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->rotate(luaL_checknumber(L, 2) * 180.0f / static_cast<float>(M_PI));
    return 0;
}

int frame_scale(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->scale(luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    return 0;
}

int frame_transform_matrix(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const char *names[] = { "xx", "yx", "xy", "yy", "x0", "y0" };
    SkScalar values[6] = {};
    for (int index = 0; index < 6; ++index)
    {
        lua_getfield(L, 2, names[index]);
        values[index] = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    SkMatrix matrix;
    matrix.setAll(values[0], values[2], values[4], values[1], values[3], values[5],
                  0, 0, 1);
    canvas(frame)->concat(matrix);
    return 0;
}

int frame_clip(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->clipPath(frame->path.snapshot(), SkClipOp::kIntersect, true);
    reset_path(frame);
    return 0;
}

int frame_clip_rect(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->clipRect(SkRect::MakeXYWH(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                              luaL_checknumber(L, 4), luaL_checknumber(L, 5)));
    return 0;
}

int frame_clip_extents(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkRect bounds;
    if (!canvas(frame)->getLocalClipBounds(&bounds))
        bounds = SkRect::MakeEmpty();
    lua_pushnumber(L, bounds.left());
    lua_pushnumber(L, bounds.top());
    lua_pushnumber(L, bounds.right());
    lua_pushnumber(L, bounds.bottom());
    return 4;
}

int frame_rectangle(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    frame->path.addRect(SkRect::MakeXYWH(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                          luaL_checknumber(L, 4), luaL_checknumber(L, 5)));
    return 0;
}

int frame_move_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    frame->current = { static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.moveTo(frame->current);
    frame->force_move_to = false;
    return 0;
}

int frame_line_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    frame->current = { static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.lineTo(frame->current);
    return 0;
}

int frame_rel_move_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    frame->current = { frame->current.fX + static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       frame->current.fY + static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.moveTo(frame->current);
    return 0;
}

int frame_rel_line_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    frame->current = { frame->current.fX + static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       frame->current.fY + static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.lineTo(frame->current);
    return 0;
}

int frame_curve_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    const SkScalar x1 = luaL_checknumber(L, 2);
    const SkScalar y1 = luaL_checknumber(L, 3);
    const SkScalar x2 = luaL_checknumber(L, 4);
    const SkScalar y2 = luaL_checknumber(L, 5);
    frame->current = { static_cast<SkScalar>(luaL_checknumber(L, 6)),
                       static_cast<SkScalar>(luaL_checknumber(L, 7)) };
    frame->path.cubicTo(x1, y1, x2, y2, frame->current.fX, frame->current.fY);
    return 0;
}

int frame_close_path(lua_State *L)
{
    check_frame(L, 1)->path.close();
    return 0;
}

int frame_new_path(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    reset_path(frame);
    frame->force_move_to = false;
    return 0;
}

int frame_new_sub_path(lua_State *L)
{
    /* Keeps the accumulated path, but detaches the current point so a
     * following arc does not draw a connecting line into it. */
    check_frame(L, 1)->force_move_to = true;
    return 0;
}

/* Append a Cairo-style arc to the current path.
 *
 * Skia's arcTo() emits nothing for a full revolution: the start and stop
 * vectors coincide, so it produces zero conics. Cairo draws the whole circle,
 * and gears.shape.circle relies on that, so a full sweep becomes an oval. */
void append_arc(lua_skia_frame *frame, float x, float y, float radius,
                float start, float sweep)
{
    const SkRect oval = SkRect::MakeXYWH(x - radius, y - radius, radius * 2, radius * 2);
    constexpr float k_full = 2.0f * static_cast<float>(M_PI);

    if (std::abs(sweep) >= k_full - 1e-4f)
        frame->path.addOval(oval, sweep < 0 ? SkPathDirection::kCCW : SkPathDirection::kCW);
    else
        frame->path.arcTo(oval, start * 180.0f / static_cast<float>(M_PI),
                          sweep * 180.0f / static_cast<float>(M_PI), frame->force_move_to);

    frame->force_move_to = false;
}

int frame_arc(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    const float x = luaL_checknumber(L, 2);
    const float y = luaL_checknumber(L, 3);
    const float radius = luaL_checknumber(L, 4);
    const float start = luaL_checknumber(L, 5);
    const float finish = luaL_checknumber(L, 6);
    float sweep = finish - start;
    while (sweep < 0)
        sweep += 2.0f * static_cast<float>(M_PI);
    if (sweep > 2.0f * static_cast<float>(M_PI))
        sweep = 2.0f * static_cast<float>(M_PI);
    append_arc(frame, x, y, radius, start, sweep);
    frame->current = { x + radius * std::cos(finish), y + radius * std::sin(finish) };
    return 0;
}

int frame_arc_negative(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    note_path_start(frame);
    const float x = luaL_checknumber(L, 2);
    const float y = luaL_checknumber(L, 3);
    const float radius = luaL_checknumber(L, 4);
    const float start = luaL_checknumber(L, 5);
    const float finish = luaL_checknumber(L, 6);
    float sweep = finish - start;
    while (sweep > 0)
        sweep -= 2.0f * static_cast<float>(M_PI);
    if (sweep < -2.0f * static_cast<float>(M_PI))
        sweep = -2.0f * static_cast<float>(M_PI);
    append_arc(frame, x, y, radius, start, sweep);
    frame->current = { x + radius * std::cos(finish), y + radius * std::sin(finish) };
    return 0;
}

int frame_fill(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kFill_Style);
    draw_path(frame, frame->path.detach(), paint);
    frame->has_path_matrix = false;
    return 0;
}

int frame_fill_preserve(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kFill_Style);
    draw_path(frame, frame->path.snapshot(), paint);
    return 0;
}

int frame_stroke(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kStroke_Style);
    draw_path(frame, frame->path.detach(), paint);
    frame->has_path_matrix = false;
    return 0;
}

int frame_stroke_preserve(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kStroke_Style);
    draw_path(frame, frame->path.snapshot(), paint);
    return 0;
}

int frame_paint(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->drawPaint(frame->state.paint);
    return 0;
}

int frame_paint_with_alpha(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setAlphaf(std::clamp(static_cast<float>(luaL_checknumber(L, 2)), 0.0f, 1.0f));
    canvas(frame)->drawPaint(paint);
    return 0;
}

int frame_set_source_rgba(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setShader(nullptr);
    frame->state.paint.setColor(rgba(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                     luaL_checknumber(L, 4), luaL_checknumber(L, 5)));
    return 0;
}

int frame_set_source_rgb(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setShader(nullptr);
    frame->state.paint.setColor(rgba(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                     luaL_checknumber(L, 4), 1));
    return 0;
}

void set_source_from_index(lua_State *L, lua_skia_frame *frame, int index)
{
    frame->state.paint.setShader(nullptr);
    if (lua_skia_pattern *pattern = test_pattern(L, index))
    {
        if (pattern->shader)
            frame->state.paint.setShader(pattern->shader);
        else
            frame->state.paint.setColor(pattern->color);
    }
    else
        frame->state.paint.setColor(check_color(L, index));
}

int frame_set_source(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    set_source_from_index(L, frame, 2);
    return 0;
}

int frame_set_line_width(lua_State *L)
{
    check_frame(L, 1)->state.paint.setStrokeWidth(luaL_checknumber(L, 2));
    return 0;
}

int frame_set_line_cap(lua_State *L)
{
    const lua_Integer cap = luaL_checkinteger(L, 2);
    if (cap < static_cast<lua_Integer>(SkPaint::Cap::kButt_Cap) ||
        cap > static_cast<lua_Integer>(SkPaint::Cap::kSquare_Cap))
        return luaL_argerror(L, 2, "invalid Skia line cap");
    check_frame(L, 1)->state.paint.setStrokeCap(static_cast<SkPaint::Cap>(cap));
    return 0;
}

int frame_set_dash(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const size_t count = lua_rawlen(L, 2);
    if (count == 0)
    {
        frame->state.paint.setPathEffect(nullptr);
        return 0;
    }
    std::vector<SkScalar> intervals(count);
    for (size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(L, 2, static_cast<lua_Integer>(index + 1));
        intervals[index] = static_cast<SkScalar>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
    }
    const SkScalar offset = static_cast<SkScalar>(luaL_optnumber(L, 4, 0));
    frame->state.paint.setPathEffect(SkDashPathEffect::Make(
        SkSpan<const SkScalar>(intervals.data(), intervals.size()), offset));
    return 0;
}

/* cr:push_group() / cr:push_group_with_content(content) -- cairo's group
 * redirection. `content` only changes whether Cairo keeps colour data; a
 * Skia layer is always full ARGB and pop_group's mask use only reads alpha,
 * so the two spellings behave identically here. */
int frame_push_group(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkCanvas *target = canvas(frame);

    sk_sp<SkSurface> layer = target->makeSurface(target->imageInfo());
    if (!layer)
    {
        const SkImageInfo info = target->imageInfo();
        layer = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(
            std::max(info.width(), 1), std::max(info.height(), 1)));
    }
    if (!layer)
        return luaL_error(L, "could not create a group layer");
    layer->getCanvas()->clear(SK_ColorTRANSPARENT);
    layer->getCanvas()->setMatrix(target->getTotalMatrix());

    frame->group_stack.push_back({std::move(layer), target->getTotalMatrix()});
    return 0;
}

/* cr:pop_group() -- returns the popped layer as a Pattern, positioned so
 * that using it under the *same* CTM the group was pushed under (the only
 * way this codebase uses it) lands back on the same device pixels. */
int frame_pop_group(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (frame->group_stack.empty())
        return luaL_error(L, "pop_group() called without a matching push_group()");

    auto layer = std::move(frame->group_stack.back());
    frame->group_stack.pop_back();

    auto *pattern = new_pattern(L);
    pattern->kind = pattern_kind::image;
    pattern->image = layer.surface->makeImageSnapshot();
    if (!layer.push_matrix.invert(&pattern->local_matrix))
        pattern->local_matrix = SkMatrix::I();
    pattern->has_local_matrix = true;
    refresh_gradient(pattern);
    return 1;
}

int frame_pop_group_to_source(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (frame->group_stack.empty())
        return luaL_error(L, "pop_group_to_source() called without a matching push_group()");

    auto layer = std::move(frame->group_stack.back());
    frame->group_stack.pop_back();

    SkMatrix local_matrix;
    if (!layer.push_matrix.invert(&local_matrix))
        local_matrix = SkMatrix::I();

    frame->state.paint.setShader(layer.surface->makeImageSnapshot()->makeShader(
        SkTileMode::kClamp, SkTileMode::kClamp,
        SkSamplingOptions(SkFilterMode::kLinear), &local_matrix));
    return 0;
}

/* cr:mask(pattern) -- paint the current source, weighted by the alpha of
 * pattern's image. Composites as: draw the source, then multiply it by the
 * mask's alpha (Skia's kDstIn), then blend that result onto the destination
 * using whatever operator is already active. */
int frame_mask(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    lua_skia_pattern *pattern = test_pattern(L, 2);
    if (!pattern || !pattern->image)
        return luaL_argerror(L, 2, "expected an image pattern, e.g. from pop_group()");
    const SkScalar x = static_cast<SkScalar>(luaL_optnumber(L, 3, 0));
    const SkScalar y = static_cast<SkScalar>(luaL_optnumber(L, 4, 0));

    SkCanvas *target = canvas(frame);
    SkPaint restore_paint;
    restore_paint.setBlendMode(frame->state.paint.getBlendMode_or(SkBlendMode::kSrcOver));
    target->saveLayer(SkCanvas::SaveLayerRec(nullptr, &restore_paint));

    SkPaint fill_paint = frame->state.paint;
    fill_paint.setBlendMode(SkBlendMode::kSrcOver);
    target->drawPaint(fill_paint);

    SkPaint mask_paint;
    mask_paint.setBlendMode(SkBlendMode::kDstIn);
    target->save();
    if (pattern->has_local_matrix)
        target->concat(pattern->local_matrix);
    target->translate(x, y);
    target->drawImage(pattern->image, 0, 0, SkSamplingOptions(), &mask_paint);
    target->restore();

    target->restore();
    return 0;
}

int frame_set_fill_rule(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const char *rule = luaL_checkstring(L, 2);
    frame->path.setFillType(std::strcmp(rule, "EVEN_ODD") == 0 ||
                            std::strcmp(rule, "even_odd") == 0
                                ? SkPathFillType::kEvenOdd
                                : SkPathFillType::kWinding);
    return 0;
}

int frame_show_text(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    size_t length = 0;
    const char *text = luaL_checklstring(L, 2, &length);
    const char *family = luaL_optstring(L, 5, "sans");
    const float size = static_cast<float>(luaL_optnumber(L, 6, 12));
    sk_sp<SkTypeface> typeface;
    if (sk_sp<SkFontMgr> manager = font_manager())
        typeface = manager->matchFamilyStyle(family, SkFontStyle());
    SkFont font(typeface, std::max(size, 1.0f));
    canvas(frame)->drawString(text, luaL_checknumber(L, 3), luaL_checknumber(L, 4),
                              font, frame->state.paint);
    return 0;
}

/* Accepts either a file path (decoded and cached via load_encoded_image) or
 * an in-memory awesome.skia.image produced by an offscreen surface's
 * :snapshot(), e.g. a procedurally-drawn icon that never touched disk. */
sk_sp<SkImage> check_drawable_image(lua_State *L, int index)
{
    if (lua_skia_surface *surface = test_surface(L, index))
    {
        sk_sp<SkImage> image = surface_source(surface);
        if (!image)
            luaL_error(L, "surface has no pixels to draw");
        return image;
    }
    if (lua_skia_image *image = test_image(L, index))
    {
        if (!image->image)
            luaL_error(L, "Skia image has no pixel data");
        return image->image;
    }
    const char *path = luaL_checkstring(L, index);
    sk_sp<SkImage> image = load_encoded_image(path);
    if (!image)
        luaL_error(L, "Skia could not decode image '%s'", path);
    return image;
}

/* Pango supplies an already-resolved font run (including markup and
 * fallback). Skia/HarfBuzz shapes and draws that run; resolving this exact
 * Fontconfig pattern avoids a lossy family-name match in Skia. */
sk_sp<SkTypeface> typeface_for_pango_font(PangoFont *font)
{
    static std::unordered_map<std::string, sk_sp<SkTypeface>> cache;

    if (!PANGO_IS_FC_FONT(font))
        return nullptr;
    FcPattern *pattern = pango_fc_font_get_pattern(PANGO_FC_FONT(font));
    if (!pattern)
        return nullptr;

    FcChar8 *file = nullptr;
    if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch || !file)
        return nullptr;
    int index = 0;
    if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
        index = 0;
    int weight = FC_WEIGHT_REGULAR;
    FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight);
    int slant = FC_SLANT_ROMAN;
    FcPatternGetInteger(pattern, FC_SLANT, 0, &slant);

    const std::string key = std::string(reinterpret_cast<const char *>(file)) + "#" +
                            std::to_string(index) + "#" + std::to_string(weight) + "#" +
                            std::to_string(slant);
    const auto cached = cache.find(key);
    if (cached != cache.end())
        return cached->second;

    sk_sp<SkTypeface> typeface;
    if (sk_sp<SkFontMgr> manager = font_manager())
    {
        /* Fontconfig stores a named variable-font instance in the high bits
         * of FC_INDEX.  Skia expects only the collection-face index there;
         * passing the packed value selected the default face and silently
         * lost Pango's requested weight. */
        typeface = manager->makeFromFile(reinterpret_cast<const char *>(file), index & 0xffff);
        if (typeface)
        {
            const SkFontArguments::VariationPosition::Coordinate coordinates[] = {
                {SkFontArguments::VariationPosition::Coordinate::wght,
                 static_cast<float>(FcWeightToOpenType(weight))},
            };
            SkFontArguments arguments;
            arguments.setVariationDesignPosition({coordinates, 1});
            arguments.setSyntheticOblique(slant != FC_SLANT_ROMAN);
            if (sk_sp<SkTypeface> varied = typeface->makeClone(arguments))
                typeface = std::move(varied);
        }
    }
    cache.emplace(key, typeface);
    return typeface;
}

/* Pango reports sizes in PANGO_SCALE-ths of a pixel. */
float pango_font_pixel_size(PangoFont *font)
{
    PangoFontDescription *description = pango_font_describe_with_absolute_size(font);
    if (!description)
        return 0;
    const float size = static_cast<float>(pango_font_description_get_size(description)) /
                       static_cast<float>(PANGO_SCALE);
    pango_font_description_free(description);
    return size;
}

/* PangoFT2's Fontconfig pattern also carries its rasterisation request. The
 * glyph IDs alone are not enough: Skia otherwise picks its own default
 * hinting, which makes small UI text visibly heavier/different despite using
 * the same file and variation instance. */
void configure_sk_font_from_pango(SkFont *sk_font, PangoFont *font)
{
    sk_font->setSubpixel(true);
    sk_font->setEdging(SkFont::Edging::kAntiAlias);

    if (!PANGO_IS_FC_FONT(font))
        return;
    FcPattern *pattern = pango_fc_font_get_pattern(PANGO_FC_FONT(font));
    if (!pattern)
        return;

    int hint_style = FC_HINT_SLIGHT;
    FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &hint_style);
    switch (hint_style)
    {
    case FC_HINT_NONE:
        sk_font->setHinting(SkFontHinting::kNone);
        break;
    case FC_HINT_MEDIUM:
        sk_font->setHinting(SkFontHinting::kNormal);
        break;
    case FC_HINT_FULL:
        sk_font->setHinting(SkFontHinting::kFull);
        break;
    case FC_HINT_SLIGHT:
    default:
        sk_font->setHinting(SkFontHinting::kSlight);
        break;
    }

    FcBool auto_hint = FcFalse;
    if (FcPatternGetBool(pattern, FC_AUTOHINT, 0, &auto_hint) == FcResultMatch)
        sk_font->setForceAutoHinting(auto_hint == FcTrue);
}

SkPaint paint_for_pango_run(const lua_skia_frame *frame, const PangoGlyphItem *run)
{
    SkPaint paint = frame->state.paint;
    uint8_t red = SkColorGetR(paint.getColor());
    uint8_t green = SkColorGetG(paint.getColor());
    uint8_t blue = SkColorGetB(paint.getColor());
    uint8_t alpha = SkColorGetA(paint.getColor());

    for (GSList *node = run->item->analysis.extra_attrs; node; node = node->next)
    {
        auto *attribute = static_cast<PangoAttribute *>(node->data);
        if (!attribute || !attribute->klass)
            continue;
        if (attribute->klass->type == PANGO_ATTR_FOREGROUND)
        {
            const auto *color = reinterpret_cast<const PangoAttrColor *>(attribute);
            red = color->color.red >> 8;
            green = color->color.green >> 8;
            blue = color->color.blue >> 8;
        }
        else if (attribute->klass->type == PANGO_ATTR_FOREGROUND_ALPHA)
        {
            const auto *opacity = reinterpret_cast<const PangoAttrInt *>(attribute);
            alpha = CLAMP(opacity->value, 0, 65535) >> 8;
        }
    }
    paint.setColor(SkColorSetARGB(alpha, red, green, blue));
    return paint;
}

void draw_pango_glyph_run(lua_skia_frame *frame, PangoGlyphItem *run,
                          float origin_x, float baseline_y, const SkPaint &paint)
{
    PangoFont *font = run->item->analysis.font;
    sk_sp<SkTypeface> typeface = typeface_for_pango_font(font);
    const float size = pango_font_pixel_size(font);
    if (!typeface || size <= 0)
        return;

    SkFont sk_font(typeface, size);
    configure_sk_font_from_pango(&sk_font, font);

    const int count = run->glyphs->num_glyphs;
    if (count <= 0)
        return;

    SkTextBlobBuilder builder;
    const SkTextBlobBuilder::RunBuffer &buffer = builder.allocRunPos(sk_font, count);

    float pen = origin_x;
    int emitted = 0;
    for (int i = 0; i < count; ++i)
    {
        const PangoGlyphInfo &info = run->glyphs->glyphs[i];
        /* Pango marks unknown glyphs with a flag in the high bits; drawing
         * that raw value would index a nonsense glyph in the Skia face. */
        if (info.glyph != PANGO_GLYPH_EMPTY && !(info.glyph & PANGO_GLYPH_UNKNOWN_FLAG))
        {
            buffer.glyphs[emitted] = static_cast<SkGlyphID>(info.glyph);
            buffer.points()[emitted] = SkPoint::Make(
                pen + static_cast<float>(info.geometry.x_offset) / PANGO_SCALE,
                baseline_y + static_cast<float>(info.geometry.y_offset) / PANGO_SCALE);
            ++emitted;
        }
        pen += static_cast<float>(info.geometry.width) / PANGO_SCALE;
    }
    /* Unused slots would otherwise draw glyph 0 at (0,0). */
    for (int i = emitted; i < count; ++i)
    {
        buffer.glyphs[i] = 0;
        buffer.points()[i] = SkPoint::Make(-10000, -10000);
    }

    if (emitted > 0)
        canvas(frame)->drawTextBlob(builder.make(), 0, 0, paint);
}

void draw_pango_run(lua_skia_frame *frame, PangoGlyphItem *run,
                    float origin_x, float baseline_y)
{
    /* Pango has already shaped this exact run. Reusing its glyph IDs and
     * positions preserves markup features, letter spacing, fallback, bidi,
     * and alignment; Skia performs only the GPU rasterisation. */
    draw_pango_glyph_run(frame, run, origin_x, baseline_y,
                         paint_for_pango_run(frame, run));
}

/* frame:show_layout(layout_pointer, x, y) -- layout is an lgi PangoLayout's
 * native pointer. Pango applies markup, wrapping and line breaking; Skia
 * shapes and draws the final text. */
int frame_show_layout(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    auto *layout = static_cast<PangoLayout *>(lua_touserdata(L, 2));
    if (!layout || !PANGO_IS_LAYOUT(layout))
        return luaL_argerror(L, 2, "expected a PangoLayout pointer");

    const float origin_x = static_cast<float>(luaL_optnumber(L, 3, 0));
    const float origin_y = static_cast<float>(luaL_optnumber(L, 4, 0));

    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    if (!iter)
        return 0;

    do
    {
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
        if (!line)
            continue;

        const float baseline = origin_y +
            static_cast<float>(pango_layout_iter_get_baseline(iter)) / PANGO_SCALE;

        PangoRectangle logical = {};
        /* The iterator extents include PangoLayout's center/right alignment;
         * PangoLayoutLine's own extents are local to the line and start at 0. */
        pango_layout_iter_get_line_extents(iter, nullptr, &logical);
        float pen = origin_x + static_cast<float>(logical.x) / PANGO_SCALE;

        for (GSList *item = line->runs; item; item = item->next)
        {
            auto *run = static_cast<PangoGlyphItem *>(item->data);
            if (!run)
                continue;
            draw_pango_run(frame, run, pen, baseline);
            pen += static_cast<float>(pango_glyph_string_get_width(run->glyphs)) / PANGO_SCALE;
        }
    } while (pango_layout_iter_next_line(iter));

    pango_layout_iter_free(iter);
    return 0;
}

int frame_draw_image(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    sk_sp<SkImage> image = check_drawable_image(L, 2);
    canvas(frame)->drawImage(image, luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    return 0;
}

int skia_image_dimensions(lua_State *L)
{
    if (lua_skia_image *image = test_image(L, 1))
    {
        if (!image->image)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, image->image->width());
        lua_pushinteger(L, image->image->height());
        return 2;
    }
    const char *path = luaL_checkstring(L, 1);
    sk_sp<SkImage> image = load_encoded_image(path);
    if (!image)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, image->width());
    lua_pushinteger(L, image->height());
    return 2;
}

int skia_new_image_surface(lua_State *L)
{
    const int width = static_cast<int>(luaL_checkinteger(L, 1));
    const int height = static_cast<int>(luaL_checkinteger(L, 2));
    if (width <= 0 || height <= 0)
        return luaL_error(L, "image surface dimensions must be positive");

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface)
        return luaL_error(L, "could not create offscreen Skia surface");
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);

    auto *frame = static_cast<lua_skia_frame *>(lua_newuserdata(L, sizeof(lua_skia_frame)));
    new (frame) lua_skia_frame();
    frame->offscreen = std::move(surface);
    frame->state.paint.setAntiAlias(true);
    frame->states.push_back(frame->state);
    luaL_getmetatable(L, k_frame_type);
    lua_setmetatable(L, -2);
    return 1;
}

/* Wrap an SkImage as the awesome.skia.image userdata. */
void push_image(lua_State *L, sk_sp<SkImage> source)
{
    auto *image = static_cast<lua_skia_image *>(lua_newuserdata(L, sizeof(lua_skia_image)));
    new (image) lua_skia_image();
    image->image = std::move(source);
    luaL_getmetatable(L, k_image_type);
    lua_setmetatable(L, -2);
}

/* skia.ImageSurface(format, width, height) -- cairo.ImageSurface */
int skia_image_surface(lua_State *L)
{
    /* cairo spells this both as a call and as ImageSurface.create(...); the
     * latter arrives with the table as argument 1. */
    int base = lua_istable(L, 1) ? 1 : 0;
    const SkColorType color_type = color_type_from_format(L, base + 1);
    const int width = static_cast<int>(luaL_checkinteger(L, base + 2));
    const int height = static_cast<int>(luaL_checkinteger(L, base + 3));
    if (width <= 0 || height <= 0)
        return luaL_error(L, "image surface dimensions must be positive");

    sk_sp<SkSurface> raster = SkSurfaces::Raster(
        SkImageInfo::Make(width, height, color_type, kPremul_SkAlphaType));
    if (!raster)
        return luaL_error(L, "could not create a %dx%d Skia surface", width, height);
    raster->getCanvas()->clear(SK_ColorTRANSPARENT);

    lua_skia_surface *surface = push_surface(L);
    surface->raster = std::move(raster);
    surface->width = width;
    surface->height = height;
    return 1;
}

/* skia.SvgSurface(path, width, height) -- a vector surface backed by Skia's
 * SVG canvas. Context(surface) exposes the same Cairo-shaped drawing methods
 * as ImageSurface, and finish() commits the document. */
int skia_svg_surface(lua_State *L)
{
    const int base = lua_istable(L, 1) ? 1 : 0;
    const char *path = luaL_checkstring(L, base + 1);
    const int width = static_cast<int>(luaL_checkinteger(L, base + 2));
    const int height = static_cast<int>(luaL_checkinteger(L, base + 3));
    if (width <= 0 || height <= 0)
        return luaL_error(L, "SVG surface dimensions must be positive");

    lua_skia_surface *surface = push_surface(L);
    surface->svg_stream = std::make_unique<SkFILEWStream>(path);
    if (!surface->svg_stream->isValid())
        return luaL_error(L, "could not open SVG '%s'", path);
    surface->svg_canvas = SkSVGCanvas::Make(
        SkRect::MakeWH(width, height), surface->svg_stream.get());
    if (!surface->svg_canvas)
        return luaL_error(L, "could not create SVG canvas for '%s'", path);
    surface->width = width;
    surface->height = height;
    return 1;
}

/* skia.Surface.load(path) -- decode a file into a source-only surface. */
int skia_surface_load(lua_State *L)
{
    const int index = lua_istable(L, 1) ? 2 : 1;
    const char *path = luaL_checkstring(L, index);
    sk_sp<SkImage> image = load_encoded_image(path);
    if (!image)
    {
        lua_pushnil(L);
        lua_pushfstring(L, "Skia could not decode image '%s'", path);
        return 2;
    }
    lua_skia_surface *surface = push_surface(L);
    surface->width = image->width();
    surface->height = image->height();
    surface->image = std::move(image);
    return 1;
}

/* skia.Surface.from_bgra(data, width, height, stride) -- create a source
 * surface from packed BGRA pixels. This is used for D-Bus notification image
 * data; Skia owns the copied bytes and uploads them when the image is drawn. */
int skia_surface_from_bgra(lua_State *L)
{
    size_t length = 0;
    const char *pixels = luaL_checklstring(L, 1, &length);
    const int width = static_cast<int>(luaL_checkinteger(L, 2));
    const int height = static_cast<int>(luaL_checkinteger(L, 3));
    const size_t stride = static_cast<size_t>(luaL_checkinteger(L, 4));
    if (width <= 0 || height <= 0 || stride < static_cast<size_t>(width) * 4 ||
        length < stride * static_cast<size_t>(height))
        return luaL_error(L, "invalid BGRA image data");

    sk_sp<SkData> data = SkData::MakeWithCopy(pixels, stride * static_cast<size_t>(height));
    sk_sp<SkImage> image = SkImages::RasterFromData(
        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
        std::move(data), stride);
    if (!image)
        return luaL_error(L, "could not create Skia image from BGRA data");

    lua_skia_surface *surface = push_surface(L);
    surface->image = std::move(image);
    surface->width = width;
    surface->height = height;
    return 1;
}

#ifdef AWESOME_SKIA_HAS_SVG
/* skia.svg_dimensions(path) -- the SVG's intrinsic size (from its width/
 * height or viewBox), before any target size is chosen. Mirrors what
 * Rsvg.Handle:get_dimensions() gave callers under Cairo. */
int skia_svg_dimensions(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    std::unique_ptr<SkFILEStream> stream = SkFILEStream::Make(path);
    if (!stream || !stream->isValid())
    {
        lua_pushnil(L);
        return 1;
    }
    sk_sp<SkSVGDOM> dom = SkSVGDOM::MakeFromStream(*stream);
    if (!dom)
    {
        lua_pushnil(L);
        return 1;
    }
    const SkSize size = dom->containerSize();
    float width = size.width();
    float height = size.height();
    if ((width <= 0 || height <= 0) && !svg_intrinsic_size(path, &width, &height))
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, width);
    lua_pushnumber(L, height);
    return 2;
}

/* skia.load_svg(path, width, height) -- rasterize an SVG at a given size into
 * a normal surface, so every other API (Context, draw_image, get_width...)
 * treats it exactly like any other image. Replaces RSVG-over-Cairo: Skia
 * renders it directly, no Cairo bridge involved. */
int skia_load_svg(lua_State *L)
{
    const int base = lua_istable(L, 1) ? 1 : 0;
    const char *path = luaL_checkstring(L, base + 1);
    const int width = static_cast<int>(luaL_checkinteger(L, base + 2));
    const int height = static_cast<int>(luaL_checkinteger(L, base + 3));
    const char *stylesheet = luaL_optstring(L, base + 4, nullptr);
    if (width <= 0 || height <= 0)
        return luaL_error(L, "SVG surface dimensions must be positive");

    const std::string source = styled_svg(path, stylesheet);
    if (source.empty())
    {
        lua_pushnil(L);
        lua_pushfstring(L, "could not open SVG '%s'", path);
        return 2;
    }
    SkMemoryStream stream(source.data(), source.size(), false);
    sk_sp<SkSVGDOM> dom = SkSVGDOM::MakeFromStream(stream);
    if (!dom)
    {
        lua_pushnil(L);
        lua_pushfstring(L, "could not parse SVG '%s'", path);
        return 2;
    }
    dom->setContainerSize(SkSize::Make(static_cast<float>(width), static_cast<float>(height)));

    sk_sp<SkSurface> raster = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!raster)
        return luaL_error(L, "could not create a %dx%d Skia surface", width, height);
    raster->getCanvas()->clear(SK_ColorTRANSPARENT);
    dom->render(raster->getCanvas());

    lua_skia_surface *surface = push_surface(L);
    surface->raster = std::move(raster);
    surface->width = width;
    surface->height = height;
    return 1;
}
#else
int skia_svg_dimensions(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

int skia_load_svg(lua_State *L)
{
    lua_pushnil(L);
    lua_pushliteral(L, "this Skia build was linked without the SVG module");
    return 2;
}
#endif

int skia_surface_is_type_of(lua_State *L)
{
    const int index = lua_istable(L, 1) ? 2 : 1;
    lua_pushboolean(L, test_surface(L, index) != nullptr);
    return 1;
}

int surface_get_width(lua_State *L)
{
    lua_pushinteger(L, static_cast<lua_skia_surface *>(
        luaL_checkudata(L, 1, k_surface_type))->width);
    return 1;
}

int surface_get_height(lua_State *L)
{
    lua_pushinteger(L, static_cast<lua_skia_surface *>(
        luaL_checkudata(L, 1, k_surface_type))->height);
    return 1;
}

int surface_write_to_png(lua_State *L)
{
    auto *surface = static_cast<lua_skia_surface *>(luaL_checkudata(L, 1, k_surface_type));
    const char *path = luaL_checkstring(L, 2);
    sk_sp<SkImage> image = surface_source(surface);
    SkPixmap pixmap;
    if (!image || !image->peekPixels(&pixmap))
        return luaL_error(L, "surface has no readable pixels for '%s'", path);
    SkFILEWStream stream(path);
    if (!stream.isValid() || !SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options()))
        return luaL_error(L, "could not write PNG '%s'", path);
    return 0;
}

/* cairo_surface_finish(): explicitly releases backend resources. Skia surfaces
 * are entirely reference-counted, so there is nothing to do early; this exists
 * so callers that finish() a surface deterministically still work unchanged. */
int surface_finish(lua_State *L)
{
    auto *surface = static_cast<lua_skia_surface *>(luaL_checkudata(L, 1, k_surface_type));
    surface->svg_canvas.reset();
    surface->svg_stream.reset();
    return 0;
}

int surface_gc(lua_State *L)
{
    auto *surface = static_cast<lua_skia_surface *>(luaL_checkudata(L, 1, k_surface_type));
    surface->~lua_skia_surface();
    return 0;
}

int surface_index(lua_State *L)
{
    luaL_getmetatable(L, k_surface_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);

    auto *surface = static_cast<lua_skia_surface *>(luaL_checkudata(L, 1, k_surface_type));
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "width") == 0)
    {
        lua_pushinteger(L, surface->width);
        return 1;
    }
    if (std::strcmp(name, "height") == 0)
    {
        lua_pushinteger(L, surface->height);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int frame_snapshot(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (!frame->offscreen)
        return luaL_error(L, "snapshot() only applies to an offscreen image surface "
                             "(created with skia.new_image_surface)");
    push_image(L, frame->offscreen->makeImageSnapshot());
    frame->offscreen.reset();
    return 1;
}

/* skia.load_image(path) -> image, or nil plus a message. This is the Skia
 * counterpart to gears.surface loading a file into a cairo surface. */
int skia_load_image(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    sk_sp<SkImage> image = load_encoded_image(path);
    if (!image)
    {
        lua_pushnil(L);
        lua_pushfstring(L, "Skia could not decode image '%s'", path);
        return 2;
    }
    push_image(L, std::move(image));
    return 1;
}

int image_get_width(lua_State *L)
{
    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    lua_pushinteger(L, image->image ? image->image->width() : 0);
    return 1;
}

int image_get_height(lua_State *L)
{
    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    lua_pushinteger(L, image->image ? image->image->height() : 0);
    return 1;
}

/* Write the image out as a PNG. Intended for tests and for eyeballing what a
 * widget actually rendered during the Cairo migration. */
int image_save_png(lua_State *L)
{
    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    const char *path = luaL_checkstring(L, 2);
    if (!image->image)
        return luaL_error(L, "Skia image has no pixel data");

    SkPixmap pixmap;
    if (!image->image->peekPixels(&pixmap))
        return luaL_error(L, "Skia image is not raster-backed; cannot save '%s'", path);

    SkFILEWStream stream(path);
    if (!stream.isValid())
        return luaL_error(L, "could not open '%s' for writing", path);
    if (!SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options()))
        return luaL_error(L, "could not encode PNG '%s'", path);
    return 0;
}

int image_gc(lua_State *L)
{
    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    image->~lua_skia_image();
    return 0;
}

int image_index(lua_State *L)
{
    /* Methods registered in the metatable win; the names below are computed
     * properties rather than stored fields. */
    luaL_getmetatable(L, k_image_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);

    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "width") == 0)
    {
        lua_pushinteger(L, image->image ? image->image->width() : 0);
        return 1;
    }
    if (std::strcmp(name, "height") == 0)
    {
        lua_pushinteger(L, image->image ? image->image->height() : 0);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int frame_index(lua_State *L)
{
    luaL_getmetatable(L, k_frame_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "status") == 0)
    {
        lua_pushliteral(L, "SUCCESS");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int frame_newindex(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "operator") == 0)
    {
        frame->state.paint.setBlendMode(blend_mode_from_operator(L, 3));
        return 0;
    }
    if (std::strcmp(name, "source") == 0)
    {
        set_source_from_index(L, frame, 3);
        return 0;
    }
    if (std::strcmp(name, "line_width") == 0)
    {
        frame->state.paint.setStrokeWidth(luaL_checknumber(L, 3));
        return 0;
    }
    return luaL_error(L, "Skia canvas has no writable '%s' property", name);
}

int frame_set_operator(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setBlendMode(blend_mode_from_operator(L, 2));
    return 0;
}

int pattern_create_rgba(lua_State *L)
{
    auto *pattern = new_pattern(L);
    pattern->color = rgba(luaL_checknumber(L, 1), luaL_checknumber(L, 2),
                          luaL_checknumber(L, 3), luaL_optnumber(L, 4, 1));
    return 1;
}

int pattern_create_linear(lua_State *L)
{
    auto *pattern = new_pattern(L);
    pattern->kind = pattern_kind::linear;
    pattern->start = {static_cast<SkScalar>(luaL_checknumber(L, 1)),
                      static_cast<SkScalar>(luaL_checknumber(L, 2))};
    pattern->end = {static_cast<SkScalar>(luaL_checknumber(L, 3)),
                    static_cast<SkScalar>(luaL_checknumber(L, 4))};
    return 1;
}

int pattern_create_radial(lua_State *L)
{
    auto *pattern = new_pattern(L);
    pattern->kind = pattern_kind::radial;
    pattern->start = {static_cast<SkScalar>(luaL_checknumber(L, 1)),
                      static_cast<SkScalar>(luaL_checknumber(L, 2))};
    pattern->start_radius = static_cast<SkScalar>(luaL_checknumber(L, 3));
    pattern->end = {static_cast<SkScalar>(luaL_checknumber(L, 4)),
                    static_cast<SkScalar>(luaL_checknumber(L, 5))};
    pattern->end_radius = static_cast<SkScalar>(luaL_checknumber(L, 6));
    return 1;
}

/* skia.Pattern.create_for_surface(surface) -- cairo.Pattern.create_for_surface */
int pattern_create_for_surface(lua_State *L)
{
    const int index = lua_istable(L, 1) ? 2 : 1;
    sk_sp<SkImage> image;
    if (lua_skia_surface *surface = test_surface(L, index))
        image = surface_source(surface);
    else if (lua_skia_image *stored = test_image(L, index))
        image = stored->image;
    if (!image)
        return luaL_argerror(L, index, "expected a surface with pixels");

    auto *pattern = new_pattern(L);
    pattern->kind = pattern_kind::image;
    pattern->image = std::move(image);
    refresh_gradient(pattern);
    return 1;
}

int pattern_add_color_stop_rgba(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind == pattern_kind::solid)
        return luaL_error(L, "cannot add a color stop to a solid pattern");

    const SkScalar offset = std::clamp(static_cast<SkScalar>(luaL_checknumber(L, 2)), 0.0f, 1.0f);
    const SkColor4f color = SkColor4f::FromColor(rgba(luaL_checknumber(L, 3), luaL_checknumber(L, 4),
                                                        luaL_checknumber(L, 5), luaL_checknumber(L, 6)));
    const auto position = std::upper_bound(pattern->offsets.begin(), pattern->offsets.end(), offset);
    const size_t index = static_cast<size_t>(position - pattern->offsets.begin());
    pattern->offsets.insert(position, offset);
    pattern->colors.insert(pattern->colors.begin() + index, color);
    refresh_gradient(pattern);
    return 0;
}

int pattern_set_extend(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    const char *extend = luaL_checkstring(L, 2);
    if (std::strcmp(extend, "REPEAT") == 0 || std::strcmp(extend, "repeat") == 0)
        pattern->tile_mode = SkTileMode::kRepeat;
    else if (std::strcmp(extend, "REFLECT") == 0 || std::strcmp(extend, "reflect") == 0)
        pattern->tile_mode = SkTileMode::kMirror;
    else if (std::strcmp(extend, "NONE") == 0 || std::strcmp(extend, "none") == 0)
        pattern->tile_mode = SkTileMode::kDecal;
    else
        pattern->tile_mode = SkTileMode::kClamp;
    refresh_gradient(pattern);
    return 0;
}

int pattern_get_extend(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    const char *extend = "PAD";
    if (pattern->tile_mode == SkTileMode::kRepeat)
        extend = "REPEAT";
    else if (pattern->tile_mode == SkTileMode::kMirror)
        extend = "REFLECT";
    else if (pattern->tile_mode == SkTileMode::kDecal)
        extend = "NONE";
    lua_pushstring(L, extend);
    return 1;
}

int pattern_is_type_of(lua_State *L)
{
    /* LGI exposes this as Pattern:is_type_of(value), while callers may also
     * use it as Pattern.is_type_of(value). Accept both spellings. */
    const int value_index = lua_istable(L, 1) ? 2 : 1;
    lua_pushboolean(L, test_pattern(L, value_index) != nullptr);
    return 1;
}

int pattern_get_type(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    lua_pushstring(L, pattern_kind_name(pattern->kind));
    return 1;
}

int pattern_get_rgba(lua_State *L)
{
    const auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, SkColorGetR(pattern->color) / 255.0);
    lua_pushnumber(L, SkColorGetG(pattern->color) / 255.0);
    lua_pushnumber(L, SkColorGetB(pattern->color) / 255.0);
    lua_pushnumber(L, SkColorGetA(pattern->color) / 255.0);
    return 5;
}

int pattern_get_color_stop_count(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    lua_pushliteral(L, "SUCCESS");
    lua_pushinteger(L, static_cast<lua_Integer>(pattern->colors.size()));
    return 2;
}

int pattern_get_color_stop_rgba(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index < 0 || static_cast<size_t>(index) >= pattern->colors.size())
        return luaL_error(L, "color stop index out of range");
    const SkColor color = pattern->colors[static_cast<size_t>(index)].toSkColor();
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, pattern->offsets[static_cast<size_t>(index)]);
    lua_pushnumber(L, SkColorGetR(color) / 255.0);
    lua_pushnumber(L, SkColorGetG(color) / 255.0);
    lua_pushnumber(L, SkColorGetB(color) / 255.0);
    lua_pushnumber(L, SkColorGetA(color) / 255.0);
    return 6;
}

int pattern_get_linear_points(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind != pattern_kind::linear)
    {
        lua_pushliteral(L, "PATTERN_TYPE_MISMATCH");
        return 1;
    }
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, pattern->start.x());
    lua_pushnumber(L, pattern->start.y());
    lua_pushnumber(L, pattern->end.x());
    lua_pushnumber(L, pattern->end.y());
    return 5;
}

int pattern_get_radial_circles(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind != pattern_kind::radial)
    {
        lua_pushliteral(L, "PATTERN_TYPE_MISMATCH");
        return 1;
    }
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, pattern->start.x());
    lua_pushnumber(L, pattern->start.y());
    lua_pushnumber(L, pattern->start_radius);
    lua_pushnumber(L, pattern->end.x());
    lua_pushnumber(L, pattern->end.y());
    lua_pushnumber(L, pattern->end_radius);
    return 7;
}

/* cairo.Pattern:get_surface() -- the surface an image pattern was made from. */
int pattern_get_surface(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind != pattern_kind::image || !pattern->image)
    {
        lua_pushliteral(L, "PATTERN_TYPE_MISMATCH");
        return 1;
    }
    lua_pushliteral(L, "SUCCESS");
    lua_skia_surface *surface = push_surface(L);
    surface->image = pattern->image;
    surface->width = pattern->image->width();
    surface->height = pattern->image->height();
    return 2;
}

int pattern_gc(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    pattern->~lua_skia_pattern();
    return 0;
}

int pattern_index(lua_State *L)
{
    luaL_getmetatable(L, k_pattern_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);

    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "type") == 0)
        return pattern_get_type(L);
    lua_pushnil(L);
    return 1;
}

int pattern_newindex(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "extend") == 0)
    {
        /* pattern_set_extend reads its value from argument 2 (the method
         * form is pattern:set_extend(value)); property assignment puts the
         * value at argument 3, so shift it down before delegating. */
        lua_remove(L, 2);
        return pattern_set_extend(L);
    }
    return luaL_error(L, "Skia pattern has no writable '%s' property", name);
}

void set_method(lua_State *L, const char *name, lua_CFunction method)
{
    lua_pushcfunction(L, method);
    lua_setfield(L, -2, name);
}

/* cairo exposes its enums as plain integer fields; mirror that so existing
 * code like cairo.Operator.SOURCE keeps working after the require swap. */
void set_enum_table(lua_State *L, const char *name,
                    std::initializer_list<std::pair<const char *, int>> values)
{
    lua_newtable(L);
    for (const auto &entry : values)
    {
        lua_pushinteger(L, entry.second);
        lua_setfield(L, -2, entry.first);
    }
    lua_setfield(L, -2, name);
}

} // namespace

extern "C" awesome_skia_image_t *awesome_skia_image_from_argb32(
    int width, int height, const uint32_t *pixels)
{
    if (width <= 0 || height <= 0 || !pixels ||
        static_cast<size_t>(width) > SIZE_MAX / static_cast<size_t>(height))
        return nullptr;

    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint32_t> premultiplied(count);
    for (size_t index = 0; index < count; ++index)
    {
        const uint32_t source = pixels[index];
        const uint8_t alpha = source >> 24;
        const uint8_t red = (source >> 16) & 0xff;
        const uint8_t green = (source >> 8) & 0xff;
        const uint8_t blue = source & 0xff;
        premultiplied[index] = SkColorSetARGB(
            alpha, (red * alpha + 127) / 255, (green * alpha + 127) / 255,
            (blue * alpha + 127) / 255);
    }

    sk_sp<SkData> data = SkData::MakeWithCopy(premultiplied.data(), count * sizeof(uint32_t));
    sk_sp<SkImage> image = SkImages::RasterFromData(
        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
        std::move(data), static_cast<size_t>(width) * sizeof(uint32_t));
    if (!image)
        return nullptr;

    auto *result = new (std::nothrow) awesome_skia_image;
    if (!result)
        return nullptr;
    result->image = std::move(image);
    return result;
}

extern "C" awesome_skia_image_t *awesome_skia_image_ref(awesome_skia_image_t *image)
{
    if (image)
        image->references.fetch_add(1, std::memory_order_relaxed);
    return image;
}

extern "C" void awesome_skia_image_unref(awesome_skia_image_t *image)
{
    if (image && image->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete image;
}

extern "C" int awesome_skia_image_width(const awesome_skia_image_t *image)
{
    return image && image->image ? image->image->width() : 0;
}

extern "C" int awesome_skia_image_height(const awesome_skia_image_t *image)
{
    return image && image->image ? image->image->height() : 0;
}

extern "C" void awesome_skia_image_push_lua(lua_State *L,
                                               const awesome_skia_image_t *image)
{
    if (image && image->image)
        push_image(L, image->image);
    else
        lua_pushnil(L);
}

extern "C" awesome_skia_image_t *awesome_skia_image_from_lua(lua_State *L, int index)
{
    lua_skia_image *lua_image = test_image(L, index);
    sk_sp<SkImage> source = lua_image ? lua_image->image : nullptr;
    if (!source)
        if (lua_skia_surface *surface = test_surface(L, index))
            source = surface_source(surface);
    if (!source)
        return nullptr;
    auto *result = new (std::nothrow) awesome_skia_image;
    if (result)
        result->image = std::move(source);
    return result;
}

extern "C" bool awesome_skia_alpha_mask_from_lua(lua_State *L, int index,
                                                   uint8_t *alpha, int width,
                                                   int height)
{
    if (!alpha || width <= 0 || height <= 0)
        return false;
    sk_sp<SkImage> image;
    if (lua_skia_surface *surface = test_surface(L, index))
        image = surface_source(surface);
    else if (lua_skia_image *stored = test_image(L, index))
        image = stored->image;
    if (!image || image->width() != width || image->height() != height)
        return false;

    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    if (!image->readPixels(SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType),
                           pixels.data(), static_cast<size_t>(width) * sizeof(uint32_t),
                           0, 0))
        return false;
    for (size_t i = 0; i < pixels.size(); ++i)
        alpha[i] = pixels[i] >> 24;
    return true;
}

extern "C" void awesome_skia_lua_extend(lua_State *L)
{
    luaL_getmetatable(L, k_frame_type);
    set_method(L, "__gc", frame_gc);
    set_method(L, "__index", frame_index);
    set_method(L, "__newindex", frame_newindex);
    set_method(L, "present", frame_present);
    set_method(L, "clear", frame_clear);
    set_method(L, "save", frame_save);
    set_method(L, "restore", frame_restore);
    set_method(L, "translate", frame_translate);
    set_method(L, "scale", frame_scale);
    set_method(L, "rotate", frame_rotate);
    set_method(L, "transform_matrix", frame_transform_matrix);
    /* Cairo spelling. A gears.matrix instance carries the same
     * xx/yx/xy/yy/x0/y0 fields, so it is accepted without conversion. */
    set_method(L, "transform", frame_transform_matrix);
    set_method(L, "clip", frame_clip);
    set_method(L, "clip_rect", frame_clip_rect);
    set_method(L, "clip_extents", frame_clip_extents);
    set_method(L, "rectangle", frame_rectangle);
    set_method(L, "move_to", frame_move_to);
    set_method(L, "line_to", frame_line_to);
    set_method(L, "rel_move_to", frame_rel_move_to);
    set_method(L, "rel_line_to", frame_rel_line_to);
    set_method(L, "curve_to", frame_curve_to);
    set_method(L, "arc", frame_arc);
    set_method(L, "arc_negative", frame_arc_negative);
    set_method(L, "close_path", frame_close_path);
    set_method(L, "new_path", frame_new_path);
    set_method(L, "new_sub_path", frame_new_sub_path);
    set_method(L, "fill", frame_fill);
    set_method(L, "fill_preserve", frame_fill_preserve);
    set_method(L, "stroke", frame_stroke);
    set_method(L, "stroke_preserve", frame_stroke_preserve);
    set_method(L, "paint", frame_paint);
    set_method(L, "paint_with_alpha", frame_paint_with_alpha);
    set_method(L, "set_source", frame_set_source);
    set_method(L, "set_source_rgb", frame_set_source_rgb);
    set_method(L, "set_source_rgba", frame_set_source_rgba);
    set_method(L, "set_source_surface", frame_set_source_surface);
    set_method(L, "set_line_width", frame_set_line_width);
    set_method(L, "set_line_cap", frame_set_line_cap);
    set_method(L, "set_dash", frame_set_dash);
    set_method(L, "push_group", frame_push_group);
    set_method(L, "push_group_with_content", frame_push_group);
    set_method(L, "pop_group", frame_pop_group);
    set_method(L, "pop_group_to_source", frame_pop_group_to_source);
    set_method(L, "mask", frame_mask);
    set_method(L, "set_fill_rule", frame_set_fill_rule);
    set_method(L, "set_operator", frame_set_operator);
    set_method(L, "show_text", frame_show_text);
    set_method(L, "show_layout", frame_show_layout);
    set_method(L, "draw_image", frame_draw_image);
    set_method(L, "snapshot", frame_snapshot);
    lua_pop(L, 1);

    luaL_newmetatable(L, k_image_type);
    set_method(L, "__gc", image_gc);
    set_method(L, "__index", image_index);
    set_method(L, "save_png", image_save_png);
    /* Cairo surface spelling, so call sites that measure an icon work. */
    set_method(L, "get_width", image_get_width);
    set_method(L, "get_height", image_get_height);
    lua_pop(L, 1);

    luaL_newmetatable(L, k_pattern_type);
    set_method(L, "__gc", pattern_gc);
    set_method(L, "__index", pattern_index);
    set_method(L, "__newindex", pattern_newindex);
    set_method(L, "get_type", pattern_get_type);
    set_method(L, "get_rgba", pattern_get_rgba);
    set_method(L, "add_color_stop_rgba", pattern_add_color_stop_rgba);
    set_method(L, "set_extend", pattern_set_extend);
    set_method(L, "get_extend", pattern_get_extend);
    set_method(L, "get_color_stop_count", pattern_get_color_stop_count);
    set_method(L, "get_color_stop_rgba", pattern_get_color_stop_rgba);
    set_method(L, "get_linear_points", pattern_get_linear_points);
    set_method(L, "get_radial_circles", pattern_get_radial_circles);
    set_method(L, "get_surface", pattern_get_surface);
    lua_pop(L, 1);

    luaL_newmetatable(L, k_surface_type);
    set_method(L, "__gc", surface_gc);
    set_method(L, "__index", surface_index);
    set_method(L, "get_width", surface_get_width);
    set_method(L, "get_height", surface_get_height);
    set_method(L, "write_to_png", surface_write_to_png);
    set_method(L, "finish", surface_finish);
    lua_pop(L, 1);

    lua_getglobal(L, "skia");

    /* Cairo-shaped namespace. Call sites keep the spelling they already use,
     * so porting a config is a require swap rather than a rewrite. */
    set_method(L, "Context", skia_context);

    /* cairo code calls this both as ImageSurface(...) and as
     * ImageSurface.create(...), so expose a table that supports both. */
    lua_newtable(L);
    set_method(L, "create", skia_image_surface);
    lua_newtable(L);
    set_method(L, "__call", skia_image_surface);
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "ImageSurface");

    lua_newtable(L);
    set_method(L, "create", skia_svg_surface);
    lua_newtable(L);
    set_method(L, "__call", skia_svg_surface);
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "SvgSurface");

    lua_newtable(L);
    set_method(L, "load", skia_surface_load);
    set_method(L, "from_bgra", skia_surface_from_bgra);
    set_method(L, "is_type_of", skia_surface_is_type_of);
    lua_setfield(L, -2, "Surface");

    set_enum_table(L, "Format", {{"ARGB32", 0}, {"RGB24", 2}, {"A8", 3}, {"A1", 4}});
    set_enum_table(L, "Content", {{"COLOR", 0x1000}, {"ALPHA", 0x2000},
                                  {"COLOR_ALPHA", 0x3000}});
    set_enum_table(L, "Operator", {{"CLEAR", 0}, {"SOURCE", 1}, {"OVER", 2}, {"IN", 3},
                                   {"OUT", 4}, {"ATOP", 5}, {"DEST_OVER", 7},
                                   {"XOR", 11}, {"ADD", 12},
                                   /* richer than cairo's named set */
                                   {"MULTIPLY", 14}, {"SCREEN", 15}, {"OVERLAY", 16},
                                   {"DARKEN", 17}, {"LIGHTEN", 18}});
    set_enum_table(L, "Extend", {{"NONE", 0}, {"REPEAT", 1}, {"REFLECT", 2}, {"PAD", 3}});
    set_enum_table(L, "Filter", {{"FAST", 0}, {"GOOD", 1}, {"BEST", 2},
                                 {"NEAREST", 3}, {"BILINEAR", 4}});
    set_enum_table(L, "LineCap", {{"BUTT", static_cast<int>(SkPaint::Cap::kButt_Cap)},
                                  {"ROUND", static_cast<int>(SkPaint::Cap::kRound_Cap)},
                                  {"SQUARE", static_cast<int>(SkPaint::Cap::kSquare_Cap)}});

    /* frame_set_fill_rule() takes these as strings directly, unlike the
     * integer-valued enums above. */
    lua_newtable(L);
    lua_pushliteral(L, "WINDING");
    lua_setfield(L, -2, "WINDING");
    lua_pushliteral(L, "EVEN_ODD");
    lua_setfield(L, -2, "EVEN_ODD");
    lua_setfield(L, -2, "FillRule");

    set_method(L, "begin", frame_begin);
    set_method(L, "is_canvas", skia_is_canvas);
    set_method(L, "is_image", skia_is_image);
    set_method(L, "image_dimensions", skia_image_dimensions);
    set_method(L, "new_image_surface", skia_new_image_surface);
    set_method(L, "load_image", skia_load_image);
    set_method(L, "load_svg", skia_load_svg);
    set_method(L, "svg_dimensions", skia_svg_dimensions);
    lua_newtable(L);
    set_method(L, "create_rgba", pattern_create_rgba);
    set_method(L, "create_linear", pattern_create_linear);
    set_method(L, "create_radial", pattern_create_radial);
    set_method(L, "create_for_surface", pattern_create_for_surface);
    set_method(L, "is_type_of", pattern_is_type_of);
    lua_setfield(L, -2, "Pattern");
    lua_pop(L, 1);
}
