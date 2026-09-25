/* Executes the EXACT SDK low-level function, extracted with SHA256 guards.
 * Only OS/TLS dependencies are stubbed; no copy of the connect state machine. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef EXPECT_PATCHED
#define EXPECT_PATCHED 0
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 115
#define EALREADY 114
#define ENETUNREACH 101
#define ECONNREFUSED 111
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
typedef unsigned mock_fd_set;
#undef FD_ZERO
#undef FD_SET
#undef FD_ISSET
#define fd_set mock_fd_set
#define FD_ZERO(p) (*(p)=0)
#define FD_SET(n,p) (*(p)|=1u<<(n))
#define FD_ISSET(n,p) ((*(p)&(1u<<(n)))!=0)
typedef unsigned mock_socklen_t;
#define socklen_t mock_socklen_t
struct mock_timeval { long tv_sec, tv_usec; };
#define timeval mock_timeval
typedef int esp_err_t;
enum { ESP_OK=0, ESP_TLS_INIT=0, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_FAIL, ESP_TLS_DONE };
enum { ESP_TLS_ERR_TYPE_ESP, ESP_TLS_ERR_TYPE_SYSTEM, ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST=0x8004, ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED=0x8005 };
#define SOL_SOCKET 1
#define SO_ERROR 4
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct { bool is_plain_tcp, non_block; int timeout_ms; } esp_tls_cfg_t;
typedef struct { int esp, system; } errors;
typedef struct { int conn_state, sockfd; bool is_tls; fd_set rset,wset; errors *error_handle; void (*read)(void),(*write)(void); } esp_tls_t;
#define ESP_INT_EVENT_TRACKER_CAPTURE(h,type,value) ((type)==ESP_TLS_ERR_TYPE_ESP ? ((h)->esp=(value)) : ((h)->system=(value)))
static unsigned checks, polls, setups, handshakes;
static int select_error, socket_error, getopt_error, initial_error;
static bool first_timeout, bad_ready, null_timeout;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void _esp_tls_net_init(esp_tls_t *t) { (void)t; }
static void tcp_read(void) {} static void tcp_write(void) {}
static void _esp_tls_read(void) {} static void _esp_tls_write(void) {}
static int tcp_connect(const char *h,int n,int p,const esp_tls_cfg_t *c,errors *e,int *fd)
{ (void)h;(void)n;(void)p;(void)c; *fd=3; if(initial_error){e->system=initial_error;return ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST;} return ESP_OK; }
static void ms_to_timeval(int ms, struct timeval *t) { t->tv_sec=ms/1000;t->tv_usec=(ms%1000)*1000; }
static int mock_select(int nfds,fd_set *r,fd_set *w,void *x,struct timeval *tv)
{
    (void)x;CHECK(nfds==4);++polls;
    if(!tv) null_timeout=true; else CHECK(tv->tv_sec==0 && tv->tv_usec==1000);
    if(select_error){errno=select_error;*r=*w=0;select_error=0;return -1;}
    if((first_timeout && polls==1) || (!FD_ISSET(3,r) && !FD_ISSET(3,w))){*r=*w=0;return 0;}
    *r=0;if(bad_ready){*w=0;return 1;} *w=1u<<3;return 1;
}
static int mock_getsockopt(int fd,int level,int opt,int *e,socklen_t *len)
{ CHECK(fd==3 && level==SOL_SOCKET && opt==SO_ERROR && *len==sizeof(int));if(getopt_error){errno=getopt_error;getopt_error=0;return -1;}*e=socket_error;return 0; }
static int create_ssl_handle(const char *h,int n,const esp_tls_cfg_t *c,esp_tls_t *t)
{(void)h;(void)n;(void)c;(void)t;++setups;return ESP_OK;}
static int esp_tls_handshake(esp_tls_t *t,const esp_tls_cfg_t *c)
{(void)c;++handshakes;t->conn_state=ESP_TLS_DONE;return 1;}
#define select mock_select
#define getsockopt mock_getsockopt
#include SDK_EXTRACT
static errors captured;
static esp_tls_t tls;
static esp_tls_cfg_t cfg={false,true,1};
static void reset(void)
{ polls=setups=handshakes=0;select_error=socket_error=getopt_error=initial_error=0;first_timeout=bad_ready=null_timeout=false;memset(&tls,0,sizeof(tls));memset(&captured,0,sizeof(captured));tls.error_handle=&captured;cfg.timeout_ms=1; }
static int poll_connect(void) { return esp_tls_low_level_conn("192.0.2.1",9,7844,&cfg,&tls); }
int main(void)
{
    reset();first_timeout=true;CHECK(poll_connect()==0);CHECK(setups==0);
#if EXPECT_PATCHED
    CHECK(poll_connect()==1);CHECK(setups==1 && handshakes==1);
    reset();select_error=EINTR;CHECK(poll_connect()==0);CHECK(setups==0);CHECK(poll_connect()==1);
    reset();select_error=EAGAIN;CHECK(poll_connect()==0);CHECK(poll_connect()==1);
    reset();select_error=EBADF;CHECK(poll_connect()==-1);CHECK(captured.system==EBADF && setups==0 && tls.conn_state==ESP_TLS_FAIL);
    reset();socket_error=ECONNREFUSED;CHECK(poll_connect()==-1);CHECK(captured.system==ECONNREFUSED && captured.esp==ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST && setups==0);
    reset();socket_error=ENETUNREACH;CHECK(poll_connect()==-1);CHECK(captured.system==ENETUNREACH && setups==0);
    reset();initial_error=ENETUNREACH;CHECK(poll_connect()==-1);CHECK(captured.system==ENETUNREACH && polls==0);
    reset();getopt_error=EINTR;CHECK(poll_connect()==0);CHECK(setups==0);CHECK(poll_connect()==1);
    reset();getopt_error=EBADF;CHECK(poll_connect()==-1);CHECK(captured.esp==ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED && captured.system==EBADF);
    reset();socket_error=EINPROGRESS;CHECK(poll_connect()==0);socket_error=0;CHECK(poll_connect()==1);
    reset();socket_error=EALREADY;CHECK(poll_connect()==0);socket_error=0;CHECK(poll_connect()==1);
    reset();bad_ready=true;CHECK(poll_connect()==0 && setups==0);
    /* A pending poll returns to the owner, allowing immediate stop/close.
     * This tests cooperative cancellation, not a real FreeRTOS lifecycle. */
    for(unsigned cycle=0;cycle<100;++cycle){reset();first_timeout=true;CHECK(poll_connect()==0);tls.sockfd=-1;CHECK(polls==1 && setups==0);reset();CHECK(poll_connect()==1);}
#else
    for(unsigned i=0;i<10;++i) CHECK(poll_connect()==0);
    CHECK(setups==0); /* Reproduces lost fd_sets after the first timeout. */
    reset();socket_error=ECONNREFUSED;CHECK(poll_connect()==1 && setups==1);
    reset();select_error=EBADF;CHECK(poll_connect()==1 && setups==1);
#endif
    /* Document why timeout_ms=0 is NOT a nonblocking-poll workaround. */
    reset();cfg.timeout_ms=0;CHECK(poll_connect()==1 && null_timeout);
    printf("PASS: %u checks against %s SDK function\n",checks,EXPECT_PATCHED ? "patched" : "original (defects reproduced)");return 0;
}
