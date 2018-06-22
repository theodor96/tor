/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef TOR_COMPAT_H
#define TOR_COMPAT_H

#include "orconfig.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_IDEAL_SEND_BACKLOG_QUERY
#define SIO_IDEAL_SEND_BACKLOG_QUERY 0x4004747b
#endif
#endif
#include "lib/cc/torint.h"
#include "lib/testsupport/testsupport.h"
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <stdarg.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_NETINET6_IN6_H
#include <netinet6/in6.h>
#endif

#include "lib/cc/compat_compiler.h"
#include "common/compat_time.h"
#include "lib/string/compat_ctype.h"
#include "lib/string/compat_string.h"
#include "lib/string/printf.h"

#include <stdio.h>
#include <errno.h>

/* ===== Compiler compatibility */

/** Represents an mmaped file. Allocated via tor_mmap_file; freed with
 * tor_munmap_file. */
typedef struct tor_mmap_t {
  const char *data; /**< Mapping of the file's contents. */
  size_t size; /**< Size of the file. */

  /* None of the fields below should be accessed from outside compat.c */
#ifdef HAVE_MMAP
  size_t mapping_size; /**< Size of the actual mapping. (This is this file
                        * size, rounded up to the nearest page.) */
#elif defined _WIN32
  HANDLE mmap_handle;
#endif /* defined(HAVE_MMAP) || ... */

} tor_mmap_t;

tor_mmap_t *tor_mmap_file(const char *filename) ATTR_NONNULL((1));
int tor_munmap_file(tor_mmap_t *handle) ATTR_NONNULL((1));

const void *tor_memmem(const void *haystack, size_t hlen, const void *needle,
                       size_t nlen) ATTR_NONNULL((1,3));
static const void *tor_memstr(const void *haystack, size_t hlen,
                           const char *needle) ATTR_NONNULL((1,3));
static inline const void *
tor_memstr(const void *haystack, size_t hlen, const char *needle)
{
  return tor_memmem(haystack, hlen, needle, strlen(needle));
}

char *tor_strtok_r_impl(char *str, const char *sep, char **lasts);
#ifdef HAVE_STRTOK_R
#define tor_strtok_r(str, sep, lasts) strtok_r(str, sep, lasts)
#else
#define tor_strtok_r(str, sep, lasts) tor_strtok_r_impl(str, sep, lasts)
#endif

#ifdef _WIN32
#define SHORT_FILE__ (tor_fix_source_file(__FILE__))
const char *tor_fix_source_file(const char *fname);
#else
#define SHORT_FILE__ (__FILE__)
#define tor_fix_source_file(s) (s)
#endif /* defined(_WIN32) */

/* ===== Time compatibility */

struct tm *tor_localtime_r(const time_t *timep, struct tm *result);
struct tm *tor_gmtime_r(const time_t *timep, struct tm *result);

#ifndef timeradd
/** Replacement for timeradd on platforms that do not have it: sets tvout to
 * the sum of tv1 and tv2. */
#define timeradd(tv1,tv2,tvout) \
  do {                                                  \
    (tvout)->tv_sec = (tv1)->tv_sec + (tv2)->tv_sec;    \
    (tvout)->tv_usec = (tv1)->tv_usec + (tv2)->tv_usec; \
    if ((tvout)->tv_usec >= 1000000) {                  \
      (tvout)->tv_usec -= 1000000;                      \
      (tvout)->tv_sec++;                                \
    }                                                   \
  } while (0)
#endif /* !defined(timeradd) */

#ifndef timersub
/** Replacement for timersub on platforms that do not have it: sets tvout to
 * tv1 minus tv2. */
#define timersub(tv1,tv2,tvout) \
  do {                                                  \
    (tvout)->tv_sec = (tv1)->tv_sec - (tv2)->tv_sec;    \
    (tvout)->tv_usec = (tv1)->tv_usec - (tv2)->tv_usec; \
    if ((tvout)->tv_usec < 0) {                         \
      (tvout)->tv_usec += 1000000;                      \
      (tvout)->tv_sec--;                                \
    }                                                   \
  } while (0)
#endif /* !defined(timersub) */

#ifndef timercmp
/** Replacement for timercmp on platforms that do not have it: returns true
 * iff the relational operator "op" makes the expression tv1 op tv2 true.
 *
 * Note that while this definition should work for all boolean operators, some
 * platforms' native timercmp definitions do not support >=, <=, or ==.  So
 * don't use those.
 */
#define timercmp(tv1,tv2,op)                    \
  (((tv1)->tv_sec == (tv2)->tv_sec) ?           \
   ((tv1)->tv_usec op (tv2)->tv_usec) :         \
   ((tv1)->tv_sec op (tv2)->tv_sec))
#endif /* !defined(timercmp) */

/* ===== File compatibility */
int tor_open_cloexec(const char *path, int flags, unsigned mode);
FILE *tor_fopen_cloexec(const char *path, const char *mode);
int tor_rename(const char *path_old, const char *path_new);

int replace_file(const char *from, const char *to);
int touch_file(const char *fname);

typedef struct tor_lockfile_t tor_lockfile_t;
tor_lockfile_t *tor_lockfile_lock(const char *filename, int blocking,
                                  int *locked_out);
void tor_lockfile_unlock(tor_lockfile_t *lockfile);

off_t tor_fd_getpos(int fd);
int tor_fd_setpos(int fd, off_t pos);
int tor_fd_seekend(int fd);
int tor_ftruncate(int fd);

int64_t tor_get_avail_disk_space(const char *path);

#ifdef _WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

/* ===== Net compatibility */

#if (SIZEOF_SOCKLEN_T == 0)
typedef int socklen_t;
#endif

#ifdef _WIN32
/* XXX Actually, this should arguably be SOCKET; we use intptr_t here so that
 * any inadvertent checks for the socket being <= 0 or > 0 will probably
 * still work. */
#define tor_socket_t intptr_t
#define TOR_SOCKET_T_FORMAT INTPTR_T_FORMAT
#define SOCKET_OK(s) ((SOCKET)(s) != INVALID_SOCKET)
#define TOR_INVALID_SOCKET INVALID_SOCKET
#else /* !(defined(_WIN32)) */
/** Type used for a network socket. */
#define tor_socket_t int
#define TOR_SOCKET_T_FORMAT "%d"
/** Macro: true iff 's' is a possible value for a valid initialized socket. */
#define SOCKET_OK(s) ((s) >= 0)
/** Error/uninitialized value for a tor_socket_t. */
#define TOR_INVALID_SOCKET (-1)
#endif /* defined(_WIN32) */

int tor_close_socket_simple(tor_socket_t s);
MOCK_DECL(int, tor_close_socket, (tor_socket_t s));
void tor_take_socket_ownership(tor_socket_t s);
tor_socket_t tor_open_socket_with_extensions(
                                           int domain, int type, int protocol,
                                           int cloexec, int nonblock);
MOCK_DECL(tor_socket_t,
tor_open_socket,(int domain, int type, int protocol));
tor_socket_t tor_open_socket_nonblocking(int domain, int type, int protocol);
tor_socket_t tor_accept_socket(tor_socket_t sockfd, struct sockaddr *addr,
                                  socklen_t *len);
tor_socket_t tor_accept_socket_nonblocking(tor_socket_t sockfd,
                                           struct sockaddr *addr,
                                           socklen_t *len);
tor_socket_t tor_accept_socket_with_extensions(tor_socket_t sockfd,
                                               struct sockaddr *addr,
                                               socklen_t *len,
                                               int cloexec, int nonblock);
MOCK_DECL(tor_socket_t,
tor_connect_socket,(tor_socket_t socket,const struct sockaddr *address,
                    socklen_t address_len));
int get_n_open_sockets(void);

MOCK_DECL(int,
tor_getsockname,(tor_socket_t socket, struct sockaddr *address,
                 socklen_t *address_len));
struct tor_addr_t;
int tor_addr_from_getsockname(struct tor_addr_t *addr_out, tor_socket_t sock);

#define tor_socket_send(s, buf, len, flags) send(s, buf, len, flags)
#define tor_socket_recv(s, buf, len, flags) recv(s, buf, len, flags)

/** Implementation of struct in6_addr for platforms that do not have it.
 * Generally, these platforms are ones without IPv6 support, but we want to
 * have a working in6_addr there anyway, so we can use it to parse IPv6
 * addresses. */
#if !defined(HAVE_STRUCT_IN6_ADDR)
struct in6_addr
{
  union {
    uint8_t u6_addr8[16];
    uint16_t u6_addr16[8];
    uint32_t u6_addr32[4];
  } in6_u;
#define s6_addr   in6_u.u6_addr8
#define s6_addr16 in6_u.u6_addr16
#define s6_addr32 in6_u.u6_addr32
};
#endif /* !defined(HAVE_STRUCT_IN6_ADDR) */

/** @{ */
/** Many BSD variants seem not to define these. */
#if defined(__APPLE__) || defined(__darwin__) || \
  defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#ifndef s6_addr16
#define s6_addr16 __u6_addr.__u6_addr16
#endif
#ifndef s6_addr32
#define s6_addr32 __u6_addr.__u6_addr32
#endif
#endif /* defined(__APPLE__) || defined(__darwin__) || ... */
/** @} */

#ifndef HAVE_SA_FAMILY_T
typedef uint16_t sa_family_t;
#endif

/** @{ */
/** Apparently, MS and Solaris don't define s6_addr16 or s6_addr32; these
 * macros get you a pointer to s6_addr32 or local equivalent. */
#ifdef HAVE_STRUCT_IN6_ADDR_S6_ADDR32
#define S6_ADDR32(x) ((uint32_t*)(x).s6_addr32)
#else
#define S6_ADDR32(x) ((uint32_t*)((char*)&(x).s6_addr))
#endif
#ifdef HAVE_STRUCT_IN6_ADDR_S6_ADDR16
#define S6_ADDR16(x) ((uint16_t*)(x).s6_addr16)
#else
#define S6_ADDR16(x) ((uint16_t*)((char*)&(x).s6_addr))
#endif
/** @} */

/** Implementation of struct sockaddr_in6 on platforms that do not have
 * it. See notes on struct in6_addr. */
#if !defined(HAVE_STRUCT_SOCKADDR_IN6)
struct sockaddr_in6 {
  sa_family_t sin6_family;
  uint16_t sin6_port;
  // uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  // uint32_t sin6_scope_id;
};
#endif /* !defined(HAVE_STRUCT_SOCKADDR_IN6) */

MOCK_DECL(int,tor_gethostname,(char *name, size_t namelen));
int tor_inet_aton(const char *cp, struct in_addr *addr) ATTR_NONNULL((1,2));
const char *tor_inet_ntop(int af, const void *src, char *dst, size_t len);
int tor_inet_pton(int af, const char *src, void *dst);
MOCK_DECL(int,tor_lookup_hostname,(const char *name, uint32_t *addr));
int set_socket_nonblocking(tor_socket_t socket);
int tor_socketpair(int family, int type, int protocol, tor_socket_t fd[2]);
int network_init(void);

/* For stupid historical reasons, windows sockets have an independent
 * set of errnos, and an independent way to get them.  Also, you can't
 * always believe WSAEWOULDBLOCK.  Use the macros below to compare
 * errnos against expected values, and use tor_socket_errno to find
 * the actual errno after a socket operation fails.
 */
#if defined(_WIN32)
/** Expands to WSA<b>e</b> on Windows, and to <b>e</b> elsewhere. */
#define SOCK_ERRNO(e) WSA##e
/** Return true if e is EAGAIN or the local equivalent. */
#define ERRNO_IS_EAGAIN(e)           ((e) == EAGAIN || (e) == WSAEWOULDBLOCK)
/** Return true if e is EINPROGRESS or the local equivalent. */
#define ERRNO_IS_EINPROGRESS(e)      ((e) == WSAEINPROGRESS)
/** Return true if e is EINPROGRESS or the local equivalent as returned by
 * a call to connect(). */
#define ERRNO_IS_CONN_EINPROGRESS(e) \
  ((e) == WSAEINPROGRESS || (e)== WSAEINVAL || (e) == WSAEWOULDBLOCK)
/** Return true if e is EAGAIN or another error indicating that a call to
 * accept() has no pending connections to return. */
#define ERRNO_IS_ACCEPT_EAGAIN(e)    ERRNO_IS_EAGAIN(e)
/** Return true if e is EMFILE or another error indicating that a call to
 * accept() has failed because we're out of fds or something. */
#define ERRNO_IS_RESOURCE_LIMIT(e) \
  ((e) == WSAEMFILE || (e) == WSAENOBUFS)
/** Return true if e is EADDRINUSE or the local equivalent. */
#define ERRNO_IS_EADDRINUSE(e)      ((e) == WSAEADDRINUSE)
/** Return true if e is EINTR  or the local equivalent */
#define ERRNO_IS_EINTR(e)            ((e) == WSAEINTR || 0)
int tor_socket_errno(tor_socket_t sock);
const char *tor_socket_strerror(int e);
#else /* !(defined(_WIN32)) */
#define SOCK_ERRNO(e) e
#if EAGAIN == EWOULDBLOCK
/* || 0 is for -Wparentheses-equality (-Wall?) appeasement under clang */
#define ERRNO_IS_EAGAIN(e)           ((e) == EAGAIN || 0)
#else
#define ERRNO_IS_EAGAIN(e)           ((e) == EAGAIN || (e) == EWOULDBLOCK)
#endif /* EAGAIN == EWOULDBLOCK */
#define ERRNO_IS_EINTR(e)            ((e) == EINTR || 0)
#define ERRNO_IS_EINPROGRESS(e)      ((e) == EINPROGRESS || 0)
#define ERRNO_IS_CONN_EINPROGRESS(e) ((e) == EINPROGRESS || 0)
#define ERRNO_IS_ACCEPT_EAGAIN(e) \
  (ERRNO_IS_EAGAIN(e) || (e) == ECONNABORTED)
#define ERRNO_IS_RESOURCE_LIMIT(e) \
  ((e) == EMFILE || (e) == ENFILE || (e) == ENOBUFS || (e) == ENOMEM)
#define ERRNO_IS_EADDRINUSE(e)       (((e) == EADDRINUSE) || 0)
#define tor_socket_errno(sock)       (errno)
#define tor_socket_strerror(e)       strerror(e)
#endif /* defined(_WIN32) */

/** Specified SOCKS5 status codes. */
typedef enum {
  SOCKS5_SUCCEEDED                  = 0x00,
  SOCKS5_GENERAL_ERROR              = 0x01,
  SOCKS5_NOT_ALLOWED                = 0x02,
  SOCKS5_NET_UNREACHABLE            = 0x03,
  SOCKS5_HOST_UNREACHABLE           = 0x04,
  SOCKS5_CONNECTION_REFUSED         = 0x05,
  SOCKS5_TTL_EXPIRED                = 0x06,
  SOCKS5_COMMAND_NOT_SUPPORTED      = 0x07,
  SOCKS5_ADDRESS_TYPE_NOT_SUPPORTED = 0x08,
} socks5_reply_status_t;

/* ===== OS compatibility */
MOCK_DECL(const char *, get_uname, (void));

uint16_t get_uint16(const void *cp) ATTR_NONNULL((1));
uint32_t get_uint32(const void *cp) ATTR_NONNULL((1));
uint64_t get_uint64(const void *cp) ATTR_NONNULL((1));
void set_uint16(void *cp, uint16_t v) ATTR_NONNULL((1));
void set_uint32(void *cp, uint32_t v) ATTR_NONNULL((1));
void set_uint64(void *cp, uint64_t v) ATTR_NONNULL((1));

/* These uint8 variants are defined to make the code more uniform. */
#define get_uint8(cp) (*(const uint8_t*)(cp))
static void set_uint8(void *cp, uint8_t v);
static inline void
set_uint8(void *cp, uint8_t v)
{
  *(uint8_t*)cp = v;
}

#if !defined(HAVE_RLIM_T)
typedef unsigned long rlim_t;
#endif
int get_max_sockets(void);
int set_max_file_descriptors(rlim_t limit, int *max);
int tor_disable_debugger_attach(void);

#if defined(HAVE_SYS_CAPABILITY_H) && defined(HAVE_CAP_SET_PROC)
#define HAVE_LINUX_CAPABILITIES
#endif

int have_capability_support(void);

/** Flag for switch_id; see switch_id() for documentation */
#define SWITCH_ID_KEEP_BINDLOW    (1<<0)
/** Flag for switch_id; see switch_id() for documentation */
#define SWITCH_ID_WARN_IF_NO_CAPS (1<<1)
int switch_id(const char *user, unsigned flags);
#ifdef HAVE_PWD_H
char *get_user_homedir(const char *username);
#endif

#ifndef _WIN32
const struct passwd *tor_getpwnam(const char *username);
const struct passwd *tor_getpwuid(uid_t uid);
#endif

int get_parent_directory(char *fname);
char *make_path_absolute(char *fname);

char **get_environment(void);

MOCK_DECL(int, get_total_system_memory, (size_t *mem_out));

int compute_num_cpus(void);

int tor_mlockall(void);

/** Macros for MIN/MAX.  Never use these when the arguments could have
 * side-effects.
 * {With GCC extensions we could probably define a safer MIN/MAX.  But
 * depending on that safety would be dangerous, since not every platform
 * has it.}
 **/
#ifndef MAX
#define MAX(a,b) ( ((a)<(b)) ? (b) : (a) )
#endif
#ifndef MIN
#define MIN(a,b) ( ((a)>(b)) ? (b) : (a) )
#endif

/* Platform-specific helpers. */
#ifdef _WIN32
char *format_win32_error(DWORD err);
#endif

/*for some reason my compiler doesn't have these version flags defined
  a nice homework assignment for someone one day is to define the rest*/
//these are the values as given on MSDN
#ifdef _WIN32

#ifndef VER_SUITE_EMBEDDEDNT
#define VER_SUITE_EMBEDDEDNT 0x00000040
#endif

#ifndef VER_SUITE_SINGLEUSERTS
#define VER_SUITE_SINGLEUSERTS 0x00000100
#endif

#endif /* defined(_WIN32) */

#ifdef COMPAT_PRIVATE
#if !defined(HAVE_SOCKETPAIR) || defined(_WIN32) || defined(TOR_UNIT_TESTS)
#define NEED_ERSATZ_SOCKETPAIR
STATIC int tor_ersatz_socketpair(int family, int type, int protocol,
                                   tor_socket_t fd[2]);
#endif
#endif /* defined(COMPAT_PRIVATE) */

ssize_t tor_getpass(const char *prompt, char *output, size_t buflen);

/* This needs some of the declarations above so we include it here. */
#include "common/compat_threads.h"

#endif /* !defined(TOR_COMPAT_H) */
