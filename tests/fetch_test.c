/* Runs the actual PS4 fetcher against deterministic sceHttp responses. */
#include <assert.h>
#include <stdarg.h>
#include "../ports/netsurf/fetch_ps4.c"

struct nsurl { const char *text; int refs; };
struct fetch { struct ps4_http_ctx *ctx; int status, finished, errors, redirects; size_t bytes; };
static int response_status, response_kind, sends, reads, deletes, fail_ssl;
static size_t response_size, body_size, body_pos;
static bool abort_data;
static char response_headers[300];
static uint64_t clock_us;
const char *nsurl_access(const nsurl *u) { return u->text; }
nsurl *nsurl_ref(nsurl *u) { u->refs++; return u; }
void nsurl_unref(nsurl *u) { u->refs--; }
void nslog_log(const char *f,const char *fn,int l,const char *fmt,...) {}
nserror fetch_set_http_code(struct fetch *f,http_response_code c) { f->status=c; return NSERROR_OK; }
void fetch_send_callback(const fetch_msg *m,struct fetch *f) {
    switch(m->type) {
    case FETCH_DATA: f->bytes += m->data.header_or_data.len; if(abort_data) ps4_abort(f->ctx); break;
    case FETCH_FINISHED: f->finished++; break;
    case FETCH_ERROR: f->errors++; break;
    case FETCH_REDIRECT: f->redirects++; assert(strcmp(m->data.redirect,"/next")==0); break;
    default: break;
    }
}
void fetch_remove_from_queues(struct fetch *f) {}
void fetch_free(struct fetch *f) { ps4_free(f->ctx); f->ctx=NULL; }
int sceSysmoduleLoadModuleInternal(int a) { return 0; }
int sceNetInit(void) { return 0; }
int sceNetPoolCreate(const char *s,int a,int b) { return 1; }
int sceNetPoolDestroy(int a) { return 0; }
int sceSslInit(size_t a) { return fail_ssl ? -77 : 2; }
int sceSslTerm(int a) { return 0; }
int sceHttpInit(int a,int b,size_t c) { return 3; }
int sceHttpTerm(int a) { return 0; }
int sceHttpCreateTemplate(int a,const char*b,int c,int d) { return 4; }
int sceHttpSetResolveTimeOut(int a,unsigned b) { return 0; }
int sceHttpSetConnectTimeOut(int a,unsigned b) { return 0; }
int sceHttpSetSendTimeOut(int a,unsigned b) { return 0; }
void sceHttpSetRecvTimeOut(int a,unsigned b) {}
void sceHttpSetAutoRedirect(int a,int b) { assert(b==0); }
int sceHttpCreateConnectionWithURL(int a,const char*b,bool c) { return 5; }
int sceHttpCreateRequestWithURL(int a,int b,const char*c,uint64_t d) { return 6; }
int sceHttpAddRequestHeader(int a,const char*b,const char*c,int d) { return 0; }
int sceHttpSendRequest(int a,const void*b,size_t c) { sends++; return 0; }
int sceHttpGetLastErrno(int a,int*b) { *b=0; return 0; }
int sceHttpGetStatusCode(int a,int*b) { *b=response_status; return 0; }
int sceHttpGetResponseContentLength(int a,int*b,size_t*c) { *b=response_kind; *c=response_size; return 0; }
int sceHttpGetAllResponseHeaders(int a,char**b,size_t*c) { *b=response_headers; *c=strlen(*b); return 0; }
int sceHttpReadData(int a,void*b,unsigned c) {
    reads++;
    if(body_pos==body_size) return 0;
    size_t n=body_size-body_pos; if(n>c)n=c;
    memset(b,'a',n); body_pos+=n; return (int)n;
}
int sceHttpDeleteRequest(int a) { deletes++; return 0; }
int sceHttpDeleteConnection(int a) { return 0; }
int sceHttpDeleteTemplate(int a) { return 0; }
uint64_t sceKernelGetProcessTime(void) { return clock_us++; }

static nsurl url={"https://www.google.com/?gbv=1",0};
static struct fetch new_fetch(void) {
    struct fetch f={0};
    return f;
}
static void setup(struct fetch *f) {
    f->ctx=ps4_setup(f,&url,false,false,NULL,NULL,NULL);
    assert(f->ctx);
}
static void reset(int kind,size_t advertised,size_t actual) {
    assert(ps4_ring==NULL && url.refs==0);
    response_kind=kind; response_size=advertised; body_size=actual; body_pos=0;
    response_status=200; sends=reads=deletes=0; abort_data=false; fail_ssl=0;
    strcpy(response_headers,"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n");
}
int main(void) {
    struct fetch f;
    reset(ORBIS_HTTP_CONTENTLEN_EXIST,5,5); f=new_fetch(); setup(&f);
    ps4_poll(NULL); assert(sends==0 && f.ctx); /* queued is not active */
    ps4_start(f.ctx); ps4_poll(NULL);
    assert(f.bytes==5 && f.finished==1 && f.errors==0 && reads==1 && !f.ctx);

    reset(ORBIS_HTTP_CONTENTLEN_EXIST,0,0); f=new_fetch(); setup(&f);
    response_status=204; ps4_start(f.ctx); ps4_poll(NULL);
    assert(f.finished==1 && reads==0);

    reset(ORBIS_HTTP_CONTENTLEN_CHUNK_ENC,0,70000); f=new_fetch(); setup(&f);
    ps4_start(f.ctx); ps4_poll(NULL);
    assert(f.finished==1 && f.bytes==70000 && reads==3);

    reset(ORBIS_HTTP_CONTENTLEN_EXIST,9,3); f=new_fetch(); setup(&f);
    ps4_start(f.ctx); ps4_poll(NULL);
    assert(f.errors==1 && f.finished==0 && deletes==1);

    reset(ORBIS_HTTP_CONTENTLEN_EXIST,3,3); f=new_fetch(); setup(&f);
    response_status=302; strcpy(response_headers,"HTTP/1.1 302 Found\r\nLocation: /next\r\n");
    ps4_start(f.ctx); ps4_poll(NULL); assert(f.redirects==1 && reads==0);

    reset(ORBIS_HTTP_CONTENTLEN_EXIST,70000,70000); f=new_fetch(); setup(&f);
    abort_data=true; ps4_start(f.ctx); ps4_poll(NULL);
    assert(f.bytes==65536 && f.finished==0 && f.errors==0 && !f.ctx);

    reset(ORBIS_HTTP_CONTENTLEN_EXIST,3,3); f=new_fetch(); setup(&f);
    ps4_abort(f.ctx); ps4_poll(NULL); assert(sends==0 && !f.ctx);

    reset(ORBIS_HTTP_CONTENTLEN_EXIST,3,3); f=new_fetch(); setup(&f);
    ps4_transport_fini(); fail_ssl=1; ps4_start(f.ctx); ps4_poll(NULL);
    assert(f.errors==1 && g_net_pool==-1 && g_ssl==-1 && g_http==-1);
    assert(url.refs==0);
    puts("PASS: queue, exact length, 204, chunks, truncation, redirect, abort and init cleanup");
}
