/*
 * NetSurf HTTP(S) fetcher for PlayStation 4 / OpenOrbis.
 *
 * First functional transport slice:
 *   - GET over http:// and https:// using sceNet/sceSsl/sceHttp
 *   - response status, response headers and streamed response body
 *   - abort support between reads
 *   - keeps the platform TLS verifier enabled: no "accept all" callback,
 *     no sceHttpsDisableOption(), no certificate-validation bypass.
 *
 * Current limitations (deliberate for the first proof):
 *   - synchronous request execution inside fetcher poll()
 *   - no POST/multipart/auth UI yet
 *   - redirects rely on the sceHttp template default until explicit redirect
 *     handling is added and verified on hardware.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#include <libwapcaplet/libwapcaplet.h>

#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/ring.h"

#include "content/fetch.h"
#include "content/fetchers.h"

#define PS4_NET_POOL_SIZE   (256 * 1024)
#define PS4_HTTP_POOL_SIZE  (512 * 1024)
#define PS4_SSL_POOL_SIZE   (512 * 1024)
#define PS4_READ_CHUNK      (64 * 1024)
#define PS4_USER_AGENT      "Mozilla/5.0 (PlayStation 4; NetSurf PS4) NetSurf/PS4"

struct ps4_http_ctx {
    struct fetch *parent;
    nsurl *url;
    char **headers;
    bool aborted;
    bool locked;
    struct ps4_http_ctx *r_next;
    struct ps4_http_ctx *r_prev;
};

static struct ps4_http_ctx *ps4_ring;
static int g_net_pool = -1;
static int g_ssl = -1;
static int g_http = -1;

static void ps4_send(struct ps4_http_ctx *ctx, const fetch_msg *msg)
{
    ctx->locked = true;
    fetch_send_callback(msg, ctx->parent);
    ctx->locked = false;
}

static void ps4_send_error(struct ps4_http_ctx *ctx, const char *message)
{
    fetch_msg msg;
    msg.type = FETCH_ERROR;
    msg.data.error = message;
    ps4_send(ctx, &msg);
}

static bool ps4_transport_init(void)
{
    int ret;

    /* These calls are idempotent enough for a one-process homebrew proof:
     * a negative "already loaded" result is not itself fatal. */
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    (void)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);

    /*
     * The already-working browser downloader intentionally does not treat
     * sceNetInit()'s return as fatal; on a running PS4 networking may already
     * be initialised by the process/system environment.
     */
    ret = sceNetInit();
    if (ret < 0) {
        NSLOG(fetch, WARNING,
              "sceNetInit returned 0x%08x; continuing to pool creation",
              (unsigned)ret);
    }

    g_net_pool = sceNetPoolCreate("netsurf-ps4", PS4_NET_POOL_SIZE, 0);
    if (g_net_pool < 0) {
        NSLOG(fetch, ERROR, "sceNetPoolCreate failed: 0x%08x", (unsigned)g_net_pool);
        return false;
    }

    g_ssl = sceSslInit(PS4_SSL_POOL_SIZE);
    if (g_ssl < 0) {
        NSLOG(fetch, ERROR, "sceSslInit failed: 0x%08x", (unsigned)g_ssl);
        return false;
    }

    g_http = sceHttpInit(g_net_pool, g_ssl, PS4_HTTP_POOL_SIZE);
    if (g_http < 0) {
        NSLOG(fetch, ERROR, "sceHttpInit failed: 0x%08x", (unsigned)g_http);
        return false;
    }

    NSLOG(fetch, INFO, "PS4 native HTTP transport initialised");
    return true;
}

static void ps4_transport_fini(void)
{
    if (g_http >= 0) {
        sceHttpTerm(g_http);
        g_http = -1;
    }
    if (g_ssl >= 0) {
        sceSslTerm(g_ssl);
        g_ssl = -1;
    }
    if (g_net_pool >= 0) {
        sceNetPoolDestroy(g_net_pool);
        g_net_pool = -1;
    }
}

static bool ps4_initialise(lwc_string *scheme)
{
    /*
     * Registration must not touch PS4 network/sysmodule state.  NetSurf calls
     * this while netsurf_init() is still starting, before the browser window
     * exists.  Initialise the transport lazily on the first actual request.
     */
    (void)scheme;
    return true;
}

static bool ps4_acceptable(const nsurl *url)
{
    const char *u = nsurl_access(url);
    return (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0);
}

static void *ps4_setup(struct fetch *parent_fetch,
                       nsurl *url,
                       bool only_2xx,
                       bool downgrade_tls,
                       const char *post_urlenc,
                       const struct fetch_multipart_data *post_multipart,
                       const char **headers)
{
    struct ps4_http_ctx *ctx;

    (void)only_2xx;
    (void)downgrade_tls;
    (void)post_multipart;

    /* First milestone is GET navigation. Do not silently turn a POST into GET. */
    if (post_urlenc != NULL)
        return NULL;

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    ctx->parent = parent_fetch;
    ctx->url = nsurl_ref(url);

    if (headers != NULL) {
        size_t n = 0;
        while (headers[n] != NULL) n++;
        ctx->headers = calloc(n + 1, sizeof(char *));
        if (ctx->headers == NULL) {
            nsurl_unref(ctx->url);
            free(ctx);
            return NULL;
        }
        for (size_t i = 0; i < n; i++) {
            ctx->headers[i] = strdup(headers[i]);
            if (ctx->headers[i] == NULL) {
                while (i > 0) free(ctx->headers[--i]);
                free(ctx->headers);
                nsurl_unref(ctx->url);
                free(ctx);
                return NULL;
            }
        }
    }

    RING_INSERT(ps4_ring, ctx);
    return ctx;
}

static bool ps4_start(void *vctx)
{
    (void)vctx;
    return true;
}

static void ps4_abort(void *vctx)
{
    struct ps4_http_ctx *ctx = vctx;
    ctx->aborted = true;
}

static void ps4_free(void *vctx)
{
    struct ps4_http_ctx *ctx = vctx;
    if (ctx->headers != NULL) {
        for (size_t i = 0; ctx->headers[i] != NULL; i++)
            free(ctx->headers[i]);
        free(ctx->headers);
    }
    nsurl_unref(ctx->url);
    free(ctx);
}

static void ps4_emit_headers(struct ps4_http_ctx *ctx, char *headers, size_t len)
{
    size_t start = 0;

    while (start < len && !ctx->aborted) {
        size_t end = start;
        while (end < len && headers[end] != '\n') end++;
        if (end < len) end++;

        if (end > start) {
            fetch_msg msg;
            msg.type = FETCH_HEADER;
            msg.data.header_or_data.buf = (const uint8_t *)(headers + start);
            msg.data.header_or_data.len = end - start;
            ps4_send(ctx, &msg);
        }
        start = end;
    }
}

static bool ps4_apply_request_headers(int req, struct ps4_http_ctx *ctx)
{
    if (ctx->headers == NULL)
        return true;

    for (size_t i = 0; ctx->headers[i] != NULL; i++) {
        char *copy = strdup(ctx->headers[i]);
        char *colon;
        int ret;

        if (copy == NULL)
            return false;

        colon = strchr(copy, ':');
        if (colon == NULL) {
            free(copy);
            continue;
        }

        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') colon++;

        ret = sceHttpAddRequestHeader(req, copy, colon, 0);
        free(copy);

        if (ret < 0) {
            NSLOG(fetch, WARNING, "sceHttpAddRequestHeader failed: 0x%08x",
                  (unsigned)ret);
        }
    }
    return true;
}

static void ps4_process_one(struct ps4_http_ctx *ctx)
{
    int tpl = -1;
    int conn = -1;
    int req = -1;
    int ret;
    int status = 0;
    const char *url = nsurl_access(ctx->url);
    uint8_t *buf = NULL;

    if (g_http < 0 && !ps4_transport_init()) {
        ps4_send_error(ctx, "PS4: native HTTP transport init failed");
        goto done;
    }

    tpl = sceHttpCreateTemplate(g_http, PS4_USER_AGENT,
                                ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        ps4_send_error(ctx, "PS4: failed to create HTTP template");
        goto done;
    }

    /*
     * IMPORTANT: deliberately no sceHttpsSetSslCallback() and no
     * sceHttpsDisableOption(). The platform's default certificate validation
     * remains authoritative for HTTPS.
     */

    conn = sceHttpCreateConnectionWithURL(tpl, url, true);
    if (conn < 0) {
        NSLOG(fetch, ERROR, "sceHttpCreateConnectionWithURL: 0x%08x",
              (unsigned)conn);
        ps4_send_error(ctx, "PS4: connection creation failed");
        goto done;
    }

    req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0);
    if (req < 0) {
        NSLOG(fetch, ERROR, "sceHttpCreateRequestWithURL: 0x%08x",
              (unsigned)req);
        ps4_send_error(ctx, "PS4: request creation failed");
        goto done;
    }

    if (!ps4_apply_request_headers(req, ctx)) {
        ps4_send_error(ctx, "PS4: request header allocation failed");
        goto done;
    }

    ret = sceHttpSendRequest(req, NULL, 0);
    if (ret < 0) {
        int last_errno = 0;
        (void)sceHttpGetLastErrno(req, &last_errno);
        NSLOG(fetch, ERROR,
              "sceHttpSendRequest failed: 0x%08x errno=0x%08x",
              (unsigned)ret, (unsigned)last_errno);
        ps4_send_error(ctx, "PS4: HTTP/TLS request failed");
        goto done;
    }

    ret = sceHttpGetStatusCode(req, &status);
    if (ret < 0) {
        ps4_send_error(ctx, "PS4: failed to read HTTP status");
        goto done;
    }
    fetch_set_http_code(ctx->parent, status);

    {
        char *all = NULL;
        size_t all_len = 0;
        ret = sceHttpGetAllResponseHeaders(req, &all, &all_len);
        if (ret >= 0 && all != NULL && all_len > 0)
            ps4_emit_headers(ctx, all, all_len);
    }

    if (ctx->aborted)
        goto done;

    buf = malloc(PS4_READ_CHUNK);
    if (buf == NULL) {
        ps4_send_error(ctx, "PS4: out of memory for network buffer");
        goto done;
    }

    while (!ctx->aborted) {
        ret = sceHttpReadData(req, buf, PS4_READ_CHUNK);
        if (ret < 0) {
            int last_errno = 0;
            (void)sceHttpGetLastErrno(req, &last_errno);
            NSLOG(fetch, ERROR,
                  "sceHttpReadData failed: 0x%08x errno=0x%08x",
                  (unsigned)ret, (unsigned)last_errno);
            ps4_send_error(ctx, "PS4: response read failed");
            goto done;
        }
        if (ret == 0)
            break;

        {
            fetch_msg msg;
            msg.type = FETCH_DATA;
            msg.data.header_or_data.buf = buf;
            msg.data.header_or_data.len = (size_t)ret;
            ps4_send(ctx, &msg);
        }
    }

    if (!ctx->aborted) {
        fetch_msg msg;
        msg.type = FETCH_FINISHED;
        ps4_send(ctx, &msg);
    }

done:
    free(buf);
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tpl >= 0) sceHttpDeleteTemplate(tpl);
}

static void ps4_poll(lwc_string *scheme)
{
    struct ps4_http_ctx *ctx;
    struct ps4_http_ctx *saved = NULL;

    (void)scheme;

    while (ps4_ring != NULL) {
        ctx = ps4_ring;
        RING_REMOVE(ps4_ring, ctx);

        if (ctx->locked) {
            RING_INSERT(saved, ctx);
            continue;
        }

        if (!ctx->aborted)
            ps4_process_one(ctx);

        fetch_remove_from_queues(ctx->parent);
        fetch_free(ctx->parent);
    }

    ps4_ring = saved;
}

static int ps4_fdset(lwc_string *scheme,
                     fd_set *read_set,
                     fd_set *write_set,
                     fd_set *error_set)
{
    (void)scheme;
    (void)read_set;
    (void)write_set;
    (void)error_set;
    return -1;
}

static void ps4_finalise(lwc_string *scheme)
{
    (void)scheme;
    ps4_transport_fini();
}

nserror fetch_ps4_register(void)
{
    const struct fetcher_operation_table ops = {
        .initialise = ps4_initialise,
        .acceptable = ps4_acceptable,
        .setup = ps4_setup,
        .start = ps4_start,
        .abort = ps4_abort,
        .free = ps4_free,
        .poll = ps4_poll,
        .fdset = ps4_fdset,
        .finalise = ps4_finalise,
    };
    nserror err;

    err = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
    if (err != NSERROR_OK)
        return err;

    return fetcher_add(lwc_string_ref(corestring_lwc_https), &ops);
}
