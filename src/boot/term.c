
#include "term_font.c"

// Margins for text output
#define TERM_MARGIN_X 5
#define TERM_MARGIN_Y 5

// On-screen font size
#define TERM_FONT_CHAR_SZ_X 9
#define TERM_FONT_CHAR_SZ_Y 16

Framebuffer term_fb;
u64 term_cur_x;
u64 term_cur_y;
u64 term_cnt_x;
u64 term_cnt_y;
u32 term_color_bg;
u32 term_color_fg;
bool term_wrap;

static void term_set_framebuffer(Framebuffer fb) {
    term_fb = fb;
    term_cur_x = 0;
    term_cur_y = 0;
    term_color_bg = 0xff000000;
    term_color_fg = 0xffa8a8a8;
    term_cnt_x = (fb.width - 2*TERM_MARGIN_X) / TERM_FONT_CHAR_SZ_X;
    term_cnt_y = (fb.height - 2*TERM_MARGIN_Y) / TERM_FONT_CHAR_SZ_Y;
}

static void term_set_wrap(bool wrap) {
    term_wrap = wrap;
}

static void term_colors(u32 bg, u32 fg) {
    term_color_bg = bg;
    term_color_fg = fg;
}

static void term_print_char_at(int char_x, int char_y, char ch) {
    int offset_x = TERM_MARGIN_X + char_x * TERM_FONT_CHAR_SZ_X;
    int offset_y = TERM_MARGIN_Y + char_y * TERM_FONT_CHAR_SZ_Y;
    for(int y = 0; y != TERM_FONT_CHAR_SZ_Y; ++y) {
        u8 byte = term_font[16 * (u8) ch + y];
        for(int x = 0; x != 8; ++x) {
            bool is_fg = (byte >> x) & 1;
            u32 color;
            if(is_fg) {
                color = term_color_fg;
            }
            else {
                color = term_color_bg;
            }
            int pixel_x = offset_x + x;
            int pixel_y = offset_y + y;
            if(pixel_x > term_fb.width) {
                break;
            }
            if(pixel_y > term_fb.height) {
                return;
            }
            term_fb.pixels[pixel_x + pixel_y * term_fb.stride] = color;
        }
        // Fill in 9'th row
        {
            int x = 8;
            u32 color = term_color_bg;
            int pixel_x = offset_x + x;
            int pixel_y = offset_y + y;
            if(pixel_x > term_fb.width) {
                break;
            }
            if(pixel_y > term_fb.height) {
                return;
            }
            term_fb.pixels[pixel_x + pixel_y * term_fb.stride] = color;
        }
    }
}

static void term_scroll_down1() {
    if(0) {
        // TODO(bumbread): seems that there's a bug
        int term_rows_to_overwrite = (term_cnt_y - 1) * TERM_FONT_CHAR_SZ_Y;
        int next_row_stride = term_fb.stride * TERM_FONT_CHAR_SZ_Y;
        // Shift lines from 0 to line_count-1
        for(int y = 0; y < term_rows_to_overwrite; ++y) {
            for(int x = 0; x < term_fb.width; ++x) {
                int src_y = y + next_row_stride;
                int dst_y = y;
                term_fb.pixels[x + dst_y * term_fb.stride] = term_fb.pixels[x + src_y * term_fb.stride];
            }
        }
        // Zero-out the last line
        for(int y = term_rows_to_overwrite; y < term_fb.height; ++y) {
            for(int x = 0; x < term_fb.width; ++x) {
                int src_y = y;
                int dst_y = y + next_row_stride;
                term_fb.pixels[x + dst_y * term_fb.stride] = term_color_bg;
            }
        }
        // Put cursor on the line above
        term_cur_y -= 1;
    }
    else {
        for(int y = 0; y < term_fb.height; ++y) {
            for(int x = 0; x < term_fb.width; ++x) {
                term_fb.pixels[x + y * term_fb.stride] = term_color_bg;
            }
        }
        term_cur_y = 0;
    }
}

static void term_printc(char c) {
    if(c == '\n') {
        term_cur_x = 0;
        term_cur_y += 1;
        if(term_cur_y == term_cnt_y) {
            term_scroll_down1();
        }
        return;
    }
    term_print_char_at(term_cur_x, term_cur_y, c);
    term_cur_x += 1;
    if(term_cur_x >= term_cnt_x) {
        if(term_wrap) {
            term_cur_x = 0;
            term_cur_y += 1;
            if(term_cur_y == term_cnt_y) {
                term_scroll_down1();
            }
        }
    }
}