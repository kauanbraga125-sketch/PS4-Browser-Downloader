/*
 * PS4 SDL2 surface for libnsfb.
 *
 * Uses the SDL window-surface path that is already proven by the working
 * PS4 Browser Downloader on hardware: SDL_GetWindowSurface() followed by
 * direct software drawing and SDL_UpdateWindowSurface().
 *
 * It does NOT call Sony's WebBrowser/WebView APIs.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"

struct ps4_sdl2_surface {
    SDL_Window *window;
    SDL_Surface *surface;
    SDL_Joystick *pad;
    int pointer_x;
    int pointer_y;
};

static enum nsfb_key_code_e map_key(SDL_Keycode key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return (enum nsfb_key_code_e)(NSFB_KEY_a + (key - SDLK_a));
    if (key >= SDLK_0 && key <= SDLK_9)
        return (enum nsfb_key_code_e)(NSFB_KEY_0 + (key - SDLK_0));

    switch (key) {
    case SDLK_BACKSPACE: return NSFB_KEY_BACKSPACE;
    case SDLK_TAB: return NSFB_KEY_TAB;
    case SDLK_RETURN: return NSFB_KEY_RETURN;
    case SDLK_ESCAPE: return NSFB_KEY_ESCAPE;
    case SDLK_SPACE: return NSFB_KEY_SPACE;
    case SDLK_DELETE: return NSFB_KEY_DELETE;
    case SDLK_UP: return NSFB_KEY_UP;
    case SDLK_DOWN: return NSFB_KEY_DOWN;
    case SDLK_LEFT: return NSFB_KEY_LEFT;
    case SDLK_RIGHT: return NSFB_KEY_RIGHT;
    case SDLK_PAGEUP: return NSFB_KEY_PAGEUP;
    case SDLK_PAGEDOWN: return NSFB_KEY_PAGEDOWN;
    case SDLK_HOME: return NSFB_KEY_HOME;
    case SDLK_END: return NSFB_KEY_END;
    case SDLK_MINUS: return NSFB_KEY_MINUS;
    case SDLK_EQUALS: return NSFB_KEY_EQUALS;
    case SDLK_PERIOD: return NSFB_KEY_PERIOD;
    case SDLK_SLASH: return NSFB_KEY_SLASH;
    default: return NSFB_KEY_UNKNOWN;
    }
}

static void present(nsfb_t *nsfb)
{
    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)nsfb->surface_priv;

    if (s == NULL || s->window == NULL || s->surface == NULL)
        return;

    SDL_UpdateWindowSurface(s->window);
}

static bool ps4_copy(nsfb_t *nsfb, nsfb_bbox_t *srcbox, nsfb_bbox_t *dstbox)
{
    const int width = srcbox->x1 - srcbox->x0;
    const int height = srcbox->y1 - srcbox->y0;
    if (width <= 0 || height <= 0)
        return true;

    uint8_t *base = (uint8_t *)nsfb->ptr;
    const int pitch = nsfb->linelen;
    const size_t row_bytes = (size_t)width * 4;

    if (dstbox->y0 > srcbox->y0) {
        for (int y = height - 1; y >= 0; --y) {
            memmove(base + (dstbox->y0 + y) * pitch + dstbox->x0 * 4,
                    base + (srcbox->y0 + y) * pitch + srcbox->x0 * 4,
                    row_bytes);
        }
    } else {
        for (int y = 0; y < height; ++y) {
            memmove(base + (dstbox->y0 + y) * pitch + dstbox->x0 * 4,
                    base + (srcbox->y0 + y) * pitch + srcbox->x0 * 4,
                    row_bytes);
        }
    }

    return true;
}

static int bind_window_surface(nsfb_t *nsfb, struct ps4_sdl2_surface *s)
{
    s->surface = SDL_GetWindowSurface(s->window);
    if (s->surface == NULL)
        return -1;

    if (s->surface->format == NULL || s->surface->format->BytesPerPixel != 4)
        return -1;

    nsfb->width = s->surface->w;
    nsfb->height = s->surface->h;
    nsfb->bpp = 32;
    nsfb->format = NSFB_FMT_XRGB8888;
    nsfb->ptr = s->surface->pixels;
    nsfb->linelen = s->surface->pitch;

    select_plotters(nsfb);
    nsfb->plotter_fns->copy = ps4_copy;

    return 0;
}

static int ps4_geometry(nsfb_t *nsfb, int width, int height,
                        enum nsfb_format_e format)
{
    (void)format;

    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    nsfb->width = width;
    nsfb->height = height;
    nsfb->bpp = 32;
    nsfb->format = NSFB_FMT_XRGB8888;

    select_plotters(nsfb);
    nsfb->plotter_fns->copy = ps4_copy;

    if (nsfb->surface_priv != NULL) {
        struct ps4_sdl2_surface *s =
            (struct ps4_sdl2_surface *)nsfb->surface_priv;
        if (s->surface != NULL) {
            nsfb->width = s->surface->w;
            nsfb->height = s->surface->h;
            nsfb->ptr = s->surface->pixels;
            nsfb->linelen = s->surface->pitch;
        }
    }

    return 0;
}

static int ps4_initialise(nsfb_t *nsfb)
{
    if (nsfb->surface_priv != NULL)
        return -1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
        return -1;

    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)calloc(1, sizeof(*s));
    if (s == NULL) {
        SDL_Quit();
        return -1;
    }

    nsfb->surface_priv = s;

    if (nsfb->width <= 0) nsfb->width = 1920;
    if (nsfb->height <= 0) nsfb->height = 1080;

    s->window = SDL_CreateWindow("NetSurf PS4",
                                 SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 nsfb->width,
                                 nsfb->height,
                                 0);
    if (s->window == NULL)
        goto fail;

    if (bind_window_surface(nsfb, s) != 0)
        goto fail;

    memset(nsfb->ptr, 0xff, (size_t)nsfb->linelen * (size_t)nsfb->height);

    s->pointer_x = nsfb->width / 2;
    s->pointer_y = nsfb->height / 2;

    if (SDL_NumJoysticks() > 0)
        s->pad = SDL_JoystickOpen(0);

    SDL_ShowCursor(SDL_DISABLE);
    present(nsfb);
    return 0;

fail:
    if (s->pad) SDL_JoystickClose(s->pad);
    if (s->window) SDL_DestroyWindow(s->window);
    free(s);
    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    SDL_Quit();
    return -1;
}

static int ps4_finalise(nsfb_t *nsfb)
{
    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)nsfb->surface_priv;

    if (s != NULL) {
        if (s->pad) SDL_JoystickClose(s->pad);
        if (s->window) SDL_DestroyWindow(s->window);
        free(s);
    }

    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    SDL_Quit();
    return 0;
}

static bool ps4_emit_stick_motion(nsfb_t *nsfb,
                                   struct ps4_sdl2_surface *s,
                                   nsfb_event_t *event)
{
    if (s == NULL || s->pad == NULL)
        return false;

    static Uint32 last_move_ms;
    const Uint32 now = SDL_GetTicks();

    /* Cap pointer updates to roughly 60 Hz. */
    if ((Uint32)(now - last_move_ms) < 16)
        return false;

    SDL_JoystickUpdate();
    const int ax = SDL_JoystickGetAxis(s->pad, 0);
    const int ay = SDL_JoystickGetAxis(s->pad, 1);
    const int deadzone = 7000;

    int dx = 0;
    int dy = 0;

    if (abs(ax) > deadzone) {
        int speed = 1 + (abs(ax) - deadzone) / 5000;
        if (speed > 6) speed = 6;
        dx = (ax < 0) ? -speed : speed;
    }
    if (abs(ay) > deadzone) {
        int speed = 1 + (abs(ay) - deadzone) / 5000;
        if (speed > 6) speed = 6;
        dy = (ay < 0) ? -speed : speed;
    }

    if (dx == 0 && dy == 0)
        return false;

    last_move_ms = now;
    s->pointer_x += dx;
    s->pointer_y += dy;

    if (s->pointer_x < 0) s->pointer_x = 0;
    if (s->pointer_y < 0) s->pointer_y = 0;
    if (s->pointer_x >= nsfb->width) s->pointer_x = nsfb->width - 1;
    if (s->pointer_y >= nsfb->height) s->pointer_y = nsfb->height - 1;

    event->type = NSFB_EVENT_MOVE_ABSOLUTE;
    event->value.vector.x = s->pointer_x;
    event->value.vector.y = s->pointer_y;
    event->value.vector.z = 0;
    return true;
}

static bool ps4_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)nsfb->surface_priv;
    SDL_Event in;
    int got = 0;

    /*
     * Axis events are edge/change driven in SDL. Polling the held stick here
     * makes the PS4 pointer move continuously and allows acceleration.
     */
    if (ps4_emit_stick_motion(nsfb, s, event))
        return true;

    if (timeout == 0) {
        got = SDL_PollEvent(&in);
    } else if (timeout > 0) {
        got = SDL_WaitEventTimeout(&in, timeout);
        if (!got) {
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_TIMEOUT;
            return true;
        }
    } else {
        got = SDL_WaitEvent(&in);
    }

    if (!got)
        return false;

    event->type = NSFB_EVENT_NONE;

    switch (in.type) {
    case SDL_QUIT:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        event->type = (in.type == SDL_KEYDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = map_key(in.key.keysym.sym);
        return event->value.keycode != NSFB_KEY_UNKNOWN;

    case SDL_MOUSEMOTION:
        s->pointer_x = in.motion.x;
        s->pointer_y = in.motion.y;
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = s->pointer_x;
        event->value.vector.y = s->pointer_y;
        event->value.vector.z = 0;
        return true;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        event->type = (in.type == SDL_MOUSEBUTTONDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = NSFB_KEY_MOUSE_1;
        return true;

    case SDL_JOYAXISMOTION:
        return ps4_emit_stick_motion(nsfb, s, event);

    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
        event->type = (in.type == SDL_JOYBUTTONDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;

        if (in.jbutton.button == 0) {
            event->value.keycode = NSFB_KEY_MOUSE_1;
            return true;
        }

        if (in.jbutton.button == 1) {
            event->value.keycode = NSFB_KEY_ESCAPE;
            return true;
        }

        return false;

    default:
        return false;
    }
}

static int ps4_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if (cursor != NULL && cursor->plotted &&
        nsfb_plot_bbox_intersect(box, &cursor->loc))
        nsfb_cursor_clear(nsfb, cursor);
    return 0;
}

static int ps4_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    if (cursor != NULL && cursor->plotted)
        nsfb_cursor_clear(nsfb, cursor);
    if (cursor != NULL)
        nsfb_cursor_plot(nsfb, cursor);
    present(nsfb);
    return 0;
}

static int ps4_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)box;
    if (nsfb->cursor != NULL && !nsfb->cursor->plotted)
        nsfb_cursor_plot(nsfb, nsfb->cursor);
    present(nsfb);
    return 0;
}

static const nsfb_surface_rtns_t ps4_sdl2_rtns = {
    .initialise = ps4_initialise,
    .finalise = ps4_finalise,
    .input = ps4_input,
    .claim = ps4_claim,
    .update = ps4_update,
    .cursor = ps4_cursor,
    .geometry = ps4_geometry,
};

NSFB_SURFACE_DEF(ps4, NSFB_SURFACE_SDL, &ps4_sdl2_rtns)
