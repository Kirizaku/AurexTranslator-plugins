/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "overlay_text.h"

#include "imgui.h"

#include <cfloat>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
#include "host/overlay_protocol.h"
}

namespace {

struct TextState {
    std::mutex  mtx;
    std::string src;
    std::string text;
    bool        visible = false;
};

TextState& state()
{
    static TextState s;
    return s;
}

std::atomic<bool> g_session_active{false};

// ===============================================================
// Style (capture_style_data)
// ===============================================================

struct StyleState {
    std::mutex mtx;
    uint8_t  align      = 0;
    uint32_t text_color = 0xFFFFFFFFu;   /* opaque white, matches the old default */
    uint32_t bg_color   = 0x000000A8u;   /* black @ alpha 168/255 ~= the old 0.65f */
    float    font_size  = 0.0f;          /* 0 = "use the env-var/built-in default" */
    bool     font_size_auto = false;     /* ignore font_size, size to the panel's own box instead */
    float    corner_radius = 8.0f;       /* px; ImGuiStyleVar_WindowRounding */
    float    margin_top = 9.0f, margin_bottom = 9.0f, margin_left = 9.0f, margin_right = 9.0f;
    float    zone_x = 0.5f, zone_y = 0.88f, zone_w = 0.8f, zone_h = 0.5f;

    float loaded_font_size = 0.0f;
    bool  font_dirty       = false;
    std::string font_path;

    uint32_t seen_scripts = 0;
    bool     warned_no_cjk_font = false;
};

StyleState& style_state()
{
    static StyleState s;
    return s;
}

/* 0xRRGGBBAA -> ImGui's ImVec4 (0..1 floats) */
ImVec4 color_from_u32(uint32_t c)
{
    return ImVec4(((c >> 24) & 0xFF) / 255.0f,
                  ((c >> 16) & 0xFF) / 255.0f,
                  ((c >> 8)  & 0xFF) / 255.0f,
                  (c & 0xFF) / 255.0f);
}

// ===============================================================
// Font selection
// ===============================================================

const char* const kFontCandidates[] = {
#if defined(_WIN32)
    "C:\\Windows\\Fonts\\segoeui.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
    "C:\\Windows\\Fonts\\tahoma.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-fonts/LiberationSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
#endif
    nullptr,
};

const char* const kCjkFontCandidates[] = {
#if defined(_WIN32)
    "C:\\Windows\\Fonts\\msgothic.ttc", /* Japanese, all supported releases */
    "C:\\Windows\\Fonts\\meiryo.ttc",   /* Japanese, Vista+ */
    "C:\\Windows\\Fonts\\malgun.ttf",   /* Korean, Vista+ */
    "C:\\Windows\\Fonts\\simsun.ttc",   /* Simplified Chinese, all supported releases */
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
#endif
    nullptr,
};

enum : uint32_t {
    kScriptJapanese = 1u << 0,
    kScriptKorean   = 1u << 1,
    kScriptChinese  = 1u << 2,
};

uint32_t utf8_next(const char** p, const char* end)
{
    const unsigned char* s = (const unsigned char*)*p;
    uint32_t c = *s;
    int extra;
    if      ((c & 0x80) == 0x00) { extra = 0; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; c &= 0x07; }
    else { *p += 1; return 0xFFFD; }
    if ((const unsigned char*)end - s < 1 + extra) { *p += 1; return 0xFFFD; }
    for (int i = 1; i <= extra; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p += 1 + extra;
    return c;
}

uint32_t scripts_in_utf8(const char* text, uint32_t len)
{
    uint32_t scripts = 0;
    const char* p = text;
    const char* end = text + len;
    while (p < end) {
        uint32_t c = utf8_next(&p, end);
        if ((c >= 0x3040 && c <= 0x30FF) ||         /* Hiragana + Katakana */
            (c >= 0xFF66 && c <= 0xFF9D)) {         /* halfwidth Katakana */
            scripts |= kScriptJapanese;
        } else if (c >= 0xAC00 && c <= 0xD7A3) {    /* Hangul syllables */
            scripts |= kScriptKorean;
        } else if ((c >= 0x4E00 && c <= 0x9FFF) ||  /* CJK Unified Ideographs */
                   (c >= 0x3400 && c <= 0x4DBF)) {  /* CJK Ext-A */
            scripts |= kScriptChinese;
        }
    }
    return scripts;
}

bool file_readable(const char* p)
{
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

const ImWchar* pick_ranges(ImGuiIO& io)
{
    const char* r = std::getenv("AT_OVERLAY_FONT_RANGES");
    if (!r || !*r) return io.Fonts->GetGlyphRangesCyrillic();

    if (!std::strcmp(r, "cyrillic"))   return io.Fonts->GetGlyphRangesCyrillic();
    if (!std::strcmp(r, "japanese"))   return io.Fonts->GetGlyphRangesJapanese();
    if (!std::strcmp(r, "korean"))     return io.Fonts->GetGlyphRangesKorean();
    if (!std::strcmp(r, "greek"))      return io.Fonts->GetGlyphRangesGreek();
    if (!std::strcmp(r, "thai"))       return io.Fonts->GetGlyphRangesThai();
    if (!std::strcmp(r, "vietnamese")) return io.Fonts->GetGlyphRangesVietnamese();
    if (!std::strcmp(r, "chinese"))    return io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    if (!std::strcmp(r, "default"))    return io.Fonts->GetGlyphRangesDefault();

    return io.Fonts->GetGlyphRangesCyrillic();
}

float pick_font_size()
{
    return 22.0f;
}

template <typename OnLine>
void for_each_wrapped_line(ImFont* font, float font_size,
                           const char* text, const char* text_end,
                           float avail_w, OnLine&& on_line)
{
    const char* p = text;
    while (p < text_end) {
        const char* para_end = p;
        while (para_end < text_end && *para_end != '\n') ++para_end;

        if (p == para_end) {
            on_line(static_cast<const char*>(nullptr), static_cast<const char*>(nullptr));
        } else {
            const char* line = p;
            while (line < para_end) {
                const char* wrap_end = font->CalcWordWrapPosition(font_size, line, para_end, avail_w);
                if (wrap_end == line) wrap_end = para_end;
                on_line(line, wrap_end);
                line = wrap_end;
                while (line < para_end && *line == ' ') ++line;
            }
        }

        p = (para_end < text_end) ? para_end + 1 : para_end;
    }
}

float measure_paragraph_height(ImFont* font, float font_size,
                               const char* text, const char* text_end,
                               float avail_w)
{
    const float item_spacing_y = ImGui::GetStyle().ItemSpacing.y;
    float total_h = 0.0f;
    for_each_wrapped_line(font, font_size, text, text_end, avail_w,
        [&](const char* line, const char* line_end) {
            const float h = line ? font->CalcTextSizeA(font_size, FLT_MAX, -1.0f, line, line_end).y
                                  : font_size;
            total_h += h + item_spacing_y;
        });
    return total_h;
}

float fit_font_size(ImFont* font, const std::string& src, const std::string& text, float wrap_w, float avail_h)
{
    if (!font || wrap_w <= 0.0f || avail_h <= 0.0f) return 8.0f;

    auto fits = [&](float size) {
        float total_h = 0.0f;
        if (!src.empty())
            total_h += measure_paragraph_height(font, size, src.c_str(), src.c_str() + src.size(), wrap_w)
                     + ImGui::GetStyle().ItemSpacing.y;
        total_h += measure_paragraph_height(font, size, text.c_str(), text.c_str() + text.size(), wrap_w);
        return total_h <= avail_h;
    };

    const float kMin = 8.0f, kMax = 96.0f;
    if (fits(kMax)) return kMax;
    if (!fits(kMin)) return kMin;

    float lo = kMin, hi = kMax;
    for (int i = 0; i < 12; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (fits(mid)) lo = mid; else hi = mid;
    }
    return lo;
}

void draw_aligned_paragraph(ImFont* font, float font_size, const char* text, const char* text_end,
                            float left_x, float avail_w, uint8_t align)
{
    for_each_wrapped_line(font, font_size, text, text_end, avail_w,
        [&](const char* line, const char* line_end) {
            if (!line) {
                ImGui::NewLine();
                return;
            }

            const ImVec2 line_size = font->CalcTextSizeA(font_size, FLT_MAX, -1.0f, line, line_end);
            float x = left_x;
            if (align == 1) x += (avail_w - line_size.x) * 0.5f;      /* center */
            else if (align == 2) x += (avail_w - line_size.x);        /* right */
            if (x < left_x) x = left_x;

            ImGui::SetCursorPosX(x);
            ImGui::TextUnformatted(line, line_end);
        });
}

} // namespace

// ===============================================================
// C API — called from capture.c / win_capture.cpp
// ===============================================================

extern "C" void at_text_set(int visible, const char* src,  uint32_t src_len, const char* text, uint32_t text_len)
{
    if (src_len  > CAPTURE_TEXT_SRC_MAX)  src_len  = CAPTURE_TEXT_SRC_MAX;
    if (text_len > CAPTURE_TEXT_TEXT_MAX) text_len = CAPTURE_TEXT_TEXT_MAX;

    {
        TextState& s = state();
        std::lock_guard<std::mutex> lk(s.mtx);
        s.src.assign(src  ? src  : "", src  ? src_len  : 0);
        s.text.assign(text ? text : "", text ? text_len : 0);
        s.visible = (visible != 0);
    }

    const uint32_t scripts = (src  ? scripts_in_utf8(src,  src_len)  : 0) | (text ? scripts_in_utf8(text, text_len) : 0);
    if (scripts) {
        StyleState& st = style_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        if ((scripts & ~st.seen_scripts) != 0) {
            st.seen_scripts |= scripts;
            st.font_dirty = true;
        }
    }
}

extern "C" void at_text_clear(void)
{
    TextState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.src.clear();
    s.text.clear();
    s.visible = false;
}

extern "C" void at_session_set_active(int active)
{
    g_session_active.store(active != 0, std::memory_order_relaxed);
}

bool at_session_is_active()
{
    return g_session_active.load(std::memory_order_relaxed);
}

extern "C" void at_text_set_style(uint8_t align,
                                  uint32_t text_color, uint32_t bg_color, float font_size,
                                  float zone_x, float zone_y, float zone_w, float zone_h,
                                  uint8_t font_size_auto, uint8_t corner_radius,
                                  uint8_t margin_top, uint8_t margin_bottom,
                                  uint8_t margin_left, uint8_t margin_right)
{
    StyleState& st = style_state();
    std::lock_guard<std::mutex> lk(st.mtx);
    st.align      = align;
    st.text_color = text_color;
    st.bg_color   = bg_color;
    st.font_size  = (font_size >= 8.0f && font_size <= 96.0f) ? font_size : 0.0f;
    st.font_size_auto = (font_size_auto != 0);
    st.corner_radius = (float)(corner_radius > 64 ? 64 : corner_radius);
    st.margin_top    = (float)(margin_top    > 64 ? 64 : margin_top);
    st.margin_bottom = (float)(margin_bottom > 64 ? 64 : margin_bottom);
    st.margin_left   = (float)(margin_left   > 64 ? 64 : margin_left);
    st.margin_right  = (float)(margin_right  > 64 ? 64 : margin_right);
    st.zone_x = zone_x; st.zone_y = zone_y; st.zone_w = zone_w; st.zone_h = zone_h;

    if (!st.font_size_auto) {
        const float effective = st.font_size > 0.0f ? st.font_size : pick_font_size();
        if (st.loaded_font_size > 0.0f && std::fabs(effective - st.loaded_font_size) > 0.01f)
            st.font_dirty = true;
    }
}

extern "C" void at_text_set_font_path(const char* path, uint32_t path_len)
{
    StyleState& st = style_state();
    std::lock_guard<std::mutex> lk(st.mtx);
    std::string new_path(path ? path : "", path ? path_len : 0);
    if (new_path != st.font_path) {
        st.font_path = new_path;
        st.font_dirty = true;
    }
}

// ===============================================================
// ImGui side
// ===============================================================

void at_text_setup_font()
{
    ImGuiIO& io = ImGui::GetIO();

    StyleState& st = style_state();
    float size;
    std::string user_path;
    uint32_t seen_scripts;
    bool warned_no_cjk_font;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        size = st.font_size > 0.0f ? st.font_size : pick_font_size();
        st.loaded_font_size = size;
        st.font_dirty = false;
        user_path = st.font_path;
        seen_scripts = st.seen_scripts;
        warned_no_cjk_font = st.warned_no_cjk_font;
    }

    const char* path = nullptr;
    if (!user_path.empty() && file_readable(user_path.c_str())) {
        path = user_path.c_str();
    }
    if (!path) {
        for (const char* const* c = kFontCandidates; *c; ++c) {
            if (file_readable(*c)) { path = *c; break; }
        }
    }
    if (!path) {
        std::fprintf(stderr, "[at-overlay] no usable TTF found — falling back to the built-in "
                             "ASCII-only font. Non-Latin text will not render. "
                             "Set a font file in the overlay style settings.\n");
        io.Fonts->AddFontDefault();
        return;
    }
    const ImWchar* ranges = pick_ranges(io);

    if (!io.Fonts->AddFontFromFileTTF(path, size, nullptr, ranges)) {
        std::fprintf(stderr, "[at-overlay] failed to load font %s\n", path);
        io.Fonts->AddFontDefault();
    }

    if (seen_scripts) {
        const char* cjk_path = nullptr;
        for (const char* const* c = kCjkFontCandidates; *c; ++c) {
            if (file_readable(*c)) { cjk_path = *c; break; }
        }
        if (cjk_path) {
            static ImVector<ImWchar> s_cjk_ranges;
            ImFontGlyphRangesBuilder b;
            if (seen_scripts & kScriptJapanese) b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
            if (seen_scripts & kScriptKorean)   b.AddRanges(io.Fonts->GetGlyphRangesKorean());
            if (seen_scripts & kScriptChinese)  b.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            s_cjk_ranges.clear();
            b.BuildRanges(&s_cjk_ranges);

            ImFontConfig cfg;
            cfg.MergeMode  = true;
            cfg.PixelSnapH = true;
            if (!io.Fonts->AddFontFromFileTTF(cjk_path, size, &cfg, s_cjk_ranges.Data))
                std::fprintf(stderr, "[at-overlay] failed to load CJK font %s\n", cjk_path);
        } else if (!warned_no_cjk_font) {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.warned_no_cjk_font = true;
            std::fprintf(stderr, "[at-overlay] Japanese/Chinese/Korean text detected but no CJK-capable "
                                 "font was found on this system - it will show as boxes. Install one "
                                 "(e.g. `fonts-noto-cjk` on Debian/Ubuntu) or set one in the overlay "
                                 "style settings.\n");
        }
    }
}

bool at_text_font_dirty()
{
    StyleState& st = style_state();
    std::lock_guard<std::mutex> lk(st.mtx);
    return st.font_dirty;
}

void at_text_font_rebuilt()
{
    ImGui::GetIO().Fonts->Clear();
    at_text_setup_font();
}

void at_text_draw()
{
    std::string src, text;
    {
        TextState& s = state();
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.visible || s.text.empty()) return;
        src  = s.src;
        text = s.text;
    }

    uint32_t text_color, bg_color;
    float zone_x, zone_y, zone_w, zone_h, font_size, corner_radius;
    float margin_top, margin_bottom, margin_left, margin_right;
    uint8_t align;
    bool font_size_auto;
    {
        StyleState& st = style_state();
        std::lock_guard<std::mutex> lk(st.mtx);
        text_color = st.text_color;
        bg_color   = st.bg_color;
        zone_x = st.zone_x; zone_y = st.zone_y; zone_w = st.zone_w; zone_h = st.zone_h;
        font_size_auto = st.font_size_auto;
        font_size = st.font_size > 0.0f ? st.font_size : pick_font_size();
        corner_radius = st.corner_radius;
        margin_top = st.margin_top; margin_bottom = st.margin_bottom;
        margin_left = st.margin_left; margin_right = st.margin_right;
        align = st.align;
    }

    ImGuiIO& io = ImGui::GetIO();
    const float sw = io.DisplaySize.x;
    const float sh = io.DisplaySize.y;
    if (sw <= 0.0f || sh <= 0.0f) return;

    const float win_w = sw * zone_w;
    const float win_h = sh * zone_h;
    const float avail_w = win_w - margin_left - margin_right;
    const float avail_h = win_h - margin_top - margin_bottom;

    ImFont* font = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
    if (font_size_auto) {
        font_size = fit_font_size(font, src, text, avail_w, avail_h);
    }

    ImGui::SetNextWindowPos(ImVec2(sw * zone_x, sh * zone_y),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    ImGui::PushFont(font, font_size);
    ImFont* activeFont = ImGui::GetFont();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, color_from_u32(bg_color));
    ImGui::PushStyleColor(ImGuiCol_Text, color_from_u32(text_color));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, corner_radius);
    ImGui::Begin("##at-text", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(margin_left, margin_top));
    if (!src.empty()) {
        draw_aligned_paragraph(activeFont, font_size, src.c_str(), src.c_str() + src.size(), margin_left, avail_w, align);
        ImGui::Spacing();
    }
    draw_aligned_paragraph(activeFont, font_size, text.c_str(), text.c_str() + text.size(), margin_left, avail_w, align);

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
}
