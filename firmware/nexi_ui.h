// Станция Nexi: сенсор экрана и кнопка настроек (громкость, яркость, тема, гашение экрана) прямо на колонке.
// Подключается в файле платы quandong/esp32-s3-spi-2.8 после создания дисплея.
#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "board.h"
#include "audio_codec.h"
#include "display/display.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "nexi_station.h"
#include "nexi_ota.h"

namespace nexi::ui {

inline esp_lcd_touch_handle_t tp = nullptr;
// как повернуть касания относительно экрана: бит 1 — поменять оси, 2 — отразить X, 4 — отразить Y.
// Подбирается кнопкой «Повернуть касание» (или с телефона), запоминается.
inline int touch_mode = 0;   // экран стоит вертикально (240×320) — сенсор обычно совпадает без поворота
inline esp_lcd_panel_handle_t lcd = nullptr;
// как развернуть картинку: бит 1 — отразить X, бит 2 — отразить Y (3 = на 180°). Подбирается кнопкой «Повернуть экран».
inline int screen_mode = 0;
inline void apply_screen() {
    if (lcd) esp_lcd_panel_mirror(lcd, screen_mode & 1, screen_mode & 2);
}
inline lv_obj_t* panel = nullptr;

// ------------------------------------------------------------------ экран гаснет, когда колонкой не пользуются
// Касание, разговор, будильник, обновление — экран загорается. Первое касание по тёмному экрану только будит его.
inline int scr_off_sec = 60;               // 0 — не гасить; выбирается в настройках, хранится в NVS «scroff»
inline volatile uint32_t last_act = 0;      // lv_tick последней активности
inline volatile bool scr_dark = false;
inline volatile bool eat_touch = false;     // палец, разбудивший экран, интерфейсу не отдаём
inline bool wiz_step_active();
inline bool panel_open();
inline void scr_wake() {
    last_act = lv_tick_get();
    if (!scr_dark) return;
    scr_dark = false;
    auto* b = Board::GetInstance().GetBacklight();
    if (b) b->RestoreBrightness();
}
inline void scr_tick(lv_timer_t*) {
    auto& app = Application::GetInstance();
    bool busy = app.GetDeviceState() != kDeviceStateIdle || nexi::alarm::ringing || nexi::ota::progress >= 0 || wiz_step_active();
    if (busy) { scr_wake(); return; }
    if (scr_dark || scr_off_sec <= 0 || panel_open()) return;
    if (lv_tick_elaps(last_act) > (uint32_t)scr_off_sec * 1000) {
        auto* b = Board::GetInstance().GetBacklight();
        if (b) { b->SetBrightness(0, false); scr_dark = true; }
    }
}

/** «Сырые» координаты сенсора (портрет 240×320) → точка на экране при повороте касания mode. */
inline void map_touch(int mode, int rx, int ry, int& ox, int& oy) {
    int w = 240, h = 320;
    if (mode & 1) { int t = rx; rx = ry; ry = t; w = 320; h = 240; }
    if (mode & 2) rx = w - 1 - rx;
    if (mode & 4) ry = h - 1 - ry;
    lv_display_t* d = lv_display_get_default();
    int dw = lv_display_get_horizontal_resolution(d), dh = lv_display_get_vertical_resolution(d);
    ox = LV_CLAMP(0, rx * dw / w, dw - 1);
    oy = LV_CLAMP(0, ry * dh / h, dh - 1);
}

// ------------------------------------------------------------------ первая настройка: «нажми на кружок»
// Колонка при первом включении сама узнаёт, как повёрнут сенсор: человек жмёт на 3 кружка,
// мы смотрим «сырые» координаты и выбираем поворот, при котором нажатия ложатся точно на кружки.
inline int wiz_step = -1;   // -1 — не идёт; 0..2 — кружки; 3 — «готово»
inline int wiz_raw[3][2];
inline lv_obj_t* wiz = nullptr;
inline bool wiz_down = false;
inline bool wiz_skip = false;   // палец ещё держит звезду, которой запустили настройку — ждём, пока отпустит
inline int wiz_last[2];
inline void wiz_show(void*);
inline bool wiz_step_active() { return wiz_step >= 0; }
inline bool panel_open() { return panel != nullptr; }

inline void wiz_target(int i, int& x, int& y) {
    lv_display_t* d = lv_display_get_default();
    int dw = lv_display_get_horizontal_resolution(d), dh = lv_display_get_vertical_resolution(d);
    const float fx[3] = {0.15f, 0.85f, 0.15f}, fy[3] = {0.18f, 0.18f, 0.85f};
    x = (int)(dw * fx[i]); y = (int)(dh * fy[i]);
}

inline void wiz_solve() {
    int best = 0;
    long best_err = -1;
    for (int m = 0; m < 8; m++) {
        long err = 0;
        for (int i = 0; i < 3; i++) {
            int x, y, tx, ty;
            map_touch(m, wiz_raw[i][0], wiz_raw[i][1], x, y);
            wiz_target(i, tx, ty);
            err += (long)(x - tx) * (x - tx) + (long)(y - ty) * (y - ty);
        }
        if (best_err < 0 || err < best_err) { best_err = err; best = m; }
    }
    touch_mode = best;
    Settings s("nexi", true);
    s.SetInt("touchp", best);
    s.SetInt("setup", 1);
    ESP_LOGI("NexiUI", "касание настроено: режим %d", best);
}

inline void wiz_tap(void*) {
    if (wiz_step < 0) return;
    if (wiz_step < 3) {
        wiz_raw[wiz_step][0] = wiz_last[0];
        wiz_raw[wiz_step][1] = wiz_last[1];
        wiz_step++;
        if (wiz_step == 3) wiz_solve();
        wiz_show(nullptr);
    } else {
        wiz_step = -1;
        if (wiz) { lv_obj_delete(wiz); wiz = nullptr; }
    }
}

inline void touch_read(lv_indev_t*, lv_indev_data_t* data) {
    data->state = LV_INDEV_STATE_RELEASED;
    if (!tp) return;
    esp_lcd_touch_read_data(tp);
    uint16_t x[1], y[1];
    uint8_t cnt = 0;
    bool down = esp_lcd_touch_get_coordinates(tp, x, y, nullptr, &cnt, 1) && cnt > 0;
    if (down) {
        if (scr_dark) { eat_touch = true; lv_async_call([](void*) { scr_wake(); }, nullptr); }
        last_act = lv_tick_get();
    }
    if (eat_touch) { if (!down) eat_touch = false; return; }
    if (wiz_step >= 0) {
        // идёт первая настройка: касания не отдаём интерфейсу, а запоминаем «сырые»
        if (wiz_skip) { if (!down) wiz_skip = false; return; }
        if (down) { wiz_last[0] = x[0]; wiz_last[1] = y[0]; wiz_down = true; }
        else if (wiz_down) { wiz_down = false; lv_async_call(wiz_tap, nullptr); }
        return;
    }
    if (!down) return;
    int ox, oy;
    map_touch(touch_mode, x[0], y[0], ox, oy);
    data->point.x = ox;
    data->point.y = oy;
    data->state = LV_INDEV_STATE_PRESSED;
}

inline void set_touch_mode(int m) {
    touch_mode = m & 7;
    Settings s("nexi", true);
    s.SetInt("touchp", touch_mode);
}

/** Следующий вариант экрана: 0 → 180° → отражения. Касание поворачиваем вместе с картинкой. */
inline void next_screen() {
    int old = screen_mode;
    screen_mode = (screen_mode == 0) ? 3 : (screen_mode == 3) ? 1 : (screen_mode == 1) ? 2 : 0;
    int diff = old ^ screen_mode;
    int t = touch_mode;
    if (diff & 1) t ^= (t & 1) ? 4 : 2;
    if (diff & 2) t ^= (t & 1) ? 2 : 4;
    set_touch_mode(t);
    apply_screen();
    Settings s("nexi", true);
    s.SetInt("screenp", screen_mode);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

inline const lv_font_t* font() { return lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN); }

inline lv_obj_t* label(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font(), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

inline lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x7C5CFF), 0);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = label(b, text);
    lv_obj_center(l);
    return b;
}

inline void close_panel() {
    if (panel) { lv_obj_delete_async(panel); panel = nullptr; }
}

inline lv_obj_t* slider_row(lv_obj_t* parent, const char* title, int min, int max, int value, lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 12, 0);
    // кружок бегунка шире самой полоски — даём ему место по краям и не обрезаем
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* t = label(row, title);
    lv_obj_set_style_translate_x(t, -12, 0);
    lv_obj_t* s = lv_slider_create(row);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_height(s, 12);
    lv_obj_add_flag(s, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x7C5CFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, nullptr);
    return s;
}

// ------------------------------------------------------------------ игра: крестики-нолики против Nexi
// Ты — X, Nexi — O. Nexi играет хорошо, но иногда зевает (иначе её не обыграть). Кто ходит первым — по очереди.
namespace ttt {
inline int cell[9];                 // 0 пусто, 1 ты, 2 Nexi
inline lv_obj_t* root = nullptr;    // экран игры (он же panel, пока открыт)
inline void later(lv_timer_cb_t cb, uint32_t ms) { lv_timer_t* t = lv_timer_create(cb, ms, nullptr); lv_timer_set_repeat_count(t, 1); }
inline lv_obj_t* btn[9];
inline lv_obj_t* status = nullptr;
inline lv_obj_t* score_l = nullptr;
inline int you = 0, nexi = 0, draws = 0;
inline bool over = false, nexi_first = false;
inline const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};

inline int winner() {
    for (auto& l : L) if (cell[l[0]] && cell[l[0]] == cell[l[1]] && cell[l[1]] == cell[l[2]]) return cell[l[0]];
    for (int i = 0; i < 9; i++) if (!cell[i]) return 0;
    return 3;   // ничья
}
inline const char* pick(const char* const* a, int n) { return a[esp_random() % n]; }
inline void say(const char* t) { if (status) lv_label_set_text(status, t); }
inline void draw() {
    for (int i = 0; i < 9; i++) {
        lv_obj_t* l = lv_obj_get_child(btn[i], 0);
        lv_label_set_text(l, cell[i] == 1 ? "X" : cell[i] == 2 ? "O" : "");
        lv_obj_set_style_text_color(l, cell[i] == 1 ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xC9B8FF), 0);
    }
    static char sc[48];
    snprintf(sc, sizeof(sc), "Ты %d : %d Nexi", you, nexi);
    if (score_l) lv_label_set_text(score_l, sc);
}
// ход Nexi: выиграть → помешать → центр → угол → что есть; в 1 из 4 ходов — просто наугад
inline int nexi_move() {
    int free_[9], nf = 0;
    for (int i = 0; i < 9; i++) if (!cell[i]) free_[nf++] = i;
    if (!nf) return -1;
    if (esp_random() % 4 == 0) return free_[esp_random() % nf];
    for (int who : {2, 1})
        for (auto& l : L) {
            int c = 0, e = -1;
            for (int k = 0; k < 3; k++) { if (cell[l[k]] == who) c++; else if (!cell[l[k]]) e = l[k]; }
            if (c == 2 && e >= 0) return e;
        }
    if (!cell[4]) return 4;
    int corners[4] = {0, 2, 6, 8}, nc = 0, cs[4];
    for (int c : corners) if (!cell[c]) cs[nc++] = c;
    if (nc) return cs[esp_random() % nc];
    return free_[esp_random() % nf];
}
inline void finish(int w) {
    over = true;
    static const char* const lose[] = {"Ты победил! Реванш?", "Ладно, ты сильнее... пока что", "Повезло! Ещё раз?"};
    static const char* const win[] = {"Я победил! Ещё?", "Хе-хе, моя взяла", "Не расстраивайся, бывает"};
    static const char* const drw[] = {"Ничья! Ещё?", "Ничья. Достойно!", "Никто не сдался"};
    if (w == 1) { you++; say(pick(lose, 3)); }
    else if (w == 2) { nexi++; say(pick(win, 3)); }
    else { draws++; say(pick(drw, 3)); }
    draw();
}
inline void nexi_turn(lv_timer_t*) {
    if (over || !root || panel != root) return;   // игру закрыли, пока Nexi «думал»
    int m = nexi_move();
    if (m >= 0) cell[m] = 2;
    draw();
    int w = winner();
    if (w) finish(w); else say("Твой ход");
}
inline void reset() {
    for (int& c : cell) c = 0;
    over = false;
    nexi_first = !nexi_first;
    draw();
    if (nexi_first) { say("Я хожу первым..."); later(nexi_turn, 600); }
    else say("Твой ход: ты — X");
}
inline void tap(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (over) { reset(); return; }   // игра закончилась — касание по полю начинает новую
    if (cell[i]) return;
    // ход Nexi ещё не сделан (таймер) — ждём
    int mine = 0, hers = 0;
    for (int c : cell) { if (c == 1) mine++; else if (c == 2) hers++; }
    if (mine > hers || (nexi_first && mine == hers)) return;
    cell[i] = 1;
    draw();
    int w = winner();
    if (w) { finish(w); return; }
    static const char* const think[] = {"Хм...", "Так-так...", "Думаю...", "Интересно..."};
    say(pick(think, 4));
    later(nexi_turn, 500);
}
}  // namespace ttt

inline void open_game() {
    close_panel();
    panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x161829), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    ttt::root = panel;
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);

    lv_obj_t* head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ttt::score_l = label(head, "");
    lv_obj_t* x = button(head, "Выход", [](lv_event_t*) { ttt::root = nullptr; ttt::status = nullptr; ttt::score_l = nullptr; close_panel(); });
    lv_obj_set_width(x, 84);

    ttt::status = label(panel, "");
    lv_obj_set_style_text_color(ttt::status, lv_color_hex(0xB9A8FF), 0);

    lv_obj_t* grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 3 * 64 + 2 * 6, 3 * 64 + 2 * 6);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 9; i++) {
        lv_obj_t* b = lv_obj_create(grid);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 64, 64);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2D4A), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3E66), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* l = label(b, "");
        lv_obj_set_style_transform_scale(l, 512, 0);   // крупно (×2)
        lv_obj_set_style_transform_pivot_x(l, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(l, LV_PCT(50), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, ttt::tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        ttt::btn[i] = b;
    }
    ttt::reset();
}

inline void open_panel() {
    if (panel) return;
    auto& board = Board::GetInstance();
    panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x161829), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 10, 0);

    lv_obj_t* head = lv_obj_create(panel);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(head, "Настройки");
    lv_obj_t* x = button(head, "Готово", [](lv_event_t*) { close_panel(); });
    lv_obj_set_width(x, 96);

    auto* codec = board.GetAudioCodec();
    slider_row(panel, "Громкость", 0, 100, codec ? codec->output_volume() : 70, [](lv_event_t* e) {
        auto* c = Board::GetInstance().GetAudioCodec();
        if (c) c->SetOutputVolume(lv_slider_get_value((lv_obj_t*)lv_event_get_target(e)));
    });
    auto* bl = board.GetBacklight();
    slider_row(panel, "Яркость", 5, 100, bl ? bl->brightness() : 80, [](lv_event_t* e) {
        auto* b = Board::GetInstance().GetBacklight();
        if (b) b->SetBrightness(lv_slider_get_value((lv_obj_t*)lv_event_get_target(e)), true);
    });

    // экран вертикальный и узкий — кнопки друг под другом, панель листается
    auto* disp = board.GetDisplay();
    bool dark = disp && disp->GetTheme() && disp->GetTheme()->name() == "dark";
    lv_obj_t* th = button(panel, dark ? "Тёмная тема" : "Светлая тема", [](lv_event_t* e) {
        auto* d = Board::GetInstance().GetDisplay();
        if (!d || !d->GetTheme()) return;
        bool to_dark = d->GetTheme()->name() != "dark";
        auto* t = LvglThemeManager::GetInstance().GetTheme(to_dark ? "dark" : "light");
        if (!t) return;
        d->SetTheme(t);
        lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
        lv_label_set_text(lv_obj_get_child(b, 0), to_dark ? "Тёмная тема" : "Светлая тема");
    });
    lv_obj_set_width(th, LV_PCT(100));

    // через сколько гаснет экран: 30 с → 1 мин → 5 мин → никогда
    static char scr_txt[40];
    auto scr_name = []() {
        snprintf(scr_txt, sizeof(scr_txt), scr_off_sec <= 0 ? "Экран: не гасить" : scr_off_sec < 60 ? "Экран гаснет: %d с" : "Экран гаснет: %d мин",
                 scr_off_sec < 60 ? scr_off_sec : scr_off_sec / 60);
        return (const char*)scr_txt;
    };
    lv_obj_t* so = button(panel, scr_name(), [](lv_event_t* e) {
        scr_off_sec = scr_off_sec == 30 ? 60 : scr_off_sec == 60 ? 300 : scr_off_sec == 300 ? 0 : 30;
        { Settings st("nexi", true); st.SetInt("scroff", scr_off_sec); }
        snprintf(scr_txt, sizeof(scr_txt), scr_off_sec <= 0 ? "Экран: не гасить" : scr_off_sec < 60 ? "Экран гаснет: %d с" : "Экран гаснет: %d мин",
                 scr_off_sec < 60 ? scr_off_sec : scr_off_sec / 60);
        lv_label_set_text(lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0), scr_txt);
    });
    lv_obj_set_width(so, LV_PCT(100));
}

// экран первой настройки: приветствие, кружок для нажатия, «готово»
inline void wiz_show(void*) {
    if (wiz) { lv_obj_delete(wiz); wiz = nullptr; }
    if (wiz_step < 0) return;
    close_panel();
    wiz = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wiz);
    lv_obj_set_size(wiz, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wiz, lv_color_hex(0x12101F), 0);
    lv_obj_set_style_bg_opa(wiz, LV_OPA_COVER, 0);
    lv_obj_remove_flag(wiz, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(wiz);
    if (wiz_step < 3) {
        lv_obj_t* t = label(wiz, wiz_step == 0 ? "Привет! Я Nexi\nНастроим экран:\nнажми на кружок" : "Отлично!\nТеперь сюда");
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
        static char step[32];
        snprintf(step, sizeof(step), "%d из 3", wiz_step + 1);
        lv_obj_t* n = label(wiz, step);
        lv_obj_set_style_text_color(n, lv_color_hex(0x8C80B8), 0);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, 64);
        int x, y;
        wiz_target(wiz_step, x, y);
        lv_obj_t* c = lv_obj_create(wiz);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, 40, 40);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x7C5CFF), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c, lv_color_white(), 0);
        lv_obj_set_style_border_width(c, 3, 0);
        lv_obj_set_pos(c, x - 20, y - 20);
    } else {
        lv_obj_t* t = label(wiz, "Готово!\n\nКасание настроено.\nВключи «Станцию Nexi»\nв приложении на телефоне.\n\nНажми, чтобы начать");
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    }
}

/** Запустить первую настройку (при первом включении или долгим нажатием на звезду). */
inline void wiz_start() {
    if (!tp) return;
    wiz_step = 0;
    wiz_down = false;
    wiz_skip = true;
    wiz_show(nullptr);
}

// ------------------------------------------------------------------ будильник на экране
inline lv_obj_t* alarm_label = nullptr;
inline lv_obj_t* alarm_ring = nullptr;

inline void alarm_ui(int state, int hh, int mm) {
    if (!lvgl_port_lock(300)) return;
    if (alarm_ring && state != 2) { lv_obj_delete_async(alarm_ring); alarm_ring = nullptr; }
    // заведённый будильник отдельной надписью больше не показываем — она налезала на текст ответа
    (void)hh; (void)mm;
    if (alarm_label) {
        lv_obj_add_flag(alarm_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (state == 2 && !alarm_ring) {
        close_panel();
        // прозрачный слой поверх экрана: сверху часы, снизу подсказка, а звезда посередине остаётся видна —
        // слой не ловит касания, они доходят до звезды
        alarm_ring = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(alarm_ring);
        lv_obj_set_size(alarm_ring, LV_PCT(100), LV_PCT(100));
        lv_obj_remove_flag(alarm_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(alarm_ring, LV_OBJ_FLAG_SCROLLABLE);
        time_t now = time(nullptr) + (time_t)nexi::alarm::tz_min.load() * 60;
        static char t[16];
        snprintf(t, sizeof(t), "%02d:%02d", (int)((now / 3600) % 24), (int)((now / 60) % 60));
        lv_obj_t* big = label(alarm_ring, t);
        lv_obj_set_style_transform_scale(big, 512, 0);   // крупно (×2)
        lv_obj_set_style_transform_pivot_x(big, LV_PCT(50), 0);
        lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_t* l = label(alarm_ring, "Будильник!\nНажми на звезду");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -40);
    }
    lvgl_port_unlock();
}

/** Сенсор FT6336 (на общей шине I2C с аудио) и кнопка ≡ в углу экрана. Ошибки не фатальны: без сенсора колонка работает как раньше. */
inline void Init(i2c_master_bus_handle_t bus) {
    {
        Settings s("nexi");
        touch_mode = s.GetInt("touchp", 0) & 7;
        scr_off_sec = s.GetInt("scroff", 60);
        screen_mode = s.GetInt("screenp", 0) & 3;
    }
    nexi::alarm::on_ui = alarm_ui;
    nexi::on_game = [](const char*) { lv_async_call([](void*) { scr_wake(); open_game(); }, nullptr); };
    nexi::alarm::show();
    if (screen_mode) apply_screen();
    esp_lcd_touch_config_t cfg = {};
    cfg.x_max = 240;
    cfg.y_max = 320;
    cfg.rst_gpio_num = GPIO_NUM_18;  // у ES3C28P сенсор FT6336: RST 18, INT 17, I2C общий с ES8311
    cfg.int_gpio_num = GPIO_NUM_17;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.scl_speed_hz = 400 * 1000;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK || esp_lcd_touch_new_i2c_ft5x06(io, &cfg, &tp) != ESP_OK) {
        ESP_LOGW("NexiUI", "сенсор не найден");
        tp = nullptr;
        return;
    }
    if (!lvgl_port_lock(0)) return;
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, lv_display_get_default());

    // кнопка ≡ — три полоски, без значков из шрифта
    lv_obj_t* g = lv_button_create(lv_layer_top());
    lv_obj_set_size(g, 44, 44);
    lv_obj_align(g, LV_ALIGN_TOP_RIGHT, -6, 30);   // справа сверху, под строкой состояния
    lv_obj_set_style_radius(g, 22, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(0x7C5CFF), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_80, 0);
    lv_obj_set_ext_click_area(g, 12);
    for (int i = 0; i < 3; i++) {
        lv_obj_t* bar = lv_obj_create(g);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 20, 3);
        lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, (i - 1) * 7);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(g, [](lv_event_t*) { open_panel(); }, LV_EVENT_CLICKED, nullptr);

    // нажать на звезду Nexi посередине — как кнопка BOOT: слушать / перестать (и выключить будильник)
    lv_obj_t* star = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(star);
    lv_obj_set_size(star, 150, 150);
    lv_obj_align(star, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(star, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(star);   // окна настроек и будильника — поверх неё
    lv_obj_add_event_cb(star, [](lv_event_t*) {
        if (nexi::alarm::Stop()) return;
        Application::GetInstance().ToggleChatState();
    }, LV_EVENT_SHORT_CLICKED, nullptr);
    // держать звезду 5 секунд — настроить касание заново (если вдруг нажатия «уехали»)
    static uint32_t star_down = 0;
    lv_obj_add_event_cb(star, [](lv_event_t*) { star_down = lv_tick_get(); }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(star, [](lv_event_t*) {
        if (star_down && lv_tick_elaps(star_down) > 5000) { star_down = 0; lv_async_call([](void*) { wiz_start(); }, nullptr); }
    }, LV_EVENT_PRESSING, nullptr);

    last_act = lv_tick_get();
    lv_timer_create(scr_tick, 1000, nullptr);

    // первое включение — сразу первая настройка
    {
        Settings st("nexi");
        // чуть позже: пусть сначала поднимется интерфейс со шрифтом с русскими буквами
        if (st.GetInt("setup", 0) == 0) {
            lv_timer_t* t = lv_timer_create([](lv_timer_t*) { wiz_start(); }, 3500, nullptr);
            lv_timer_set_repeat_count(t, 1);
        }
    }
    lvgl_port_unlock();
    ESP_LOGI("NexiUI", "сенсор и кнопка настроек готовы");
}

}  // namespace nexi::ui
