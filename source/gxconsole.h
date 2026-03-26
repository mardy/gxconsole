#ifndef GX_CONSOLE_H
#define GX_CONSOLE_H

#include <gccore.h>

typedef struct gx_console_t GxConsole;

typedef struct gx_console_font_t {
    u8 char_width;
    u8 char_height;
    u8 bpp;
    u8 ascii_offset;
    u16 num_chars;
    const u8 *pixels;
} GxConsoleFont;

GxConsole *gx_console_new(u16 width_chars, u16 height_chars,
                          const GxConsoleFont *font);
void gx_console_get_cursor_pos(GxConsole *console, int *row, int *column);
void gx_console_set_input(GxConsole *console, int fd);
void gx_console_set_alpha(GxConsole *console, u8 alpha);
void gx_console_draw(GxConsole *console, int x, int y);
GXTexObj *gx_console_get_texobj(GxConsole *console);

const GxConsoleFont *gx_console_font_default();

extern const GxConsoleFont font_tamzen_8x15;
extern const GxConsoleFont font_cozette_6x13;

#endif /* GX_CONSOLE_H */
