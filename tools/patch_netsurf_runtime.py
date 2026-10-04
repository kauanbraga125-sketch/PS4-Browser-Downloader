#!/usr/bin/env python3
"""Asserted patches for the pinned upstream NetSurf tree (PS4 v1.10)."""
from pathlib import Path
import sys

root = Path(sys.argv[1])

def replace(path, old, new):
    file = root / path
    text = file.read_text()
    if text.count(old) != 1:
        raise SystemExit(f'{path}: expected one patch anchor, found {text.count(old)}')
    file.write_text(text.replace(old, new))

replace('frontends/framebuffer/fetch.c',
        '/* table for fetch operations */',
        '#include "framebuffer/ps4_resources.h"\n\n/* table for fetch operations */')
replace('frontends/framebuffer/fetch.c',
        '\t.get_resource_url = get_resource_url,',
        '\t.get_resource_url = get_resource_url,\n'
        '\t.get_resource_data = ps4_get_resource_data,\n'
        '\t.release_resource_data = ps4_release_resource_data,')

# Timers must advance while idle and must not depend on libc clock-id mappings.
replace('frontends/framebuffer/schedule.c', '#include <time.h>',
        '#include <time.h>\n#include <orbis/libkernel.h>')
file = root / 'frontends/framebuffer/schedule.c'
text = file.read_text()
anchor = '/* linked list of scheduled callbacks */'
text = text.replace(anchor, '''static void ps4_schedule_time(struct timeval *tv)
{
    uint64_t us = sceKernelGetProcessTime();
    tv->tv_sec = us / 1000000;
    tv->tv_usec = us % 1000000;
}

''' + anchor)
text = text.replace('gettimeofday(&nscb->tv, NULL);', 'ps4_schedule_time(&nscb->tv);')
text = text.replace('gettimeofday(&tv, NULL);', 'ps4_schedule_time(&tv);')
text = text.replace('nscb = calloc(1, sizeof(struct nscallback));',
                    'nscb = calloc(1, sizeof(struct nscallback));\n'
                    '\tif (nscb == NULL) return NSERROR_NOMEM;')
text = text.replace('timercmp(&tv, &cur_nscb->tv, >)', 'timercmp(&tv, &cur_nscb->tv, >=)')
file.write_text(text)

# A failed error page must never recursively load another HTML error page.
replace('desktop/browser_window.c',
        '\thlcache_handle_release(c);\n\n\tswitch (code) {',
        '''\tif (bw->internal_nav || code == NSERROR_CSS_BASE) {
        browser_window_set_status(bw, message);
        browser_window_stop_throbber(bw);
        hlcache_handle_release(c);
        return NSERROR_OK;
    }

\thlcache_handle_release(c);

\tswitch (code) {''')

# SDL must return regularly even with a held stick and no new OS events.
replace('frontends/framebuffer/gui.c',
        '\t\tif (fbtk_event(fbtk, &event, timeout)) {',
        '\t\tif (timeout < 0 || timeout > 16) timeout = 16;\n'
        '\t\tif (fbtk_event(fbtk, &event, timeout)) {')

# Keep transport errors available even when the page could not be rendered.
replace('frontends/framebuffer/gui.c',
        '\tram_register_surface();',
        '\t(void)freopen("/data/ps4-browser-netsurf.log", "w", stderr);\n'
        '\tsetbuf(stderr, NULL);\n'
        '\tfprintf(stderr, "PS4 Browser NetSurf 1.10: embedded resources, stat ABI fixed\\n");\n'
        '\tram_register_surface();')

# File URLs are still supported, but a 4-KiB Unix mmap alignment assumption
# must not be required to read a small file on a PS4 (16-KiB pages).
replace('utils/config.h', '#define HAVE_MMAP', '#define HAVE_MMAP')
file = root / 'content/fetchers/file/file.c'
text = file.read_text().replace('#include "utils/config.h"',
                               '#include "utils/config.h"\n#undef HAVE_MMAP')
file.write_text(text)
