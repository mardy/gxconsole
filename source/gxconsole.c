#include "gxconsole.h"

#include "console_internal.h"

#include <gccore.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>

#define _ANSI_MAXARGS 16

typedef struct
{
    struct
    {
        int flags;
        u16 fg;
        u16 bg;
    } color;
    int arg_idx;
    int args[_ANSI_MAXARGS];
    int color_arg_count;
    unsigned int color_args[3];
    bool has_arg;
    enum
    {
        ESC_NONE,
        ESC_START,
        ESC_BUILDING_UNKNOWN,
        ESC_BUILDING_FORMAT_FG,
        ESC_BUILDING_FORMAT_BG,
        ESC_BUILDING_FORMAT_FG_NONRGB,
        ESC_BUILDING_FORMAT_BG_NONRGB,
        ESC_BUILDING_FORMAT_FG_RGB,
        ESC_BUILDING_FORMAT_BG_RGB,
    } state;
} EscapeSeq;

struct gx_console_t {
    GXTexObj texobj;
    u16 width_chars;
    u16 height_chars;
    u32 texture_size;
    const GxConsoleFont *font;
    u8 char_w;
    u8 char_h;
    u16 pitch;
    u16 tiles_per_row; /* GX 4x4 tiles in a row */
    u16 bg;
    u16 fg;
    int flags;
    u8 tab_size;
    u8 alpha;
    u16 cursor_x;
    u16 cursor_y;
    u16 prev_cursor_x;
    u16 prev_cursor_y;
    EscapeSeq esc;
    devoptab_t dotab;
    u8 texels[] ATTRIBUTE_ALIGN(32);
};

static const u32 s_color_table[] = {
    0x000000,     /* black */
    0x800000,     /* red */
    0x008000,     /* green */
    0x808000,     /* yellow */
    0x000080,     /* blue */
    0x800080,     /* magenta */
    0x008080,     /* cyan */
    0xc0c0c0,     /* white */

    0x000000,     /* bright black */
    0xff0000,     /* bright red */
    0x00ff00,     /* bright green */
    0xffff00,     /* bright yellow */
    0x0000ff,     /* bright blue */
    0xff00ff,     /* bright magenta */
    0x00ffff,     /* bright cyan */
    0xffffff,     /* bright white */

    0x000000,     /* faint black */
    0x400000,     /* faint red */
    0x004000,     /* faint green */
    0x404000,     /* faint yellow */
    0x000040,     /* faint blue */
    0x400040,     /* faint magenta */
    0x004040,     /* faint cyan */
    0x808080,     /* faint white */
};

static const u8 s_color_cube[] = {
    0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff,
};

static const u8 s_gray_scale[] = {
    0x08, 0x12, 0x1c, 0x26, 0x30, 0x3a, 0x44, 0x4e,
    0x58, 0x62, 0x6c, 0x76, 0x80, 0x8a, 0x94, 0x9e,
    0xa8, 0xb2, 0xbc, 0xc6, 0xd0, 0xda, 0xe4, 0xee,
};

static GxConsole *s_console_stdout = NULL;
static GxConsole *s_console_stderr = NULL;

extern u8 console_font_8x16[];

static u16 rgb_to_rgb16(u8 r, u8 g, u8 b)
{
    return ((r * 0x1f / 255) << (6 + 5)) |
           ((g * 0x3f / 255) << 5) |
           ((b * 0x1f / 255) << 0);
}

static u16 rgb32_to_rgb16(u32 rgb)
{
    u8 r = rgb >> 16;
    u8 g = (rgb >> 8) & 0xff;
    u8 b = rgb & 0xff;
    return rgb_to_rgb16(r, g, b);
}

static u16 console_color(u8 index)
{
    if (index > 24) index = 0;
    return rgb32_to_rgb16(s_color_table[index]);
}

static void *console_tile_by_pixel(GxConsole *con, int x, int y, int *tx, int *ty)
{
    int tile_x = x / 4;
    int tile_y = y / 4;
    void *ptr = &con->texels[32 * (tile_y * con->tiles_per_row + tile_x)];
    *tx = x % 4;
    *ty = y % 4;
    return ptr;
}

static void *console_get_char_tile_tl(GxConsole *con, int x, int y, int *tx, int *ty)
{
    int px = x * con->char_w;
    int py = y * con->char_h;
    return console_tile_by_pixel(con, px, py, tx, ty);
}

static void *console_get_char_tile_tr(GxConsole *con, int x, int y, int *tx, int *ty)
{
    int px = (x + 1) * con->char_w - 1;
    int py = y * con->char_h;
    return console_tile_by_pixel(con, px, py, tx, ty);
}

static void *console_get_char_tile_bl(GxConsole *con, int x, int y, int *tx, int *ty)
{
    int px = x * con->char_w;
    int py = (y + 1) * con->char_h - 1;
    return console_tile_by_pixel(con, px, py, tx, ty);
}

static void *console_get_char_tile_br(GxConsole *con, int x, int y, int *tx, int *ty)
{
    int px = (x + 1) * con->char_w - 1;
    int py = (y + 1) * con->char_h - 1;
    return console_tile_by_pixel(con, px, py, tx, ty);
}

static inline void *console_get_cursor_start_tile(GxConsole *con, int *tx, int *ty)
{
    return console_get_char_tile_tl(con, con->cursor_x - 1, con->cursor_y - 1, tx, ty);
}

static inline void *console_get_cursor_end_tile(GxConsole *con, int *tx, int *ty)
{
    return console_get_char_tile_br(con, con->cursor_x - 1, con->cursor_y - 1, tx, ty);
}

static inline void draw_pixel_1bpp(const u8 *bits, int offset,
                                   u16 fgcolor, u16 bgcolor, u16 *p)
{
    u8 byte = bits[offset / 8];
    bool bit = byte & (1 << (7 - offset % 8));
    *p = bit ? fgcolor : bgcolor;
}

static void console_drawc(GxConsole *con, int c)
{
    u16 fgcolor, bgcolor;

    if (!con) return;
    const GxConsoleFont *font = con->font;
    c -= font->ascii_offset;
    if (c < 0 || c > font->num_chars) return;

    int char_size_bytes = (font->char_width * font->char_height * font->bpp + 7) / 8;
    const u8 *pbits = &font->pixels[c * char_size_bytes];

    fgcolor = con->fg;
    bgcolor = con->bg;

    if (!(con->flags & CONSOLE_FG_CUSTOM)) {
        if (con->flags & (CONSOLE_COLOR_BOLD | CONSOLE_COLOR_FG_BRIGHT)) {
            fgcolor += 8;
        } else if (con->flags & CONSOLE_COLOR_FAINT) {
            fgcolor += 16;
        }
        fgcolor = rgb32_to_rgb16(s_color_table[fgcolor]);
    }

    if (!(con->flags & CONSOLE_BG_CUSTOM)) {
        if (con->flags & CONSOLE_COLOR_BG_BRIGHT) bgcolor += 8;
        bgcolor = rgb32_to_rgb16(s_color_table[bgcolor]);
    }

    if (con->flags & CONSOLE_COLOR_REVERSE) {
        u16 tmp = fgcolor;
        fgcolor = bgcolor;
        bgcolor = tmp;
    }

    /* For strikeover or underline */
    int line_thickness = font->char_height / 8;
    int strikeover_y = con->flags & CONSOLE_CROSSED_OUT ? (font->char_height / 2) : 1000;
    int underline_y = con->flags & CONSOLE_UNDERLINE ? (font->char_height - line_thickness - 1) : 1000;

    int tx0, ty0;
    u16 *tile = console_get_cursor_start_tile(con, &tx0, &ty0);
    u16 *px0 = tile + ty0 * 4 + tx0;
    for (int cy = 0; cy < font->char_height; cy++) {
        if ((ty0 + cy) % 4 == 0) {
            /* Move to one tile down */
            px0 = tile + ((cy + ty0) / 4) * (con->tiles_per_row * 16) + tx0;
        }
        u16 *p = px0;
        for (int cx = 0; cx < font->char_width; cx++) {
            int offset = cy * font->char_width + cx;
            if ((cy >= strikeover_y && cy < strikeover_y + line_thickness) ||
                (cy >= underline_y && cy < underline_y + line_thickness)) {
                *p = fgcolor;
            } else if (font->bpp == 1) {
                draw_pixel_1bpp(pbits, offset, fgcolor, bgcolor, p);
            } else {
                /* TODO */
            }
            if ((tx0 + cx) % 4 == 3) {
                /* Move to the tile right */
                p += 16 - 3;
            } else {
                /* Same tile, one pixel right */
                p++;
            }
        }

        /* Same tile, one row down */
        px0 += 4;
    }
}

static void tile_row_move_sub(
    void *texels, void *dest, int tile_from, int tile_to, int num_tile_rows,
    int row_from, int row_to, int num_pixel_rows, int tiles_per_row)
{
    if (num_pixel_rows <= 0 || num_tile_rows <= 0) return;
    int byte_size = num_pixel_rows * 8; /* 8 = 4 pixels with bpp16 */
    u8 *src = (u8 *)texels + 32 * tiles_per_row * tile_from + 8 * row_from;
    u8 *dst = (u8 *)dest + 32 * tiles_per_row * tile_to + 8 * row_to;
    for (int i = 0; i < num_tile_rows; i++) {
        for (int j = 0; j < tiles_per_row; j++) {
            memmove(dst, src, byte_size);
            dst += 32;
            src += 32;
        }
    }
}

static void tile_row_move(void *texels, int from_y, int to_y, int height, int tiles_per_row)
{
    if (from_y % 4 == 0 && to_y % 4 == 0 && height % 4 == 0) {
        /* Simplest case: we are moving whole tiles */
        u8 *ptr = texels;
        /* 8 = 32 / 4, since we don't move the whole tile, but just one row */
        memmove(ptr + 8 * tiles_per_row * to_y,
                ptr + 8 * tiles_per_row * from_y,
                8 * tiles_per_row * height);
    } else {
        /* Split the operation in three phases:
         *                  Phase 1  Phase 2  Phase 3
         *        ....       ....     ....     ....
         * from_y a1aa ->    ....     ....     ....
         *    |   a2aa       ....     ....     ....
         *    |   a2aa  to_y a1aa     a1aa     a1aa
         *    h
         *    e   a3aa       ....     a2aa     a2aa
         *    i   b1bb ->    ....     a2aa     a2aa
         *    g   b2bb       ....     ....     a3aa
         *    h   b2bb       b1bb     b1bb     b1bb
         *    t
         *    |   b3bb       ....     b2bb     b2bb
         *    |   c1cc ->    ....     b2bb     b2bb
         *    |__ c2cc       ....     ....     b3bb
         *        ....       c1cc     c1cc     c1cc
         *
         *        ....       ....     c2cc     c2cc
         *        ....       ....     ....     ....
         */
        u32 buffer_size = ((from_y + height + 3) / 4) * tiles_per_row * 32;
        void *temp_buffer = malloc(buffer_size);
        memcpy(temp_buffer, texels, buffer_size);

        int copied_pixel_rows = 0;

        /* Phase 1 */
        int tile_to = to_y / 4;
        int tile_from = from_y / 4;
        int row_to = to_y % 4;
        int row_from = from_y % 4;
        int num_pixel_rows = MIN(4 - row_from, 4 - row_to);
        int num_tile_rows = (height + 3) / 4;
        tile_row_move_sub(texels, temp_buffer, tile_from, tile_to, num_tile_rows,
                          row_from, row_to, num_pixel_rows, tiles_per_row);
        copied_pixel_rows += num_pixel_rows;

        /* Phase 2 */
        int prev_row_from = row_from;
        int prev_num_pixel_rows = num_pixel_rows;
        if ((row_from + prev_num_pixel_rows) % 4 == 0) {
            /* In phase 1 we completed the source tile */
            tile_from++;
            row_from = 0;
            row_to += prev_num_pixel_rows;
            num_pixel_rows = 4 - row_to;
        } else {
            tile_to++;
            /* tile_from stays the same */
            row_to = 0;
            row_from += prev_num_pixel_rows;
            num_pixel_rows = 4 - (prev_row_from + prev_num_pixel_rows);
        }
        num_tile_rows = (height - prev_num_pixel_rows + 3) / 4;
        tile_row_move_sub(texels, temp_buffer, tile_from, tile_to, num_tile_rows,
                          row_from, row_to, num_pixel_rows, tiles_per_row);
        copied_pixel_rows += num_pixel_rows;

        /* Phase 3 */
        prev_row_from = row_from;
        prev_num_pixel_rows = num_pixel_rows;
        /* tile_to stays the same */
        tile_from += 1;
        row_to += prev_num_pixel_rows;
        row_from = 0;
        num_pixel_rows = 4 - copied_pixel_rows;
        num_tile_rows = (height + from_y % 4 - copied_pixel_rows) / 4;
        tile_row_move_sub(texels, temp_buffer, tile_from, tile_to, num_tile_rows,
                          row_from, row_to, num_pixel_rows, tiles_per_row);

        memcpy(texels, temp_buffer, buffer_size);
        free(temp_buffer);
    }
}

static void console_clear_tile_subrect(GxConsole *con, u16 *tile0, u16 *tile1, int tile_step,
                                       int tx0, int ty0, int tx1, int ty1, u16 color)
{
    int start_x = tx0;
    int end_x = tx1;
    int start_y = ty0;
    int end_y = ty1;
    if (tile_step == 1) {
        /* we are clearing a row */
        end_x = 3;
    } else {
        end_y = 3;
    }
    for (u16 *tile = tile0; tile <= tile1; tile += (tile_step * 16)) {
        if (tile == tile1) {
            if (tile_step == 1) {
                /* we are clearing a row */
                end_x = tx1;
            } else {
                end_y = ty1;
            }
        }
        for (int x = start_x; x <= end_x; x++) {
            for (int y = start_y; y <= end_y; y++) {
                tile[y * 4 + x] = color;
            }
        }
        if (tile == tile0) {
            if (tile_step == 1) {
                /* we are clearing a row */
                start_x = 0;
            } else {
                start_y = 0;
            }
        }
    }
}

static void console_clear_rect(GxConsole *con, int line0, int line1, int col0, int col1)
{
    u16 bg = console_color(con->bg);
    int tx0, ty0, tx1, ty1, unused;
    u16 *tile_tl = console_get_char_tile_tl(con, col0, line0, &tx0, &ty0);
    u16 *tile_tr = console_get_char_tile_tr(con, col1, line0, &unused, &unused);
    u16 *tile_bl = console_get_char_tile_bl(con, col0, line1, &unused, &unused);
    u16 *tile_br = console_get_char_tile_br(con, col1, line1, &tx1, &ty1);

    u16 *tile_full_tl = tile_tl;
    u16 *tile_full_tr = tile_tr;
    u16 *tile_full_bl = tile_bl;
    u16 *tile_full_br = tile_br;

    int tile_step_x = 16;
    int tile_step_y = 16 * con->tiles_per_row;

    if (tx0 != 0) {
        console_clear_tile_subrect(con, tile_tl, tile_bl, con->tiles_per_row,
                                   tx0, ty0, 3, ty1, bg);
        tile_full_tl += tile_step_x;
        tile_full_bl += tile_step_x;
    }
    if (tx1 != 3) {
        console_clear_tile_subrect(con, tile_tr, tile_br, con->tiles_per_row,
                                   0, ty0, tx1, ty1, bg);
        tile_full_tr -= tile_step_x;
        tile_full_br -= tile_step_x;
    }
    if (ty0 != 0) {
        console_clear_tile_subrect(con, tile_tl, tile_tr, 1,
                                   tx0, ty0, tx1, 3, bg);
        tile_full_tl += tile_step_y;
        tile_full_tr += tile_step_y;
    }
    if (ty1 != 3) {
        console_clear_tile_subrect(con, tile_bl, tile_br, 1,
                                   tx0, 0, tx1, ty1, bg);
        tile_full_bl -= tile_step_y;
        tile_full_br -= tile_step_y;
    }

    /* Clear the rect made with all full files */
    int width_in_tiles = (tile_full_tr + 16 - tile_full_tl) / tile_step_x;
    if (width_in_tiles <= 0) return;

    for (u16 *tile_y = tile_full_tl; tile_y <= tile_full_bl; tile_y += tile_step_y) {
        for (int t = 0; t < width_in_tiles * 16; t++) {
            tile_y[t] = bg;
        }
    }
}

static void console_clear_line(GxConsole *con, int line, int from, int to)
{
    if (from < 1) from = 1;
    if (to > con->width_chars) to = con->width_chars;
    if (line < 1) line = 1;
    if (line > con->height_chars) line = con->height_chars;
    console_clear_rect(con, line - 1, line - 1, from - 1, to - 1);
}

static void console_clear_line_cmd(GxConsole *con, int mode) {

    switch (mode) {
    case 0:     /* cursor to end of line */
        console_clear_line(con, con->cursor_y, con->cursor_x, con->width_chars + 1);
        break;
    case 1:     /* beginning of line to cursor */
        console_clear_line(con, con->cursor_y, 1, con->cursor_x);
        break;
    case 2:     /* entire line */
        console_clear_line(con, con->cursor_y, 1, con->width_chars + 1);
        break;
    }
}

static void console_clear(GxConsole *con)
{
    u16 width = con->width_chars * con->char_w;
    u16 height = con->height_chars * con->char_h;
    u16 pitch = con->pitch;
    u16 bg = console_color(con->bg);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            *(u16 *)&con->texels[y * pitch + x * 2] = bg;
        }
    }

    con->cursor_x = 1;
    con->cursor_y = 1;
    con->prev_cursor_x = 1;
    con->prev_cursor_y = 1;
}

static void console_clear_from_cursor(GxConsole *con) {
    int cur_row = con->cursor_y;

    console_clear_line(con, cur_row, con->cursor_x, con->width_chars + 1);

    while (cur_row++ < con->height_chars)
        console_clear_line(con, cur_row, 1, con->width_chars + 1);
}

static void console_clear_to_cursor(GxConsole *con) {
    int cur_row = con->cursor_y;

    console_clear_line(con, cur_row, 1, con->cursor_x);

    while (--cur_row)
        console_clear_line(con, cur_row, 1, con->width_chars + 1);
}

static void console_cls(GxConsole *con, int mode) {

    switch (mode) {
    case 0:
        console_clear_from_cursor(con);
        break;
    case 1:
        console_clear_to_cursor(con);
        break;
    case 2:
    case 3:
        console_clear(con);
        break;
    }
}

static inline void console_position(GxConsole *con, int x, int y) {
    /* invalid position */
    if (x < 0 || y < 0)
        return;

    /* 1-based, but we'll take a 0 */
    if (x < 1)
        x = 1;
    if (y < 1)
        y = 1;

    /* clip to console edge */
    if (x > con->width_chars)
        x = con->width_chars;
    if (y > con->height_chars)
        y = con->height_chars;

    con->cursor_x = x;
    con->cursor_y = y;
}

static void console_set_color_state(GxConsole *con, int code)
{
    EscapeSeq *esc = &con->esc;
    switch (code)
    {
    case 0:     /* reset */
        esc->color.flags = 0;
        esc->color.bg    = 0;
        esc->color.fg    = 7;
        break;

    case 1:     /* bold */
        esc->color.flags &= ~CONSOLE_COLOR_FAINT;
        esc->color.flags |= CONSOLE_COLOR_BOLD;
        break;

    case 2:     /* faint */
        esc->color.flags &= ~CONSOLE_COLOR_BOLD;
        esc->color.flags |= CONSOLE_COLOR_FAINT;
        break;

    case 3:     /* italic */
        esc->color.flags |= CONSOLE_ITALIC;
        break;

    case 4:     /* underline */
        esc->color.flags |= CONSOLE_UNDERLINE;
        break;
    case 5:     /* blink slow */
        esc->color.flags &= ~CONSOLE_BLINK_FAST;
        esc->color.flags |= CONSOLE_BLINK_SLOW;
        break;
    case 6:     /* blink fast */
        esc->color.flags &= ~CONSOLE_BLINK_SLOW;
        esc->color.flags |= CONSOLE_BLINK_FAST;
        break;
    case 7:     /* reverse video */
        esc->color.flags |= CONSOLE_COLOR_REVERSE;
        break;
    case 8:     /* conceal */
        esc->color.flags |= CONSOLE_CONCEAL;
        break;
    case 9:     /* crossed-out */
        esc->color.flags |= CONSOLE_CROSSED_OUT;
        break;
    case 21:     /* bold off */
        esc->color.flags &= ~CONSOLE_COLOR_BOLD;
        break;

    case 22:     /* normal color */
        esc->color.flags &= ~CONSOLE_COLOR_BOLD;
        esc->color.flags &= ~CONSOLE_COLOR_FAINT;
        break;

    case 23:     /* italic off */
        esc->color.flags &= ~CONSOLE_ITALIC;
        break;

    case 24:     /* underline off */
        esc->color.flags &= ~CONSOLE_UNDERLINE;
        break;

    case 25:     /* blink off */
        esc->color.flags &= ~CONSOLE_BLINK_SLOW;
        esc->color.flags &= ~CONSOLE_BLINK_FAST;
        break;

    case 27:     /* reverse off */
        esc->color.flags &= ~CONSOLE_COLOR_REVERSE;
        break;

    case 29:     /* crossed-out off */
        esc->color.flags &= ~CONSOLE_CROSSED_OUT;
        break;

    case 30 ... 37:     /* writing color */
        esc->color.flags &= ~CONSOLE_FG_CUSTOM;
        esc->color.fg     = code - 30;
        break;

    case 38:     /* custom foreground color */
        esc->state = ESC_BUILDING_FORMAT_FG;
        esc->color_arg_count = 0;
        break;

    case 39:     /* reset foreground color */
        esc->color.flags &= ~CONSOLE_FG_CUSTOM;
        esc->color.fg     = 7;
        break;
    case 40 ... 47:     /* screen color */
        esc->color.flags &= ~CONSOLE_BG_CUSTOM;
        esc->color.bg = code - 40;
        break;
    case 48:     /* custom background color */
        esc->state = ESC_BUILDING_FORMAT_BG;
        esc->color_arg_count = 0;
        break;
    case 49:     /* reset background color */
        esc->color.flags &= ~CONSOLE_BG_CUSTOM;
        esc->color.bg = 0;
        break;
    case 90 ... 97:     /* bright foreground */
        esc->color.flags &= ~CONSOLE_COLOR_FAINT;
        esc->color.flags |= CONSOLE_COLOR_FG_BRIGHT;
        esc->color.flags &= ~CONSOLE_BG_CUSTOM;
        esc->color.fg = code - 90;
        break;
    case 100 ... 107:     /* bright background */
        esc->color.flags &= ~CONSOLE_COLOR_FAINT;
        esc->color.flags |= CONSOLE_COLOR_BG_BRIGHT;
        esc->color.flags &= ~CONSOLE_BG_CUSTOM;
        esc->color.bg = code - 100;
        break;
    }
}

static void console_handle_color_esc(GxConsole *con, int arg_count)
{
    EscapeSeq *esc = &con->esc;
    esc->color.bg = con->bg;
    esc->color.fg = con->fg;
    esc->color.flags = con->flags;

    for (int arg = 0; arg < arg_count; arg++)
    {
        int code = esc->args[arg];
        switch (esc->state)
        {
        case ESC_BUILDING_UNKNOWN:
            console_set_color_state(con, code);
            break;
        case ESC_BUILDING_FORMAT_FG:
            if (code == 5)
                esc->state = ESC_BUILDING_FORMAT_FG_NONRGB;
            else if (code == 2)
                esc->state = ESC_BUILDING_FORMAT_FG_RGB;
            else
                esc->state = ESC_BUILDING_UNKNOWN;
            break;
        case ESC_BUILDING_FORMAT_BG:
            if (code == 5)
                esc->state = ESC_BUILDING_FORMAT_BG_NONRGB;
            else if (code == 2)
                esc->state = ESC_BUILDING_FORMAT_BG_RGB;
            else
                esc->state = ESC_BUILDING_UNKNOWN;
            break;
        case ESC_BUILDING_FORMAT_FG_NONRGB:
            if (code <= 15) {
                esc->color.fg  = code;
                esc->color.flags &= ~CONSOLE_FG_CUSTOM;
            } else if (code <= 231) {
                code -= 16;
                unsigned int r = code / 36;
                unsigned int g = (code - r * 36) / 6;
                unsigned int b = code - r * 36 - g * 6;

                esc->color.fg  = rgb_to_rgb16(s_color_cube[r], s_color_cube[g], s_color_cube[b]);
                esc->color.flags |= CONSOLE_FG_CUSTOM;
            } else if (code <= 255) {
                code -= 232;

                esc->color.fg  = rgb_to_rgb16(s_gray_scale[code], s_gray_scale[code], s_gray_scale[code]);
                esc->color.flags |= CONSOLE_FG_CUSTOM;
            }
            esc->state = ESC_BUILDING_UNKNOWN;
            break;
        case ESC_BUILDING_FORMAT_BG_NONRGB:
            if (code <= 15) {
                esc->color.bg  = code;
                esc->color.flags &= ~CONSOLE_BG_CUSTOM;
            } else if (code <= 231) {
                code -= 16;
                unsigned int r = code / 36;
                unsigned int g = (code - r * 36) / 6;
                unsigned int b = code - r * 36 - g * 6;

                esc->color.bg  = rgb_to_rgb16(s_color_cube[r], s_color_cube[g], s_color_cube[b]);
                esc->color.flags |= CONSOLE_BG_CUSTOM;
            } else if (code <= 255) {
                code -= 232;

                esc->color.bg  = rgb_to_rgb16(s_gray_scale[code], s_gray_scale[code], s_gray_scale[code]);
                esc->color.flags |= CONSOLE_BG_CUSTOM;
            }
            esc->state = ESC_BUILDING_UNKNOWN;
            break;
        case ESC_BUILDING_FORMAT_FG_RGB:
            esc->color_args[esc->color_arg_count++] = code;
            if (esc->color_arg_count == 3) {
                esc->color.fg = rgb_to_rgb16(esc->color_args[0], esc->color_args[1], esc->color_args[2]);
                esc->color.flags |= CONSOLE_FG_CUSTOM;
                esc->state = ESC_BUILDING_UNKNOWN;
            }
            break;
        case ESC_BUILDING_FORMAT_BG_RGB:
            esc->color_args[esc->color_arg_count++] = code;
            if (esc->color_arg_count == 3) {
                esc->color.bg = rgb_to_rgb16(esc->color_args[0], esc->color_args[1], esc->color_args[2]);
                esc->color.flags |= CONSOLE_BG_CUSTOM;
                esc->state = ESC_BUILDING_UNKNOWN;
            }
        default:
            break;
        }
    }
    esc->arg_idx = 0;

    con->bg = esc->color.bg;
    con->fg = esc->color.fg;
    con->flags = esc->color.flags;
}

static void new_row(GxConsole *con)
{
    con->cursor_y++;
    /* if bottom border reached, scroll */
    if (con->cursor_y > con->height_chars) {
        /* scrolling copies all rows except the very first one (that gets overwritten) */
        const u32 scroll_height = (con->height_chars - 1) * con->char_h;
        tile_row_move(con->texels, con->char_h, 0, scroll_height, con->tiles_per_row);

        /* clear last line */
        console_clear_line(con, con->height_chars, 1, con->width_chars + 1);
        con->cursor_y = con->height_chars;
    }
}

static void console_print_char(GxConsole *con, int c)
{
    int tabspaces;

    if (c == 0) return;

    switch (c) {
    /*
	The only special characters we will handle are tab (\t), carriage return (\r), line feed (\n)
	and backspace (\b).
	Carriage return & line feed will function the same: go to next line and put cursor at the beginning.
	For everything else, use VT sequences.

	Reason: VT sequences are more specific to the task of cursor placement.
	The special escape sequences \b \f & \v are archaic and non-portable.
	*/
    case 8:
        con->cursor_x--;

        if (con->cursor_x < 1) {
            if (con->cursor_y > 1) {
                con->cursor_x = con->width_chars;
                con->cursor_y--;
            } else {
                con->cursor_x = 1;
            }
        }

        console_drawc(con, ' ');
        break;

    case 9:
        tabspaces = con->tab_size - ((con->cursor_x - 1) % con->tab_size);
        if (con->cursor_x + tabspaces > con->width_chars)
            tabspaces = con->width_chars - con->cursor_x;
        for (int i = 0; i < tabspaces; i++) console_print_char(con, ' ');
        break;
    case 10:
        new_row(con);
    case 13:
        con->cursor_x = 1;
        break;
    default:
        if (con->cursor_x > con->width_chars) {
            con->cursor_x = 1;
            new_row(con);
        }
        console_drawc(con, c);
        ++con->cursor_x;
        break;
    }
}

static ssize_t console_write(struct _reent *r, GxConsole *con, const char *ptr, size_t len)
{
    if (!con) return -1;

    char chr;

    int i, count = 0;
    char *tmp = (char *)ptr;
    EscapeSeq *esc = &con->esc;

    i = 0;

    if (!tmp || len <= 0) return -1;

    while (i < len) {

        chr = *(tmp++);
        i++; count++;

        switch (esc->state)
        {
        case ESC_NONE:
            if (chr == 0x1b)
                esc->state = ESC_START;
            else
                console_print_char(con, chr);
            break;
        case ESC_START:
            if (chr == '[') {
                esc->state = ESC_BUILDING_UNKNOWN;
                esc->has_arg = false;
                memset(esc->args, 0, sizeof(esc->args));
                esc->color.bg = con->bg;
                esc->color.fg = con->fg;
                esc->color.flags = con->flags;
                esc->arg_idx = 0;
            } else   {
                console_print_char(con, 0x1b);
                console_print_char(con, chr);
                esc->state = ESC_NONE;
            }
            break;
        case ESC_BUILDING_UNKNOWN:
            switch (chr)
            {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                esc->has_arg = true;
                esc->args[esc->arg_idx] = esc->args[esc->arg_idx] * 10 + (chr - '0');
                break;
            case ';':
                if (esc->has_arg) {
                    if (esc->arg_idx < _ANSI_MAXARGS) {
                        esc->arg_idx++;
                    }
                }
                esc->has_arg = false;
                break;
            /*---------------------------------------			// Cursor directional movement */
            /*--------------------------------------- */
            case 'A':
                if (!esc->has_arg && !esc->arg_idx)
                    esc->args[0] = 1;
                con->cursor_y  =  con->cursor_y - esc->args[0];
                if (con->cursor_y < 1)
                    con->cursor_y = 1;
                esc->state = ESC_NONE;
                break;
            case 'B':
                if (!esc->has_arg && !esc->arg_idx)
                    esc->args[0] = 1;
                con->cursor_y  =  con->cursor_y + esc->args[0];
                if (con->cursor_y > con->height_chars)
                    con->cursor_y = con->height_chars;
                esc->state = ESC_NONE;
                break;
            case 'C':
                if (!esc->has_arg && !esc->arg_idx)
                    esc->args[0] = 1;
                con->cursor_x  =  con->cursor_x  + esc->args[0];
                if (con->cursor_x > con->width_chars)
                    con->cursor_x = con->width_chars;
                esc->state = ESC_NONE;
                break;
            case 'D':
                if (!esc->has_arg && !esc->arg_idx)
                    esc->args[0] = 1;
                con->cursor_x  =  con->cursor_x  - esc->args[0];
                if (con->cursor_x < 1)
                    con->cursor_x = 1;
                esc->state = ESC_NONE;
                break;
            case 'G':
                con->cursor_x = esc->args[0];
                esc->state = ESC_NONE;
                break;
            /*--------------------------------------- */
            /* Cursor position movement */
            /*--------------------------------------- */
            case 'H':
            case 'f':
                console_position(con, esc->args[1], esc->args[0]);
                esc->state = ESC_NONE;
                break;
            /*--------------------------------------- */
            /* Screen clear */
            /*--------------------------------------- */
            case 'J':
                if (esc->arg_idx == 0 && !esc->has_arg) {
                    esc->args[0] = 0;
                }
                console_cls(con, esc->args[0]);
                esc->state = ESC_NONE;
                break;
            /*--------------------------------------- */
            /* Line clear */
            /*--------------------------------------- */
            case 'K':
                if (esc->arg_idx == 0 && !esc->has_arg) {
                    esc->args[0] = 0;
                }
                console_clear_line_cmd(con, esc->args[0]);
                esc->state = ESC_NONE;
                break;
            /*--------------------------------------- */
            /* Save cursor position */
            /*--------------------------------------- */
            case 's':
                con->prev_cursor_x = con->cursor_x;
                con->prev_cursor_y = con->cursor_y;
                esc->state = ESC_NONE;
                break;
            /*--------------------------------------- */
            /* Load cursor position */
            /*--------------------------------------- */
            case 'u':
                con->cursor_x = con->prev_cursor_x;
                con->cursor_y = con->prev_cursor_y;
                esc->state = ESC_NONE;
                break;
            /*--------------------------------------- */
            /* Color scan codes */
            /*--------------------------------------- */
            case 'm':
                if (esc->arg_idx == 0 && !esc->has_arg) esc->args[esc->arg_idx++] = 0;
                if (esc->has_arg) esc->arg_idx++;
                console_handle_color_esc(con, esc->arg_idx);
                esc->state = ESC_NONE;
                break;
            default:
                /* some sort of unsupported escape; just gloss over it */
                esc->state = ESC_NONE;
                break;
            }
        default:
            break;
        }
    }
    /* TODO: only store changed areas, and invalidate if changed */
    DCStoreRange(con->texels, con->texture_size);
    GX_InvalidateTexAll();

    return count;
}

static ssize_t stdout_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    return console_write(r, s_console_stdout, ptr, len);
}

static ssize_t stderr_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    return console_write(r, s_console_stderr, ptr, len);
}

GxConsole *gx_console_new(u16 width_chars, u16 height_chars,
                          const GxConsoleFont *font)
{
    u8 char_width = font->char_width;
    u8 char_height = font->char_height;
    u16 width = width_chars * char_width;
    u16 height = height_chars * char_height;
    u32 buffer_size = GX_GetTexBufferSize(width, height,
                                          GX_TF_RGB565, GX_FALSE, GX_FALSE);
    u32 total_size = sizeof(GxConsole) + buffer_size;
    GxConsole *con = memalign(32, total_size);
    memset(con, 0, total_size);
    GX_InitTexObj(&con->texobj, con->texels, width, height, GX_TF_RGB565,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);

    con->width_chars = width_chars;
    con->height_chars = height_chars;
    con->char_w = char_width;
    con->char_h = char_height;
    con->tiles_per_row = (width + 3) / 4;
    con->pitch = 2 * 4 * ((width + 3) / 4);
    con->font = font;
    con->fg = CONSOLE_COLOR_WHITE;
    con->bg = CONSOLE_COLOR_BLACK;
    con->tab_size = 3;
    con->alpha = 255;
    con->texture_size = buffer_size;
    con->cursor_x = con->cursor_y = 1;
    con->prev_cursor_x = con->prev_cursor_y = 1;

    con->dotab.name = "gx_console";
    con->dotab.deviceData = con;

    DCFlushRange(con->texels, buffer_size);
    return con;
}

void gx_console_get_cursor_pos(GxConsole *con, int *row, int *column)
{
    *row = con->cursor_y;
    *column = con->cursor_x;
}

void gx_console_set_input(GxConsole *con, int fd)
{
    if (fd == STD_OUT) {
        s_console_stdout = con;
        con->dotab.write_r = stdout_write;
    } else if (fd == STD_ERR) {
        s_console_stderr = con;
        con->dotab.write_r = stderr_write;
    }
    devoptab_list[fd] = &con->dotab;
}

void gx_console_set_alpha(GxConsole *con, u8 alpha)
{
    con->alpha = alpha;
}

void gx_console_draw(GxConsole *con, int x, int y)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0);

    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_LoadTexObj(&con->texobj, GX_TEXMAP0);
    GXColor color = {0, 0, 0, con->alpha};
    GX_SetTevColor(GX_TEVREG0, color);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_OR);

    u16 width = GX_GetTexObjWidth(&con->texobj);
    u16 height = GX_GetTexObjHeight(&con->texobj);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(x, y);
    GX_TexCoord2s16(0, 0);
    GX_Position2s16(x + width, y);
    GX_TexCoord2s16(1, 0);
    GX_Position2s16(x + width, y + height);
    GX_TexCoord2s16(1, 1);
    GX_Position2s16(x, y + height);
    GX_TexCoord2s16(0, 1);
    GX_End();
}

GXTexObj *gx_console_get_texobj(GxConsole *con)
{
    return &con->texobj;
}

const GxConsoleFont *gx_console_font_default()
{
    const static GxConsoleFont font = {
        8, 16,
        1,
        0, 256,
        console_font_8x16,
    };
    return &font;
}
