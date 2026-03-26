#include "gxconsole.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

#define FIFO_SIZE       (256*1024)

extern const GxConsoleFont font_tamzen_8x15;
extern const GxConsoleFont font_cozette_6x13;

static void print_line_h(char ch, int count)
{
    for (int i = 0; i < count; i++)
        printf("%c", ch);
}

static void print_line_v(char ch, int count)
{
    for (int i = 0; i < count; i++) {
        printf("%c", ch);
        if (i != count - 1) {
            /* Move down */
            printf("\x1b[1D\x1b[1B");
        }
    }
}

static void set_pos(int row, int column)
{
    printf("\x1b[%d;%dH", row, column);
}

static void move_left(int n) {
    printf("\x1b[%dD", n);
}

void draw_triangle(Mtx viewMatrix)
{
    Mtx modelView;
    static s16 vertices[] ATTRIBUTE_ALIGN(32) = {
        0, 15, 0,
        -15, -15, 0,
        15, -15, 0
    };

    static u8 colors[] ATTRIBUTE_ALIGN(32)     = {
        255, 0, 0, 255,         /* red */
        0, 255, 0, 255,         /* green */
        0, 0, 255, 255
    };                          /* blue */

    static float angle = 0.0;

    angle += 0.01;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetArray(GX_VA_POS, vertices, 3 * sizeof(s16));
    GX_SetArray(GX_VA_CLR0, colors, 4 * sizeof(u8));
    GX_SetNumChans(1);
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    guMtxIdentity(modelView);
    guMtxRotRad(modelView, 'y', angle);
    guMtxTransApply(modelView, modelView, 0.0F, 0.0F, -50.0F);
    guMtxConcat(viewMatrix, modelView, modelView);

    GX_LoadPosMtxImm(modelView, GX_PNMTX0);

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);

    GX_Position1x8(0);
    GX_Color1x8(0);
    GX_Position1x8(1);
    GX_Color1x8(1);
    GX_Position1x8(2);
    GX_Color1x8(2);

    GX_End();
}

int main(int argc, char **argv)
{
    /* Typical GX setup */
    VIDEO_Init();

    WPAD_Init();
    WPAD_SetDataFormat(0, WPAD_FMT_BTNS_ACC);

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    void *fifoBuffer = NULL;
    fifoBuffer = MEM_K0_TO_K1(memalign(32, FIFO_SIZE));
    memset(fifoBuffer, 0, FIFO_SIZE);

    GX_Init(fifoBuffer, FIFO_SIZE);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    float yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);

    Mtx44 console_proj;
    guOrtho(console_proj, 0, rmode->efbHeight, 0, rmode->fbWidth, 0, 1);
    GX_LoadProjectionMtx(console_proj, GX_ORTHOGRAPHIC);

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    /* Create the console for stdout */
    const GxConsoleFont *font = gx_console_font_default();
    int con_width = 80;
    GxConsole *con_stdout = gx_console_new(con_width, rmode->efbHeight / 16, font);
    gx_console_set_input(con_stdout, 1);

    /* Create the overlay console for stderr */
    GxConsole *con_stderr = gx_console_new(60, 20, &font_cozette_6x13);
    gx_console_set_input(con_stderr, 2);
    gx_console_set_alpha(con_stderr, 128);


    /* The console understands VT terminal escape codes */
    set_pos(2, 0);
#define BRIGHT_WHITE_ON_BLUE "\x1b[1;37;44m"
    printf(BRIGHT_WHITE_ON_BLUE);
    printf("\xc9");
    print_line_h('\xcd', con_width - 2);
    printf("\xbb");
    printf("\n\x1b[2K"); /* Erase line */
    printf("\xba");
    printf("\x1b[32C\x1b[1;33mGxConsole example");
    printf(BRIGHT_WHITE_ON_BLUE "\x1b[%dG\xba", con_width);
    printf("\n\xcc");
    print_line_h('\xcd', 78);
    printf("\xb9");
    int height = 24;
    print_line_v('\xba', height);
    printf("\n\xc8");
    print_line_h('\xcd', con_width - 2);
    printf("\xbc");
    /* Return up, move to the right border */
    printf("\x1b[%dA" "\x1b[%dG", height, con_width);
    print_line_v('\xba', height);

    int content_row = 6;
    int content_col = 4;
    set_pos(content_col + 15, content_col);
#define COMMENT_STYLE "\x1b[0;1;42m"
    printf(COMMENT_STYLE "This example show how you can draw a console as your main GUI, complete ");
    set_pos(content_col + 16, content_col);
    printf("with " "\x1b[1;31mc" "\x1b[32mo" "\x1b[33ml" "\x1b[34mo" "\x1b[35mr" "\x1b[36ms, "
           COMMENT_STYLE "\x1b[37;9mdifferent " COMMENT_STYLE "\x1b[4mstyles"
           COMMENT_STYLE ", and how it can be integrated with 3D ");
    set_pos(content_col + 17, content_col);
    printf(COMMENT_STYLE "graphics and another console (for printing the stderr, for example).");
    set_pos(content_col + 18, content_col);
    printf(COMMENT_STYLE "Note how the two consoles can use different fonts.");
    fflush(stdout);

    printf("\x1b[0m");

    Mtx view;
    Mtx44 projection;
    guVector camera =       {0.0F, 0.0F, 0.0F};
    guVector up =   {0.0F, 1.0F, 0.0F};
    guVector look   = {0.0F, 0.0F, -1.0F};
    guLookAt(view, &camera, &up, &look);
    guPerspective(projection, 60, (CONF_GetAspectRatio() == CONF_ASPECT_16_9) ? 16.0F / 9.0F : 4.0F / 3.0F, 10.0F, 300.0F);
    GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);

    char letter = 'a';
    while (SYS_MainLoop()) {

        /* Call WPAD_ScanPads each loop, this reads the latest controller states */
        WPAD_ScanPads();

        /* WPAD_ButtonsDown tells us which buttons were pressed in this loop */
        /* this is a "one shot" state which will not fire again until the button has been released */
        u32 pressed = WPAD_ButtonsDown(0);

        /* We return to the launcher application via exit */
        if (pressed & WPAD_BUTTON_HOME) exit(0);

        u32 buttons = WPAD_ButtonsDown(0) | WPAD_ButtonsHeld(0);
        struct vec3w_t accel;
        WPAD_Accel(0, &accel);

#define STYLE_LABEL "\x1b[0;1;37m"
#define STYLE_VALUE "\x1b[0;37m"
        set_pos(content_row, content_col);
        printf(STYLE_LABEL "Buttons:" STYLE_VALUE);
        print_line_h(' ', 20);
        move_left(20);
        if (buttons & WPAD_BUTTON_A) printf(" A");
        if (buttons & WPAD_BUTTON_B) printf(" B");
        if (buttons & WPAD_BUTTON_1) printf(" 1");
        if (buttons & WPAD_BUTTON_2) printf(" 2");

        set_pos(content_row + 1, content_col);
        printf(STYLE_LABEL "D-pad:" STYLE_VALUE);
        print_line_h(' ', 20);
        move_left(20);
        if (buttons & WPAD_BUTTON_LEFT) printf(" Left");
        if (buttons & WPAD_BUTTON_UP) printf(" Up");
        if (buttons & WPAD_BUTTON_RIGHT) printf(" Right");
        if (buttons & WPAD_BUTTON_DOWN) printf(" Down");

        static const char axis[] = "XYZ";
        uword values[3] = { accel.x, accel.y, accel.z };
        for (int i = 0; i < 3; i++) {
            set_pos(content_row + 3 + i, content_col);
            printf(STYLE_LABEL "Accel %c: " STYLE_VALUE, axis[i]);
            int slots = values[i] / 20;
            if (slots < 0) slots = 0;
            else if (slots > 60) slots = 60;
            print_line_h('\xdb', slots);
            print_line_h(' ', 60 - slots);
        }
        fflush(stdout);

        fprintf(stderr, "Acceleration %d %d %d    \n", accel.x, accel.y, accel.z);
        fprintf(stderr, "Buttons %08x\n", buttons);

        /* Draw the main console */
        GX_LoadProjectionMtx(console_proj, GX_ORTHOGRAPHIC);
        GX_SetCurrentMtx(GX_IDENTITY);
        gx_console_draw(con_stdout, 0, 0);

        /* Draw the triangle */
        GX_LoadProjectionMtx(projection, GX_PERSPECTIVE);
        GX_SetCurrentMtx(GX_PNMTX0);
        draw_triangle(view);

        /* Draw the stderr console */
        GX_LoadProjectionMtx(console_proj, GX_ORTHOGRAPHIC);
        GX_SetCurrentMtx(GX_IDENTITY);
        gx_console_draw(con_stderr, 300, 10);

        GX_DrawDone();
        GX_CopyDisp(xfb, GX_TRUE);

        VIDEO_WaitVSync();
    }

    return 0;
}
