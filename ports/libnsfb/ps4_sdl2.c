/*
 * PS4 SDL2 surface for libnsfb.
 *
 * This is intentionally a small first-port surface:
 * - 32-bit XRGB framebuffer only
 * - SDL2 window/renderer/streaming texture
 * - DualShock-style joystick mapping: X = click, O = browser-back key
 * - left stick moves a software pointer
 *
 * It does NOT call any Sony WebBrowser/WebView API.
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
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Joystick *pad;
    uint8_t *pixels;
    size_t pixels_size;
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
    struct ps4_sdl2_surface *s = (struct ps4_sdl2_surface *)nsfb->surface_priv;
    if (s == NULL || s->texture == NULL || s->renderer == NULL)
        return;

    SDL_UpdateTexture(s->texture, NULL, s->pixels, nsfb->linelen);
    SDL_RenderClear(s->renderer);
    SDL_RenderCopy(s->renderer, s->texture, NULL, NULL);
    SDL_RenderPresent(s->renderer);
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

static int rebuild_pixels(nsfb_t *nsfb, struct ps4_sdl2_surface *s)
{
    const size_t needed = (size_t)nsfb->width * (size_t)nsfb->height * 4;
    uint8_t *p = (uint8_t *)realloc(s->pixels, needed);
    if (p == NULL)
        return -1;

    s->pixels = p;
    s->pixels_size = needed;
    memset(s->pixels, 0xff, needed);

    if (s->texture != NULL) {
        SDL_DestroyTexture(s->texture);
        s->texture = NULL;
    }

    s->texture = SDL_CreateTexture(s->renderer,
                                   SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   nsfb->width,
                                   nsfb->height);
    if (s->texture == NULL)
        return -1;

    nsfb->ptr = s->pixels;
    nsfb->linelen = nsfb->width * 4;
    nsfb->bpp = 32;
    return 0;
}

static int ps4_geometry(nsfb_t *nsfb, int width, int height,
                        enum nsfb_format_e format)
{
    if (format != NSFB_FMT_XRGB8888 && format != NSFB_FMT_XBGR8888)
        format = NSFB_FMT_XRGB8888;

    nsfb->width = width;
    nsfb->height = height;
    nsfb->format = format;
    nsfb->bpp = 32;

    select_plotters(nsfb);
    nsfb->plotter_fns->copy = ps4_copy;

    if (nsfb->surface_priv != NULL) {
        struct ps4_sdl2_surface *s =
            (struct ps4_sdl2_surface *)nsfb->surface_priv;
        if (rebuild_pixels(nsfb, s) != 0)
            return -1;
    }

    return 0;
}

static int ps4_initialise(nsfb_t *nsfb)
{
    if (nsfb->surface_priv != NULL)
        return -1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_JOYSTICK) != 0)
        return -1;

    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;

    nsfb->surface_priv = s;
    nsfb->bpp = 32;
    nsfb->format = NSFB_FMT_XRGB8888;

    select_plotters(nsfb);
    nsfb->plotter_fns->copy = ps4_copy;

    s->window = SDL_CreateWindow("NetSurf PS4",
                                 SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 nsfb->width,
                                 nsfb->height,
                                 0);
    if (s->window == NULL)
        goto fail;

    s->renderer = SDL_CreateRenderer(s->window, -1, SDL_RENDERER_SOFTWARE);
    if (s->renderer == NULL)
        goto fail;

    if (rebuild_pixels(nsfb, s) != 0)
        goto fail;

    s->pointer_x = nsfb->width / 2;
    s->pointer_y = nsfb->height / 2;

    if (SDL_NumJoysticks() > 0)
        s->pad = SDL_JoystickOpen(0);

    SDL_ShowCursor(SDL_DISABLE);
    present(nsfb);
    return 0;

fail:
    if (s->pad) SDL_JoystickClose(s->pad);
    if (s->texture) SDL_DestroyTexture(s->texture);
    if (s->renderer) SDL_DestroyRenderer(s->renderer);
    if (s->window) SDL_DestroyWindow(s->window);
    free(s->pixels);
    free(s);
    nsfb->surface_priv = NULL;
    SDL_Quit();
    return -1;
}

static int ps4_finalise(nsfb_t *nsfb)
{
    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)nsfb->surface_priv;

    if (s != NULL) {
        if (s->pad) SDL_JoystickClose(s->pad);
        if (s->texture) SDL_DestroyTexture(s->texture);
        if (s->renderer) SDL_DestroyRenderer(s->renderer);
        if (s->window) SDL_DestroyWindow(s->window);
        free(s->pixels);
        free(s);
    }

    nsfb->surface_priv = NULL;
    nsfb->ptr = NULL;
    SDL_Quit();
    return 0;
}

static uint32_t wake_timer(uint32_t interval, void *opaque)
{
    (void)interval;
    (void)opaque;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_USEREVENT;
    SDL_PushEvent(&ev);
    return 0;
}

static bool ps4_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct ps4_sdl2_surface *s =
        (struct ps4_sdl2_surface *)nsfb->surface_priv;
    SDL_Event in;
    int got = 0;
    SDL_TimerID timer = 0;

    if (timeout == 0) {
        got = SDL_PollEvent(&in);
    } else {
        if (timeout > 0)
            timer = SDL_AddTimer((uint32_t)timeout, wake_timer, NULL);
        got = SDL_WaitEvent(&in);
        if (timer != 0 && (!got || in.type != SDL_USEREVENT))
            SDL_RemoveTimer(timer);
    }

    if (!got)
        return false;

    event->type = NSFB_EVENT_NONE;

    switch (in.type) {
    case SDL_QUIT:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;

    case SDL_USEREVENT:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_TIMEOUT;
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

    case SDL_JOYAXISMOTION: {
        const int deadzone = 9000;
        int dx = 0;
        int dy = 0;
        if (in.jaxis.axis == 0 && abs(in.jaxis.value) > deadzone)
            dx = in.jaxis.value / 5000;
        if (in.jaxis.axis == 1 && abs(in.jaxis.value) > deadzone)
            dy = in.jaxis.value / 5000;
        if (dx == 0 && dy == 0)
            return false;

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

    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
        event->type = (in.type == SDL_JOYBUTTONDOWN) ?
            NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        if (in.jbutton.button == 0) {
            /* Cross / X = activate focused/cursor target */
            event->value.keycode = NSFB_KEY_MOUSE_1;
            return true;
        }
        if (in.jbutton.button == 1) {
            /* Circle / O = patched by NetSurf PS4 frontend as history back */
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
