#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include <glad/gl.h>
#include <pvz2native/config.h>
#include <pvz2native/game_parameters.h>
#include <pvz2native/input/input_queue.h>
#include <pvz2native/pvz2_session.h>

/* The engine renders at a fixed resolution (pvz2_render_size); the window can be
 * any size and the compositor scales to it. So input arrives in window pixels
 * and has to be mapped back into that fixed render space, or a click lands
 * somewhere else once the window is not exactly the render size. Filled from
 * pvz2_render_size() at startup; the window size is tracked on every resize. */
static int g_render_w = 960, g_render_h = 540;
static int g_win_w = 960, g_win_h = 540;

static int to_render_x(int x) { return g_win_w > 0 ? x * g_render_w / g_win_w : x; }
static int to_render_y(int y) { return g_win_h > 0 ? y * g_render_h / g_win_h : y; }

/* Turns one SDL event into a guest input event.
 *
 * The engine has exactly one input channel -- UI_ProcessEvents -- so
 * everything funnels through the queue and is serialised there once per frame.
 *
 * Only ENTER and BACKSPACE are sent as key events, because the engine's
 * CharToKeyCode maps every other keycode to 0 and drops it; printable
 * characters have to arrive as SDL_TEXTINPUT text instead. ESCAPE is
 * deliberately NOT mapped to Android's BACK key: it already means "quit" here,
 * and having one key do both would make the window impossible to close once
 * the game starts consuming input. */
static void feed_input_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_MOUSEBUTTONDOWN:
            if (e->button.button == SDL_BUTTON_LEFT) {
                pvz2_input_push_touch(PVZ2_TOUCH_DOWN, to_render_x(e->button.x), to_render_y(e->button.y));
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e->button.button == SDL_BUTTON_LEFT) {
                pvz2_input_push_touch(PVZ2_TOUCH_UP, to_render_x(e->button.x), to_render_y(e->button.y));
            }
            break;
        case SDL_MOUSEMOTION:
            /* A mouse with no button held has no touch equivalent; sending it
             * would look to the engine like a finger dragging across the
             * screen at all times. */
            if (e->motion.state & SDL_BUTTON_LMASK) {
                pvz2_input_push_touch(PVZ2_TOUCH_MOVE, to_render_x(e->motion.x), to_render_y(e->motion.y));
            }
            break;
        case SDL_TEXTINPUT:
            pvz2_input_push_text(e->text.text);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int code = 0;
            if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) {
                code = PVZ2_KEY_ENTER;
            } else if (e->key.keysym.sym == SDLK_BACKSPACE) {
                code = PVZ2_KEY_DEL;
            }
            if (code != 0) pvz2_input_push_key(code, e->type == SDL_KEYDOWN ? 1 : 0);
            break;
        }
        default:
            break;
    }
}

/* SDL only delivers SDL_TEXTINPUT while text input is active, so this is what
 * makes typing reach the game -- driven by the engine's own
 * Device_ShowKeyboard/Device_HideKeyboard calls. */
static void sync_text_input(void) {
    int wanted = pvz2_input_keyboard_wanted();
    SDL_bool active = SDL_IsTextInputActive();
    if (wanted && !active) {
        SDL_StartTextInput();
    } else if (!wanted && active) {
        SDL_StopTextInput();
    }
}

/* Set once the window exists, so the boot-time pump can present to it. */
static SDL_Window *g_window = NULL;
static int g_quit_requested = 0;

/* Re-reads the window's drawable size and re-renders the game at it: the
 * compositor is told the new size immediately (so THIS frame already fills the
 * window), and the engine re-runs onSurfaceChanged so it re-fits its projection.
 *
 * Note what is deliberately NOT updated here: g_render_w/h. The engine's TOUCH
 * coordinate space is frozen at the launch resolution (pvz2_render_size) --
 * onSurfaceChanged updates the projection but the touch scaler keys off
 * mOrigScreenWidth, which SetWidthHeight sets once at startup and the resize
 * path never revisits. So input must always map window pixels back into that
 * fixed space; making g_render track the window (identity) put every click in
 * the wrong place. g_render stays at the launch size and to_render_* rescales. */
static void update_window_size(void) {
    if (!g_window) return;
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    if (dw > 0 && dh > 0) {
        g_win_w = dw;
        g_win_h = dh;
        pvz2_set_drawable_size(dw, dh);
        pvz2_session_request_resize(dw, dh);
    }
}

/* Closing the window has to take effect immediately. The boot runs for many
 * seconds inside a single guest call that cannot be unwound, so setting a flag
 * would leave the window on screen -- ignoring the close -- until the boot
 * finishes. Exiting straight from the event handler is the only way to make the
 * X button respond during startup; the OS reclaims everything. */
static void handle_quit(void) {
    fflush(stdout);
    exit(0);
}

/* Called from inside long guest calls (see pvz2_session_set_host_pump). The
 * boot sequence runs for minutes in one call, so without draining the message
 * queue the OS marks the window "Not responding" for all of startup. Rate
 * limited because it is invoked on every JIT slice boundary, which during
 * instruction-level tracing is every few dozen ticks. */
static void host_pump(void) {
    static Uint32 last_pump = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last_pump < 100) return;
    last_pump = now;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
            handle_quit();
        } else if (event.type == SDL_WINDOWEVENT &&
                   event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            update_window_size();
        }
        /* Also collected here: the boot runs for seconds inside a single guest
         * call, and anything typed or clicked in that window would otherwise be
         * discarded by this drain rather than reaching the queue. */
        feed_input_event(&event);
    }
    sync_text_input();

    /* The boot takes minutes behind a dark window; without this there is no
     * way to tell a working startup from a hang. Title updates need no GL, so
     * they are safe while the guest owns the context. */
    if (g_window) {
        char status[160];
        char title[224];
        pvz2_session_status(status, sizeof(status));
        SDL_snprintf(title, sizeof(title), "PvZ2Native - %s", status);
        SDL_SetWindowTitle(g_window, title);
    }

    /* Deliberately no SDL_GL_SwapWindow here. The guest owns the GL state, so
     * we cannot redraw, and swapping without drawing just flips between the
     * two buffers -- one holding the colour painted before the boot, the other
     * undefined -- which reads on screen as a dark tone that keeps changing.
     * The single clear+swap done before the boot already painted the window;
     * draining the message queue is all that is needed to keep it alive. */
}

int main(int argc, char **argv) {
    parse_game_parameters(argc, argv);

    printf("pvz2native skeleton build OK\n");
    printf("game_path=%s home_path=%s\n", game_parameters.game_path, game_parameters.home_path);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Read config.ini from the folder the .exe lives in, and resolve the game
     * data paths relative to it (<exe>/lib/...) unless the file overrides them.
     * SDL_GetBasePath returns that folder with a trailing separator. */
    char *base_path = SDL_GetBasePath();
    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base_path ? base_path : "");
    pvz2_config_load(ini_path, base_path ? base_path : "");
    if (base_path) SDL_free(base_path);
    const pvz2_config_t *cfg = pvz2_config();
    printf("so=%s\n", cfg->so_path);
    printf("obb=%s\n", cfg->obb_path);

    /* PvZ2's .so only imports gl* symbols, never egl* -- on real Android the
     * Java-side GLSurfaceView owns EGL context creation and the native side
     * just receives an already-current context in onSurfaceCreated. SDL's
     * GL context here plays that same role. GL2.0 compatibility profile
     * (matches gles_compat's `glad_add_library` config) keeps both the
     * fixed-function pipeline (matrix stack, client-state arrays) GLES1.1
     * code needs and the shader entry points GLES2 code needs. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    /* The window defaults to the engine's render size and is freely resizable;
     * the compositor scales the fixed-resolution scene to whatever size it
     * becomes, and update_window_size keeps input and the composite viewport in
     * step. */
    pvz2_render_size(&g_render_w, &g_render_h);
    g_win_w = g_render_w;
    g_win_h = g_render_h;
    SDL_Window *window = SDL_CreateWindow("PvZ2Native", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           g_render_w, g_render_h,
                                           SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        printf("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);

    /* Cap the frame rate to the display via vsync. The engine advances its
     * simulation from the real wall clock (gettimeofday), not from a frame
     * count, so presenting fewer frames does NOT slow the game down -- it only
     * stops the loop from spinning onDrawFrame thousands of times a second and
     * pinning a core at 100%. That is the single biggest win for a weak CPU like
     * the N2805: it drops the per-second work by roughly the ratio of that
     * uncapped rate to the refresh rate. Adaptive vsync first (no stutter when a
     * frame misses vblank), plain vsync if the driver lacks it. */
    if (SDL_GL_SetSwapInterval(-1) != 0) {
        SDL_GL_SetSwapInterval(1);
    }

    int glad_version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (glad_version == 0) {
        printf("gladLoadGL failed to load any GL functions\n");
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    printf("GL context ready: %s (loaded via glad %d.%d)\n", (const char *)glGetString(GL_VERSION),
           GLAD_VERSION_MAJOR(glad_version), GLAD_VERSION_MINOR(glad_version));

    /* Resolved from config.ini above: <exe>/lib/libPVZ2.so by default, or the
     * [paths] so override. The matching .obb is resolved the same way and read
     * by the VFS layer from the config. */
    const char *so_path = cfg->so_path;

    /* Paint a defined colour before the (multi-minute) boot so the window
     * shows something deliberate rather than uninitialised backbuffer, then
     * let the boot keep the message queue drained through host_pump. */
    g_window = window;
    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);
    SDL_PumpEvents();

    pvz2_session_set_host_pump(host_pump);
    pvz2_session_t *session = pvz2_session_start(so_path);
    /* Nonzero exit when the session never started -- a missing .so, or a build
     * of the game whose addresses we do not have (see src/game/symbols.cpp). */
    int exit_code = session ? 0 : 1;

    if (session) {
        /* The window's real drawable size (HiDPI can differ from the requested
         * size), handed to the compositor before the first frame. */
        update_window_size();

        /* host_pump may already have seen a close during the boot. */
        int running = !g_quit_requested;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                    handle_quit();
                } else if (event.type == SDL_WINDOWEVENT &&
                           event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    update_window_size();
                }
                feed_input_event(&event);
            }
            sync_text_input();
            if (!running || g_quit_requested) break;

            /* The guest renders its scene into an offscreen FBO and composites
             * it to the default framebuffer with a degenerate (zero-width)
             * viewport, so it never actually writes the backbuffer. With
             * double buffering, swapping every frame then alternates between
             * two buffers holding different stale content, which reads on
             * screen as flicker. Clearing here gives a stable window until the
             * composite works; the guest binds its own FBO immediately after,
             * so this does not disturb its rendering. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            /* The guest issues its GL calls into the context made current on
             * this thread, so the frame has to run here, between the pump and
             * the swap -- not on a worker. */
            if (!pvz2_session_frame(session)) break;
            SDL_GL_SwapWindow(window);
        }
        pvz2_session_end(session);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
