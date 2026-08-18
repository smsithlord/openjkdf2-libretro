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

    char basefolder[1024];
    char episode_name[256]; /* ROM filename without extension (for later autostart) */
    char cmdline[512];
    bool is_mots;

    /* Core-owned absolute mouse position in window pixels. */
    int mouse_abs_x;
    int mouse_abs_y;
    int last_mouse_l;
    int last_mouse_r;

    int16_t silence[CORE_AUDIO_FRAMES * 2];
} core_state_t;

static core_state_t g_core;

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

static void core_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers)
{
    (void)key_modifiers;

    uint32_t now = (uint32_t)stdPlatform_GetTimeMsec();

    int scancode = retro_key_to_sdl_scancode(keycode);
    if (scancode > 0 && scancode < 256)
    {
        g_keyboard_state[scancode] = down;
        /* Safe pre-startup: stdControl's scancode map is zeroed until its
         * _Startup, and the window-message handler table below is empty --
         * and boot-time modal dialogs need input to be dismissable. */
        stdControl_SetSDLKeydown(scancode, down ? 1 : 0, now);
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

    int dx = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int dy = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    core_update_mouse_position(dx, dy);

    int l = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) ? 1 : 0;
    int r = input(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) ? 1 : 0;

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

static void core_context_reset(void)
{
    g_core.context_alive = true;
    core_log(RETRO_LOG_INFO, "context_reset\n");
}

static void core_context_destroy(void)
{
    g_core.context_alive = false;
    core_log(RETRO_LOG_WARN, "context_destroy (context loss recovery is not implemented yet)\n");
}

/* ------------------------------------------------------------------------
 * Engine fiber
 *
 * Win32 fibers for now (this platform module is MSVC-only); swap in libco or
 * ucontext for the Linux build later.
 * ------------------------------------------------------------------------ */

static void* s_frontend_fiber;
static void* s_engine_fiber;
static bool s_engine_exit_requested;
static bool s_gl_ready; /* glewInit has run (on the engine fiber); GLEW function
                         * pointers are NULL before that -- calling any gl* from
                         * retro_run earlier is a jump to address 0. */

/* Called from engine code (Window_MessageLoop's LIBRETRO_BUILD patch) and from
 * the engine fiber's own frame loop: hand control back to retro_run. */
void libretro_yield_to_frontend(void)
{
    if (s_frontend_fiber)
        SwitchToFiber(s_frontend_fiber);
}

/* Called from jk_exit (LIBRETRO_BUILD patch): the engine wants the process to
 * exit. Park its fiber forever and let retro_run signal the frontend. */
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
        Window_Main_Loop(); /* one frame: game/menu logic + render */
        if (g_should_exit)
            s_engine_exit_requested = true;
        libretro_yield_to_frontend();
    }
}

/* The engine fiber is parked somewhere inside its loop -- possibly deep in a
 * modal menu -- and cannot be unwound safely; save settings, then drop the
 * fiber and its stack. In-place engine restart is a later milestone. */
static void core_drop_engine_fiber(void)
{
    if (g_core.engine_started && jkPlayer_bHasLoadedSettingsOnce)
        jkPlayer_WriteConf(jkPlayer_playerShortName);
    if (s_engine_fiber)
    {
        DeleteFiber(s_engine_fiber);
        s_engine_fiber = NULL;
    }
    g_core.engine_started = false;
}

/* ------------------------------------------------------------------------
 * libretro API
 * ------------------------------------------------------------------------ */

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    g_core.environ_cb = cb;
    if (!cb)
        return;

    cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &g_core.log);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { g_core.video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { g_core.audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_core.audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { g_core.input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { g_core.input_state_cb = cb; }

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
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
        return false;
    }

    size_t len = strlen(game->path);
    g_core.is_mots = (len > 4) && (_strcmpi(game->path + len - 4, ".goo") == 0);

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

    g_core.mouse_abs_x = CORE_BASE_WIDTH / 2;
    g_core.mouse_abs_y = CORE_BASE_HEIGHT / 2;

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
    core_drop_engine_fiber();
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

    /* Run the engine until it yields: one frame, or one modal-menu iteration. */
    SwitchToFiber(s_engine_fiber);

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

    /* Audio: silence until the M2 consolidation (engine audio currently plays
     * through its own OpenAL device, outside the frontend's pipeline). */
    if (g_core.audio_batch_cb)
        g_core.audio_batch_cb(g_core.silence, CORE_AUDIO_FRAMES);

    if (s_engine_exit_requested && g_core.environ_cb)
        g_core.environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

RETRO_API void retro_reset(void)
{
    /* In-place restart needs a clean engine teardown from a parked fiber;
     * deferred (DESIGN.md M3). */
    core_log(RETRO_LOG_WARN, "retro_reset is not supported yet; reload the content instead\n");
}

/* No save states: the engine has no snapshot mechanism. Native saves live in
 * <basefolder>/player/. */
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void* data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void* data, size_t size) { (void)data; (void)size; return false; }

RETRO_API void* retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port; (void)device;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
    (void)index; (void)enabled; (void)code;
}
