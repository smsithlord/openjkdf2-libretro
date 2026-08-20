/*
 * OpenJKDF2 libretro core.
 *
 * Drives the engine as a hardware-render (OpenGL 3.3 core) libretro core.
 * See DESIGN.md at the repo root for the full lifecycle/audio/input design.
 *
 * Shape of the port (all verified against the vendored engine):
 *  - One retro_run() == one Window_Main_Loop() == one engine frame
 *    (jkMain_GuiAdvance + WM_PAINT render/present, src/Win95/Window.c).
 *  - Engine startup is DEFERRED to the first retro_run() because Main_Startup
 *    initializes GL (std3D) and a HW-render core may only touch GL inside
 *    retro_run()/context_reset().
 *  - The engine is compiled in its stock desktop SDL configuration but the core
 *    never creates an SDL window/GL context and (for now) never calls SDL_Init;
 *    the engine's SDL calls degrade to safe no-ops without them. Input is
 *    injected through the same entry points the SDL event pump uses.
 *  - std3D composites its final image into std3D_windowFbo, which the core
 *    points at hw_render.get_current_framebuffer() every frame.
 *  - The engine runs on its own FIBER. Its menu system is modal (every menu
 *    blocks in jkGuiRend_DisplayAndReturnClicked pumping Window_MessageLoop
 *    until a click), which would never return from retro_run. Instead,
 *    retro_run resumes the engine fiber, and the engine yields back once per
 *    frame -- either at the Window_Main_Loop frame boundary or from inside
 *    Window_MessageLoop for modal loops (LIBRETRO_BUILD patch). All fibers
 *    share one OS thread: no locking, and the GL context stays current.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef _WIN32
#include <direct.h>
#define core_chdir _chdir
#define core_getcwd _getcwd
#else
#include <unistd.h>
#define core_chdir chdir
#define core_getcwd getcwd
#endif

#include <GL/glew.h>

#include "libretro.h"

#include "Win95/Window.h"      /* pulls types.h + generated globals.h (g_hWnd, g_should_exit, ...) */
#include "Main/Main.h"
#include "Main/jkSession.h"
#include "World/jkPlayer.h"
#include "Platform/stdControl.h"
#include "stdPlatform.h"

/* Engine globals/functions that live in Window.c / main_globals.c without
 * header declarations. */
void Window_Main_Loop(void);
extern int Window_screenXSize;
extern int Window_screenYSize;
extern int Window_resized;
extern int Window_needsRecreate;
extern int Window_mouseX;
extern int Window_mouseY;
extern int Window_lastMouseX;
extern int Window_lastMouseY;
extern char openjkdf2_aOrigCwd[512];

/* std3D (Platform/GL/std3D.c) — LIBRETRO_BUILD helpers added there. */
extern void std3D_SetWindowFbo(GLint fbo);
extern void std3D_RebindVAO(void);
extern void std3D_FreeResources(void);
extern int libretro_std3D_HasGlResources(void);

/* jkGUIRend.c / stdSound.c — LIBRETRO_BUILD helpers added there. */
extern void libretro_ForcePopActiveMenu(void);
extern void libretro_ForceCloseAudioDevice(void);
extern int libretro_stdSound_RenderAudio(int16_t* pOut, int nFrames);
extern void libretro_stdMci_Pump(void); /* stdMci.c: music -> AL stream */

#ifndef OPENJKDF2_RELEASE_VERSION_STRING
#define OPENJKDF2_RELEASE_VERSION_STRING "unknown"
#endif

#define CORE_BASE_WIDTH   640
#define CORE_BASE_HEIGHT  480
#define CORE_MAX_WIDTH    1920
#define CORE_MAX_HEIGHT   1440
#define CORE_FPS          60.0
#define CORE_SAMPLE_RATE  48000.0
#define CORE_AUDIO_FRAMES 800 /* 48000 / 60 */

/* Cursor wedge: don't draw until the pointer has actually been used, and
 * auto-hide after this many frames without motion/button activity (~3s). */
#define CURSOR_IDLE_HIDE_FRAMES 180

typedef struct core_state_t
{
    retro_environment_t environ_cb;
    retro_video_refresh_t video_cb;
    retro_audio_sample_t audio_cb;
    retro_audio_sample_batch_t audio_batch_cb;
    retro_input_poll_t input_poll_cb;
    retro_input_state_t input_state_cb;
    struct retro_hw_render_callback hw_render;
    struct retro_log_callback log;

    bool game_loaded;
    bool context_alive;
    bool engine_started;
    bool engine_start_failed;

    /* Port 0 device selection (SET_CONTROLLER_INFO). Defaults to the retro
     * keyboard; frontends that never call set_controller_port_device get
     * keyboard+mouse behavior, which is also what RETRO_DEVICE_JOYPAD gets
     * until the M1 RetroPad mapping lands. */
    unsigned port0_device;

    char basefolder[1024];
    char episode_name[256]; /* ROM filename without extension (for later autostart) */
    char cmdline[512];
    bool is_mots;

    /* Core-owned absolute mouse position in window pixels. */
    int mouse_abs_x;
    int mouse_abs_y;
    int last_mouse_l;
    int last_mouse_r;

    /* Cursor auto-hide (always on): no wedge until the pointer is used, then
     * hide again after CURSOR_IDLE_HIDE_FRAMES without activity. */
    bool cursor_seen_motion;
    int cursor_idle_frames;

    /* Boot/resume core options (consumed at engine boot via jkSession). */
    int boot_mode;           /* JKSESSION_BOOT_* (INTRO plays the stock movie; every other mode skips it) */
    int direct_boot_filter;  /* JKSESSION_DIRECT_* — which episode types direct-boot */

    /* Deferred savestate restore: retro_unserialize before the engine is up
     * (the frontend's auto-load-state fires right after content load) parks a
     * copy here; retro_run retries it until the booted engine has a profile
     * and a quiet moment, or the retry budget runs out. */
    void*    pending_state;
    unsigned pending_state_len;
    int      pending_state_frames;

    int16_t audio_out[CORE_AUDIO_FRAMES * 2];
} core_state_t;

static core_state_t g_core;

/* Fiber machinery state (see the "Engine fiber" section below). */
static void* s_frontend_fiber;
static void* s_engine_fiber;
static bool s_engine_exit_requested;
static bool s_gl_ready; /* glewInit has run (on the engine fiber) AND the HW
                         * context is usable; GLEW function pointers are NULL
                         * before glewInit -- calling any gl* from retro_run
                         * earlier is a jump to address 0. */

/* SDL-scancode-indexed key state consumed by Platform/SDL2/stdControl.c through
 * libretro_GetKeyboardState() (the SDL_GetKeyboardState array never populates
 * without an SDL window). Indices are USB HID usages == SDL scancodes. */
static bool g_keyboard_state[256];

const bool* libretro_GetKeyboardState(void)
{
    return g_keyboard_state;
}

static void core_log(enum retro_log_level level, const char* fmt, ...)
{
    char buf[1024];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (g_core.log.log)
        g_core.log.log(level, "[openjkdf2] %s", buf);
    else
        fprintf(stderr, "[openjkdf2] %s", buf);
}

/* Engine print mirror (stdPlatform_Printf's LIBRETRO_BUILD hook; devdocs/07
 * §5): every engine subsystem's console output lands here, one formatted
 * chunk at a time. Forward to the frontend's log, with consecutive-duplicate
 * suppression (some engine states print the same line every frame) and an
 * optional OPENJKDF2_LOG=<path> file mirror, flushed per line so a crash
 * still leaves the tail that explains it. */
void libretro_EnginePrint(const char* line)
{
    static char s_last[256];
    static int s_repeats;
    static FILE* s_mirror;
    static bool s_mirror_tried;

    if (!line || !line[0])
        return;

    /* Normalize: strip trailing newline(s); skip ANSI console-control spam. */
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = 0;
    if (!len || buf[0] == '\x1b')
        return;

    if (!s_mirror_tried)
    {
        s_mirror_tried = true;
        const char* path = getenv("OPENJKDF2_LOG");
        if (path && path[0])
            s_mirror = fopen(path, "a");
    }
    if (s_mirror)
    {
        fputs(buf, s_mirror);
        fputc('\n', s_mirror);
        fflush(s_mirror);
    }

    if (strcmp(buf, s_last) == 0)
    {
        s_repeats++;
        return;
    }
    if (s_repeats > 0)
    {
        core_log(RETRO_LOG_INFO, "engine: (last line repeated %d more times)\n", s_repeats);
        s_repeats = 0;
    }
    strcpy(s_last, buf);
    core_log(RETRO_LOG_INFO, "engine: %s\n", buf);
}

/* ------------------------------------------------------------------------
 * Key mapping
 * ------------------------------------------------------------------------ */

/* retro_key -> SDL scancode (USB HID usage). Only keys the game uses. */
static int retro_key_to_sdl_scancode(unsigned k)
{
    if (k >= RETROK_a && k <= RETROK_z)
        return 4 + (k - RETROK_a); /* SDL_SCANCODE_A.. */
    if (k >= RETROK_1 && k <= RETROK_9)
        return 30 + (k - RETROK_1);
    if (k >= RETROK_F1 && k <= RETROK_F12)
        return 58 + (k - RETROK_F1);
    if (k >= RETROK_KP1 && k <= RETROK_KP9)
        return 89 + (k - RETROK_KP1);

    switch (k)
    {
    case RETROK_0:            return 39;
    case RETROK_RETURN:       return 40;
    case RETROK_ESCAPE:       return 41;
    case RETROK_BACKSPACE:    return 42;
    case RETROK_TAB:          return 43;
    case RETROK_SPACE:        return 44;
    case RETROK_MINUS:        return 45;
    case RETROK_EQUALS:       return 46;
    case RETROK_LEFTBRACKET:  return 47;
    case RETROK_RIGHTBRACKET: return 48;
    case RETROK_BACKSLASH:    return 49;
    case RETROK_SEMICOLON:    return 51;
    case RETROK_QUOTE:        return 52;
    case RETROK_BACKQUOTE:    return 53;
    case RETROK_COMMA:        return 54;
    case RETROK_PERIOD:       return 55;
    case RETROK_SLASH:        return 56;
    case RETROK_CAPSLOCK:     return 57;
    case RETROK_PRINT:        return 70;
    case RETROK_SCROLLOCK:    return 71;
    case RETROK_PAUSE:        return 72;
    case RETROK_INSERT:       return 73;
    case RETROK_HOME:         return 74;
    case RETROK_PAGEUP:       return 75;
    case RETROK_DELETE:       return 76;
    case RETROK_END:          return 77;
    case RETROK_PAGEDOWN:     return 78;
    case RETROK_RIGHT:        return 79;
    case RETROK_LEFT:         return 80;
    case RETROK_DOWN:         return 81;
    case RETROK_UP:           return 82;
    case RETROK_NUMLOCK:      return 83;
    case RETROK_KP_DIVIDE:    return 84;
    case RETROK_KP_MULTIPLY:  return 85;
    case RETROK_KP_MINUS:     return 86;
    case RETROK_KP_PLUS:      return 87;
    case RETROK_KP_ENTER:     return 88;
    case RETROK_KP0:          return 98;
    case RETROK_KP_PERIOD:    return 99;
    case RETROK_LCTRL:        return 224;
    case RETROK_LSHIFT:       return 225;
    case RETROK_LALT:         return 226;
    case RETROK_LSUPER:       return 227;
    case RETROK_RCTRL:        return 228;
    case RETROK_RSHIFT:       return 229;
    case RETROK_RALT:         return 230;
    case RETROK_RSUPER:       return 231;
    default:                  return 0;
    }
}

/* GUI (jkGui) keys travel as windows messages with VK codes; mirrors the exact
 * key list the SDL event pump translates in Window.c. Returns 0 if the key is
 * not part of the GUI set. bSendChar mirrors the pump also posting WM_CHAR. */
static unsigned retro_key_to_vk(unsigned k, int* bSendChar)
{
    *bSendChar = 0;
    switch (k)
    {
    case RETROK_ESCAPE:    *bSendChar = 1; return 0x1B; /* VK_ESCAPE */
    case RETROK_PAGEUP:    return 0x21;                 /* VK_PRIOR */
    case RETROK_PAGEDOWN:  return 0x22;                 /* VK_NEXT */
    case RETROK_LEFT:      return 0x25;
    case RETROK_UP:        return 0x26;
    case RETROK_RIGHT:     return 0x27;
    case RETROK_DOWN:      return 0x28;
    case RETROK_BACKSPACE: *bSendChar = 1; return 0x08; /* VK_BACK */
    case RETROK_DELETE:    return 0x2E;
    case RETROK_INSERT:    *bSendChar = 1; return 0x2D;
    case RETROK_RETURN:    *bSendChar = 1; return 0x0D;
    case RETROK_KP_ENTER:  *bSendChar = 1; return 0x0D;
    case RETROK_LSHIFT:    return 0xA0;
    case RETROK_RSHIFT:    return 0xA1;
    case RETROK_TAB:       *bSendChar = 1; return 0x09;
    case RETROK_END:       return 0x23;
    case RETROK_HOME:      return 0x24;
    case RETROK_BACKQUOTE: return 0xC0;                 /* VK_OEM_3 */
    default:               return 0;
    }
}

/* Shared by the frontend's keyboard callback and the polled fallback below. */
static void core_inject_key(bool down, unsigned keycode, uint32_t character)
{
    uint32_t now = (uint32_t)stdPlatform_GetTimeMsec();

    int scancode = retro_key_to_sdl_scancode(keycode);
    if (scancode > 0 && scancode < 256)
    {
        /* stdControl reads gameplay keys by POLLING this array inside
         * stdControl_ReadControls. Do NOT call SetSDLKeydown for presses here:
         * that consumes the 0->1 edge between frames, before ReadControls'
         * per-frame reset, silently killing every edge-triggered action
         * (weapon select, activate) while held keys (movement) still work.
         * Mirror the SDL pump: presses via the poll, releases forwarded. */
        g_keyboard_state[scancode] = down;
        if (!down)
            stdControl_SetSDLKeydown(scancode, 0, now);
    }

    int bSendChar = 0;
    unsigned vk = retro_key_to_vk(keycode, &bSendChar);
    if (vk)
    {
        if (down)
        {
            Window_msg_main_handler(g_hWnd, 0x100 /* WM_KEYFIRST/WM_KEYDOWN */, vk, 0);
            if (bSendChar)
                Window_msg_main_handler(g_hWnd, 0x102 /* WM_CHAR */, vk, 0);
        }
        else
        {
            Window_msg_main_handler(g_hWnd, 0x101 /* WM_KEYUP */, vk, 0);
        }
    }
    else if (down && character >= 0x20 && character < 0x10000)
    {
        /* Printable text entry (player name, cheats, console). */
        Window_msg_main_handler(g_hWnd, 0x102 /* WM_CHAR */, (WPARAM)character, 0);
    }
}

/* True once the frontend has proven it delivers keyboard EVENTS; until then a
 * per-frame poll of RETRO_DEVICE_KEYBOARD drives the same injection (some
 * frontend configs deliver key events only in Game Focus mode, but polled key
 * state works in both). The flag keeps the two paths from double-typing. */
static bool s_kbd_events_seen;
/* True once the frontend has delivered a nonzero `character`. RetroArch's
 * Windows dinput driver never does -- every event arrives with char=0 -- so
 * text entry would be impossible without synthesizing characters ourselves.
 * The flag disables synthesis on frontends that do send real characters. */
static bool s_kbd_chars_seen;
static int s_kbd_log_budget = 16;

/* US-layout character for a retro_key, for frontends that leave `character`
 * empty. RETROK_* printable codes are ASCII. */
static uint32_t core_char_from_retrok(unsigned k, uint16_t mods)
{
    bool shift = (mods & RETROKMOD_SHIFT) != 0;
    bool caps = (mods & RETROKMOD_CAPSLOCK) != 0;

    if (k >= RETROK_a && k <= RETROK_z)
        return (shift != caps) ? (k - 0x20) : k;

    if (k >= RETROK_KP0 && k <= RETROK_KP9)
        return '0' + (k - RETROK_KP0);

    switch (k)
    {
    case RETROK_KP_PERIOD:   return '.';
    case RETROK_KP_DIVIDE:   return '/';
    case RETROK_KP_MULTIPLY: return '*';
    case RETROK_KP_MINUS:    return '-';
    case RETROK_KP_PLUS:     return '+';
    default:                 break;
    }

    if (k < 0x20 || k >= 0x7F)
        return 0;

    if (!shift)
        return k;

    switch (k) /* shifted US layout */
    {
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    case '`': return '~';
    default:  return k;
    }
}

static void core_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers)
{
    if (s_kbd_log_budget > 0)
    {
        s_kbd_log_budget--;
        core_log(RETRO_LOG_INFO, "kbd event: down=%d key=%u char=0x%x mods=0x%x\n",
                 down ? 1 : 0, keycode, character, key_modifiers);
    }

    if (keycode != RETROK_UNKNOWN)
        s_kbd_events_seen = true;
    if (character != 0)
        s_kbd_chars_seen = true;
    else if (down && !s_kbd_chars_seen)
        character = core_char_from_retrok(keycode, key_modifiers);

    core_inject_key(down, keycode, character);
}

static void core_poll_keyboard_fallback(void)
{
    static bool state[RETROK_LAST];

    retro_input_state_t input = g_core.input_state_cb;
    if (!input || s_kbd_events_seen)
        return;

    bool shift = input(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LSHIFT) ||
                 input(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RSHIFT);

    for (unsigned k = 1; k < RETROK_LAST; k++)
    {
        int bSendChar;
        if (!retro_key_to_sdl_scancode(k) && !retro_key_to_vk(k, &bSendChar))
            continue;

        bool down = input(0, RETRO_DEVICE_KEYBOARD, 0, k) != 0;
        if (down == state[k])
            continue;
        state[k] = down;

        /* RETROK_* printable codes are ASCII; uppercase letters under shift is
         * enough for name entry until real key events arrive. */
        uint32_t character = 0;
        if (k >= 0x20 && k < 0x7F)
            character = (shift && k >= RETROK_a && k <= RETROK_z) ? (k - 0x20) : k;

        core_inject_key(down, k, character);
    }
}

/* ------------------------------------------------------------------------
 * Mouse
 * ------------------------------------------------------------------------ */

/* Replicates Window_HandleMouseMove (Window.c): menu coordinates are mapped
 * into 640x480 letterboxed space; gameplay reads accumulated relative deltas. */
static void core_update_mouse_position(int dx, int dy)
{
    g_core.mouse_abs_x += dx;
    g_core.mouse_abs_y += dy;
    if (g_core.mouse_abs_x < 0) g_core.mouse_abs_x = 0;
    if (g_core.mouse_abs_y < 0) g_core.mouse_abs_y = 0;
    if (g_core.mouse_abs_x >= Window_xSize) g_core.mouse_abs_x = Window_xSize - 1;
    if (g_core.mouse_abs_y >= Window_ySize) g_core.mouse_abs_y = Window_ySize - 1;

    Window_lastMouseX = Window_mouseX;
    Window_lastMouseY = Window_mouseY;

    if (!jkGame_isDDraw)
    {
        float fX = (float)g_core.mouse_abs_x;
        float fY = (float)g_core.mouse_abs_y;
        float menu_x = ((float)Window_screenXSize - ((float)Window_screenYSize * (640.0f / 480.0f))) / 2.0f;
        float menu_w = ((float)Window_screenYSize * (640.0f / 480.0f));

        Window_mouseX = (int)(((fX - menu_x) / menu_w) * 640.0f);
        Window_mouseY = (int)((fY / (float)Window_screenYSize) * 480.0f);
    }
    else
    {
        Window_mouseX = g_core.mouse_abs_x;
        Window_mouseY = g_core.mouse_abs_y;
    }

    if (Window_mouseX < 0)
        Window_mouseX = 0;

    Window_lastXRel += dx;
    Window_lastYRel += dy;

    if (dx || dy)
    {
        uint32_t pos = ((uint32_t)Window_mouseX & 0xFFFF) | (((uint32_t)Window_mouseY << 16) & 0xFFFF0000);
        Window_msg_main_handler(g_hWnd, 0x200 /* WM_MOUSEMOVE */, 0, pos);
    }
}

static void core_poll_input(void)
{
    retro_input_state_t input = g_core.input_state_cb;
    if (!input)
        return;

    if (g_core.input_poll_cb)
        g_core.input_poll_cb();

    core_poll_keyboard_fallback();

    int dx = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int dy = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    core_update_mouse_position(dx, dy);

    int l = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) ? 1 : 0;
    int r = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) ? 1 : 0;

    /* Cursor auto-hide bookkeeping: motion or a button edge counts as pointer
     * activity (a click mid-hover shouldn't leave the pointer invisible). */
    if (dx || dy || l != g_core.last_mouse_l || r != g_core.last_mouse_r)
    {
        g_core.cursor_seen_motion = true;
        g_core.cursor_idle_frames = 0;
    }
    else if (g_core.cursor_idle_frames <= CURSOR_IDLE_HIDE_FRAMES)
    {
        g_core.cursor_idle_frames++;
    }

    uint32_t pos = ((uint32_t)Window_mouseX & 0xFFFF) | (((uint32_t)Window_mouseY << 16) & 0xFFFF0000);

    /* Mirrors the SDL pump's button handling: Window_bMouseLeft is 1/0,
     * Window_bMouseRight is 2/0, messages carry (left | right). */
    if (l != g_core.last_mouse_l)
    {
        Window_bMouseLeft = l ? 1 : 0;
        Window_msg_main_handler(g_hWnd, l ? 0x201 /* WM_LBUTTONDOWN */ : 0x202 /* WM_LBUTTONUP */,
                                (WPARAM)(Window_bMouseLeft | Window_bMouseRight), pos);
        g_core.last_mouse_l = l;
    }
    if (r != g_core.last_mouse_r)
    {
        Window_bMouseRight = r ? 2 : 0;
        Window_msg_main_handler(g_hWnd, r ? 0x204 /* WM_RBUTTONDOWN */ : 0x205 /* WM_RBUTTONUP */,
                                (WPARAM)(Window_bMouseLeft | Window_bMouseRight), pos);
        g_core.last_mouse_r = r;
    }

    if (input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP))
        Window_mouseWheelY += 1;
    if (input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN))
        Window_mouseWheelY -= 1;
}

/* ------------------------------------------------------------------------
 * Engine lifecycle
 * ------------------------------------------------------------------------ */

/* Forward decls (defined in the cursor / fiber / API sections below). */
static void core_free_cursor_gl(bool delete_objects);
static void core_refresh_options(void);

static void core_context_reset(void)
{
    g_core.context_alive = true;
    /* If the engine survived a context loss, its GLEW pointers are still
     * loaded (WGL function pointers are process-wide); std3D rebuilds its GL
     * state lazily via StartScene's init path on the next frame. */
    if (g_core.engine_started)
        s_gl_ready = true;
    core_log(RETRO_LOG_INFO, "context_reset (engine_started=%d)\n", g_core.engine_started ? 1 : 0);
}

static void core_context_destroy(void)
{
    /* Empirical (RetroArch 1.21, close content): this arrives BEFORE
     * retro_unload_game, and the context is only current during the callback
     * -- so engine GL teardown must happen here, and the unload quiesce that
     * follows must not render a frame. */
    core_log(RETRO_LOG_INFO, "context_destroy (gl_ready=%d, engine_started=%d, glctx=%p)\n",
             s_gl_ready ? 1 : 0, g_core.engine_started ? 1 : 0, (void*)wglGetCurrentContext());
    if (s_gl_ready)
    {
        if (libretro_std3D_HasGlResources())
            std3D_FreeResources();
        core_free_cursor_gl(true);
    }
    else
    {
        core_free_cursor_gl(false);
    }
    s_gl_ready = false;
    g_core.context_alive = false;
}

/* ------------------------------------------------------------------------
 * Core-drawn mouse cursor
 *
 * The GUI expects the OS cursor to hover over its menus
 * (jkGuiRend_SetCursorVisible drives SDL/Win32 cursor visibility), but no OS
 * cursor ever overlays a libretro frontend's viewport. Draw a small wedge at
 * the engine's menu-space mouse position after the engine finishes its frame.
 * ------------------------------------------------------------------------ */

extern int libretro_GetCursorVisible(void); /* jkGUIRend.c (LIBRETRO_BUILD) */

static GLuint s_cursor_prog, s_cursor_vao, s_cursor_vbo;
static GLint s_cursor_u_xform, s_cursor_u_color;
static bool s_cursor_init_failed;

static const char* CURSOR_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "uniform vec4 xform;\n" /* scale.xy, offset.xy (NDC) */
    "void main() { gl_Position = vec4(pos * xform.xy + xform.zw, 0.0, 1.0); }\n";

static const char* CURSOR_FS =
    "#version 330 core\n"
    "uniform vec4 color;\n"
    "out vec4 frag;\n"
    "void main() { frag = color; }\n";

/* Arrow wedge in cursor-local pixels, y down. */
static const float CURSOR_TRI[6] = { 0.0f, 0.0f, 0.0f, 16.0f, 11.0f, 11.0f };

static bool core_cursor_init(void)
{
    if (s_cursor_prog)
        return true;
    if (s_cursor_init_failed)
        return false;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &CURSOR_VS, NULL);
    glShaderSource(fs, 1, &CURSOR_FS, NULL);
    glCompileShader(vs);
    glCompileShader(fs);

    GLint vs_ok = 0, fs_ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
    if (!vs_ok || !fs_ok)
    {
        core_log(RETRO_LOG_WARN, "cursor shader compile failed; no cursor overlay\n");
        glDeleteShader(vs);
        glDeleteShader(fs);
        s_cursor_init_failed = true;
        return false;
    }

    s_cursor_prog = glCreateProgram();
    glAttachShader(s_cursor_prog, vs);
    glAttachShader(s_cursor_prog, fs);
    glLinkProgram(s_cursor_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(s_cursor_prog, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        core_log(RETRO_LOG_WARN, "cursor shader link failed; no cursor overlay\n");
        glDeleteProgram(s_cursor_prog);
        s_cursor_prog = 0;
        s_cursor_init_failed = true;
        return false;
    }

    s_cursor_u_xform = glGetUniformLocation(s_cursor_prog, "xform");
    s_cursor_u_color = glGetUniformLocation(s_cursor_prog, "color");

    glGenVertexArrays(1, &s_cursor_vao);
    glGenBuffers(1, &s_cursor_vbo);
    glBindVertexArray(s_cursor_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_cursor_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(CURSOR_TRI), CURSOR_TRI, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    return true;
}

static void core_draw_cursor(void)
{
    if (!core_cursor_init())
        return;

    /* Menu-space (640x480) mouse position -> window pixels -> NDC. */
    float px = (float)Window_mouseX * ((float)Window_xSize / 640.0f);
    float py = (float)Window_mouseY * ((float)Window_ySize / 480.0f);
    float s = (float)Window_ySize / 480.0f; /* cursor scales with resolution */

    float sx = 2.0f * s / (float)Window_xSize;
    float sy = -2.0f * s / (float)Window_ySize; /* local y grows down-screen */
    float ox = 2.0f * px / (float)Window_xSize - 1.0f;
    float oy = 1.0f - 2.0f * py / (float)Window_ySize;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, Window_xSize, Window_ySize);
    glUseProgram(s_cursor_prog);
    glBindVertexArray(s_cursor_vao);

    /* Black underlay offset a pixel for contrast, then white wedge. */
    glUniform4f(s_cursor_u_xform, sx, sy, ox + 1.5f * sx, oy + 1.5f * sy);
    glUniform4f(s_cursor_u_color, 0.0f, 0.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUniform4f(s_cursor_u_xform, sx, sy, ox, oy);
    glUniform4f(s_cursor_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* delete_objects: the GL context is still current, actually free them; false
 * when the context is already gone and only the stale handles are dropped. */
static void core_free_cursor_gl(bool delete_objects)
{
    if (delete_objects && s_cursor_prog)
    {
        glDeleteProgram(s_cursor_prog);
        glDeleteVertexArrays(1, &s_cursor_vao);
        glDeleteBuffers(1, &s_cursor_vbo);
    }
    s_cursor_prog = 0;
    s_cursor_vao = 0;
    s_cursor_vbo = 0;
    s_cursor_init_failed = false;
}

/* ------------------------------------------------------------------------
 * Engine fiber
 *
 * Win32 fibers for now (this platform module is MSVC-only); swap in libco or
 * ucontext for the Linux build later.
 * ------------------------------------------------------------------------ */

/* ---- Engine quiesce ----------------------------------------------------
 *
 * Getting the engine OUT of a session cleanly has two shapes (empirical: on
 * close-content RetroArch calls context_destroy BEFORE retro_unload_game, so
 * no frame may render during an unload quiesce):
 *
 *  - UNWIND (retro_reset; GL context alive): set g_should_exit -- the
 *    engine's own clean-exit convention, which menu loops like jkGuiMain_Show
 *    already honor -- and force-pop one modal menu per resumed frame until
 *    the fiber unwinds to its frame-boundary loop, which then runs the same
 *    WriteConf -> Main_Shutdown sequence Window_Main_Linux runs after its
 *    main loop breaks.
 *
 *  - INLINE (retro_unload_game; context gone): resume the fiber once and run
 *    the shutdown sequence right at its park point, leaving the frames below
 *    frozen forever. Precedent: the engine's WM_DESTROY handler calls
 *    Main_Shutdown from inside the modal pump the same way (Window.c).
 *
 * Either way Main_Shutdown closes the OpenAL device and joins its threads --
 * the threads that otherwise pin the core DLL against unmapping. */
enum { QUIESCE_NONE, QUIESCE_UNWIND, QUIESCE_INLINE };
static int s_quiesce_mode;
static bool s_engine_shutdown_done;

#define QUIESCE_UNWIND_BUDGET_FRAMES 120

/* Consumed by jkGUIRend.c's modal pump: suppresses jk_exit while the menu
 * stack is being unwound with g_should_exit set. */
int libretro_InQuiesce(void)
{
    return s_quiesce_mode != QUIESCE_NONE;
}

/* Runs ON the engine fiber; never returns. Mirrors the standalone's exit path
 * (Window.c: main loop breaks on g_should_exit -> WriteConf -> Main_Shutdown). */
static void core_engine_shutdown_and_park(const char* how)
{
    core_log(RETRO_LOG_INFO, "engine shutdown (%s) starting\n", how);
    g_should_exit = 1;
    /* Session record: capture where the player is before teardown (no-op /
     * non-clobbering when there's nothing valid to record). */
    jkSession_SaveCurrent();
    if (jkPlayer_bHasLoadedSettingsOnce)
        jkPlayer_WriteConf(jkPlayer_playerShortName);
    Main_Shutdown();
    s_engine_shutdown_done = true;
    core_log(RETRO_LOG_INFO, "engine shutdown (%s) complete; fiber parked\n", how);
    for (;;)
        SwitchToFiber(s_frontend_fiber); /* raw: must not re-trigger quiesce hooks */
}

/* Called from engine code (Window_MessageLoop's LIBRETRO_BUILD patch) and from
 * the engine fiber's own frame loop: hand control back to retro_run. On
 * resume, a pending quiesce is driven from here -- this is the "checked at
 * the yield" part of the design. */
void libretro_yield_to_frontend(void)
{
    if (s_frontend_fiber)
        SwitchToFiber(s_frontend_fiber);
    /* Resumed by retro_run or by a quiesce driver. */
    if (s_quiesce_mode == QUIESCE_INLINE && g_core.engine_started && !s_engine_shutdown_done)
        core_engine_shutdown_and_park("inline at park point");
    if (s_quiesce_mode == QUIESCE_UNWIND)
        libretro_ForcePopActiveMenu(); /* one modal level per resumed frame */
}

/* Called from jk_exit (LIBRETRO_BUILD patch): the engine wants the process to
 * exit. Park its fiber forever and let retro_run signal the frontend; a later
 * quiesce (the frontend will unload us) shuts the engine down from the park
 * point via the yield hook above. */
void libretro_engine_exit(int code)
{
    core_log(RETRO_LOG_INFO, "engine requested exit (%d); signaling frontend shutdown\n", code);
    s_engine_exit_requested = true;
    for (;;)
        libretro_yield_to_frontend();
}

/* De-SDL'd replica of Window_Main_Linux()'s init order (Window.c), run inside
 * retro_run() so the frontend's GL context is current for Main_Startup's GL
 * bring-up. */
static bool core_boot_engine(void)
{
    core_log(RETRO_LOG_INFO, "engine boot: basefolder='%s' mots=%d\n", g_core.basefolder, g_core.is_mots);

    OpenJKDF2_Globals_Reset();

    if (g_core.is_mots)
        Main_bMotsCompat = 1;

    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK && glew_err != 4 /* GLEW_ERROR_NO_GLX_DISPLAY */)
    {
        core_log(RETRO_LOG_ERROR, "glewInit failed: %d\n", (int)glew_err);
        return false;
    }
    s_gl_ready = true;

    Window_xSize = CORE_BASE_WIDTH;
    Window_ySize = CORE_BASE_HEIGHT;
    Window_screenXSize = CORE_BASE_WIDTH;
    Window_screenYSize = CORE_BASE_HEIGHT;
    Window_resized = 1;

    /* Make std3D's startup capture of GL_FRAMEBUFFER_BINDING see the
     * frontend's framebuffer, not 0. */
    GLint fbo = (GLint)g_core.hw_render.get_current_framebuffer();
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = 0;
    g_nShowCmd = 0;

    core_getcwd(openjkdf2_aOrigCwd, sizeof(openjkdf2_aOrigCwd));

    /* Boot-mode options (menu / direct / resume) are applied by the
     * jkSession_ArmBoot hook inside Main_Startup, after cmdline parsing. */
    core_refresh_options();
    jkSession_ConfigureBoot(g_core.boot_mode, g_core.direct_boot_filter,
                            g_core.episode_name);

    int result = Main_Startup(g_core.cmdline);
    if (!result)
    {
        core_log(RETRO_LOG_ERROR, "Main_Startup failed\n");
        return false;
    }

    g_window_not_destroyed = 1;

    Window_msg_main_handler(g_hWnd, 0x1, 0, 0);  /* WM_CREATE */
    Window_msg_main_handler(g_hWnd, 0x6, 2, 0);  /* WM_ACTIVATE */
    Window_msg_main_handler(g_hWnd, 0x1C, 1, 0); /* WM_ACTIVATEAPP */
    Window_msg_main_handler(g_hWnd, 0x18, 0, 0); /* WM_SHOWWINDOW */

    core_log(RETRO_LOG_INFO, "engine boot complete\n");
    return true;
}

/* The engine's entire life happens on this fiber: boot, then one
 * Window_Main_Loop per resume. Modal menu loops inside jkMain_GuiAdvance yield
 * from Window_MessageLoop instead of reaching the frame-boundary yield here. */
static void CALLBACK core_engine_fiber_proc(void* param)
{
    (void)param;

    if (!core_boot_engine())
    {
        g_core.engine_start_failed = true;
        s_engine_exit_requested = true;
        for (;;)
            libretro_yield_to_frontend();
    }
    g_core.engine_started = true;

    for (;;)
    {
        if (s_quiesce_mode != QUIESCE_NONE)
            core_engine_shutdown_and_park("frame boundary");
        Window_Main_Loop(); /* one frame: game/menu logic + render */
        if (g_should_exit)
            s_engine_exit_requested = true;
        libretro_yield_to_frontend();
    }
}

/* Frontend-fiber driver: bring the engine to a clean Main_Shutdown and delete
 * its fiber. allow_unwind pumps real frames so modal menus pop cooperatively
 * -- only safe with a live GL context (frames render). Returns true if the
 * engine ran its shutdown; false means the fallback (WriteConf + force-close
 * audio) was applied instead. Always leaves the fiber deleted. */
static bool core_quiesce_engine(bool allow_unwind)
{
    bool clean = s_engine_shutdown_done;

    if (s_engine_fiber && g_core.engine_started && !clean)
    {
        if (allow_unwind)
        {
            s_quiesce_mode = QUIESCE_UNWIND;
            g_should_exit = 1;
            int frames = 0;
            while (frames < QUIESCE_UNWIND_BUDGET_FRAMES && !s_engine_shutdown_done)
            {
                SwitchToFiber(s_engine_fiber);
                frames++;
            }
            if (s_engine_shutdown_done)
                core_log(RETRO_LOG_INFO, "unwind quiesce complete after %d frames\n", frames);
            else
                core_log(RETRO_LOG_WARN, "unwind quiesce incomplete after %d frames; forcing inline shutdown\n", frames);
        }
        if (!s_engine_shutdown_done)
        {
            s_quiesce_mode = QUIESCE_INLINE;
            SwitchToFiber(s_engine_fiber); /* shutdown runs at the fiber's park point */
        }
        s_quiesce_mode = QUIESCE_NONE;
        clean = s_engine_shutdown_done;
    }

    if (!clean && !s_engine_fiber && !g_core.engine_started && !g_core.engine_start_failed)
    {
        clean = true; /* engine never existed; nothing to quiesce */
    }
    else if (!clean)
    {
        /* Engine failed during boot or (unexpectedly) refused to shut down:
         * save what we can and make sure no audio thread outlives the
         * content to pin the DLL. */
        core_log(RETRO_LOG_WARN, "engine quiesce fell back (started=%d failed=%d); force-closing audio device\n",
                 g_core.engine_started, g_core.engine_start_failed);
        if (jkPlayer_bHasLoadedSettingsOnce)
            jkPlayer_WriteConf(jkPlayer_playerShortName);
        libretro_ForceCloseAudioDevice();
    }

    /* GL teardown runs in context_destroy or here, whichever comes first
     * (RetroArch destroys the context before unloading; retro_reset arrives
     * with it alive). */
    if (s_gl_ready && libretro_std3D_HasGlResources())
    {
        core_log(RETRO_LOG_INFO, "freeing engine GL resources (context still alive)\n");
        std3D_FreeResources();
    }

    if (s_engine_fiber)
    {
        DeleteFiber(s_engine_fiber);
        s_engine_fiber = NULL;
    }
    g_core.engine_started = false;
    g_core.engine_start_failed = false;
    s_engine_shutdown_done = false;
    s_engine_exit_requested = false;
    return clean;
}

/* ------------------------------------------------------------------------
 * libretro API
 * ------------------------------------------------------------------------ */

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

/* Read current core option values (call at load and whenever the frontend
 * flags them dirty). */
static void core_refresh_options(void)
{
    struct retro_variable var;

    var.key = "openjkdf2_boot";
    var.value = NULL;
    /* Default: straight into the loaded episode -- content load means "play
     * this game"; the stock intro/menu click-through is opt-in (owner
     * decision, 2026-08-20). Keep in sync with the option defs below. */
    g_core.boot_mode = JKSESSION_BOOT_DIRECT;
    if (g_core.environ_cb && g_core.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "intro"))
            g_core.boot_mode = JKSESSION_BOOT_INTRO;
        else if (!strcmp(var.value, "menu"))
            g_core.boot_mode = JKSESSION_BOOT_MENU;
        else if (!strcmp(var.value, "level"))
            g_core.boot_mode = JKSESSION_BOOT_LEVEL;
        else if (!strcmp(var.value, "resume"))
            g_core.boot_mode = JKSESSION_BOOT_RESUME;
    }

    /* Direct-boot episode-type filter; the game mode itself always follows
     * the episode's own TYPE (jkSession_ResolveAutoBootMode). */
    var.key = "openjkdf2_boot_game_type";
    var.value = NULL;
    g_core.direct_boot_filter = JKSESSION_DIRECT_ALL;
    if (g_core.environ_cb && g_core.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "singleplayer"))
            g_core.direct_boot_filter = JKSESSION_DIRECT_SP_ONLY;
        else if (!strcmp(var.value, "multiplayer"))
            g_core.direct_boot_filter = JKSESSION_DIRECT_MP_ONLY;
    }

}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    g_core.environ_cb = cb;
    if (!cb)
        return;

    cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &g_core.log);

    /* Core options (v2 with legacy fallback). */
    {
        static const struct retro_core_option_definition option_defs[] = {
            {
                "openjkdf2_boot",
                "Boot mode (restart content to apply)",
                "How loading content starts the game. 'Intro video' is the stock flow; 'Game main menu' skips the movie. "
                "'Straight into episode' always starts the loaded episode over from its first level. "
                "'Continue from last level' starts the level you last played at its normal start point. "
                "'Resume last session' puts you back at the exact spot you left. "
                "The continue/resume modes fall back to the episode start, then the menu, when there is no matching session.",
                { { "episode", "Straight into episode" },
                  { "intro", "Intro video" },
                  { "menu", "Game main menu" },
                  { "level", "Continue from last level" },
                  { "resume", "Resume last session (exact spot)" },
                  { NULL, NULL } },
                "episode",
            },
            {
                "openjkdf2_boot_game_type",
                "Direct boot: episode types",
                "The game mode (singleplayer, or hosting a local multiplayer session) always follows the loaded episode's "
                "own type. This limits which episode types 'Straight into episode' applies to; episodes outside the "
                "selection boot to the game's main menu instead.",
                { { "all", "All episodes" },
                  { "singleplayer", "Singleplayer episodes only" },
                  { "multiplayer", "Multiplayer episodes only" },
                  { NULL, NULL } },
                "all",
            },
            { NULL, NULL, NULL, { { NULL, NULL } }, NULL },
        };
        unsigned version = 0;
        if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 1)
        {
            cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, (void*)option_defs);
        }
        else
        {
            static const struct retro_variable vars[] = {
                { "openjkdf2_boot", "Boot mode; episode|intro|menu|level|resume" },
                { "openjkdf2_boot_game_type", "Direct boot episode types; all|singleplayer|multiplayer" },
                { NULL, NULL },
            };
            cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
        }
    }

    /* This is a keyboard+mouse game first; the frontend's Controls menu should
     * say so instead of assuming only a RetroPad exists. Note RetroArch still
     * defaults the port SELECTION to RetroPad regardless of list order --
     * that's fine: keyboard/mouse injection is always active no matter the
     * selection (the engine natively reads all devices at once), so the game
     * defaults to keyboard in practice. The selection gates only the M1
     * RetroPad mapping; picking "Keyboard + Mouse" will opt out of pad
     * injection (e.g. to avoid RetroArch's keyboard->RetroPad double-binds). */
    static const struct retro_controller_description port0_types[] = {
        { "Keyboard + Mouse", RETRO_DEVICE_KEYBOARD },
        { "RetroPad", RETRO_DEVICE_JOYPAD },
    };
    static const struct retro_controller_info ports[] = {
        { port0_types, 2 },
        { NULL, 0 },
    };
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { g_core.video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { g_core.audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_core.audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { g_core.input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { g_core.input_state_cb = cb; }

RETRO_API void retro_init(void)
{
    g_core.port0_device = RETRO_DEVICE_KEYBOARD;
}

RETRO_API void retro_deinit(void)
{
    /* retro_unload_game already quiesced; this is belt-and-suspenders for
     * frontends that skip it. */
    if (s_engine_fiber)
    {
        core_log(RETRO_LOG_WARN, "retro_deinit: engine fiber still alive; quiescing now\n");
        core_quiesce_engine(false);
    }
    if (s_frontend_fiber)
    {
        /* Leave the frontend's main thread a plain thread again -- a fiber
         * outliving its creating module is a stale-pointer hazard. */
        ConvertFiberToThread();
        s_frontend_fiber = NULL;
    }
    core_log(RETRO_LOG_INFO, "retro_deinit complete\n");
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "OpenJKDF2";
    info->library_version = OPENJKDF2_RELEASE_VERSION_STRING;
    info->valid_extensions = "gob|goo";
    info->need_fullpath = true; /* GOBs are streamed containers; never load to memory */
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = CORE_BASE_WIDTH;
    info->geometry.base_height = CORE_BASE_HEIGHT;
    info->geometry.max_width = CORE_MAX_WIDTH;
    info->geometry.max_height = CORE_MAX_HEIGHT;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = CORE_FPS;
    info->timing.sample_rate = CORE_SAMPLE_RATE;
}

static void core_setenv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* Put an error on the frontend's OSD (users don't see the log). */
static void core_show_message(const char* text)
{
    if (!g_core.environ_cb)
        return;

    struct retro_message_ext ext;
    memset(&ext, 0, sizeof(ext));
    ext.msg = text;
    ext.duration = 6000;
    ext.priority = 3;
    ext.level = RETRO_LOG_ERROR;
    ext.target = RETRO_MESSAGE_TARGET_ALL;
    ext.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
    if (!g_core.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &ext))
    {
        struct retro_message legacy = { text, 360 };
        g_core.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &legacy);
    }
}

/* --------------------------------------------------------------------------
 * Savestates (devdocs/09). Not a memory snapshot -- the engine has none --
 * but the engine's own savegame (~350 KB) carried in a fixed-size envelope.
 * Save works whenever the native Save Game menu would (loaded SP world, live
 * player, also from inside the ESC menu). Load is a restore SIGNAL: it
 * queues the engine's own Load Game flow and completes over the following
 * frames -- fine for the user-facing state slots, useless for frame-exact
 * features (rewind/run-ahead/netplay), which the serialization quirks and
 * the savestate-context check keep away from this path. */

/* Fixed size cap: libretro forbids serialize_size ever growing during a
 * session. Engine saves scale with level size but stay well under 1 MB; the
 * zero padding compresses to nothing in the frontend's state files. */
#define CORE_STATE_CAP   (8u * 1024u * 1024u)
#define CORE_STATE_MAGIC "JKSTATE1"
typedef struct core_state_envelope_t
{
    char     magic[8];    /* CORE_STATE_MAGIC, no terminator */
    uint32_t payload_len; /* engine .jks bytes that follow this header */
    uint32_t flags;       /* bit 0: MoTS content */
    uint32_t reserved[4];
} core_state_envelope_t;

/* Deferred-restore retry budget: a cold boot needs a few seconds to reach a
 * profile; a fresh install that never creates one gives up after this. */
#define CORE_STATE_PENDING_RETRY_FRAMES (60 * 60)

/* Only serve genuine to-disk savestates. Frontends that don't support the
 * context query get the benefit of the doubt. */
static bool core_savestate_context_ok(void)
{
    int ctx = RETRO_SAVESTATE_CONTEXT_NORMAL;
    if (g_core.environ_cb && g_core.environ_cb(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &ctx))
        return ctx == RETRO_SAVESTATE_CONTEXT_NORMAL || ctx == RETRO_SAVESTATE_CONTEXT_UNKNOWN;
    return true;
}

static void core_drop_pending_state(const char* why)
{
    if (!g_core.pending_state)
        return;
    core_log(RETRO_LOG_INFO, "dropping deferred state restore (%s)\n", why);
    free(g_core.pending_state);
    g_core.pending_state = NULL;
    g_core.pending_state_len = 0;
    g_core.pending_state_frames = 0;
}

static bool core_path_is_dir(const char* path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* rom: <basefolder>/episode/<name>.gob -> basefolder */
static bool core_derive_basefolder(const char* rom_path)
{
    char tmp[1024];
    strncpy(tmp, rom_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;

    /* strip filename */
    char* last = strrchr(tmp, '/');
    char* lastb = strrchr(tmp, '\\');
    if (lastb > last) last = lastb;
    if (!last)
        return false;

    /* remember episode name (filename minus extension) */
    strncpy(g_core.episode_name, last + 1, sizeof(g_core.episode_name) - 1);
    g_core.episode_name[sizeof(g_core.episode_name) - 1] = 0;
    char* dot = strrchr(g_core.episode_name, '.');
    if (dot) *dot = 0;
    *last = 0;

    /* strip the episode directory */
    last = strrchr(tmp, '/');
    lastb = strrchr(tmp, '\\');
    if (lastb > last) last = lastb;
    if (!last)
        return false;
    *last = 0;

    strncpy(g_core.basefolder, tmp, sizeof(g_core.basefolder) - 1);
    g_core.basefolder[sizeof(g_core.basefolder) - 1] = 0;
    return g_core.basefolder[0] != 0;
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
    if (!game || !game->path || !game->path[0])
        return false;

    memset(g_keyboard_state, 0, sizeof(g_keyboard_state));

    if (!core_derive_basefolder(game->path))
    {
        core_log(RETRO_LOG_ERROR, "could not derive basefolder from '%s' (expected <basefolder>/episode/<name>.gob)\n", game->path);
        core_show_message("OpenJKDF2: load an episode GOB from inside your game folder, e.g. MyJK/episode/JK1.GOB");
        return false;
    }

    size_t len = strlen(game->path);
    g_core.is_mots = (len > 4) && (_strcmpi(game->path + len - 4, ".goo") == 0);

    /* Fail with a visible explanation instead of booting into missing-data
     * chaos: the folder above the GOB's directory must be a JK install layout
     * with resource GOBs (Res2.gob etc.). fcaseopen handles casing later; on
     * Windows the filesystem is case-insensitive anyway. */
    {
        char resdir[sizeof(g_core.basefolder) + 16];
        snprintf(resdir, sizeof(resdir), "%s/resource", g_core.basefolder); /* Win32 APIs accept '/' */
        if (!core_path_is_dir(resdir))
        {
            core_log(RETRO_LOG_ERROR,
                     "no resource/ directory in '%s'. Expected layout:\n"
                     "  <YourJKFolder>/episode/JK1.GOB   <- load this file\n"
                     "  <YourJKFolder>/resource/Res2.gob (from your Jedi Knight install)\n"
                     "  <YourJKFolder>/MUSIC/            (optional, music tracks)\n",
                     g_core.basefolder);
            core_show_message("OpenJKDF2: game data not found - the folder containing episode/ must also contain resource/ (Res2.gob) from your Jedi Knight install");
            return false;
        }
    }

    if (core_chdir(g_core.basefolder) != 0)
    {
        core_log(RETRO_LOG_ERROR, "chdir('%s') failed\n", g_core.basefolder);
        return false;
    }

    /* Pin InstallHelper: the env override trumps all of its detection, so it
     * can never chdir somewhere clever or open a locate-your-install dialog. */
    core_setenv(g_core.is_mots ? "OPENJKMOTS_ROOT" : "OPENJKDF2_ROOT", g_core.basefolder);

    strcpy(g_core.cmdline, "");
    if (g_core.is_mots)
        strcat(g_core.cmdline, "-motsCompat ");

    /* Hardware render context: GL 3.3 core, matching WIN64_STANDALONE's request. */
    memset(&g_core.hw_render, 0, sizeof(g_core.hw_render));
    g_core.hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    g_core.hw_render.version_major = 3;
    g_core.hw_render.version_minor = 3;
    g_core.hw_render.context_reset = core_context_reset;
    g_core.hw_render.context_destroy = core_context_destroy;
    g_core.hw_render.depth = true;
    g_core.hw_render.stencil = true;
    g_core.hw_render.bottom_left_origin = true;
    g_core.hw_render.cache_context = true;

    if (!g_core.environ_cb || !g_core.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_core.hw_render))
    {
        core_log(RETRO_LOG_ERROR,
                 "frontend refused an OpenGL 3.3 core context. Set the video driver to 'gl' or 'glcore'.\n");
        return false;
    }

    struct retro_keyboard_callback kb = { core_keyboard_event };
    g_core.environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);

    /* Savestates are engine savegames restored asynchronously over following
     * frames: usable for the save/load slots, incomplete for frame-exact
     * features. Declare that so netplay/run-ahead don't build on them. */
    {
        uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE
                        | RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT
                        | RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT;
        g_core.environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
    }

    g_core.mouse_abs_x = CORE_BASE_WIDTH / 2;
    g_core.mouse_abs_y = CORE_BASE_HEIGHT / 2;
    g_core.cursor_seen_motion = false;
    g_core.cursor_idle_frames = 0;
    core_refresh_options();

    g_core.game_loaded = true;
    g_core.engine_started = false;
    g_core.engine_start_failed = false;

    core_log(RETRO_LOG_INFO, "loaded '%s' (basefolder '%s', episode '%s', %s)\n",
             game->path, g_core.basefolder, g_core.episode_name, g_core.is_mots ? "MoTS" : "DF2");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
    (void)game_type; (void)info; (void)num_info;
    return false;
}

RETRO_API void retro_unload_game(void)
{
    core_log(RETRO_LOG_INFO, "unload: engine_started=%d ctx_alive=%d glctx=%p\n",
             g_core.engine_started ? 1 : 0, g_core.context_alive ? 1 : 0,
             (void*)wglGetCurrentContext());

    /* Inline quiesce only: on RetroArch the GL context is already gone here
     * (context_destroy freed the GL side), so no frame may render -- the
     * engine shuts down at its current park point instead of unwinding. */
    core_drop_pending_state("content unloading");
    core_quiesce_engine(false);
    g_core.game_loaded = false;
}

RETRO_API void retro_run(void)
{
    if (!g_core.game_loaded || g_core.engine_start_failed)
        return;

    if (!s_frontend_fiber)
    {
        s_frontend_fiber = ConvertThreadToFiber(NULL);
        if (!s_frontend_fiber && GetLastError() == ERROR_ALREADY_FIBER)
            s_frontend_fiber = GetCurrentFiber();
        if (!s_frontend_fiber)
        {
            core_log(RETRO_LOG_ERROR, "ConvertThreadToFiber failed (%lu)\n", GetLastError());
            g_core.engine_start_failed = true;
            return;
        }
    }
    if (!s_engine_fiber)
    {
        /* Explicit reserve: CreateFiber's single size is only the commit and
         * inherits the host exe's (small) default reserve. */
        s_engine_fiber = CreateFiberEx(1 * 1024 * 1024, 8 * 1024 * 1024, 0,
                                       core_engine_fiber_proc, NULL);
        if (!s_engine_fiber)
        {
            core_log(RETRO_LOG_ERROR, "CreateFiberEx failed (%lu)\n", GetLastError());
            g_core.engine_start_failed = true;
            return;
        }
        core_log(RETRO_LOG_INFO, "engine fiber created\n");
    }

    /* Re-read core options when the frontend flags them changed. */
    {
        bool updated = false;
        if (g_core.environ_cb && g_core.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
            core_refresh_options();
    }

    /* Injecting input before engine startup is safe: the window-message
     * handler table is empty and stdControl's scancode map is zeroed until
     * their _Startups run. */
    core_poll_input();

    /* The frontend's framebuffer handle may change every frame, and its
     * compositor leaves a different VAO bound (core-profile draws would no-op
     * without the engine's VAO). Before glewInit (first boot resume), the boot
     * path does its own bind and every gl* pointer here is still NULL. */
    if (s_gl_ready)
    {
        GLint fbo = (GLint)g_core.hw_render.get_current_framebuffer();
        std3D_SetWindowFbo(fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
        std3D_RebindVAO();
    }

    /* A deferred savestate restore (auto-load-state fired before the engine
     * booted) arms itself as soon as the engine has a profile and no
     * save/load/level transition in flight. The engine fiber is parked here
     * between frames -- the same quiesce point the menus operate from. */
    if (g_core.pending_state && g_core.engine_started)
    {
        int rc = jkSession_StateRestore(g_core.pending_state, g_core.pending_state_len);
        if (rc == JKSESSION_STATE_OK)
            core_drop_pending_state("restore queued");
        else if (rc == JKSESSION_STATE_BAD)
        {
            core_show_message("OpenJKDF2: the auto-loaded savestate is unreadable");
            core_drop_pending_state("bad payload");
        }
        else if (--g_core.pending_state_frames <= 0)
        {
            core_show_message("OpenJKDF2: gave up loading the savestate (no player profile became ready)");
            core_drop_pending_state("retry budget exhausted");
        }
    }

    /* Run the engine until it yields: one frame, or one modal-menu iteration. */
    SwitchToFiber(s_engine_fiber);

    /* Overlay the cursor wedge whenever the GUI wants a visible cursor --
     * only after the pointer has been used and not left idle (auto-hide is
     * always on; no OS cursor overlays a frontend viewport). */
    if (s_gl_ready && g_core.engine_started && !jkGame_isDDraw && libretro_GetCursorVisible()
        && g_core.cursor_seen_motion && g_core.cursor_idle_frames < CURSOR_IDLE_HIDE_FRAMES)
        core_draw_cursor();

    /* Don't leak engine GL state into the frontend's own rendering. */
    if (s_gl_ready)
    {
        glBindVertexArray(0);
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_SCISSOR_TEST);
    }

    if (g_core.video_cb)
        g_core.video_cb(RETRO_HW_FRAME_BUFFER_VALID, Window_xSize, Window_ySize, 0);

    /* Audio (M2 consolidation): pull one tick of the engine's OpenAL loopback
     * mix -- exactly 800 frames (48000/60, integer, no drift). The engine
     * opens no real audio device; every audio path (SFX, cutscene, stdMci
     * music) is mixed by OpenAL Soft into this render. Engine fiber and core
     * share one thread, so this is race-free by construction. Silence until
     * the engine's sound startup (or after shutdown / loopback fallback). */
    if (g_core.audio_batch_cb)
    {
        /* Keep the stdMci music stream fed (SDL_mixer decode -> queued AL
         * buffers) before pulling the mix that consumes it. */
        libretro_stdMci_Pump();
        if (!libretro_stdSound_RenderAudio(g_core.audio_out, CORE_AUDIO_FRAMES))
            memset(g_core.audio_out, 0, sizeof(g_core.audio_out));
        g_core.audio_batch_cb(g_core.audio_out, CORE_AUDIO_FRAMES);
    }

    if (s_engine_exit_requested && g_core.environ_cb)
        g_core.environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

RETRO_API void retro_reset(void)
{
    if (!g_core.game_loaded)
        return;

    core_log(RETRO_LOG_INFO, "retro_reset: restarting engine in-process (glctx=%p)\n",
             (void*)wglGetCurrentContext());

    /* Cooperative unwind: the GL context is alive across a reset, so pump
     * real frames while modal menus pop one level per frame, then the engine
     * runs its clean shutdown at the frame boundary. */
    core_quiesce_engine(true);

    /* Re-arm boot: retro_run recreates the fiber lazily and core_boot_engine
     * re-runs OpenJKDF2_Globals_Reset -- the same in-process restart the
     * standalone's own restart loop uses (main.c). */
    core_drop_pending_state("retro_reset");
    memset(g_keyboard_state, 0, sizeof(g_keyboard_state));
    g_core.mouse_abs_x = CORE_BASE_WIDTH / 2;
    g_core.mouse_abs_y = CORE_BASE_HEIGHT / 2;
    g_core.last_mouse_l = 0;
    g_core.last_mouse_r = 0;
    g_core.cursor_seen_motion = false;
    g_core.cursor_idle_frames = 0;
    core_log(RETRO_LOG_INFO, "retro_reset: engine quiesced; fresh boot on next frame\n");
}

/* Savestates: see the envelope/context block above core_derive_basefolder
 * and the jkSession_StateCapture/StateRestore bridge (src/Main/jkSession.c). */
RETRO_API size_t retro_serialize_size(void)
{
    return CORE_STATE_CAP;
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
    if (!data || size < sizeof(core_state_envelope_t))
        return false;
    if (!core_savestate_context_ok())
        return false; /* rewind/run-ahead/netplay snapshot: not supported */

    /* Deterministic padding: zero everything, then let the capture fill in. */
    memset(data, 0, size);
    core_state_envelope_t* env = (core_state_envelope_t*)data;
    memcpy(env->magic, CORE_STATE_MAGIC, sizeof(env->magic));
    env->flags = g_core.is_mots ? 1u : 0u;

    unsigned payload_len = 0;
    if (g_core.engine_started && !g_core.engine_start_failed && !g_core.pending_state
        && jkSession_StateCapture((uint8_t*)data + sizeof(*env),
                                  (unsigned)(size - sizeof(*env)), &payload_len))
    {
        env->payload_len = payload_len;
        core_log(RETRO_LOG_INFO, "savestate captured (%u byte savegame)\n", payload_len);
        return true;
    }

    /* Nothing capturable (menus with no world, MP, engine not booted, a
     * restore still in flight): succeed with an EMPTY state instead of
     * failing. The frontend's load-state flow first snapshots the current
     * state for undo and ABORTS the whole load if that snapshot fails --
     * failing here would make loading a state impossible from exactly the
     * places a user most wants it (the title menu, before the engine is up).
     * Loading an empty state back is an explicit no-op. */
    core_log(RETRO_LOG_INFO, "savestate: nothing to capture here - wrote an empty state\n");
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
    if (!data || size < sizeof(core_state_envelope_t))
        return false;
    const core_state_envelope_t* env = (const core_state_envelope_t*)data;
    if (memcmp(env->magic, CORE_STATE_MAGIC, sizeof(env->magic)) != 0)
    {
        core_log(RETRO_LOG_ERROR, "unserialize: not an OpenJKDF2 state\n");
        return false;
    }
    if (env->payload_len == 0 || env->payload_len > size - sizeof(*env))
    {
        core_log(RETRO_LOG_ERROR, "unserialize: bad payload length %u\n", env->payload_len);
        return false;
    }
    if ((env->flags & 1u) != (g_core.is_mots ? 1u : 0u))
    {
        core_show_message("OpenJKDF2: that state belongs to the other game (DF2 vs MoTS)");
        return false;
    }
    if (env->payload_len == 0)
    {
        /* An empty state (saved with no capturable game -- see serialize). */
        core_show_message("OpenJKDF2: that savestate is empty (it was saved outside a singleplayer level) - nothing restored");
        return true;
    }
    const uint8_t* payload = (const uint8_t*)data + sizeof(*env);

    if (g_core.engine_started)
    {
        int rc = jkSession_StateRestore(payload, env->payload_len);
        if (rc == JKSESSION_STATE_OK)
            return true; /* queued: the engine loads it over the next frames */
        if (rc == JKSESSION_STATE_BAD)
        {
            core_show_message("OpenJKDF2: that state's savegame is unreadable");
            return false;
        }
        /* JKSESSION_STATE_RETRY: engine live but not ready (no player
         * profile picked yet, save/load mid-flight, or multiplayer).
         * Fall through and park it -- the retro_run retry loop arms it the
         * moment the engine becomes ready (e.g. right after the user's
         * profile loads). */
    }

    /* Park a copy; retro_run retries until the engine is ready (covers both
     * the frontend's auto-load-state before the engine boots, and a live
     * load from a not-ready state like the title screen). */
    void* copy = malloc(env->payload_len);
    if (!copy)
        return false;
    memcpy(copy, payload, env->payload_len);
    core_drop_pending_state("superseded by a newer unserialize");
    g_core.pending_state = copy;
    g_core.pending_state_len = env->payload_len;
    g_core.pending_state_frames = CORE_STATE_PENDING_RETRY_FRAMES;
    if (g_core.engine_started)
        core_show_message("OpenJKDF2: state load queued - it applies as soon as the game is ready");
    core_log(RETRO_LOG_INFO, "state restore deferred (engine_started=%d)\n",
             g_core.engine_started ? 1 : 0);
    return true;
}

RETRO_API void* retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port != 0)
        return;
    g_core.port0_device = device;
    core_log(RETRO_LOG_INFO, "port 0 device: %s (%u)\n",
             device == RETRO_DEVICE_KEYBOARD ? "Keyboard + Mouse" :
             device == RETRO_DEVICE_JOYPAD   ? "RetroPad" : "other",
             device);
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
    (void)index; (void)enabled; (void)code;
}
