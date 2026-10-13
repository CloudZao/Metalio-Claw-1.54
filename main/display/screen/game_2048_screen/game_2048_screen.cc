#include "game_2048_screen.h"
#include "i18n.h"

#include "home_screen/home_screen.h"
#include "panel_metrics.h"
#include "screen_util.h"

#include <stdlib.h>
#include <string.h>

#include <cmath>

#include "lvgl.h"

LV_FONT_DECLARE(font_puhui_20_4);

/* 对齐 HTML：深色壳 + 单行分数 + 居中棋盘（240×240） */
#define SCR_W           240
#define SCR_H           240
#define MARGIN_X        8
#define HEADER_H        32
#define SCORE_H         28
#define BODY_PAD        8
#define BOARD_SIZE      160
#define BOARD_X         ((SCR_W - BOARD_SIZE) / 2)
#define BOARD_Y         (HEADER_H + BODY_PAD + SCORE_H + 6)

#define GRID_N          4
#define PAD             4
#define GAP             4
#define CELL_SIZE       ((BOARD_SIZE - 2 * PAD - 3 * GAP) / GRID_N)

#define COLOR_BG        0x0E1116
#define COLOR_HEADER    0x12151C
#define COLOR_CARD      0x1B2030
#define COLOR_BOARD     0x1A1F2A
#define COLOR_EMPTY     0x2A3142
#define COLOR_TEXT      0xFFFFFF
#define COLOR_MUTED     0x9AA3B2

namespace {

uint16_t s_grid[GRID_N][GRID_N];
uint32_t s_score;
uint32_t s_best;
bool s_game_over;
bool s_won;
bool s_win_dismissed;

lv_obj_t* s_score_val;
lv_obj_t* s_board;
lv_obj_t* s_cells[GRID_N][GRID_N];
lv_obj_t* s_labels[GRID_N][GRID_N];
lv_obj_t* s_overlay;
lv_obj_t* s_overlay_msg;
lv_point_t s_press_pt;
bool s_pressing;

uint32_t tile_bg_color(uint16_t v) {
    switch (v) {
    case 0:    return COLOR_EMPTY;
    case 2:    return 0x3B4A63;
    case 4:    return 0x4A5D7A;
    case 8:    return 0xF59E0B;
    case 16:   return 0xF97316;
    case 32:   return 0xEF4444;
    case 64:   return 0xDC2626;
    case 128:  return 0xEAB308;
    case 256:  return 0xCA8A04;
    case 512:  return 0xA16207;
    case 1024: return 0x3B82F6;
    case 2048: return 0x2563EB;
    default:   return 0x1E293B;
    }
}

uint32_t tile_fg_color(uint16_t v) {
    if (v == 8 || v == 16 || v == 128 || v == 256) {
        return 0x1A1200;
    }
    return COLOR_TEXT;
}

int cell_px(int idx) {
    return PAD + idx * (CELL_SIZE + GAP);
}

void grid_clear() {
    memset(s_grid, 0, sizeof(s_grid));
}

bool grid_has_empty() {
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (s_grid[r][c] == 0) {
                return true;
            }
        }
    }
    return false;
}

bool grid_can_merge() {
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            uint16_t v = s_grid[r][c];
            if (v == 0) {
                continue;
            }
            if (c + 1 < GRID_N && s_grid[r][c + 1] == v) {
                return true;
            }
            if (r + 1 < GRID_N && s_grid[r + 1][c] == v) {
                return true;
            }
        }
    }
    return false;
}

void spawn_tile() {
    int empties[16];
    int n = 0;
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (s_grid[r][c] == 0) {
                empties[n++] = r * GRID_N + c;
            }
        }
    }
    if (n == 0) {
        return;
    }
    int pick = empties[rand() % n];
    int r = pick / GRID_N;
    int c = pick % GRID_N;
    s_grid[r][c] = (rand() % 10 < 9) ? 2 : 4;
}

bool move_line(uint16_t line[4], uint32_t* score_add) {
    uint16_t orig[4];
    memcpy(orig, line, sizeof(orig));

    int w = 0;
    for (int i = 0; i < 4; i++) {
        if (line[i] != 0) {
            line[w++] = line[i];
        }
    }
    while (w < 4) {
        line[w++] = 0;
    }

    for (int i = 0; i < 3; i++) {
        if (line[i] != 0 && line[i] == line[i + 1]) {
            line[i] *= 2;
            *score_add += line[i];
            for (int j = i + 1; j < 3; j++) {
                line[j] = line[j + 1];
            }
            line[3] = 0;
            i--;
        }
    }

    w = 0;
    for (int i = 0; i < 4; i++) {
        if (line[i] != 0) {
            line[w++] = line[i];
        }
    }
    while (w < 4) {
        line[w++] = 0;
    }

    return memcmp(orig, line, sizeof(orig)) != 0;
}

bool move_left() {
    bool moved = false;
    uint32_t add = 0;
    for (int r = 0; r < GRID_N; r++) {
        uint16_t line[4] = {s_grid[r][0], s_grid[r][1], s_grid[r][2], s_grid[r][3]};
        if (move_line(line, &add)) {
            moved = true;
            for (int c = 0; c < GRID_N; c++) {
                s_grid[r][c] = line[c];
            }
        }
    }
    s_score += add;
    return moved;
}

bool move_right() {
    bool moved = false;
    uint32_t add = 0;
    for (int r = 0; r < GRID_N; r++) {
        uint16_t line[4] = {s_grid[r][3], s_grid[r][2], s_grid[r][1], s_grid[r][0]};
        if (move_line(line, &add)) {
            moved = true;
            for (int c = 0; c < GRID_N; c++) {
                s_grid[r][c] = line[3 - c];
            }
        }
    }
    s_score += add;
    return moved;
}

bool move_up() {
    bool moved = false;
    uint32_t add = 0;
    for (int c = 0; c < GRID_N; c++) {
        uint16_t line[4] = {s_grid[0][c], s_grid[1][c], s_grid[2][c], s_grid[3][c]};
        if (move_line(line, &add)) {
            moved = true;
            for (int r = 0; r < GRID_N; r++) {
                s_grid[r][c] = line[r];
            }
        }
    }
    s_score += add;
    return moved;
}

bool move_down() {
    bool moved = false;
    uint32_t add = 0;
    for (int c = 0; c < GRID_N; c++) {
        uint16_t line[4] = {s_grid[3][c], s_grid[2][c], s_grid[1][c], s_grid[0][c]};
        if (move_line(line, &add)) {
            moved = true;
            for (int r = 0; r < GRID_N; r++) {
                s_grid[r][c] = line[3 - r];
            }
        }
    }
    s_score += add;
    return moved;
}

void check_end_state() {
    if (s_score > s_best) {
        s_best = s_score;
    }
    if (!s_won) {
        for (int r = 0; r < GRID_N; r++) {
            for (int c = 0; c < GRID_N; c++) {
                if (s_grid[r][c] >= 2048) {
                    s_won = true;
                    break;
                }
            }
        }
    }
    if (!grid_has_empty() && !grid_can_merge()) {
        s_game_over = true;
    }
}

void update_scores_ui() {
    if (s_score_val != nullptr) {
        lv_label_set_text_fmt(s_score_val, "%u", (unsigned)s_score);
    }
}

void update_tiles_ui() {
    char buf[8];
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            uint16_t v = s_grid[r][c];
            lv_obj_t* cell = s_cells[r][c];
            lv_obj_t* lbl = s_labels[r][c];
            lv_obj_set_style_bg_color(cell, lv_color_hex(tile_bg_color(v)), LV_PART_MAIN);
            if (v == 0) {
                lv_label_set_text(lbl, "");
                lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_snprintf(buf, sizeof(buf), "%u", (unsigned)v);
                lv_label_set_text(lbl, buf);
                lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(lbl, lv_color_hex(tile_fg_color(v)), LV_PART_MAIN);
            }
        }
    }
    update_scores_ui();
}

void show_overlay(const char* msg) {
    lv_label_set_text(s_overlay_msg, msg);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void hide_overlay() {
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void refresh_after_move() {
    update_tiles_ui();
    if (s_game_over) {
        show_overlay(I18n::T("游戏结束!\n点击继续"));
    } else if (s_won && !s_win_dismissed) {
        show_overlay(I18n::T("你赢了 2048!\n继续玩或点这里"));
    }
}

bool do_move(bool (*fn)()) {
    if (s_game_over) {
        return false;
    }
    bool moved = fn();
    if (!moved) {
        return false;
    }
    spawn_tile();
    check_end_state();
    refresh_after_move();
    return true;
}

void game_reset() {
    s_score = 0;
    s_game_over = false;
    s_won = false;
    s_win_dismissed = false;
    grid_clear();
    spawn_tile();
    spawn_tile();
    hide_overlay();
    refresh_after_move();
}

void board_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_press_pt);
        s_pressing = true;
    } else if (code == LV_EVENT_RELEASED && s_pressing) {
        lv_point_t rel;
        lv_indev_get_point(indev, &rel);
        s_pressing = false;

        int dx = rel.x - s_press_pt.x;
        int dy = rel.y - s_press_pt.y;
        const int th = 40;

        if (std::abs(dx) < th && std::abs(dy) < th) {
            return;
        }

        if (std::abs(dx) > std::abs(dy)) {
            if (dx > 0) {
                do_move(move_right);
            } else {
                do_move(move_left);
            }
        } else {
            if (dy > 0) {
                do_move(move_down);
            } else {
                do_move(move_up);
            }
        }
    }
}

void overlay_click_cb(lv_event_t* e) {
    (void)e;
    if (s_game_over) {
        game_reset();
    } else if (s_won) {
        s_win_dismissed = true;
        hide_overlay();
    }
}

void back_btn_cb(lv_event_t* e) {
    (void)e;
    if (screen_nav_try_return_to_settings()) {
        return;
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void score_long_press_cb(lv_event_t* e) {
    (void)e;
    game_reset();
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* top = lv_obj_create(parent);
    screen_strip_obj_chrome(top);
    lv_obj_set_size(top, SCR_W, HEADER_H);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(title);

    // 单行分数条（对齐 HTML .score）；长按重新开始
    lv_obj_t* score_bar = lv_obj_create(parent);
    screen_strip_obj_chrome(score_bar);
    lv_obj_set_size(score_bar, SCR_W - 2 * BODY_PAD, SCORE_H);
    lv_obj_set_pos(score_bar, BODY_PAD, HEADER_H + BODY_PAD);
    lv_obj_set_style_radius(score_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(score_bar, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(score_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(score_bar, 10, LV_PART_MAIN);
    lv_obj_add_flag(score_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(score_bar, score_long_press_cb, LV_EVENT_LONG_PRESSED,
                        nullptr);
    screen_swipe_back_ignore(score_bar, true);

    lv_obj_t* score_cap = lv_label_create(score_bar);
    lv_label_set_text(score_cap, I18n::T("分数"));
    lv_obj_set_style_text_color(score_cap, lv_color_hex(COLOR_MUTED),
                                LV_PART_MAIN);
    lv_obj_align(score_cap, LV_ALIGN_LEFT_MID, 0, 0);
    screen_make_input_passive(score_cap);

    s_score_val = lv_label_create(score_bar);
    lv_label_set_text(s_score_val, "0");
    lv_obj_set_style_text_color(s_score_val, lv_color_hex(COLOR_TEXT),
                                LV_PART_MAIN);
    lv_obj_align(s_score_val, LV_ALIGN_RIGHT_MID, 0, 0);
    screen_make_input_passive(s_score_val);
}

void build_board(lv_obj_t* parent) {
    s_board = lv_obj_create(parent);
    lv_obj_remove_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_board, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_pos(s_board, BOARD_X, BOARD_Y);
    lv_obj_set_style_radius(s_board, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_board, lv_color_hex(COLOR_BOARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_board, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_board, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board, board_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_board, board_event_cb, LV_EVENT_RELEASED, NULL);
    screen_swipe_back_ignore(s_board, true);

    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            lv_obj_t* cell = lv_obj_create(s_board);
            s_cells[r][c] = cell;
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
            lv_obj_set_pos(cell, cell_px(c), cell_px(r));
            lv_obj_set_style_radius(cell, 5, LV_PART_MAIN);
            lv_obj_set_style_bg_color(cell, lv_color_hex(COLOR_EMPTY),
                                     LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* lbl = lv_label_create(cell);
            s_labels[r][c] = lbl;
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_center(lbl);
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void build_footer(lv_obj_t* parent) {
    (void)parent;
}

void build_overlay(lv_obj_t* parent) {
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_overlay, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_pos(s_overlay, BOARD_X, BOARD_Y);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_overlay, 8, LV_PART_MAIN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    s_overlay_msg = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlay_msg, lv_color_hex(COLOR_TEXT),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_overlay_msg, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_width(s_overlay_msg, BOARD_SIZE - 40);
    lv_label_set_long_mode(s_overlay_msg, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_overlay_msg);
}

}  // namespace

lv_obj_t* Game2048::Create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr, &font_puhui_20_4, LV_PART_MAIN);

    s_pressing = false;

    build_header(scr);
    build_board(scr);
    build_footer(scr);
    build_overlay(scr);
    screen_attach_swipe_back(scr, []() { back_btn_cb(nullptr); });

    srand((unsigned)lv_tick_get());
    game_reset();
    return scr;
}
