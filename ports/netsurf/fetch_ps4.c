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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
#define PS4_HTTP_POOL_SIZE  (2 * 1024 * 1024)
#define PS4_SSL_POOL_SIZE   (2 * 1024 * 1024)
#define PS4_READ_CHUNK      (64 * 1024)
#define PS4_MAX_RESPONSE    (16 * 1024 * 1024)
#define PS4_HTTP_TIMEOUT_US  (10 * 1000 * 1000)
#define PS4_USER_AGENT      "Mozilla/5.0 (PlayStation 4; NetSurf PS4) NetSurf/PS4"

struct ps4_http_ctx {
    struct fetch *parent;
    nsurl *url;
    char **headers;
    char *post_urlenc;
    bool aborted;
    bool locked;
    struct ps4_http_ctx *r_next;
    struct ps4_http_ctx *r_prev;
};

static struct ps4_http_ctx *ps4_ring;
static int g_net_pool = -1;
static int g_ssl = -1;
static int g_http = -1;
static int g_transport_stage = 0;
static int g_transport_error = 0;

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

static void ps4_send_error_code(struct ps4_http_ctx *ctx,
                                const char *stage,
                                int code)
{
    char message[128];
    snprintf(message, sizeof(message), "PS4 %s: 0x%08x",
             stage, (unsigned)code);
    ps4_send_error(ctx, message);
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

    g_transport_stage = 1;
    g_net_pool = sceNetPoolCreate("netsurf-ps4", PS4_NET_POOL_SIZE, 0);
    if (g_net_pool < 0) {
        g_transport_error = g_net_pool;
        NSLOG(fetch, ERROR, "sceNetPoolCreate failed: 0x%08x", (unsigned)g_net_pool);
        return false;
    }

    g_transport_stage = 2;
    g_ssl = sceSslInit(PS4_SSL_POOL_SIZE);
    if (g_ssl < 0) {
        g_transport_error = g_ssl;
        NSLOG(fetch, ERROR, "sceSslInit failed: 0x%08x", (unsigned)g_ssl);
        return false;
    }

    g_transport_stage = 3;
    g_http = sceHttpInit(g_net_pool, g_ssl, PS4_HTTP_POOL_SIZE);
    if (g_http < 0) {
        g_transport_error = g_http;
        NSLOG(fetch, ERROR, "sceHttpInit failed: 0x%08x", (unsigned)g_http);
        return false;
    }

    g_transport_stage = 0;
    g_transport_error = 0;
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

    /*
     * NetSurf passes ordinary HTML form submissions as an already encoded
     * application/x-www-form-urlencoded body.  The old PS4 proof rejected
     * these here, which made any POST form fail before sceHttp was reached.
     * Multipart uploads remain a later milestone; reject only those.
     */
    if (post_multipart != NULL)
        return NULL;

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    ctx->parent = parent_fetch;
    ctx->url = nsurl_ref(url);

    if (post_urlenc != NULL) {
        ctx->post_urlenc = strdup(post_urlenc);
        if (ctx->post_urlenc == NULL) {
            nsurl_unref(ctx->url);
            free(ctx);
            return NULL;
        }
    }

    if (headers != NULL) {
        size_t n = 0;
        while (headers[n] != NULL) n++;
        ctx->headers = calloc(n + 1, sizeof(char *));
        if (ctx->headers == NULL) {
            free(ctx->post_urlenc);
            nsurl_unref(ctx->url);
            free(ctx);
            return NULL;
        }
        for (size_t i = 0; i < n; i++) {
            ctx->headers[i] = strdup(headers[i]);
            if (ctx->headers[i] == NULL) {
                while (i > 0) free(ctx->headers[--i]);
                free(ctx->headers);
                free(ctx->post_urlenc);
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
    free(ctx->post_urlenc);
    nsurl_unref(ctx->url);
    free(ctx);
}

static void ps4_emit_headers(struct ps4_http_ctx *ctx, const char *headers, size_t len)
{
    size_t start = 0;

    while (start < len && !ctx->aborted) {
        size_t end = start;
        size_t line_len;
        char *line;

        while (end < len && headers[end] != '\n')
            end++;
        if (end < len)
            end++;

        line_len = end - start;
        if (line_len == 0) {
            start = end;
            continue;
        }

        /*
         * NetSurf's llcache header parser uses strchr()/strlen() internally.
         * sceHttpGetAllResponseHeaders() gives us one contiguous header blob,
         * so a slice into that blob is NOT NUL-terminated at the end of each
         * line.  In particular, the HTTP status line has no ':'; without a
         * terminator NetSurf can scan into the next header and corrupt its
         * parse.  Always hand the core an independent terminated line.
         */
        line = malloc(line_len + 1);
        if (line == NULL) {
            ps4_send_error(ctx, "PS4: out of memory while parsing headers");
            ctx->aborted = true;
            return;
        }

        memcpy(line, headers + start, line_len);
        line[line_len] = '\0';

        {
            fetch_msg msg;
            msg.type = FETCH_HEADER;
            msg.data.header_or_data.buf = (const uint8_t *)line;
            msg.data.header_or_data.len = line_len;
            ps4_send(ctx, &msg);
        }

        free(line);

        /*
         * A callback can abort the fetch.  Match NetSurf's own fetchers and
         * stop immediately instead of sending more events to an aborted job.
         */
        if (ctx->aborted)
            return;

        start = end;
    }
}

static bool ps4_find_location(const char *headers,
                              size_t len,
                              char *out,
                              size_t out_cap)
{
    size_t start = 0;

    if (out_cap == 0)
        return false;
    out[0] = '\0';

    while (start < len) {
        size_t end = start;
        size_t value_start;
        size_t value_end;

        while (end < len && headers[end] != '\n')
            end++;

        if (end > start + 9 &&
            strncasecmp(headers + start, "Location:", 9) == 0) {
            value_start = start + 9;
            while (value_start < end &&
                   (headers[value_start] == ' ' ||
                    headers[value_start] == '\t')) {
                value_start++;
            }

            value_end = end;
            while (value_end > value_start &&
                   (headers[value_end - 1] == '\r' ||
                    headers[value_end - 1] == '\n' ||
                    headers[value_end - 1] == ' ' ||
                    headers[value_end - 1] == '\t')) {
                value_end--;
            }

            size_t n = value_end - value_start;
            if (n >= out_cap)
                n = out_cap - 1;

            memcpy(out, headers + value_start, n);
            out[n] = '\0';
            return n > 0;
        }

        start = (end < len) ? end + 1 : end;
    }

    return false;
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
    const bool is_post = (ctx->post_urlenc != NULL);
    const size_t post_len = is_post ? strlen(ctx->post_urlenc) : 0;
    uint8_t *buf = NULL;

    if (g_http < 0 && !ps4_transport_init()) {
        char init_error[128];
        snprintf(init_error, sizeof(init_error),
                 "PS4 HTTP init stage %d: 0x%08x",
                 g_transport_stage, (unsigned)g_transport_error);
        ps4_send_error(ctx, init_error);
        goto done;
    }

    tpl = sceHttpCreateTemplate(g_http, PS4_USER_AGENT,
                                ORBIS_HTTP_VERSION_1_1, 1);
    if (tpl < 0) {
        ps4_send_error_code(ctx, "sceHttpCreateTemplate", tpl);
        goto done;
    }

    /*
     * Never leave the framebuffer frontend apparently frozen forever.
     * These are transport-level limits; HTTPS verification remains enabled.
     */
    (void)sceHttpSetResolveTimeOut(tpl, PS4_HTTP_TIMEOUT_US);
    (void)sceHttpSetConnectTimeOut(tpl, PS4_HTTP_TIMEOUT_US);
    (void)sceHttpSetSendTimeOut(tpl, PS4_HTTP_TIMEOUT_US);
    sceHttpSetRecvTimeOut(tpl, PS4_HTTP_TIMEOUT_US);

    /*
     * IMPORTANT: deliberately no sceHttpsSetSslCallback() and no
     * sceHttpsDisableOption(). The platform's default certificate validation
     * remains authoritative for HTTPS.
     */

    conn = sceHttpCreateConnectionWithURL(tpl, url, false);
    if (conn < 0) {
        NSLOG(fetch, ERROR, "sceHttpCreateConnectionWithURL: 0x%08x",
              (unsigned)conn);
        ps4_send_error_code(ctx, "sceHttpCreateConnectionWithURL", conn);
        goto done;
    }

    req = sceHttpCreateRequestWithURL(
        conn,
        is_post ? ORBIS_METHOD_POST : ORBIS_METHOD_GET,
        url,
        is_post ? (uint64_t)post_len : 0);
    if (req < 0) {
        NSLOG(fetch, ERROR, "sceHttpCreateRequestWithURL: 0x%08x",
              (unsigned)req);
        ps4_send_error_code(ctx, "sceHttpCreateRequestWithURL", req);
        goto done;
    }

    if (!ps4_apply_request_headers(req, ctx)) {
        ps4_send_error(ctx, "PS4: request header allocation failed");
        goto done;
    }

    /*
     * Keep the first stable browser milestone simple: no compressed body and
     * no persistent connection. NetSurf receives ordinary HTML bytes and the
     * PS4 HTTP layer has an unambiguous end-of-response.
     */
    (void)sceHttpAddRequestHeader(req, "Accept-Encoding", "identity", 1);
    (void)sceHttpAddRequestHeader(req, "Connection", "close", 1);

    if (is_post) {
        /*
         * HTML forms supplied through post_urlenc are exactly this media type.
         * Passing the length both when creating and sending the request keeps
         * libSceHttp from guessing/chunking the body.
         */
        (void)sceHttpAddRequestHeader(
            req,
            "Content-Type",
            "application/x-www-form-urlencoded",
            1);
        (void)sceHttpSetRequestContentLength(req, (uint64_t)post_len);
    }

    ret = sceHttpSendRequest(
        req,
        is_post ? (const void *)ctx->post_urlenc : NULL,
        is_post ? post_len : 0);
    if (ret < 0) {
        int last_errno = 0;
        (void)sceHttpGetLastErrno(req, &last_errno);
        NSLOG(fetch, ERROR,
              "sceHttpSendRequest failed: 0x%08x errno=0x%08x",
              (unsigned)ret, (unsigned)last_errno);
        ps4_send_error_code(ctx, "sceHttpSendRequest", ret);
        goto done;
    }

    ret = sceHttpGetStatusCode(req, &status);
    if (ret < 0) {
        ps4_send_error_code(ctx, "sceHttpGetStatusCode", ret);
        goto done;
    }
    fetch_set_http_code(ctx->parent, status);

    size_t expected_len = 0;
    size_t received_len = 0;
    int length_known = 0;
    {
        int len_ret = sceHttpGetResponseContentLength(req, &length_known, &expected_len);
        if (len_ret < 0) {
            length_known = 0;
            expected_len = 0;
        }
        if (length_known != 0 && expected_len > PS4_MAX_RESPONSE) {
            ps4_send_error(ctx, "PS4: response too large for stable browser mode");
            goto done;
        }
    }

    {
        char *all = NULL;
        size_t all_len = 0;
        char location[2048];
        bool have_location = false;

        ret = sceHttpGetAllResponseHeaders(req, &all, &all_len);
        if (ret >= 0 && all != NULL && all_len > 0) {
            ps4_emit_headers(ctx, all, all_len);
            have_location = ps4_find_location(all, all_len,
                                              location, sizeof(location));
        }

        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) && have_location) {
            fetch_msg msg;
            msg.type = FETCH_REDIRECT;
            msg.data.redirect = location;
            ps4_send(ctx, &msg);
            goto done;
        }

        if (status == 304) {
            fetch_msg msg;
            msg.type = FETCH_NOTMODIFIED;
            ps4_send(ctx, &msg);
            goto done;
        }
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
            ps4_send_error_code(ctx, "sceHttpReadData", ret);
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

        received_len += (size_t)ret;
        if (received_len > PS4_MAX_RESPONSE) {
            ps4_send_error(ctx, "PS4: streamed response exceeded stable memory limit");
            goto done;
        }
        if (length_known != 0 && expected_len > 0 &&
            received_len >= expected_len) {
            break;
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

    (void)scheme;

    /*
     * Process at most one network job per NetSurf poll.  v2.x drained the
     * entire queue synchronously, so image-heavy/modern pages could hold the
     * UI inside sceHttp for many requests in a row.  Returning to the browser
     * loop between jobs keeps controller input and redraws responsive and
     * greatly reduces long apparent freezes on real hardware.
     */
    if (ps4_ring == NULL)
        return;

    ctx = ps4_ring;
    RING_REMOVE(ps4_ring, ctx);

    if (ctx->locked) {
        RING_INSERT(ps4_ring, ctx);
        return;
    }

    if (!ctx->aborted)
        ps4_process_one(ctx);

    fetch_remove_from_queues(ctx->parent);
    fetch_free(ctx->parent);
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
