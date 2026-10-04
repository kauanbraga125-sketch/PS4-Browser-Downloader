#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ORBIS_SYSMODULE_INTERNAL_NET 1
#define ORBIS_SYSMODULE_INTERNAL_SSL 2
#define ORBIS_SYSMODULE_INTERNAL_HTTP 3
#define ORBIS_HTTP_VERSION_1_1 2
#define ORBIS_METHOD_GET 0
#define ORBIS_HTTP_CONTENTLEN_EXIST 0
#define ORBIS_HTTP_CONTENTLEN_NOT_FOUND 1
#define ORBIS_HTTP_CONTENTLEN_CHUNK_ENC 2
int sceSysmoduleLoadModuleInternal(int);
int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttpInit(int,int,size_t);
int sceHttpTerm(int);
int sceHttpCreateTemplate(int,const char*,int,int);
int sceHttpSetResolveTimeOut(int,unsigned);
int sceHttpSetConnectTimeOut(int,unsigned);
int sceHttpSetSendTimeOut(int,unsigned);
void sceHttpSetRecvTimeOut(int,unsigned);
void sceHttpSetAutoRedirect(int,int);
int sceHttpCreateConnectionWithURL(int,const char*,bool);
int sceHttpCreateRequestWithURL(int,int,const char*,uint64_t);
int sceHttpAddRequestHeader(int,const char*,const char*,int);
int sceHttpSendRequest(int,const void*,size_t);
int sceHttpGetLastErrno(int,int*);
int sceHttpGetStatusCode(int,int*);
int sceHttpGetResponseContentLength(int,int*,size_t*);
int sceHttpGetAllResponseHeaders(int,char**,size_t*);
int sceHttpReadData(int,void*,unsigned);
int sceHttpDeleteRequest(int);
int sceHttpDeleteConnection(int);
int sceHttpDeleteTemplate(int);
uint64_t sceKernelGetProcessTime(void);
