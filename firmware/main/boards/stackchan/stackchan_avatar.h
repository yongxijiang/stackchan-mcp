#ifndef STACKCHAN_AVATAR_H
#define STACKCHAN_AVATAR_H

// LvglAvatar: canvas-based face renderer for StackChan.
// Draws eyes, mouth, and overlays using LVGL drawing primitives on a
// 320x240 RGB565 canvas in PSRAM. A 50 ms timer drives blink, saccade,
// breathing, and lip-sync animations autonomously.
//
// Reference: mo-hantang/Stackchan-HtSz (shizhou_avatar namespace).
// Adapted for the stackchan-mcp fork by Xiao & Chengcheng.

#include "display/lcd_display.h"
#include <lvgl.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>

#define AVATAR_TAG "StackChanAvatar"

namespace stackchan_avatar {

enum class Expression {
    Neutral, Happy, Angry, Sad, Sleepy,
    Loving, Crying,
    Kissy, Cool, Confident,
    Shocked, Thinking, Surprised, Confused,
    Embarrassed, Silly, Winking, Laughing, Funny, Relaxed, Delicious
};

struct Overlay {
    bool tear = false;
    bool heart_eyes = false;
    bool kiss_heart = false;
    bool cheek_blush = false;
    bool cool_glasses = false;
    bool excl_mark = false;
    bool think_bubble = false;
    bool star_burst = false;
    bool wave_squiggle = false;
    bool drool = false;
    bool laugh_lines = false;
    bool question_mark = false;
    bool zzz = false;
};

class LvglAvatar {
public:
    LvglAvatar() = default;
    ~LvglAvatar() { Destroy(); }

    bool Init(lv_obj_t* parent, int w, int h) {
        if (canvas_) return true;
        w_ = w; h_ = h;
        size_t bytes = (size_t)w * h * 2;
        buf_ = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!buf_) {
            ESP_LOGE(AVATAR_TAG, "Failed to allocate %zu bytes in PSRAM for avatar canvas", bytes);
            return false;
        }
        canvas_ = lv_canvas_create(parent);
        lv_canvas_set_buffer(canvas_, buf_, w, h, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(canvas_, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_background(canvas_);
        timer_ = lv_timer_create(&LvglAvatar::TimerCb, 50, this);
        next_blink_ms_ = 3000;
        last_saccade_ms_ = 0;
        Draw();
        ESP_LOGI(AVATAR_TAG, "Avatar canvas created (%dx%d, %zu bytes PSRAM)", w, h, bytes);
        return true;
    }

    void Destroy() {
        if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
        if (canvas_) { lv_obj_delete(canvas_); canvas_ = nullptr; }
        if (buf_)   { heap_caps_free(buf_); buf_ = nullptr; }
    }

    bool IsReady() const { return canvas_ != nullptr; }

    lv_obj_t* GetCanvas() { return canvas_; }

    void SetExpression(Expression e) { expression_ = e; }
    Expression GetExpression() const { return expression_; }
    void SetOverlay(const Overlay& o) { overlay_ = o; }

    void StartSpeaking(uint32_t duration_ms) {
        speaking_until_ms_ = lv_tick_get() + duration_ms;
    }
    void StartSpeakingContinuous() {
        speaking_until_ms_ = UINT32_MAX;
    }
    void StopSpeaking() {
        speaking_until_ms_ = 0;
        mouth_open_ = 0.0f;
    }
    bool IsSpeaking() const {
        return speaking_until_ms_ != 0 && lv_tick_get() < speaking_until_ms_;
    }

    void SetVisible(bool visible) {
        if (!canvas_) return;
        if (visible) {
            lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
        }
    }

private:
    static void TimerCb(lv_timer_t* t) {
        static_cast<LvglAvatar*>(lv_timer_get_user_data(t))->OnTick();
    }

    void UpdateBreathParams() {
        breath_amp_ = 3.0f;
        breath_period_steps_ = 100;
        breath_paused_ = false;
        switch (expression_) {
            case Expression::Relaxed:
                breath_amp_ = 7.0f;
                breath_period_steps_ = 160;
                break;
            case Expression::Shocked:
                breath_paused_ = true;
                break;
            default: break;
        }
    }

    bool BlinkAllowed() const {
        switch (expression_) {
            case Expression::Cool:
            case Expression::Confident:
            case Expression::Shocked:
            case Expression::Winking:
            case Expression::Kissy:
                return false;
            default:
                return true;
        }
    }

    bool SlowBlink() const {
        return expression_ == Expression::Thinking || expression_ == Expression::Relaxed;
    }

    bool SaccadeEnabled() const {
        switch (expression_) {
            case Expression::Cool:
            case Expression::Confident:
            case Expression::Shocked:
            case Expression::Thinking:
            case Expression::Embarrassed:
            case Expression::Winking:
                return false;
            default:
                return true;
        }
    }

    void GetGazeOverride(float* gh, float* gv) const {
        switch (expression_) {
            case Expression::Thinking:
                *gh = 0; *gv = -1.0f; break;
            case Expression::Embarrassed:
                *gh = 0; *gv = 0.7f; break;
            default:
                *gh = gaze_h_; *gv = gaze_v_;
        }
    }

    void OnTick() {
        tick_count_++;
        uint32_t now = lv_tick_get();

        UpdateBreathParams();
        if (breath_paused_) {
            breath_ = 0;
        } else {
            breath_ = sinf((tick_count_ % breath_period_steps_) * 2.0f * 3.14159265f / breath_period_steps_);
        }

        if (BlinkAllowed()) {
            if (now >= next_blink_ms_) {
                uint32_t mult = SlowBlink() ? 2 : 1;
                if (eye_closed_) {
                    eye_open_ratio_ = 1.0f;
                    next_blink_ms_ = now + mult * (2500 + (rand() % 2000));
                    eye_closed_ = false;
                } else {
                    eye_open_ratio_ = 0.0f;
                    next_blink_ms_ = now + 150 + (rand() % 200);
                    eye_closed_ = true;
                }
            }
        } else {
            eye_open_ratio_ = 1.0f;
            eye_closed_ = false;
        }

        if (SaccadeEnabled() && now - last_saccade_ms_ > 1500) {
            gaze_h_ = (rand() % 21 - 10) / 10.0f;
            gaze_v_ = (rand() % 21 - 10) / 10.0f;
            last_saccade_ms_ = now;
        }

        bool speaking = (speaking_until_ms_ != 0 && now < speaking_until_ms_);
        if (!speaking && speaking_until_ms_ != 0) speaking_until_ms_ = 0;
        if (speaking) {
            mouth_open_ = 0.2f + (rand() % 80) / 100.0f;
        } else {
            mouth_open_ = 0.0f;
        }

        Draw();
    }

    void Draw() {
        if (!canvas_) return;
        const lv_color_t bg = lv_color_make(0x00, 0x00, 0x00);
        const lv_color_t fg = lv_color_make(0xFF, 0xFF, 0xFF);

        lv_canvas_fill_bg(canvas_, bg, LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(canvas_, &layer);

        DrawMouth(&layer, fg, bg);
        DrawEye(&layer, fg, bg, false);
        DrawEye(&layer, fg, bg, true);
        DrawOverlay(&layer, fg, bg);

        lv_canvas_finish_layer(canvas_, &layer);
    }

    // --- Drawing primitives ---

    void FillRect(lv_layer_t* layer, int x, int y, int w, int h, lv_color_t c) {
        if (w <= 0 || h <= 0) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c;
        d.bg_opa = LV_OPA_COVER;
        d.radius = 0;
        d.border_width = 0;
        lv_area_t a = {x, y, x + w - 1, y + h - 1};
        lv_draw_rect(layer, &d, &a);
    }

    void FillCircle(lv_layer_t* layer, int cx, int cy, int r, lv_color_t c) {
        if (r <= 0) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c;
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        d.border_width = 0;
        lv_area_t a = {cx - r, cy - r, cx + r - 1, cy + r - 1};
        lv_draw_rect(layer, &d, &a);
    }

    void FillTriangle(lv_layer_t* layer, int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c) {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        d.p[0].x = (float)x0; d.p[0].y = (float)y0;
        d.p[1].x = (float)x1; d.p[1].y = (float)y1;
        d.p[2].x = (float)x2; d.p[2].y = (float)y2;
        d.color = c;
        d.opa = LV_OPA_COVER;
        lv_draw_triangle(layer, &d);
    }

    void FillRoundRect(lv_layer_t* layer, int x, int y, int w, int h, int radius, lv_color_t c) {
        if (w <= 0 || h <= 0) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c;
        d.bg_opa = LV_OPA_COVER;
        d.radius = radius;
        d.border_width = 0;
        lv_area_t a = {x, y, x + w - 1, y + h - 1};
        lv_draw_rect(layer, &d, &a);
    }

    void DrawArc(lv_layer_t* layer, int cx, int cy, int r, int start_deg, int end_deg, int width, lv_color_t c, bool rounded = false) {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = c;
        d.opa = LV_OPA_COVER;
        d.width = width;
        d.center.x = cx;
        d.center.y = cy;
        d.radius = r;
        d.start_angle = start_deg;
        d.end_angle = end_deg;
        d.rounded = rounded ? 1 : 0;
        lv_draw_arc(layer, &d);
    }

    void DrawLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, int width, bool round, lv_color_t c) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = c;
        d.opa = LV_OPA_COVER;
        d.width = width;
        d.round_start = round ? 1 : 0;
        d.round_end = round ? 1 : 0;
        d.p1.x = (float)x1; d.p1.y = (float)y1;
        d.p2.x = (float)x2; d.p2.y = (float)y2;
        lv_draw_line(layer, &d);
    }

    // --- Face components ---

    void DrawMouth(lv_layer_t* layer, lv_color_t fg, lv_color_t bg) {
        const int cx = 163;
        const int cy = 148 + (int)(breath_ * 3.0f);
        const int y_off = (int)(breath_ * 2.0f);

        switch (expression_) {
            case Expression::Cool:
                DrawLine(layer, cx - 12, cy + y_off + 2, cx + 12, cy + y_off, 3, true, fg);
                return;
            case Expression::Confident:
                DrawLine(layer, cx - 14, cy + y_off + 3, cx + 14, cy + y_off, 3, true, fg);
                return;
            case Expression::Silly:
                DrawLine(layer, cx - 13, cy + y_off + 4, cx + 13, cy + y_off, 3, true, fg);
                return;
            case Expression::Embarrassed:
                DrawLine(layer, cx - 12, cy + y_off, cx + 12, cy + y_off, 3, true, fg);
                return;
            case Expression::Kissy: {
                int my = cy + y_off;
                DrawArc(layer, cx, my - 6, 6, 270, 450, 3, fg, false);
                DrawArc(layer, cx, my + 6, 6, 270, 450, 3, fg, false);
                FillCircle(layer, cx, my, 2, fg);
                return;
            }
            case Expression::Winking:
                DrawArc(layer, cx, cy + y_off - 5, 12, 0, 180, 3, fg);
                return;
            case Expression::Laughing: {
                int y_top = cy + y_off - 28;
                FillRoundRect(layer, cx - 40, y_top, 80, 30, 12, fg);
                return;
            }
            case Expression::Funny:
                FillRoundRect(layer, cx - 25, cy + y_off - 10, 50, 20, 8, fg);
                return;
            case Expression::Relaxed:
                DrawLine(layer, cx - 20, cy + y_off, cx + 20, cy + y_off, 4, true, fg);
                return;
            case Expression::Delicious: {
                int h = 4 + (int)((60 - 4) * 0.3f);
                int w = 50 + (int)((90 - 50) * 0.7f);
                FillRoundRect(layer, cx - w / 2, cy + y_off - h / 2, w, h, 7, fg);
                return;
            }
            case Expression::Shocked: {
                FillRoundRect(layer, cx - 25, cy + y_off - 30, 50, 60, 10, fg);
                return;
            }
            case Expression::Surprised: {
                int h = 4 + (int)((60 - 4) * 0.5f);
                int w = 50 + (int)((90 - 50) * 0.5f);
                FillRoundRect(layer, cx - w / 2, cy + y_off - h / 2, w, h, 12, fg);
                return;
            }
            case Expression::Thinking:
                DrawLine(layer, cx - 15, cy + y_off, cx + 15, cy + y_off, 3, true, fg);
                return;
            case Expression::Confused:
                DrawLine(layer, cx - 15, cy + y_off, cx + 15, cy + y_off + 2, 3, true, fg);
                return;
            default: {
                int h = 4 + (int)((60 - 4) * mouth_open_);
                int w = 50 + (int)((90 - 50) * (1.0f - mouth_open_));
                int radius = (int)(mouth_open_ * 10);
                FillRoundRect(layer, cx - w / 2, cy + y_off - h / 2, w, h, radius, fg);
                return;
            }
        }
    }

    void DrawEye(lv_layer_t* layer, lv_color_t fg, lv_color_t bg, bool is_left) {
        if (expression_ == Expression::Cool) return;

        const int cx_base = is_left ? 230 : 90;
        const int cy_base_y = is_left ? 96 : 93;
        const int cy = cy_base_y + (int)(breath_ * 3.0f);

        float gh, gv;
        GetGazeOverride(&gh, &gv);
        const int off_x = (int)(gh * 3.0f);
        const int off_y = (int)(gv * 3.0f);

        if (overlay_.heart_eyes) {
            const lv_color_t red = lv_color_make(0xFF, 0x40, 0x70);
            int hcx = cx_base + off_x;
            int hcy = cy + off_y;
            FillCircle(layer, hcx - 6, hcy - 3, 7, red);
            FillCircle(layer, hcx + 6, hcy - 3, 7, red);
            FillTriangle(layer, hcx - 12, hcy + 1, hcx + 12, hcy + 1, hcx, hcy + 13, red);
            return;
        }

        if (expression_ == Expression::Shocked) {
            FillCircle(layer, cx_base, cy, 13, fg);
            FillCircle(layer, cx_base, cy, 3, bg);
            return;
        }

        if (expression_ == Expression::Surprised) {
            FillCircle(layer, cx_base, cy, 10, fg);
            return;
        }

        if (expression_ == Expression::Confused) {
            int r = is_left ? 8 : 6;
            FillCircle(layer, cx_base + off_x, cy + off_y, r, fg);
            return;
        }

        if (expression_ == Expression::Winking) {
            if (is_left) {
                DrawLine(layer, cx_base + 8, cy - 4, cx_base - 8, cy, 5, true, fg);
                DrawLine(layer, cx_base - 8, cy, cx_base + 8, cy + 4, 5, true, fg);
            } else {
                FillCircle(layer, cx_base, cy, 8, fg);
            }
            return;
        }

        if (expression_ == Expression::Silly) {
            int r = 8;
            FillCircle(layer, cx_base + off_x, cy + off_y, r, fg);
            int x0 = cx_base + off_x - r;
            int y0 = cy + off_y;
            int w = r * 2 + 4;
            int h = r + 2;
            if (!is_left) h += 2;
            FillCircle(layer, cx_base + off_x, cy + off_y, (int)(r / 1.5f), bg);
            FillRect(layer, x0, y0, w, h, bg);
            return;
        }

        if (expression_ == Expression::Laughing) {
            int r = 8;
            FillCircle(layer, cx_base + off_x, cy + off_y, r, fg);
            int x0 = cx_base + off_x - r - 2;
            int y0 = cy + off_y;
            int w = r * 2 + 8;
            int h = r + 4;
            FillCircle(layer, cx_base + off_x, cy + off_y, (int)(r / 1.5f), bg);
            FillRect(layer, x0, y0, w, h, bg);
            return;
        }

        if (expression_ == Expression::Sleepy) {
            if (is_left) {
                DrawLine(layer, cx_base - 8 + off_x, cy - 2 + off_y,
                                cx_base + 8 + off_x, cy + 2 + off_y, 4, true, fg);
            } else {
                DrawLine(layer, cx_base - 8 + off_x, cy + 2 + off_y,
                                cx_base + 8 + off_x, cy - 2 + off_y, 4, true, fg);
            }
            return;
        }

        if (expression_ == Expression::Relaxed) {
            int r = 8;
            FillCircle(layer, cx_base + off_x, cy + off_y, r, fg);
            int x0 = cx_base + off_x - r - 1;
            int y0 = cy + off_y - 1;
            int w = r * 2 + 6;
            int h = r + 3;
            FillCircle(layer, cx_base + off_x, cy + off_y, (int)(r / 1.5f), bg);
            FillRect(layer, x0, y0, w, h, bg);
            return;
        }

        const int r = 8;

        if (eye_open_ratio_ > 0) {
            FillCircle(layer, cx_base + off_x, cy + off_y, r, fg);

            if (expression_ == Expression::Angry || expression_ == Expression::Sad || expression_ == Expression::Crying) {
                int x0 = cx_base + off_x - r;
                int y0 = cy + off_y - r;
                int x1 = x0 + r * 2;
                int y1 = y0;
                bool sad = (expression_ == Expression::Sad || expression_ == Expression::Crying);
                int x2 = ((!is_left) != (!sad)) ? x0 : x1;
                int y2 = y0 + r;
                FillTriangle(layer, x0, y0, x1, y1, x2, y2, bg);
            }

            if (expression_ == Expression::Happy
                || expression_ == Expression::Kissy || expression_ == Expression::Funny
                || expression_ == Expression::Delicious) {
                FillCircle(layer, cx_base + off_x, cy + off_y, r + 2, bg);
                DrawArc(layer, cx_base + off_x, cy + off_y + r,
                        r, 180, 360, 3, fg, true);
            }
        } else {
            FillRect(layer, cx_base - r + off_x, cy - 2 + off_y, r * 2, 4, fg);
        }
    }

    void DrawOverlay(lv_layer_t* layer, lv_color_t fg, lv_color_t bg) {
        if (overlay_.tear) {
            const lv_color_t blue = lv_color_make(0x40, 0xA0, 0xFF);
            int tx = 90;
            int ty = 115 + (int)(breath_ * 3.0f);
            FillCircle(layer, tx, ty, 7, blue);
            FillTriangle(layer, tx - 6, ty - 2, tx + 6, ty - 2, tx, ty - 15, blue);
        }

        if (overlay_.cheek_blush) {
            const lv_color_t pink = lv_color_make(0xFF, 0x64, 0x82);
            for (int i = 0; i < 3; i++) {
                int x_start = 47 + i * 8;
                int x_end = x_start + 6;
                DrawLine(layer, x_start, 138, x_end, 130, 3, true, pink);
            }
            for (int i = 0; i < 3; i++) {
                int x_start = 251 + i * 8;
                int x_end = x_start + 6;
                DrawLine(layer, x_start, 138, x_end, 130, 3, true, pink);
            }
        }

        if (overlay_.cool_glasses) {
            FillRoundRect(layer, 85, 84, 50, 24, 5, fg);
            FillRoundRect(layer, 185, 84, 50, 24, 5, fg);
            DrawLine(layer, 85, 84, 235, 84, 2, false, fg);
        }

        if (overlay_.excl_mark) {
            DrawLine(layer, 291, 50, 291, 68, 4, true, fg);
            FillCircle(layer, 291, 76, 2, fg);
        }

        if (overlay_.think_bubble) {
            FillRoundRect(layer, 245, 47, 50, 25, 12, fg);
            FillCircle(layer, 258, 60, 3, bg);
            FillCircle(layer, 270, 60, 3, bg);
            FillCircle(layer, 282, 60, 3, bg);
            FillCircle(layer, 273, 85, 6, fg);
            FillCircle(layer, 258, 110, 4, fg);
        }

        if (overlay_.star_burst) {
            const int cx_s = 290, cy_s = 60;
            FillRect(layer, cx_s - 3, cy_s - 3, 6, 6, fg);
            FillTriangle(layer, cx_s, cy_s - 18, cx_s - 3, cy_s - 3, cx_s + 3, cy_s - 3, fg);
            FillTriangle(layer, cx_s, cy_s + 18, cx_s - 3, cy_s + 3, cx_s + 3, cy_s + 3, fg);
            FillTriangle(layer, cx_s - 18, cy_s, cx_s - 3, cy_s - 3, cx_s - 3, cy_s + 3, fg);
            FillTriangle(layer, cx_s + 18, cy_s, cx_s + 3, cy_s - 3, cx_s + 3, cy_s + 3, fg);
        }

        if (overlay_.wave_squiggle) {
            DrawLine(layer, 148, 28, 154, 24, 2, true, fg);
            DrawLine(layer, 154, 24, 160, 28, 2, true, fg);
            DrawLine(layer, 160, 28, 166, 24, 2, true, fg);
            DrawLine(layer, 166, 24, 172, 28, 2, true, fg);
        }

        if (overlay_.drool) {
            const lv_color_t blue = lv_color_make(0x40, 0xA0, 0xFF);
            int dx = 143;
            int dy = 168 + (int)(breath_ * 3.0f);
            FillCircle(layer, dx, dy, 4, blue);
            FillTriangle(layer, dx - 3, dy - 2, dx + 3, dy - 2, dx, dy - 8, blue);
        }

        if (overlay_.laugh_lines) {
            DrawLine(layer, 210, 150, 220, 142, 3, true, fg);
            DrawLine(layer, 218, 156, 228, 148, 3, true, fg);
        }

        if (overlay_.question_mark) {
            DrawArc(layer, 290, 50, 7, 180, 90, 4, fg, true);
            FillCircle(layer, 290, 67, 3, fg);
        }

        if (overlay_.zzz) {
            auto draw_z = [&](int cx_z, int cy_z, int size, int w) {
                int h = size / 2;
                DrawLine(layer, cx_z - h, cy_z - h, cx_z + h, cy_z - h, w, false, fg);
                DrawLine(layer, cx_z + h, cy_z - h, cx_z - h, cy_z + h, w, false, fg);
                DrawLine(layer, cx_z - h, cy_z + h, cx_z + h, cy_z + h, w, false, fg);
            };
            draw_z(258, 80, 8, 2);
            draw_z(270, 70, 10, 3);
            draw_z(286, 55, 14, 3);
        }

        if (overlay_.kiss_heart) {
            const lv_color_t red = lv_color_make(0xFF, 0x40, 0x70);
            int hx = 195;
            int hy = 130;
            FillCircle(layer, hx - 3, hy - 1, 4, red);
            FillCircle(layer, hx + 3, hy - 1, 4, red);
            FillTriangle(layer, hx - 6, hy + 1, hx + 6, hy + 1, hx, hy + 8, red);
        }
    }

    lv_obj_t* canvas_ = nullptr;
    uint8_t* buf_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    int w_ = 320, h_ = 240;

    Expression expression_ = Expression::Neutral;
    Overlay overlay_;

    uint32_t tick_count_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t last_saccade_ms_ = 0;
    uint32_t speaking_until_ms_ = 0;
    bool eye_closed_ = false;
    float eye_open_ratio_ = 1.0f;
    float mouth_open_ = 0.0f;
    float breath_ = 0.0f;
    float gaze_h_ = 0.0f;
    float gaze_v_ = 0.0f;

    float breath_amp_ = 3.0f;
    uint32_t breath_period_steps_ = 100;
    bool breath_paused_ = false;
};

// --- Emotion string mapping ---

static Expression MapEmotion(const char* e) {
    if (!e) return Expression::Neutral;
    if (!strcmp(e, "neutral"))     return Expression::Neutral;
    if (!strcmp(e, "idle"))        return Expression::Neutral;
    if (!strcmp(e, "happy"))       return Expression::Happy;
    if (!strcmp(e, "laughing"))    return Expression::Laughing;
    if (!strcmp(e, "funny"))       return Expression::Funny;
    if (!strcmp(e, "sad"))         return Expression::Sad;
    if (!strcmp(e, "crying"))      return Expression::Crying;
    if (!strcmp(e, "angry"))       return Expression::Angry;
    if (!strcmp(e, "loving"))      return Expression::Loving;
    if (!strcmp(e, "embarrassed")) return Expression::Embarrassed;
    if (!strcmp(e, "surprised"))   return Expression::Surprised;
    if (!strcmp(e, "shocked"))     return Expression::Shocked;
    if (!strcmp(e, "thinking"))    return Expression::Thinking;
    if (!strcmp(e, "winking"))     return Expression::Winking;
    if (!strcmp(e, "cool"))        return Expression::Cool;
    if (!strcmp(e, "relaxed"))     return Expression::Relaxed;
    if (!strcmp(e, "delicious"))   return Expression::Delicious;
    if (!strcmp(e, "kissy"))       return Expression::Kissy;
    if (!strcmp(e, "confident"))   return Expression::Confident;
    if (!strcmp(e, "sleepy"))      return Expression::Sleepy;
    if (!strcmp(e, "silly"))       return Expression::Silly;
    if (!strcmp(e, "confused"))    return Expression::Confused;
    return Expression::Neutral;
}

static Overlay OverlayFor(const char* e) {
    Overlay o;
    if (!e) return o;
    if (!strcmp(e, "crying"))      o.tear = true;
    if (!strcmp(e, "loving"))      { o.heart_eyes = true; o.cheek_blush = true; }
    if (!strcmp(e, "kissy"))       o.kiss_heart = true;
    if (!strcmp(e, "embarrassed")) o.cheek_blush = true;
    if (!strcmp(e, "cool"))        o.cool_glasses = true;
    if (!strcmp(e, "shocked"))     o.excl_mark = true;
    if (!strcmp(e, "thinking"))    o.think_bubble = true;
    if (!strcmp(e, "surprised"))   o.star_burst = true;
    if (!strcmp(e, "delicious"))   o.drool = true;
    if (!strcmp(e, "confused"))    o.question_mark = true;
    if (!strcmp(e, "sleepy"))      o.zzz = true;
    return o;
}

static bool IsKnownEmotion(const char* e) {
    if (!e) return false;
    static const char* names[] = {
        "neutral", "idle", "happy", "laughing", "funny", "sad", "crying",
        "angry", "loving", "embarrassed", "surprised", "shocked", "thinking",
        "winking", "cool", "relaxed", "delicious", "kissy", "confident",
        "sleepy", "silly", "confused", "off",
        nullptr
    };
    for (const char** p = names; *p; ++p) {
        if (!strcmp(e, *p)) return true;
    }
    return false;
}

}  // namespace stackchan_avatar


// --- Display subclass that integrates the avatar ---

class StackChanAvatarDisplay : public SpiLcdDisplay {
public:
    StackChanAvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
          canvas_w_(width), canvas_h_(height)
    {
        esp_timer_create_args_t args = {};
        args.callback = &StackChanAvatarDisplay::InitTimerCallback;
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "avatar_init";
        args.skip_unhandled_events = true;
        esp_timer_create(&args, &avatar_init_timer_);
        esp_timer_start_periodic(avatar_init_timer_, 500000);

        esp_timer_create_args_t idle_args = {};
        idle_args.callback = &StackChanAvatarDisplay::IdleTimerCallback;
        idle_args.arg = this;
        idle_args.dispatch_method = ESP_TIMER_TASK;
        idle_args.name = "avatar_idle";
        idle_args.skip_unhandled_events = true;
        esp_timer_create(&idle_args, &idle_timer_);
    }

    bool IsAvatarReady() const { return avatar_.IsReady(); }

    void OnPetted() {
        if (!avatar_.IsReady()) return;
        DisplayLockGuard lock(this);
        avatar_.SetExpression(stackchan_avatar::Expression::Loving);
        stackchan_avatar::Overlay o;
        o.heart_eyes = true;
        o.cheek_blush = true;
        avatar_.SetOverlay(o);
        current_emotion_ = "loving";
        SetActiveLocked(true);
        BumpIdleTimerLocked();
    }

    void SetAvatarEmotion(const char* emotion) {
        if (!avatar_.IsReady()) return;
        DisplayLockGuard lock(this);
        HideEmojiBoxLocked();
        avatar_.SetExpression(stackchan_avatar::MapEmotion(emotion));
        avatar_.SetOverlay(stackchan_avatar::OverlayFor(emotion));
        avatar_.SetVisible(true);
        current_emotion_ = emotion ? emotion : "neutral";
        SetActiveLocked(true);
        BumpIdleTimerLocked();
    }

    void SetAvatarOff() {
        if (!avatar_.IsReady()) return;
        DisplayLockGuard lock(this);
        avatar_.SetVisible(false);
        if (emoji_box_) lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_) lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        current_emotion_ = "off";
    }

    bool IsAvatarOff() const { return current_emotion_ == "off"; }
    const std::string& GetCurrentEmotion() const { return current_emotion_; }

    void SetEmotion(const char* emotion) override {
        SpiLcdDisplay::SetEmotion(emotion);
        if (!avatar_.IsReady() || current_emotion_ == "off") return;
        DisplayLockGuard lock(this);
        HideEmojiBoxLocked();
        avatar_.SetExpression(stackchan_avatar::MapEmotion(emotion));
        avatar_.SetOverlay(stackchan_avatar::OverlayFor(emotion));
        if (emotion) current_emotion_ = emotion;
    }

    void SetChatMessage(const char* role, const char* content) override {
        SpiLcdDisplay::SetChatMessage(role, content);
        if (!avatar_.IsReady()) return;
        DisplayLockGuard lock(this);
        bool meaningful = role && content && content[0] != '\0'
            && (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
        if (meaningful) {
            SetActiveLocked(true);
            BumpIdleTimerLocked();
        }
        if (role && content && content[0] != '\0' && strcmp(role, "assistant") == 0) {
            size_t n = strlen(content);
            uint32_t ms = (uint32_t)(n * 120);
            if (ms < 800) ms = 800;
            if (ms > 15000) ms = 15000;
            avatar_.StartSpeaking(ms);
        } else if (role && (strcmp(role, "user") == 0 || strcmp(role, "system") == 0)) {
            avatar_.StopSpeaking();
        }
    }

    void StartSpeaking() {
        if (!avatar_.IsReady()) return;
        avatar_.StartSpeakingContinuous();
    }

    void StopSpeaking() {
        if (!avatar_.IsReady()) return;
        avatar_.StopSpeaking();
    }

private:
    stackchan_avatar::LvglAvatar avatar_;
    esp_timer_handle_t avatar_init_timer_ = nullptr;
    esp_timer_handle_t idle_timer_ = nullptr;
    bool active_mode_ = false;
    int canvas_w_ = 320;
    int canvas_h_ = 240;
    std::string current_emotion_ = "neutral";
    static constexpr uint64_t IDLE_TIMEOUT_US = 8 * 1000 * 1000;

    void HideEmojiBoxLocked() {
        if (avatar_.IsReady() && emoji_box_) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void SetActiveLocked(bool active) {
        if (active == active_mode_) return;
        active_mode_ = active;
        if (active) {
            if (top_bar_)    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_) lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (top_bar_)    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            if (status_bar_) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void BumpIdleTimerLocked() {
        if (!idle_timer_) return;
        esp_timer_stop(idle_timer_);
        esp_timer_start_once(idle_timer_, IDLE_TIMEOUT_US);
    }

    static void IdleTimerCallback(void* arg) {
        auto self = static_cast<StackChanAvatarDisplay*>(arg);
        DisplayLockGuard lock(self);
        self->SetActiveLocked(false);
    }

    static void InitTimerCallback(void* arg) {
        auto self = static_cast<StackChanAvatarDisplay*>(arg);
        self->TryInitAvatar();
    }

    void TryInitAvatar() {
        DisplayLockGuard lock(this);
        if (avatar_.IsReady()) return;
        lv_obj_t* screen = lv_screen_active();
        if (screen == nullptr) return;
        if (container_ == nullptr) return;

        bool ok = avatar_.Init(screen, canvas_w_, canvas_h_);
        if (!ok) return;

        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
        if (content_) lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
        if (emoji_box_) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (top_bar_)    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

        if (avatar_init_timer_) {
            esp_timer_stop(avatar_init_timer_);
            esp_timer_delete(avatar_init_timer_);
            avatar_init_timer_ = nullptr;
        }

        ESP_LOGI(AVATAR_TAG, "Avatar display initialized, xiaozhi UI hidden");
    }
};

#endif // STACKCHAN_AVATAR_H
