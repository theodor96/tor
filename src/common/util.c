/* Copyright (c) 2003, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file util.c
 * \brief Common functions for strings, IO, network, data structures,
 * process control.
 **/

#include "orconfig.h"
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#define UTIL_PRIVATE
#include "common/util.h"
#include "lib/log/torlog.h"
#include "lib/crypt_ops/crypto_digest.h"
#include "lib/cc/torint.h"
#include "lib/container/smartlist.h"
#include "lib/fdio/fdio.h"
#include "common/address.h"
#include "common/sandbox.h"
#include "lib/err/backtrace.h"
#include "common/util_process.h"
#include "common/util_format.h"

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#include <tchar.h>
#include <winbase.h>
#else /* !(defined(_WIN32)) */
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#endif /* defined(_WIN32) */

/* math.h needs this on Linux */
#ifndef _USE_ISOC99_
#define _USE_ISOC99_ 1
#endif
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_MALLOC_MALLOC_H
#include <malloc/malloc.h>
#endif
#ifdef HAVE_MALLOC_H
#if !defined(OpenBSD) && !defined(__FreeBSD__)
/* OpenBSD has a malloc.h, but for our purposes, it only exists in order to
 * scold us for being so stupid as to autodetect its presence.  To be fair,
 * they've done this since 1996, when autoconf was only 5 years old. */
#include <malloc.h>
#endif /* !defined(OpenBSD) && !defined(__FreeBSD__) */
#endif /* defined(HAVE_MALLOC_H) */
#ifdef HAVE_MALLOC_NP_H
#include <malloc_np.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#if defined(HAVE_SYS_PRCTL_H) && defined(__linux__)
#include <sys/prctl.h>
#endif

/* =====
 * Memory management
 * ===== */

DISABLE_GCC_WARNING(aggregate-return)
/** Call the platform malloc info function, and dump the results to the log at
 * level <b>severity</b>.  If no such function exists, do nothing. */
void
tor_log_mallinfo(int severity)
{
#ifdef HAVE_MALLINFO
  struct mallinfo mi;
  memset(&mi, 0, sizeof(mi));
  mi = mallinfo();
  tor_log(severity, LD_MM,
      "mallinfo() said: arena=%d, ordblks=%d, smblks=%d, hblks=%d, "
      "hblkhd=%d, usmblks=%d, fsmblks=%d, uordblks=%d, fordblks=%d, "
      "keepcost=%d",
      mi.arena, mi.ordblks, mi.smblks, mi.hblks,
      mi.hblkhd, mi.usmblks, mi.fsmblks, mi.uordblks, mi.fordblks,
      mi.keepcost);
#else /* !(defined(HAVE_MALLINFO)) */
  (void)severity;
#endif /* defined(HAVE_MALLINFO) */
}
ENABLE_GCC_WARNING(aggregate-return)

/* =====
 * Math
 * ===== */

/**
 * Returns the natural logarithm of d base e.  We defined this wrapper here so
 * to avoid conflicts with old versions of tor_log(), which were named log().
 */
double
tor_mathlog(double d)
{
  return log(d);
}

/** Return the long integer closest to <b>d</b>. We define this wrapper
 * here so that not all users of math.h need to use the right incantations
 * to get the c99 functions. */
long
tor_lround(double d)
{
#if defined(HAVE_LROUND)
  return lround(d);
#elif defined(HAVE_RINT)
  return (long)rint(d);
#else
  return (long)(d > 0 ? d + 0.5 : ceil(d - 0.5));
#endif /* defined(HAVE_LROUND) || ... */
}

/** Return the 64-bit integer closest to d.  We define this wrapper here so
 * that not all users of math.h need to use the right incantations to get the
 * c99 functions. */
int64_t
tor_llround(double d)
{
#if defined(HAVE_LLROUND)
  return (int64_t)llround(d);
#elif defined(HAVE_RINT)
  return (int64_t)rint(d);
#else
  return (int64_t)(d > 0 ? d + 0.5 : ceil(d - 0.5));
#endif /* defined(HAVE_LLROUND) || ... */
}

/** Transform a random value <b>p</b> from the uniform distribution in
 * [0.0, 1.0[ into a Laplace distributed value with location parameter
 * <b>mu</b> and scale parameter <b>b</b>. Truncate the final result
 * to be an integer in [INT64_MIN, INT64_MAX]. */
int64_t
sample_laplace_distribution(double mu, double b, double p)
{
  double result;
  tor_assert(p >= 0.0 && p < 1.0);

  /* This is the "inverse cumulative distribution function" from:
   * http://en.wikipedia.org/wiki/Laplace_distribution */
  if (p <= 0.0) {
    /* Avoid taking log(0.0) == -INFINITY, as some processors or compiler
     * options can cause the program to trap. */
    return INT64_MIN;
  }

  result = mu - b * (p > 0.5 ? 1.0 : -1.0)
                  * tor_mathlog(1.0 - 2.0 * fabs(p - 0.5));

  return clamp_double_to_int64(result);
}

/** Add random noise between INT64_MIN and INT64_MAX coming from a Laplace
 * distribution with mu = 0 and b = <b>delta_f</b>/<b>epsilon</b> to
 * <b>signal</b> based on the provided <b>random</b> value in [0.0, 1.0[.
 * The epsilon value must be between ]0.0, 1.0]. delta_f must be greater
 * than 0. */
int64_t
add_laplace_noise(int64_t signal_, double random_, double delta_f,
                  double epsilon)
{
  int64_t noise;

  /* epsilon MUST be between ]0.0, 1.0] */
  tor_assert(epsilon > 0.0 && epsilon <= 1.0);
  /* delta_f MUST be greater than 0. */
  tor_assert(delta_f > 0.0);

  /* Just add noise, no further signal */
  noise = sample_laplace_distribution(0.0,
                                      delta_f / epsilon,
                                      random_);

  /* Clip (signal + noise) to [INT64_MIN, INT64_MAX] */
  if (noise > 0 && INT64_MAX - noise < signal_)
    return INT64_MAX;
  else if (noise < 0 && INT64_MIN - noise > signal_)
    return INT64_MIN;
  else
    return signal_ + noise;
}

/* =====
 * String manipulation
 * ===== */

/** Return a pointer to a NUL-terminated hexadecimal string encoding
 * the first <b>fromlen</b> bytes of <b>from</b>. (fromlen must be \<= 32.) The
 * result does not need to be deallocated, but repeated calls to
 * hex_str will trash old results.
 */
const char *
hex_str(const char *from, size_t fromlen)
{
  static char buf[65];
  if (fromlen>(sizeof(buf)-1)/2)
    fromlen = (sizeof(buf)-1)/2;
  base16_encode(buf,sizeof(buf),from,fromlen);
  return buf;
}

/** Compare the value of the string <b>prefix</b> with the start of the
 * <b>memlen</b>-byte memory chunk at <b>mem</b>.  Return as for strcmp.
 *
 * [As fast_memcmp(mem, prefix, strlen(prefix)) but returns -1 if memlen is
 * less than strlen(prefix).]
 */
int
fast_memcmpstart(const void *mem, size_t memlen,
                const char *prefix)
{
  size_t plen = strlen(prefix);
  if (memlen < plen)
    return -1;
  return fast_memcmp(mem, prefix, plen);
}

/** Return true iff the 'len' bytes at 'mem' are all zero. */
int
tor_mem_is_zero(const char *mem, size_t len)
{
  static const char ZERO[] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
  };
  while (len >= sizeof(ZERO)) {
    /* It's safe to use fast_memcmp here, since the very worst thing an
     * attacker could learn is how many initial bytes of a secret were zero */
    if (fast_memcmp(mem, ZERO, sizeof(ZERO)))
      return 0;
    len -= sizeof(ZERO);
    mem += sizeof(ZERO);
  }
  /* Deal with leftover bytes. */
  if (len)
    return fast_memeq(mem, ZERO, len);

  return 1;
}

/** Return true iff the DIGEST_LEN bytes in digest are all zero. */
int
tor_digest_is_zero(const char *digest)
{
  static const uint8_t ZERO_DIGEST[] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
  };
  return tor_memeq(digest, ZERO_DIGEST, DIGEST_LEN);
}

/** Return true if <b>string</b> is a valid 'key=[value]' string.
 *  "value" is optional, to indicate the empty string. Log at logging
 *  <b>severity</b> if something ugly happens. */
int
string_is_key_value(int severity, const char *string)
{
  /* position of equal sign in string */
  const char *equal_sign_pos = NULL;

  tor_assert(string);

  if (strlen(string) < 2) { /* "x=" is shortest args string */
    tor_log(severity, LD_GENERAL, "'%s' is too short to be a k=v value.",
            escaped(string));
    return 0;
  }

  equal_sign_pos = strchr(string, '=');
  if (!equal_sign_pos) {
    tor_log(severity, LD_GENERAL, "'%s' is not a k=v value.", escaped(string));
    return 0;
  }

  /* validate that the '=' is not in the beginning of the string. */
  if (equal_sign_pos == string) {
    tor_log(severity, LD_GENERAL, "'%s' is not a valid k=v value.",
            escaped(string));
    return 0;
  }

  return 1;
}

/** Return true if <b>string</b> represents a valid IPv4 adddress in
 * 'a.b.c.d' form.
 */
int
string_is_valid_ipv4_address(const char *string)
{
  struct in_addr addr;

  return (tor_inet_pton(AF_INET,string,&addr) == 1);
}

/** Return true if <b>string</b> represents a valid IPv6 address in
 * a form that inet_pton() can parse.
 */
int
string_is_valid_ipv6_address(const char *string)
{
  struct in6_addr addr;

  return (tor_inet_pton(AF_INET6,string,&addr) == 1);
}

/** Return true iff <b>string</b> is a valid destination address,
 * i.e. either a DNS hostname or IPv4/IPv6 address string.
 */
int
string_is_valid_dest(const char *string)
{
  char *tmp = NULL;
  int retval;
  size_t len;

  if (string == NULL)
    return 0;

  len = strlen(string);

  if (len == 0)
    return 0;

  if (string[0] == '[' && string[len - 1] == ']')
    string = tmp = tor_strndup(string + 1, len - 2);

  retval = string_is_valid_ipv4_address(string) ||
    string_is_valid_ipv6_address(string) ||
    string_is_valid_nonrfc_hostname(string);

  tor_free(tmp);

  return retval;
}

/** Return true iff <b>string</b> matches a pattern of DNS names
 * that we allow Tor clients to connect to.
 *
 * Note: This allows certain technically invalid characters ('_') to cope
 * with misconfigured zones that have been encountered in the wild.
 */
int
string_is_valid_nonrfc_hostname(const char *string)
{
  int result = 1;
  int has_trailing_dot;
  char *last_label;
  smartlist_t *components;

  if (!string || strlen(string) == 0)
    return 0;

  if (string_is_valid_ipv4_address(string))
    return 0;

  components = smartlist_new();

  smartlist_split_string(components,string,".",0,0);

  if (BUG(smartlist_len(components) == 0))
    return 0; // LCOV_EXCL_LINE should be impossible given the earlier checks.

  /* Allow a single terminating '.' used rarely to indicate domains
   * are FQDNs rather than relative. */
  last_label = (char *)smartlist_get(components,
                                     smartlist_len(components) - 1);
  has_trailing_dot = (last_label[0] == '\0');
  if (has_trailing_dot) {
    smartlist_pop_last(components);
    tor_free(last_label);
    last_label = NULL;
  }

  SMARTLIST_FOREACH_BEGIN(components, char *, c) {
    if ((c[0] == '-') || (*c == '_')) {
      result = 0;
      break;
    }

    do {
      result = (TOR_ISALNUM(*c) || (*c == '-') || (*c == '_'));
      c++;
    } while (result && *c);

    if (result == 0) {
      break;
    }
  } SMARTLIST_FOREACH_END(c);

  SMARTLIST_FOREACH_BEGIN(components, char *, c) {
    tor_free(c);
  } SMARTLIST_FOREACH_END(c);

  smartlist_free(components);

  return result;
}

/** Return true iff the DIGEST256_LEN bytes in digest are all zero. */
int
tor_digest256_is_zero(const char *digest)
{
  return tor_mem_is_zero(digest, DIGEST256_LEN);
}

/* Helper: common code to check whether the result of a strtol or strtoul or
 * strtoll is correct. */
#define CHECK_STRTOX_RESULT()                           \
  /* Did an overflow occur? */                          \
  if (errno == ERANGE)                                  \
    goto err;                                           \
  /* Was at least one character converted? */           \
  if (endptr == s)                                      \
    goto err;                                           \
  /* Were there unexpected unconverted characters? */   \
  if (!next && *endptr)                                 \
    goto err;                                           \
  /* Illogical (max, min) inputs? */                    \
  if (BUG(max < min))                                   \
    goto err;                                           \
  /* Is r within limits? */                             \
  if (r < min || r > max)                               \
    goto err;                                           \
  if (ok) *ok = 1;                                      \
  if (next) *next = endptr;                             \
  return r;                                             \
 err:                                                   \
  if (ok) *ok = 0;                                      \
  if (next) *next = endptr;                             \
  return 0

/** Extract a long from the start of <b>s</b>, in the given numeric
 * <b>base</b>.  If <b>base</b> is 0, <b>s</b> is parsed as a decimal,
 * octal, or hex number in the syntax of a C integer literal.  If
 * there is unconverted data and <b>next</b> is provided, set
 * *<b>next</b> to the first unconverted character.  An error has
 * occurred if no characters are converted; or if there are
 * unconverted characters and <b>next</b> is NULL; or if the parsed
 * value is not between <b>min</b> and <b>max</b>.  When no error
 * occurs, return the parsed value and set *<b>ok</b> (if provided) to
 * 1.  When an error occurs, return 0 and set *<b>ok</b> (if provided)
 * to 0.
 */
long
tor_parse_long(const char *s, int base, long min, long max,
               int *ok, char **next)
{
  char *endptr;
  long r;

  if (BUG(base < 0)) {
    if (ok)
      *ok = 0;
    return 0;
  }

  errno = 0;
  r = strtol(s, &endptr, base);
  CHECK_STRTOX_RESULT();
}

/** As tor_parse_long(), but return an unsigned long. */
unsigned long
tor_parse_ulong(const char *s, int base, unsigned long min,
                unsigned long max, int *ok, char **next)
{
  char *endptr;
  unsigned long r;

  if (BUG(base < 0)) {
    if (ok)
      *ok = 0;
    return 0;
  }

  errno = 0;
  r = strtoul(s, &endptr, base);
  CHECK_STRTOX_RESULT();
}

/** As tor_parse_long(), but return a double. */
double
tor_parse_double(const char *s, double min, double max, int *ok, char **next)
{
  char *endptr;
  double r;

  errno = 0;
  r = strtod(s, &endptr);
  CHECK_STRTOX_RESULT();
}

/** As tor_parse_long, but return a uint64_t.  Only base 10 is guaranteed to
 * work for now. */
uint64_t
tor_parse_uint64(const char *s, int base, uint64_t min,
                 uint64_t max, int *ok, char **next)
{
  char *endptr;
  uint64_t r;

  if (BUG(base < 0)) {
    if (ok)
      *ok = 0;
    return 0;
  }

  errno = 0;
#ifdef HAVE_STRTOULL
  r = (uint64_t)strtoull(s, &endptr, base);
#elif defined(_WIN32)
  r = (uint64_t)_strtoui64(s, &endptr, base);
#elif SIZEOF_LONG == 8
  r = (uint64_t)strtoul(s, &endptr, base);
#else
#error "I don't know how to parse 64-bit numbers."
#endif /* defined(HAVE_STRTOULL) || ... */

  CHECK_STRTOX_RESULT();
}

/** Return a newly allocated string equal to <b>string</b>, except that every
 * character in <b>chars_to_escape</b> is preceded by a backslash. */
char *
tor_escape_str_for_pt_args(const char *string, const char *chars_to_escape)
{
  char *new_string = NULL;
  char *new_cp = NULL;
  size_t length, new_length;

  tor_assert(string);

  length = strlen(string);

  if (!length) /* If we were given the empty string, return the same. */
    return tor_strdup("");
  /* (new_length > SIZE_MAX) => ((length * 2) + 1 > SIZE_MAX) =>
     (length*2 > SIZE_MAX - 1) => (length > (SIZE_MAX - 1)/2) */
  if (length > (SIZE_MAX - 1)/2) /* check for overflow */
    return NULL;

  /* this should be enough even if all characters must be escaped */
  new_length = (length * 2) + 1;

  new_string = new_cp = tor_malloc(new_length);

  while (*string) {
    if (strchr(chars_to_escape, *string))
      *new_cp++ = '\\';

    *new_cp++ = *string++;
  }

  *new_cp = '\0'; /* NUL-terminate the new string */

  return new_string;
}

/* =====
 * Time
 * ===== */

#define TOR_USEC_PER_SEC 1000000

/** Return the difference between start->tv_sec and end->tv_sec.
 * Returns INT64_MAX on overflow and underflow.
 */
static int64_t
tv_secdiff_impl(const struct timeval *start, const struct timeval *end)
{
  const int64_t s = (int64_t)start->tv_sec;
  const int64_t e = (int64_t)end->tv_sec;

  /* This may not be the most efficient way of implemeting this check,
   * but it's easy to see that it's correct and doesn't overflow */

  if (s > 0 && e < INT64_MIN + s) {
    /* s is positive: equivalent to e - s < INT64_MIN, but without any
     * overflow */
    return INT64_MAX;
  } else if (s < 0 && e > INT64_MAX + s) {
    /* s is negative: equivalent to e - s > INT64_MAX, but without any
     * overflow */
    return INT64_MAX;
  }

  return e - s;
}

/** Return the number of microseconds elapsed between *start and *end.
 * Returns LONG_MAX on overflow and underflow.
 */
long
tv_udiff(const struct timeval *start, const struct timeval *end)
{
  /* Sanity check tv_usec */
  if (start->tv_usec > TOR_USEC_PER_SEC || start->tv_usec < 0) {
    log_warn(LD_GENERAL, "comparing times on microsecond detail with bad "
             "start tv_usec: " I64_FORMAT " microseconds",
             I64_PRINTF_ARG(start->tv_usec));
    return LONG_MAX;
  }

  if (end->tv_usec > TOR_USEC_PER_SEC || end->tv_usec < 0) {
    log_warn(LD_GENERAL, "comparing times on microsecond detail with bad "
             "end tv_usec: " I64_FORMAT " microseconds",
             I64_PRINTF_ARG(end->tv_usec));
    return LONG_MAX;
  }

  /* Some BSDs have struct timeval.tv_sec 64-bit, but time_t (and long) 32-bit
   */
  int64_t udiff;
  const int64_t secdiff = tv_secdiff_impl(start, end);

  /* end->tv_usec - start->tv_usec can be up to 1 second either way */
  if (secdiff > (int64_t)(LONG_MAX/1000000 - 1) ||
      secdiff < (int64_t)(LONG_MIN/1000000 + 1)) {
    log_warn(LD_GENERAL, "comparing times on microsecond detail too far "
             "apart: " I64_FORMAT " seconds", I64_PRINTF_ARG(secdiff));
    return LONG_MAX;
  }

  /* we'll never get an overflow here, because we check that both usecs are
   * between 0 and TV_USEC_PER_SEC. */
  udiff = secdiff*1000000 + ((int64_t)end->tv_usec - (int64_t)start->tv_usec);

  /* Some compilers are smart enough to work out this is a no-op on L64 */
#if SIZEOF_LONG < 8
  if (udiff > (int64_t)LONG_MAX || udiff < (int64_t)LONG_MIN) {
    return LONG_MAX;
  }
#endif

  return (long)udiff;
}

/** Return the number of milliseconds elapsed between *start and *end.
 * If the tv_usec difference is 500, rounds away from zero.
 * Returns LONG_MAX on overflow and underflow.
 */
long
tv_mdiff(const struct timeval *start, const struct timeval *end)
{
  /* Sanity check tv_usec */
  if (start->tv_usec > TOR_USEC_PER_SEC || start->tv_usec < 0) {
    log_warn(LD_GENERAL, "comparing times on millisecond detail with bad "
             "start tv_usec: " I64_FORMAT " microseconds",
             I64_PRINTF_ARG(start->tv_usec));
    return LONG_MAX;
  }

  if (end->tv_usec > TOR_USEC_PER_SEC || end->tv_usec < 0) {
    log_warn(LD_GENERAL, "comparing times on millisecond detail with bad "
             "end tv_usec: " I64_FORMAT " microseconds",
             I64_PRINTF_ARG(end->tv_usec));
    return LONG_MAX;
  }

  /* Some BSDs have struct timeval.tv_sec 64-bit, but time_t (and long) 32-bit
   */
  int64_t mdiff;
  const int64_t secdiff = tv_secdiff_impl(start, end);

  /* end->tv_usec - start->tv_usec can be up to 1 second either way, but the
   * mdiff calculation may add another temporary second for rounding.
   * Whether this actually causes overflow depends on the compiler's constant
   * folding and order of operations. */
  if (secdiff > (int64_t)(LONG_MAX/1000 - 2) ||
      secdiff < (int64_t)(LONG_MIN/1000 + 1)) {
    log_warn(LD_GENERAL, "comparing times on millisecond detail too far "
             "apart: " I64_FORMAT " seconds", I64_PRINTF_ARG(secdiff));
    return LONG_MAX;
  }

  /* Subtract and round */
  mdiff = secdiff*1000 +
      /* We add a million usec here to ensure that the result is positive,
       * so that the round-towards-zero behavior of the division will give
       * the right result for rounding to the nearest msec. Later we subtract
       * 1000 in order to get the correct result.
       * We'll never get an overflow here, because we check that both usecs are
       * between 0 and TV_USEC_PER_SEC. */
      ((int64_t)end->tv_usec - (int64_t)start->tv_usec + 500 + 1000000) / 1000
      - 1000;

  /* Some compilers are smart enough to work out this is a no-op on L64 */
#if SIZEOF_LONG < 8
  if (mdiff > (int64_t)LONG_MAX || mdiff < (int64_t)LONG_MIN) {
    return LONG_MAX;
  }
#endif

  return (long)mdiff;
}

/**
 * Converts timeval to milliseconds.
 */
int64_t
tv_to_msec(const struct timeval *tv)
{
  int64_t conv = ((int64_t)tv->tv_sec)*1000L;
  /* Round ghetto-style */
  conv += ((int64_t)tv->tv_usec+500)/1000L;
  return conv;
}

/** Yield true iff <b>y</b> is a leap-year. */
#define IS_LEAPYEAR(y) (!(y % 4) && ((y % 100) || !(y % 400)))
/** Helper: Return the number of leap-days between Jan 1, y1 and Jan 1, y2. */
static int
n_leapdays(int year1, int year2)
{
  --year1;
  --year2;
  return (year2/4 - year1/4) - (year2/100 - year1/100)
    + (year2/400 - year1/400);
}
/** Number of days per month in non-leap year; used by tor_timegm and
 * parse_rfc1123_time. */
static const int days_per_month[] =
  { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/** Compute a time_t given a struct tm.  The result is given in UTC, and
 * does not account for leap seconds.  Return 0 on success, -1 on failure.
 */
int
tor_timegm(const struct tm *tm, time_t *time_out)
{
  /* This is a pretty ironclad timegm implementation, snarfed from Python2.2.
   * It's way more brute-force than fiddling with tzset().
   *
   * We use int64_t rather than time_t to avoid overflow on multiplication on
   * platforms with 32-bit time_t. Since year is clipped to INT32_MAX, and
   * since 365 * 24 * 60 * 60 is approximately 31 million, it's not possible
   * for INT32_MAX years to overflow int64_t when converted to seconds. */
  int64_t year, days, hours, minutes, seconds;
  int i, invalid_year, dpm;

  /* Initialize time_out to 0 for now, to avoid bad usage in case this function
     fails and the caller ignores the return value. */
  tor_assert(time_out);
  *time_out = 0;

  /* avoid int overflow on addition */
  if (tm->tm_year < INT32_MAX-1900) {
    year = tm->tm_year + 1900;
  } else {
    /* clamp year */
    year = INT32_MAX;
  }
  invalid_year = (year < 1970 || tm->tm_year >= INT32_MAX-1900);

  if (tm->tm_mon >= 0 && tm->tm_mon <= 11) {
    dpm = days_per_month[tm->tm_mon];
    if (tm->tm_mon == 1 && !invalid_year && IS_LEAPYEAR(tm->tm_year)) {
      dpm = 29;
    }
  } else {
    /* invalid month - default to 0 days per month */
    dpm = 0;
  }

  if (invalid_year ||
      tm->tm_mon < 0 || tm->tm_mon > 11 ||
      tm->tm_mday < 1 || tm->tm_mday > dpm ||
      tm->tm_hour < 0 || tm->tm_hour > 23 ||
      tm->tm_min < 0 || tm->tm_min > 59 ||
      tm->tm_sec < 0 || tm->tm_sec > 60) {
    log_warn(LD_BUG, "Out-of-range argument to tor_timegm");
    return -1;
  }
  days = 365 * (year-1970) + n_leapdays(1970,(int)year);
  for (i = 0; i < tm->tm_mon; ++i)
    days += days_per_month[i];
  if (tm->tm_mon > 1 && IS_LEAPYEAR(year))
    ++days;
  days += tm->tm_mday - 1;
  hours = days*24 + tm->tm_hour;

  minutes = hours*60 + tm->tm_min;
  seconds = minutes*60 + tm->tm_sec;
  /* Check that "seconds" will fit in a time_t. On platforms where time_t is
   * 32-bit, this check will fail for dates in and after 2038.
   *
   * We already know that "seconds" can't be negative because "year" >= 1970 */
#if SIZEOF_TIME_T < 8
  if (seconds < TIME_MIN || seconds > TIME_MAX) {
    log_warn(LD_BUG, "Result does not fit in tor_timegm");
    return -1;
  }
#endif /* SIZEOF_TIME_T < 8 */
  *time_out = (time_t)seconds;
  return 0;
}

/* strftime is locale-specific, so we need to replace those parts */

/** A c-locale array of 3-letter names of weekdays, starting with Sun. */
static const char *WEEKDAY_NAMES[] =
  { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
/** A c-locale array of 3-letter names of months, starting with Jan. */
static const char *MONTH_NAMES[] =
  { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/** Set <b>buf</b> to the RFC1123 encoding of the UTC value of <b>t</b>.
 * The buffer must be at least RFC1123_TIME_LEN+1 bytes long.
 *
 * (RFC1123 format is "Fri, 29 Sep 2006 15:54:20 GMT". Note the "GMT"
 * rather than "UTC".)
 */
void
format_rfc1123_time(char *buf, time_t t)
{
  struct tm tm;

  tor_gmtime_r(&t, &tm);

  strftime(buf, RFC1123_TIME_LEN+1, "___, %d ___ %Y %H:%M:%S GMT", &tm);
  tor_assert(tm.tm_wday >= 0);
  tor_assert(tm.tm_wday <= 6);
  memcpy(buf, WEEKDAY_NAMES[tm.tm_wday], 3);
  tor_assert(tm.tm_mon >= 0);
  tor_assert(tm.tm_mon <= 11);
  memcpy(buf+8, MONTH_NAMES[tm.tm_mon], 3);
}

/** Parse the (a subset of) the RFC1123 encoding of some time (in UTC) from
 * <b>buf</b>, and store the result in *<b>t</b>.
 *
 * Note that we only accept the subset generated by format_rfc1123_time above,
 * not the full range of formats suggested by RFC 1123.
 *
 * Return 0 on success, -1 on failure.
*/
int
parse_rfc1123_time(const char *buf, time_t *t)
{
  struct tm tm;
  char month[4];
  char weekday[4];
  int i, m, invalid_year;
  unsigned tm_mday, tm_year, tm_hour, tm_min, tm_sec;
  unsigned dpm;

  if (strlen(buf) != RFC1123_TIME_LEN)
    return -1;
  memset(&tm, 0, sizeof(tm));
  if (tor_sscanf(buf, "%3s, %2u %3s %u %2u:%2u:%2u GMT", weekday,
             &tm_mday, month, &tm_year, &tm_hour,
             &tm_min, &tm_sec) < 7) {
    char *esc = esc_for_log(buf);
    log_warn(LD_GENERAL, "Got invalid RFC1123 time %s", esc);
    tor_free(esc);
    return -1;
  }

  m = -1;
  for (i = 0; i < 12; ++i) {
    if (!strcmp(month, MONTH_NAMES[i])) {
      m = i;
      break;
    }
  }
  if (m<0) {
    char *esc = esc_for_log(buf);
    log_warn(LD_GENERAL, "Got invalid RFC1123 time %s: No such month", esc);
    tor_free(esc);
    return -1;
  }
  tm.tm_mon = m;

  invalid_year = (tm_year >= INT32_MAX || tm_year < 1970);
  tor_assert(m >= 0 && m <= 11);
  dpm = days_per_month[m];
  if (m == 1 && !invalid_year && IS_LEAPYEAR(tm_year)) {
    dpm = 29;
  }

  if (invalid_year || tm_mday < 1 || tm_mday > dpm ||
      tm_hour > 23 || tm_min > 59 || tm_sec > 60) {
    char *esc = esc_for_log(buf);
    log_warn(LD_GENERAL, "Got invalid RFC1123 time %s", esc);
    tor_free(esc);
    return -1;
  }
  tm.tm_mday = (int)tm_mday;
  tm.tm_year = (int)tm_year;
  tm.tm_hour = (int)tm_hour;
  tm.tm_min = (int)tm_min;
  tm.tm_sec = (int)tm_sec;

  if (tm.tm_year < 1970) {
    /* LCOV_EXCL_START
     * XXXX I think this is dead code; we already checked for
     *      invalid_year above. */
    tor_assert_nonfatal_unreached();
    char *esc = esc_for_log(buf);
    log_warn(LD_GENERAL,
             "Got invalid RFC1123 time %s. (Before 1970)", esc);
    tor_free(esc);
    return -1;
    /* LCOV_EXCL_STOP */
  }
  tm.tm_year -= 1900;

  return tor_timegm(&tm, t);
}

/** Set <b>buf</b> to the ISO8601 encoding of the local value of <b>t</b>.
 * The buffer must be at least ISO_TIME_LEN+1 bytes long.
 *
 * (ISO8601 format is 2006-10-29 10:57:20)
 */
void
format_local_iso_time(char *buf, time_t t)
{
  struct tm tm;
  strftime(buf, ISO_TIME_LEN+1, "%Y-%m-%d %H:%M:%S", tor_localtime_r(&t, &tm));
}

/** Set <b>buf</b> to the ISO8601 encoding of the GMT value of <b>t</b>.
 * The buffer must be at least ISO_TIME_LEN+1 bytes long.
 */
void
format_iso_time(char *buf, time_t t)
{
  struct tm tm;
  strftime(buf, ISO_TIME_LEN+1, "%Y-%m-%d %H:%M:%S", tor_gmtime_r(&t, &tm));
}

/** As format_local_iso_time, but use the yyyy-mm-ddThh:mm:ss format to avoid
 * embedding an internal space. */
void
format_local_iso_time_nospace(char *buf, time_t t)
{
  format_local_iso_time(buf, t);
  buf[10] = 'T';
}

/** As format_iso_time, but use the yyyy-mm-ddThh:mm:ss format to avoid
 * embedding an internal space. */
void
format_iso_time_nospace(char *buf, time_t t)
{
  format_iso_time(buf, t);
  buf[10] = 'T';
}

/** As format_iso_time_nospace, but include microseconds in decimal
 * fixed-point format.  Requires that buf be at least ISO_TIME_USEC_LEN+1
 * bytes long. */
void
format_iso_time_nospace_usec(char *buf, const struct timeval *tv)
{
  tor_assert(tv);
  format_iso_time_nospace(buf, (time_t)tv->tv_sec);
  tor_snprintf(buf+ISO_TIME_LEN, 8, ".%06d", (int)tv->tv_usec);
}

/** Given an ISO-formatted UTC time value (after the epoch) in <b>cp</b>,
 * parse it and store its value in *<b>t</b>.  Return 0 on success, -1 on
 * failure.  Ignore extraneous stuff in <b>cp</b> after the end of the time
 * string, unless <b>strict</b> is set. If <b>nospace</b> is set,
 * expect the YYYY-MM-DDTHH:MM:SS format. */
int
parse_iso_time_(const char *cp, time_t *t, int strict, int nospace)
{
  struct tm st_tm;
  unsigned int year=0, month=0, day=0, hour=0, minute=0, second=0;
  int n_fields;
  char extra_char, separator_char;
  n_fields = tor_sscanf(cp, "%u-%2u-%2u%c%2u:%2u:%2u%c",
                        &year, &month, &day,
                        &separator_char,
                        &hour, &minute, &second, &extra_char);
  if (strict ? (n_fields != 7) : (n_fields < 7)) {
    char *esc = esc_for_log(cp);
    log_warn(LD_GENERAL, "ISO time %s was unparseable", esc);
    tor_free(esc);
    return -1;
  }
  if (separator_char != (nospace ? 'T' : ' ')) {
    char *esc = esc_for_log(cp);
    log_warn(LD_GENERAL, "ISO time %s was unparseable", esc);
    tor_free(esc);
    return -1;
  }
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
          hour > 23 || minute > 59 || second > 60 || year >= INT32_MAX) {
    char *esc = esc_for_log(cp);
    log_warn(LD_GENERAL, "ISO time %s was nonsensical", esc);
    tor_free(esc);
    return -1;
  }
  st_tm.tm_year = (int)year-1900;
  st_tm.tm_mon = month-1;
  st_tm.tm_mday = day;
  st_tm.tm_hour = hour;
  st_tm.tm_min = minute;
  st_tm.tm_sec = second;
  st_tm.tm_wday = 0; /* Should be ignored. */

  if (st_tm.tm_year < 70) {
    /* LCOV_EXCL_START
     * XXXX I think this is dead code; we already checked for
     *      year < 1970 above. */
    tor_assert_nonfatal_unreached();
    char *esc = esc_for_log(cp);
    log_warn(LD_GENERAL, "Got invalid ISO time %s. (Before 1970)", esc);
    tor_free(esc);
    return -1;
    /* LCOV_EXCL_STOP */
  }
  return tor_timegm(&st_tm, t);
}

/** Given an ISO-formatted UTC time value (after the epoch) in <b>cp</b>,
 * parse it and store its value in *<b>t</b>.  Return 0 on success, -1 on
 * failure. Reject the string if any characters are present after the time.
 */
int
parse_iso_time(const char *cp, time_t *t)
{
  return parse_iso_time_(cp, t, 1, 0);
}

/**
 * As parse_iso_time, but parses a time encoded by format_iso_time_nospace().
 */
int
parse_iso_time_nospace(const char *cp, time_t *t)
{
  return parse_iso_time_(cp, t, 1, 1);
}

/** Given a <b>date</b> in one of the three formats allowed by HTTP (ugh),
 * parse it into <b>tm</b>.  Return 0 on success, negative on failure. */
int
parse_http_time(const char *date, struct tm *tm)
{
  const char *cp;
  char month[4];
  char wkday[4];
  int i;
  unsigned tm_mday, tm_year, tm_hour, tm_min, tm_sec;

  tor_assert(tm);
  memset(tm, 0, sizeof(*tm));

  /* First, try RFC1123 or RFC850 format: skip the weekday.  */
  if ((cp = strchr(date, ','))) {
    ++cp;
    if (*cp != ' ')
      return -1;
    ++cp;
    if (tor_sscanf(cp, "%2u %3s %4u %2u:%2u:%2u GMT",
               &tm_mday, month, &tm_year,
               &tm_hour, &tm_min, &tm_sec) == 6) {
      /* rfc1123-date */
      tm_year -= 1900;
    } else if (tor_sscanf(cp, "%2u-%3s-%2u %2u:%2u:%2u GMT",
                      &tm_mday, month, &tm_year,
                      &tm_hour, &tm_min, &tm_sec) == 6) {
      /* rfc850-date */
    } else {
      return -1;
    }
  } else {
    /* No comma; possibly asctime() format. */
    if (tor_sscanf(date, "%3s %3s %2u %2u:%2u:%2u %4u",
               wkday, month, &tm_mday,
               &tm_hour, &tm_min, &tm_sec, &tm_year) == 7) {
      tm_year -= 1900;
    } else {
      return -1;
    }
  }
  tm->tm_mday = (int)tm_mday;
  tm->tm_year = (int)tm_year;
  tm->tm_hour = (int)tm_hour;
  tm->tm_min = (int)tm_min;
  tm->tm_sec = (int)tm_sec;
  tm->tm_wday = 0; /* Leave this unset. */

  month[3] = '\0';
  /* Okay, now decode the month. */
  /* set tm->tm_mon to dummy value so the check below fails. */
  tm->tm_mon = -1;
  for (i = 0; i < 12; ++i) {
    if (!strcasecmp(MONTH_NAMES[i], month)) {
      tm->tm_mon = i;
    }
  }

  if (tm->tm_year < 0 ||
      tm->tm_mon < 0  || tm->tm_mon > 11 ||
      tm->tm_mday < 1 || tm->tm_mday > 31 ||
      tm->tm_hour < 0 || tm->tm_hour > 23 ||
      tm->tm_min < 0  || tm->tm_min > 59 ||
      tm->tm_sec < 0  || tm->tm_sec > 60)
    return -1; /* Out of range, or bad month. */

  return 0;
}

/** Given an <b>interval</b> in seconds, try to write it to the
 * <b>out_len</b>-byte buffer in <b>out</b> in a human-readable form.
 * Returns a non-negative integer on success, -1 on failure.
 */
int
format_time_interval(char *out, size_t out_len, long interval)
{
  /* We only report seconds if there's no hours. */
  long sec = 0, min = 0, hour = 0, day = 0;

  /* -LONG_MIN is LONG_MAX + 1, which causes signed overflow */
  if (interval < -LONG_MAX)
    interval = LONG_MAX;
  else if (interval < 0)
    interval = -interval;

  if (interval >= 86400) {
    day = interval / 86400;
    interval %= 86400;
  }
  if (interval >= 3600) {
    hour = interval / 3600;
    interval %= 3600;
  }
  if (interval >= 60) {
    min = interval / 60;
    interval %= 60;
  }
  sec = interval;

  if (day) {
    return tor_snprintf(out, out_len, "%ld days, %ld hours, %ld minutes",
                        day, hour, min);
  } else if (hour) {
    return tor_snprintf(out, out_len, "%ld hours, %ld minutes", hour, min);
  } else if (min) {
    return tor_snprintf(out, out_len, "%ld minutes, %ld seconds", min, sec);
  } else {
    return tor_snprintf(out, out_len, "%ld seconds", sec);
  }
}

/* =====
 * File helpers
 * ===== */

/** Write <b>count</b> bytes from <b>buf</b> to <b>fd</b>.  <b>isSocket</b>
 * must be 1 if fd was returned by socket() or accept(), and 0 if fd
 * was returned by open().  Return the number of bytes written, or -1
 * on error.  Only use if fd is a blocking fd.  */
ssize_t
write_all(tor_socket_t fd, const char *buf, size_t count, int isSocket)
{
  size_t written = 0;
  ssize_t result;
  raw_assert(count < SSIZE_MAX);

  while (written != count) {
    if (isSocket)
      result = tor_socket_send(fd, buf+written, count-written, 0);
    else
      result = write((int)fd, buf+written, count-written);
    if (result<0)
      return -1;
    written += result;
  }
  return (ssize_t)count;
}

/** Read from <b>fd</b> to <b>buf</b>, until we get <b>count</b> bytes
 * or reach the end of the file. <b>isSocket</b> must be 1 if fd
 * was returned by socket() or accept(), and 0 if fd was returned by
 * open().  Return the number of bytes read, or -1 on error. Only use
 * if fd is a blocking fd. */
ssize_t
read_all(tor_socket_t fd, char *buf, size_t count, int isSocket)
{
  size_t numread = 0;
  ssize_t result;

  if (count > SIZE_T_CEILING || count > SSIZE_MAX) {
    errno = EINVAL;
    return -1;
  }

  while (numread < count) {
    if (isSocket)
      result = tor_socket_recv(fd, buf+numread, count-numread, 0);
    else
      result = read((int)fd, buf+numread, count-numread);
    if (result<0)
      return -1;
    else if (result == 0)
      break;
    numread += result;
  }
  return (ssize_t)numread;
}

/*
 *    Filesystem operations.
 */

/** Clean up <b>name</b> so that we can use it in a call to "stat".  On Unix,
 * we do nothing.  On Windows, we remove a trailing slash, unless the path is
 * the root of a disk. */
static void
clean_name_for_stat(char *name)
{
#ifdef _WIN32
  size_t len = strlen(name);
  if (!len)
    return;
  if (name[len-1]=='\\' || name[len-1]=='/') {
    if (len == 1 || (len==3 && name[1]==':'))
      return;
    name[len-1]='\0';
  }
#else /* !(defined(_WIN32)) */
  (void)name;
#endif /* defined(_WIN32) */
}

/** Wrapper for unlink() to make it mockable for the test suite; returns 0
 * if unlinking the file succeeded, -1 and sets errno if unlinking fails.
 */

MOCK_IMPL(int,
tor_unlink,(const char *pathname))
{
  return unlink(pathname);
}

/** Return:
 * FN_ERROR if filename can't be read, is NULL, or is zero-length,
 * FN_NOENT if it doesn't exist,
 * FN_FILE if it is a non-empty regular file, or a FIFO on unix-like systems,
 * FN_EMPTY for zero-byte regular files,
 * FN_DIR if it's a directory, and
 * FN_ERROR for any other file type.
 * On FN_ERROR and FN_NOENT, sets errno.  (errno is not set when FN_ERROR
 * is returned due to an unhandled file type.) */
file_status_t
file_status(const char *fname)
{
  struct stat st;
  char *f;
  int r;
  if (!fname || strlen(fname) == 0) {
    return FN_ERROR;
  }
  f = tor_strdup(fname);
  clean_name_for_stat(f);
  log_debug(LD_FS, "stat()ing %s", f);
  r = stat(sandbox_intern_string(f), &st);
  tor_free(f);
  if (r) {
    if (errno == ENOENT) {
      return FN_NOENT;
    }
    return FN_ERROR;
  }
  if (st.st_mode & S_IFDIR) {
    return FN_DIR;
  } else if (st.st_mode & S_IFREG) {
    if (st.st_size > 0) {
      return FN_FILE;
    } else if (st.st_size == 0) {
      return FN_EMPTY;
    } else {
      return FN_ERROR;
    }
#ifndef _WIN32
  } else if (st.st_mode & S_IFIFO) {
    return FN_FILE;
#endif
  } else {
    return FN_ERROR;
  }
}

/** Check whether <b>dirname</b> exists and is private.  If yes return 0.
 * If <b>dirname</b> does not exist:
 *  - if <b>check</b>&CPD_CREATE, try to create it and return 0 on success.
 *  - if <b>check</b>&CPD_CHECK, and we think we can create it, return 0.
 *  - if <b>check</b>&CPD_CHECK is false, and the directory exists, return 0.
 *  - otherwise, return -1.
 * If CPD_GROUP_OK is set, then it's okay if the directory
 * is group-readable, but in all cases we create the directory mode 0700.
 * If CPD_GROUP_READ is set, existing directory behaves as CPD_GROUP_OK and
 * if the directory is created it will use mode 0750 with group read
 * permission. Group read privileges also assume execute permission
 * as norm for directories. If CPD_CHECK_MODE_ONLY is set, then we don't
 * alter the directory permissions if they are too permissive:
 * we just return -1.
 * When effective_user is not NULL, check permissions against the given user
 * and its primary group.
 */
MOCK_IMPL(int,
check_private_dir,(const char *dirname, cpd_check_t check,
                   const char *effective_user))
{
  int r;
  struct stat st;

  tor_assert(dirname);

#ifndef _WIN32
  int fd;
  const struct passwd *pw = NULL;
  uid_t running_uid;
  gid_t running_gid;

  /*
   * Goal is to harden the implementation by removing any
   * potential for race between stat() and chmod().
   * chmod() accepts filename as argument. If an attacker can move
   * the file between stat() and chmod(), a potential race exists.
   *
   * Several suggestions taken from:
   * https://developer.apple.com/library/mac/documentation/
   *     Security/Conceptual/SecureCodingGuide/Articles/RaceConditions.html
   */

  /* Open directory.
   * O_NOFOLLOW to ensure that it does not follow symbolic links */
  fd = open(sandbox_intern_string(dirname), O_NOFOLLOW);

  /* Was there an error? Maybe the directory does not exist? */
  if (fd == -1) {

    if (errno != ENOENT) {
      /* Other directory error */
      log_warn(LD_FS, "Directory %s cannot be read: %s", dirname,
               strerror(errno));
      return -1;
    }

    /* Received ENOENT: Directory does not exist */

    /* Should we create the directory? */
    if (check & CPD_CREATE) {
      log_info(LD_GENERAL, "Creating directory %s", dirname);
      if (check & CPD_GROUP_READ) {
        r = mkdir(dirname, 0750);
      } else {
        r = mkdir(dirname, 0700);
      }

      /* check for mkdir() error */
      if (r) {
        log_warn(LD_FS, "Error creating directory %s: %s", dirname,
            strerror(errno));
        return -1;
      }

      /* we just created the directory. try to open it again.
       * permissions on the directory will be checked again below.*/
      fd = open(sandbox_intern_string(dirname), O_NOFOLLOW);

      if (fd == -1) {
        log_warn(LD_FS, "Could not reopen recently created directory %s: %s",
                 dirname,
                 strerror(errno));
        return -1;
      } else {
        close(fd);
      }

    } else if (!(check & CPD_CHECK)) {
      log_warn(LD_FS, "Directory %s does not exist.", dirname);
      return -1;
    }

    /* XXXX In the case where check==CPD_CHECK, we should look at the
     * parent directory a little harder. */
    return 0;
  }

  tor_assert(fd >= 0);

  //f = tor_strdup(dirname);
  //clean_name_for_stat(f);
  log_debug(LD_FS, "stat()ing %s", dirname);
  //r = stat(sandbox_intern_string(f), &st);
  r = fstat(fd, &st);
  if (r == -1) {
      log_warn(LD_FS, "fstat() on directory %s failed.", dirname);
      close(fd);
      return -1;
  }
  //tor_free(f);

  /* check that dirname is a directory */
  if (!(st.st_mode & S_IFDIR)) {
    log_warn(LD_FS, "%s is not a directory", dirname);
    close(fd);
    return -1;
  }

  if (effective_user) {
    /* Look up the user and group information.
     * If we have a problem, bail out. */
    pw = tor_getpwnam(effective_user);
    if (pw == NULL) {
      log_warn(LD_CONFIG, "Error setting configured user: %s not found",
               effective_user);
      close(fd);
      return -1;
    }
    running_uid = pw->pw_uid;
    running_gid = pw->pw_gid;
  } else {
    running_uid = getuid();
    running_gid = getgid();
  }
  if (st.st_uid != running_uid) {
    char *process_ownername = NULL, *file_ownername = NULL;

    {
      const struct passwd *pw_running = tor_getpwuid(running_uid);
      process_ownername = pw_running ? tor_strdup(pw_running->pw_name) :
        tor_strdup("<unknown>");
    }

    {
      const struct passwd *pw_stat = tor_getpwuid(st.st_uid);
      file_ownername = pw_stat ? tor_strdup(pw_stat->pw_name) :
        tor_strdup("<unknown>");
    }

    log_warn(LD_FS, "%s is not owned by this user (%s, %d) but by "
        "%s (%d). Perhaps you are running Tor as the wrong user?",
             dirname, process_ownername, (int)running_uid,
             file_ownername, (int)st.st_uid);

    tor_free(process_ownername);
    tor_free(file_ownername);
    close(fd);
    return -1;
  }
  if ( (check & (CPD_GROUP_OK|CPD_GROUP_READ))
       && (st.st_gid != running_gid) && (st.st_gid != 0)) {
    struct group *gr;
    char *process_groupname = NULL;
    gr = getgrgid(running_gid);
    process_groupname = gr ? tor_strdup(gr->gr_name) : tor_strdup("<unknown>");
    gr = getgrgid(st.st_gid);

    log_warn(LD_FS, "%s is not owned by this group (%s, %d) but by group "
             "%s (%d).  Are you running Tor as the wrong user?",
             dirname, process_groupname, (int)running_gid,
             gr ?  gr->gr_name : "<unknown>", (int)st.st_gid);

    tor_free(process_groupname);
    close(fd);
    return -1;
  }
  unsigned unwanted_bits = 0;
  if (check & (CPD_GROUP_OK|CPD_GROUP_READ)) {
    unwanted_bits = 0027;
  } else {
    unwanted_bits = 0077;
  }
  unsigned check_bits_filter = ~0;
  if (check & CPD_RELAX_DIRMODE_CHECK) {
    check_bits_filter = 0022;
  }
  if ((st.st_mode & unwanted_bits & check_bits_filter) != 0) {
    unsigned new_mode;
    if (check & CPD_CHECK_MODE_ONLY) {
      log_warn(LD_FS, "Permissions on directory %s are too permissive.",
               dirname);
      close(fd);
      return -1;
    }
    log_warn(LD_FS, "Fixing permissions on directory %s", dirname);
    new_mode = st.st_mode;
    new_mode |= 0700; /* Owner should have rwx */
    if (check & CPD_GROUP_READ) {
      new_mode |= 0050; /* Group should have rx */
    }
    new_mode &= ~unwanted_bits; /* Clear the bits that we didn't want set...*/
    if (fchmod(fd, new_mode)) {
      log_warn(LD_FS, "Could not chmod directory %s: %s", dirname,
               strerror(errno));
      close(fd);
      return -1;
    } else {
      close(fd);
      return 0;
    }
  }
  close(fd);
#else /* !(!defined(_WIN32)) */
  /* Win32 case: we can't open() a directory. */
  (void)effective_user;

  char *f = tor_strdup(dirname);
  clean_name_for_stat(f);
  log_debug(LD_FS, "stat()ing %s", f);
  r = stat(sandbox_intern_string(f), &st);
  tor_free(f);
  if (r) {
    if (errno != ENOENT) {
      log_warn(LD_FS, "Directory %s cannot be read: %s", dirname,
               strerror(errno));
      return -1;
    }
    if (check & CPD_CREATE) {
      log_info(LD_GENERAL, "Creating directory %s", dirname);
      r = mkdir(dirname);
      if (r) {
        log_warn(LD_FS, "Error creating directory %s: %s", dirname,
                 strerror(errno));
        return -1;
      }
    } else if (!(check & CPD_CHECK)) {
      log_warn(LD_FS, "Directory %s does not exist.", dirname);
      return -1;
    }
    return 0;
  }
  if (!(st.st_mode & S_IFDIR)) {
    log_warn(LD_FS, "%s is not a directory", dirname);
    return -1;
  }

#endif /* !defined(_WIN32) */
  return 0;
}

/** Create a file named <b>fname</b> with the contents <b>str</b>.  Overwrite
 * the previous <b>fname</b> if possible.  Return 0 on success, -1 on failure.
 *
 * This function replaces the old file atomically, if possible.  This
 * function, and all other functions in util.c that create files, create them
 * with mode 0600.
 */
MOCK_IMPL(int,
write_str_to_file,(const char *fname, const char *str, int bin))
{
#ifdef _WIN32
  if (!bin && strchr(str, '\r')) {
    log_warn(LD_BUG,
             "We're writing a text string that already contains a CR to %s",
             escaped(fname));
  }
#endif /* defined(_WIN32) */
  return write_bytes_to_file(fname, str, strlen(str), bin);
}

/** Represents a file that we're writing to, with support for atomic commit:
 * we can write into a temporary file, and either remove the file on
 * failure, or replace the original file on success. */
struct open_file_t {
  char *tempname; /**< Name of the temporary file. */
  char *filename; /**< Name of the original file. */
  unsigned rename_on_close:1; /**< Are we using the temporary file or not? */
  unsigned binary:1; /**< Did we open in binary mode? */
  int fd; /**< fd for the open file. */
  FILE *stdio_file; /**< stdio wrapper for <b>fd</b>. */
};

/** Try to start writing to the file in <b>fname</b>, passing the flags
 * <b>open_flags</b> to the open() syscall, creating the file (if needed) with
 * access value <b>mode</b>.  If the O_APPEND flag is set, we append to the
 * original file.  Otherwise, we open a new temporary file in the same
 * directory, and either replace the original or remove the temporary file
 * when we're done.
 *
 * Return the fd for the newly opened file, and store working data in
 * *<b>data_out</b>.  The caller should not close the fd manually:
 * instead, call finish_writing_to_file() or abort_writing_to_file().
 * Returns -1 on failure.
 *
 * NOTE: When not appending, the flags O_CREAT and O_TRUNC are treated
 * as true and the flag O_EXCL is treated as false.
 *
 * NOTE: Ordinarily, O_APPEND means "seek to the end of the file before each
 * write()".  We don't do that.
 */
int
start_writing_to_file(const char *fname, int open_flags, int mode,
                      open_file_t **data_out)
{
  open_file_t *new_file = tor_malloc_zero(sizeof(open_file_t));
  const char *open_name;
  int append = 0;

  tor_assert(fname);
  tor_assert(data_out);
#if (O_BINARY != 0 && O_TEXT != 0)
  tor_assert((open_flags & (O_BINARY|O_TEXT)) != 0);
#endif
  new_file->fd = -1;
  new_file->filename = tor_strdup(fname);
  if (open_flags & O_APPEND) {
    open_name = fname;
    new_file->rename_on_close = 0;
    append = 1;
    open_flags &= ~O_APPEND;
  } else {
    tor_asprintf(&new_file->tempname, "%s.tmp", fname);
    open_name = new_file->tempname;
    /* We always replace an existing temporary file if there is one. */
    open_flags |= O_CREAT|O_TRUNC;
    open_flags &= ~O_EXCL;
    new_file->rename_on_close = 1;
  }
#if O_BINARY != 0
  if (open_flags & O_BINARY)
    new_file->binary = 1;
#endif

  new_file->fd = tor_open_cloexec(open_name, open_flags, mode);
  if (new_file->fd < 0) {
    log_warn(LD_FS, "Couldn't open \"%s\" (%s) for writing: %s",
        open_name, fname, strerror(errno));
    goto err;
  }
  if (append) {
    if (tor_fd_seekend(new_file->fd) < 0) {
      log_warn(LD_FS, "Couldn't seek to end of file \"%s\": %s", open_name,
               strerror(errno));
      goto err;
    }
  }

  *data_out = new_file;

  return new_file->fd;

 err:
  if (new_file->fd >= 0)
    close(new_file->fd);
  *data_out = NULL;
  tor_free(new_file->filename);
  tor_free(new_file->tempname);
  tor_free(new_file);
  return -1;
}

/** Given <b>file_data</b> from start_writing_to_file(), return a stdio FILE*
 * that can be used to write to the same file.  The caller should not mix
 * stdio calls with non-stdio calls. */
FILE *
fdopen_file(open_file_t *file_data)
{
  tor_assert(file_data);
  if (file_data->stdio_file)
    return file_data->stdio_file;
  tor_assert(file_data->fd >= 0);
  if (!(file_data->stdio_file = fdopen(file_data->fd,
                                       file_data->binary?"ab":"a"))) {
    log_warn(LD_FS, "Couldn't fdopen \"%s\" [%d]: %s", file_data->filename,
             file_data->fd, strerror(errno));
  }
  return file_data->stdio_file;
}

/** Combines start_writing_to_file with fdopen_file(): arguments are as
 * for start_writing_to_file, but  */
FILE *
start_writing_to_stdio_file(const char *fname, int open_flags, int mode,
                            open_file_t **data_out)
{
  FILE *res;
  if (start_writing_to_file(fname, open_flags, mode, data_out)<0)
    return NULL;
  if (!(res = fdopen_file(*data_out))) {
    abort_writing_to_file(*data_out);
    *data_out = NULL;
  }
  return res;
}

/** Helper function: close and free the underlying file and memory in
 * <b>file_data</b>.  If we were writing into a temporary file, then delete
 * that file (if abort_write is true) or replaces the target file with
 * the temporary file (if abort_write is false). */
static int
finish_writing_to_file_impl(open_file_t *file_data, int abort_write)
{
  int r = 0;

  tor_assert(file_data && file_data->filename);
  if (file_data->stdio_file) {
    if (fclose(file_data->stdio_file)) {
      log_warn(LD_FS, "Error closing \"%s\": %s", file_data->filename,
               strerror(errno));
      abort_write = r = -1;
    }
  } else if (file_data->fd >= 0 && close(file_data->fd) < 0) {
    log_warn(LD_FS, "Error flushing \"%s\": %s", file_data->filename,
             strerror(errno));
    abort_write = r = -1;
  }

  if (file_data->rename_on_close) {
    tor_assert(file_data->tempname && file_data->filename);
    if (!abort_write) {
      tor_assert(strcmp(file_data->filename, file_data->tempname));
      if (replace_file(file_data->tempname, file_data->filename)) {
        log_warn(LD_FS, "Error replacing \"%s\": %s", file_data->filename,
                 strerror(errno));
        abort_write = r = -1;
      }
    }
    if (abort_write) {
      int res = unlink(file_data->tempname);
      if (res != 0) {
        /* We couldn't unlink and we'll leave a mess behind */
        log_warn(LD_FS, "Failed to unlink %s: %s",
                 file_data->tempname, strerror(errno));
        r = -1;
      }
    }
  }

  tor_free(file_data->filename);
  tor_free(file_data->tempname);
  tor_free(file_data);

  return r;
}

/** Finish writing to <b>file_data</b>: close the file handle, free memory as
 * needed, and if using a temporary file, replace the original file with
 * the temporary file. */
int
finish_writing_to_file(open_file_t *file_data)
{
  return finish_writing_to_file_impl(file_data, 0);
}

/** Finish writing to <b>file_data</b>: close the file handle, free memory as
 * needed, and if using a temporary file, delete it. */
int
abort_writing_to_file(open_file_t *file_data)
{
  return finish_writing_to_file_impl(file_data, 1);
}

/** Helper: given a set of flags as passed to open(2), open the file
 * <b>fname</b> and write all the sized_chunk_t structs in <b>chunks</b> to
 * the file.  Do so as atomically as possible e.g. by opening temp files and
 * renaming. */
static int
write_chunks_to_file_impl(const char *fname, const smartlist_t *chunks,
                          int open_flags)
{
  open_file_t *file = NULL;
  int fd;
  ssize_t result;
  fd = start_writing_to_file(fname, open_flags, 0600, &file);
  if (fd<0)
    return -1;
  SMARTLIST_FOREACH(chunks, sized_chunk_t *, chunk,
  {
    result = write_all(fd, chunk->bytes, chunk->len, 0);
    if (result < 0) {
      log_warn(LD_FS, "Error writing to \"%s\": %s", fname,
          strerror(errno));
      goto err;
    }
    tor_assert((size_t)result == chunk->len);
  });

  return finish_writing_to_file(file);
 err:
  abort_writing_to_file(file);
  return -1;
}

/** Given a smartlist of sized_chunk_t, write them to a file
 * <b>fname</b>, overwriting or creating the file as necessary.
 * If <b>no_tempfile</b> is 0 then the file will be written
 * atomically. */
int
write_chunks_to_file(const char *fname, const smartlist_t *chunks, int bin,
                     int no_tempfile)
{
  int flags = OPEN_FLAGS_REPLACE|(bin?O_BINARY:O_TEXT);

  if (no_tempfile) {
    /* O_APPEND stops write_chunks_to_file from using tempfiles */
    flags |= O_APPEND;
  }
  return write_chunks_to_file_impl(fname, chunks, flags);
}

/** Write <b>len</b> bytes, starting at <b>str</b>, to <b>fname</b>
    using the open() flags passed in <b>flags</b>. */
static int
write_bytes_to_file_impl(const char *fname, const char *str, size_t len,
                         int flags)
{
  int r;
  sized_chunk_t c = { str, len };
  smartlist_t *chunks = smartlist_new();
  smartlist_add(chunks, &c);
  r = write_chunks_to_file_impl(fname, chunks, flags);
  smartlist_free(chunks);
  return r;
}

/** As write_str_to_file, but does not assume a NUL-terminated
 * string. Instead, we write <b>len</b> bytes, starting at <b>str</b>. */
MOCK_IMPL(int,
write_bytes_to_file,(const char *fname, const char *str, size_t len,
                     int bin))
{
  return write_bytes_to_file_impl(fname, str, len,
                                  OPEN_FLAGS_REPLACE|(bin?O_BINARY:O_TEXT));
}

/** As write_bytes_to_file, but if the file already exists, append the bytes
 * to the end of the file instead of overwriting it. */
int
append_bytes_to_file(const char *fname, const char *str, size_t len,
                     int bin)
{
  return write_bytes_to_file_impl(fname, str, len,
                                  OPEN_FLAGS_APPEND|(bin?O_BINARY:O_TEXT));
}

/** Like write_str_to_file(), but also return -1 if there was a file
    already residing in <b>fname</b>. */
int
write_bytes_to_new_file(const char *fname, const char *str, size_t len,
                        int bin)
{
  return write_bytes_to_file_impl(fname, str, len,
                                  OPEN_FLAGS_DONT_REPLACE|
                                  (bin?O_BINARY:O_TEXT));
}

/**
 * Read the contents of the open file <b>fd</b> presuming it is a FIFO
 * (or similar) file descriptor for which the size of the file isn't
 * known ahead of time. Return NULL on failure, and a NUL-terminated
 * string on success.  On success, set <b>sz_out</b> to the number of
 * bytes read.
 */
char *
read_file_to_str_until_eof(int fd, size_t max_bytes_to_read, size_t *sz_out)
{
  ssize_t r;
  size_t pos = 0;
  char *string = NULL;
  size_t string_max = 0;

  if (max_bytes_to_read+1 >= SIZE_T_CEILING) {
    errno = EINVAL;
    return NULL;
  }

  do {
    /* XXXX This "add 1K" approach is a little goofy; if we care about
     * performance here, we should be doubling.  But in practice we shouldn't
     * be using this function on big files anyway. */
    string_max = pos + 1024;
    if (string_max > max_bytes_to_read)
      string_max = max_bytes_to_read + 1;
    string = tor_realloc(string, string_max);
    r = read(fd, string + pos, string_max - pos - 1);
    if (r < 0) {
      int save_errno = errno;
      tor_free(string);
      errno = save_errno;
      return NULL;
    }

    pos += r;
  } while (r > 0 && pos < max_bytes_to_read);

  tor_assert(pos < string_max);
  *sz_out = pos;
  string[pos] = '\0';
  return string;
}

/** Read the contents of <b>filename</b> into a newly allocated
 * string; return the string on success or NULL on failure.
 *
 * If <b>stat_out</b> is provided, store the result of stat()ing the
 * file into <b>stat_out</b>.
 *
 * If <b>flags</b> &amp; RFTS_BIN, open the file in binary mode.
 * If <b>flags</b> &amp; RFTS_IGNORE_MISSING, don't warn if the file
 * doesn't exist.
 */
/*
 * This function <em>may</em> return an erroneous result if the file
 * is modified while it is running, but must not crash or overflow.
 * Right now, the error case occurs when the file length grows between
 * the call to stat and the call to read_all: the resulting string will
 * be truncated.
 */
MOCK_IMPL(char *,
read_file_to_str, (const char *filename, int flags, struct stat *stat_out))
{
  int fd; /* router file */
  struct stat statbuf;
  char *string;
  ssize_t r;
  int bin = flags & RFTS_BIN;

  tor_assert(filename);

  fd = tor_open_cloexec(filename,O_RDONLY|(bin?O_BINARY:O_TEXT),0);
  if (fd<0) {
    int severity = LOG_WARN;
    int save_errno = errno;
    if (errno == ENOENT && (flags & RFTS_IGNORE_MISSING))
      severity = LOG_INFO;
    log_fn(severity, LD_FS,"Could not open \"%s\": %s",filename,
           strerror(errno));
    errno = save_errno;
    return NULL;
  }

  if (fstat(fd, &statbuf)<0) {
    int save_errno = errno;
    close(fd);
    log_warn(LD_FS,"Could not fstat \"%s\".",filename);
    errno = save_errno;
    return NULL;
  }

#ifndef _WIN32
/** When we detect that we're reading from a FIFO, don't read more than
 * this many bytes.  It's insane overkill for most uses. */
#define FIFO_READ_MAX (1024*1024)
  if (S_ISFIFO(statbuf.st_mode)) {
    size_t sz = 0;
    string = read_file_to_str_until_eof(fd, FIFO_READ_MAX, &sz);
    int save_errno = errno;
    if (string && stat_out) {
      statbuf.st_size = sz;
      memcpy(stat_out, &statbuf, sizeof(struct stat));
    }
    close(fd);
    if (!string)
      errno = save_errno;
    return string;
  }
#endif /* !defined(_WIN32) */

  if ((uint64_t)(statbuf.st_size)+1 >= SIZE_T_CEILING) {
    close(fd);
    errno = EINVAL;
    return NULL;
  }

  string = tor_malloc((size_t)(statbuf.st_size+1));

  r = read_all(fd,string,(size_t)statbuf.st_size,0);
  if (r<0) {
    int save_errno = errno;
    log_warn(LD_FS,"Error reading from file \"%s\": %s", filename,
             strerror(errno));
    tor_free(string);
    close(fd);
    errno = save_errno;
    return NULL;
  }
  string[r] = '\0'; /* NUL-terminate the result. */

#if defined(_WIN32) || defined(__CYGWIN__)
  if (!bin && strchr(string, '\r')) {
    log_debug(LD_FS, "We didn't convert CRLF to LF as well as we hoped "
              "when reading %s. Coping.",
              filename);
    tor_strstrip(string, "\r");
    r = strlen(string);
  }
  if (!bin) {
    statbuf.st_size = (size_t) r;
  } else
#endif /* defined(_WIN32) || defined(__CYGWIN__) */
    if (r != statbuf.st_size) {
      /* Unless we're using text mode on win32, we'd better have an exact
       * match for size. */
      int save_errno = errno;
      log_warn(LD_FS,"Could read only %d of %ld bytes of file \"%s\".",
               (int)r, (long)statbuf.st_size,filename);
      tor_free(string);
      close(fd);
      errno = save_errno;
      return NULL;
    }
  close(fd);
  if (stat_out) {
    memcpy(stat_out, &statbuf, sizeof(struct stat));
  }

  return string;
}

#define TOR_ISODIGIT(c) ('0' <= (c) && (c) <= '7')

/** Given a c-style double-quoted escaped string in <b>s</b>, extract and
 * decode its contents into a newly allocated string.  On success, assign this
 * string to *<b>result</b>, assign its length to <b>size_out</b> (if
 * provided), and return a pointer to the position in <b>s</b> immediately
 * after the string.  On failure, return NULL.
 */
const char *
unescape_string(const char *s, char **result, size_t *size_out)
{
  const char *cp;
  char *out;
  if (s[0] != '\"')
    return NULL;
  cp = s+1;
  while (1) {
    switch (*cp) {
      case '\0':
      case '\n':
        return NULL;
      case '\"':
        goto end_of_loop;
      case '\\':
        if (cp[1] == 'x' || cp[1] == 'X') {
          if (!(TOR_ISXDIGIT(cp[2]) && TOR_ISXDIGIT(cp[3])))
            return NULL;
          cp += 4;
        } else if (TOR_ISODIGIT(cp[1])) {
          cp += 2;
          if (TOR_ISODIGIT(*cp)) ++cp;
          if (TOR_ISODIGIT(*cp)) ++cp;
        } else if (cp[1] == 'n' || cp[1] == 'r' || cp[1] == 't' || cp[1] == '"'
                   || cp[1] == '\\' || cp[1] == '\'') {
          cp += 2;
        } else {
          return NULL;
        }
        break;
      default:
        ++cp;
        break;
    }
  }
 end_of_loop:
  out = *result = tor_malloc(cp-s + 1);
  cp = s+1;
  while (1) {
    switch (*cp)
      {
      case '\"':
        *out = '\0';
        if (size_out) *size_out = out - *result;
        return cp+1;

        /* LCOV_EXCL_START -- we caught this in parse_config_from_line. */
      case '\0':
        tor_fragile_assert();
        tor_free(*result);
        return NULL;
        /* LCOV_EXCL_STOP */
      case '\\':
        switch (cp[1])
          {
          case 'n': *out++ = '\n'; cp += 2; break;
          case 'r': *out++ = '\r'; cp += 2; break;
          case 't': *out++ = '\t'; cp += 2; break;
          case 'x': case 'X':
            {
              int x1, x2;

              x1 = hex_decode_digit(cp[2]);
              x2 = hex_decode_digit(cp[3]);
              if (x1 == -1 || x2 == -1) {
                /* LCOV_EXCL_START */
                /* we caught this above in the initial loop. */
                tor_assert_nonfatal_unreached();
                tor_free(*result);
                return NULL;
                /* LCOV_EXCL_STOP */
              }

              *out++ = ((x1<<4) + x2);
              cp += 4;
            }
            break;
          case '0': case '1': case '2': case '3': case '4': case '5':
          case '6': case '7':
            {
              int n = cp[1]-'0';
              cp += 2;
              if (TOR_ISODIGIT(*cp)) { n = n*8 + *cp-'0'; cp++; }
              if (TOR_ISODIGIT(*cp)) { n = n*8 + *cp-'0'; cp++; }
              if (n > 255) { tor_free(*result); return NULL; }
              *out++ = (char)n;
            }
            break;
          case '\'':
          case '\"':
          case '\\':
          case '\?':
            *out++ = cp[1];
            cp += 2;
            break;

            /* LCOV_EXCL_START */
          default:
            /* we caught this above in the initial loop. */
            tor_assert_nonfatal_unreached();
            tor_free(*result); return NULL;
            /* LCOV_EXCL_STOP */
          }
        break;
      default:
        *out++ = *cp++;
      }
  }
}

/** Removes enclosing quotes from <b>path</b> and unescapes quotes between the
 * enclosing quotes. Backslashes are not unescaped. Return the unquoted
 * <b>path</b> on success or 0 if <b>path</b> is not quoted correctly. */
char *
get_unquoted_path(const char *path)
{
  size_t len = strlen(path);

  if (len == 0) {
    return tor_strdup("");
  }

  int has_start_quote = (path[0] == '\"');
  int has_end_quote = (len > 0 && path[len-1] == '\"');
  if (has_start_quote != has_end_quote || (len == 1 && has_start_quote)) {
    return NULL;
  }

  char *unquoted_path = tor_malloc(len - has_start_quote - has_end_quote + 1);
  char *s = unquoted_path;
  size_t i;
  for (i = has_start_quote; i < len - has_end_quote; i++) {
    if (path[i] == '\"' && (i > 0 && path[i-1] == '\\')) {
      *(s-1) = path[i];
    } else if (path[i] != '\"') {
      *s++ = path[i];
    } else {  /* unescaped quote */
      tor_free(unquoted_path);
      return NULL;
    }
  }
  *s = '\0';
  return unquoted_path;
}

/** Expand any homedir prefix on <b>filename</b>; return a newly allocated
 * string. */
char *
expand_filename(const char *filename)
{
  tor_assert(filename);
#ifdef _WIN32
  /* Might consider using GetFullPathName() as described here:
   * http://etutorials.org/Programming/secure+programming/
   *     Chapter+3.+Input+Validation/3.7+Validating+Filenames+and+Paths/
   */
  return tor_strdup(filename);
#else /* !(defined(_WIN32)) */
  if (*filename == '~') {
    char *home, *result=NULL;
    const char *rest;

    if (filename[1] == '/' || filename[1] == '\0') {
      home = getenv("HOME");
      if (!home) {
        log_warn(LD_CONFIG, "Couldn't find $HOME environment variable while "
                 "expanding \"%s\"; defaulting to \"\".", filename);
        home = tor_strdup("");
      } else {
        home = tor_strdup(home);
      }
      rest = strlen(filename)>=2?(filename+2):"";
    } else {
#ifdef HAVE_PWD_H
      char *username, *slash;
      slash = strchr(filename, '/');
      if (slash)
        username = tor_strndup(filename+1,slash-filename-1);
      else
        username = tor_strdup(filename+1);
      if (!(home = get_user_homedir(username))) {
        log_warn(LD_CONFIG,"Couldn't get homedir for \"%s\"",username);
        tor_free(username);
        return NULL;
      }
      tor_free(username);
      rest = slash ? (slash+1) : "";
#else /* !(defined(HAVE_PWD_H)) */
      log_warn(LD_CONFIG, "Couldn't expand homedir on system without pwd.h");
      return tor_strdup(filename);
#endif /* defined(HAVE_PWD_H) */
    }
    tor_assert(home);
    /* Remove trailing slash. */
    if (strlen(home)>1 && !strcmpend(home,PATH_SEPARATOR)) {
      home[strlen(home)-1] = '\0';
    }
    tor_asprintf(&result,"%s"PATH_SEPARATOR"%s",home,rest);
    tor_free(home);
    return result;
  } else {
    return tor_strdup(filename);
  }
#endif /* defined(_WIN32) */
}

/** Return a new list containing the filenames in the directory <b>dirname</b>.
 * Return NULL on error or if <b>dirname</b> is not a directory.
 */
MOCK_IMPL(smartlist_t *,
tor_listdir, (const char *dirname))
{
  smartlist_t *result;
#ifdef _WIN32
  char *pattern=NULL;
  TCHAR tpattern[MAX_PATH] = {0};
  char name[MAX_PATH*2+1] = {0};
  HANDLE handle;
  WIN32_FIND_DATA findData;
  tor_asprintf(&pattern, "%s\\*", dirname);
#ifdef UNICODE
  mbstowcs(tpattern,pattern,MAX_PATH);
#else
  strlcpy(tpattern, pattern, MAX_PATH);
#endif
  if (INVALID_HANDLE_VALUE == (handle = FindFirstFile(tpattern, &findData))) {
    tor_free(pattern);
    return NULL;
  }
  result = smartlist_new();
  while (1) {
#ifdef UNICODE
    wcstombs(name,findData.cFileName,MAX_PATH);
    name[sizeof(name)-1] = '\0';
#else
    strlcpy(name,findData.cFileName,sizeof(name));
#endif /* defined(UNICODE) */
    if (strcmp(name, ".") &&
        strcmp(name, "..")) {
      smartlist_add_strdup(result, name);
    }
    if (!FindNextFile(handle, &findData)) {
      DWORD err;
      if ((err = GetLastError()) != ERROR_NO_MORE_FILES) {
        char *errstr = format_win32_error(err);
        log_warn(LD_FS, "Error reading directory '%s': %s", dirname, errstr);
        tor_free(errstr);
      }
      break;
    }
  }
  FindClose(handle);
  tor_free(pattern);
#else /* !(defined(_WIN32)) */
  const char *prot_dname = sandbox_intern_string(dirname);
  DIR *d;
  struct dirent *de;
  if (!(d = opendir(prot_dname)))
    return NULL;

  result = smartlist_new();
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") ||
        !strcmp(de->d_name, ".."))
      continue;
    smartlist_add_strdup(result, de->d_name);
  }
  closedir(d);
#endif /* defined(_WIN32) */
  return result;
}

/** Return true iff <b>filename</b> is a relative path. */
int
path_is_relative(const char *filename)
{
  if (filename && filename[0] == '/')
    return 0;
#ifdef _WIN32
  else if (filename && filename[0] == '\\')
    return 0;
  else if (filename && strlen(filename)>3 && TOR_ISALPHA(filename[0]) &&
           filename[1] == ':' && filename[2] == '\\')
    return 0;
#endif /* defined(_WIN32) */
  else
    return 1;
}

/* =====
 * Process helpers
 * ===== */

#ifndef _WIN32
/* Based on code contributed by christian grothoff */
/** True iff we've called start_daemon(). */
static int start_daemon_called = 0;
/** True iff we've called finish_daemon(). */
static int finish_daemon_called = 0;
/** Socketpair used to communicate between parent and child process while
 * daemonizing. */
static int daemon_filedes[2];
/** Start putting the process into daemon mode: fork and drop all resources
 * except standard fds.  The parent process never returns, but stays around
 * until finish_daemon is called.  (Note: it's safe to call this more
 * than once: calls after the first are ignored.)
 */
void
start_daemon(void)
{
  pid_t pid;

  if (start_daemon_called)
    return;
  start_daemon_called = 1;

  if (pipe(daemon_filedes)) {
    /* LCOV_EXCL_START */
    log_err(LD_GENERAL,"pipe failed; exiting. Error was %s", strerror(errno));
    exit(1); // exit ok: during daemonize, pipe failed.
    /* LCOV_EXCL_STOP */
  }
  pid = fork();
  if (pid < 0) {
    /* LCOV_EXCL_START */
    log_err(LD_GENERAL,"fork failed. Exiting.");
    exit(1); // exit ok: during daemonize, fork failed
    /* LCOV_EXCL_STOP */
  }
  if (pid) {  /* Parent */
    int ok;
    char c;

    close(daemon_filedes[1]); /* we only read */
    ok = -1;
    while (0 < read(daemon_filedes[0], &c, sizeof(char))) {
      if (c == '.')
        ok = 1;
    }
    fflush(stdout);
    if (ok == 1)
      exit(0); // exit ok: during daemonize, daemonizing.
    else
      exit(1); /* child reported error. exit ok: daemonize failed. */
  } else { /* Child */
    close(daemon_filedes[0]); /* we only write */

    (void) setsid(); /* Detach from controlling terminal */
    /*
     * Fork one more time, so the parent (the session group leader) can exit.
     * This means that we, as a non-session group leader, can never regain a
     * controlling terminal.   This part is recommended by Stevens's
     * _Advanced Programming in the Unix Environment_.
     */
    if (fork() != 0) {
      exit(0); // exit ok: during daemonize, fork failed (2)
    }
    set_main_thread(); /* We are now the main thread. */

    return;
  }
}

/** Finish putting the process into daemon mode: drop standard fds, and tell
 * the parent process to exit.  (Note: it's safe to call this more than once:
 * calls after the first are ignored.  Calls start_daemon first if it hasn't
 * been called already.)
 */
void
finish_daemon(const char *desired_cwd)
{
  int nullfd;
  char c = '.';
  if (finish_daemon_called)
    return;
  if (!start_daemon_called)
    start_daemon();
  finish_daemon_called = 1;

  if (!desired_cwd)
    desired_cwd = "/";
   /* Don't hold the wrong FS mounted */
  if (chdir(desired_cwd) < 0) {
    log_err(LD_GENERAL,"chdir to \"%s\" failed. Exiting.",desired_cwd);
    exit(1); // exit ok: during daemonize, chdir failed.
  }

  nullfd = tor_open_cloexec("/dev/null", O_RDWR, 0);
  if (nullfd < 0) {
    /* LCOV_EXCL_START */
    log_err(LD_GENERAL,"/dev/null can't be opened. Exiting.");
    exit(1); // exit ok: during daemonize, couldn't open /dev/null
    /* LCOV_EXCL_STOP */
  }
  /* close fds linking to invoking terminal, but
   * close usual incoming fds, but redirect them somewhere
   * useful so the fds don't get reallocated elsewhere.
   */
  if (dup2(nullfd,0) < 0 ||
      dup2(nullfd,1) < 0 ||
      dup2(nullfd,2) < 0) {
    /* LCOV_EXCL_START */
    log_err(LD_GENERAL,"dup2 failed. Exiting.");
    exit(1); // exit ok: during daemonize, dup2 failed.
    /* LCOV_EXCL_STOP */
  }
  if (nullfd > 2)
    close(nullfd);
  /* signal success */
  if (write(daemon_filedes[1], &c, sizeof(char)) != sizeof(char)) {
    log_err(LD_GENERAL,"write failed. Exiting.");
  }
  close(daemon_filedes[1]);
}
#else /* !(!defined(_WIN32)) */
/* defined(_WIN32) */
void
start_daemon(void)
{
}
void
finish_daemon(const char *cp)
{
  (void)cp;
}
#endif /* !defined(_WIN32) */

/** Write the current process ID, followed by NL, into <b>filename</b>.
 * Return 0 on success, -1 on failure.
 */
int
write_pidfile(const char *filename)
{
  FILE *pidfile;

  if ((pidfile = fopen(filename, "w")) == NULL) {
    log_warn(LD_FS, "Unable to open \"%s\" for writing: %s", filename,
             strerror(errno));
    return -1;
  } else {
#ifdef _WIN32
    int pid = (int)_getpid();
#else
    int pid = (int)getpid();
#endif
    int rv = 0;
    if (fprintf(pidfile, "%d\n", pid) < 0)
      rv = -1;
    if (fclose(pidfile) < 0)
      rv = -1;
    return rv;
  }
}

#ifdef _WIN32
HANDLE
load_windows_system_library(const TCHAR *library_name)
{
  TCHAR path[MAX_PATH];
  unsigned n;
  n = GetSystemDirectory(path, MAX_PATH);
  if (n == 0 || n + _tcslen(library_name) + 2 >= MAX_PATH)
    return 0;
  _tcscat(path, TEXT("\\"));
  _tcscat(path, library_name);
  return LoadLibrary(path);
}
#endif /* defined(_WIN32) */

/** Format a single argument for being put on a Windows command line.
 * Returns a newly allocated string */
static char *
format_win_cmdline_argument(const char *arg)
{
  char *formatted_arg;
  char need_quotes;
  const char *c;
  int i;
  int bs_counter = 0;
  /* Backslash we can point to when one is inserted into the string */
  const char backslash = '\\';

  /* Smartlist of *char */
  smartlist_t *arg_chars;
  arg_chars = smartlist_new();

  /* Quote string if it contains whitespace or is empty */
  need_quotes = (strchr(arg, ' ') || strchr(arg, '\t') || '\0' == arg[0]);

  /* Build up smartlist of *chars */
  for (c=arg; *c != '\0'; c++) {
    if ('"' == *c) {
      /* Double up backslashes preceding a quote */
      for (i=0; i<(bs_counter*2); i++)
        smartlist_add(arg_chars, (void*)&backslash);
      bs_counter = 0;
      /* Escape the quote */
      smartlist_add(arg_chars, (void*)&backslash);
      smartlist_add(arg_chars, (void*)c);
    } else if ('\\' == *c) {
      /* Count backslashes until we know whether to double up */
      bs_counter++;
    } else {
      /* Don't double up slashes preceding a non-quote */
      for (i=0; i<bs_counter; i++)
        smartlist_add(arg_chars, (void*)&backslash);
      bs_counter = 0;
      smartlist_add(arg_chars, (void*)c);
    }
  }
  /* Don't double up trailing backslashes */
  for (i=0; i<bs_counter; i++)
    smartlist_add(arg_chars, (void*)&backslash);

  /* Allocate space for argument, quotes (if needed), and terminator */
  const size_t formatted_arg_len = smartlist_len(arg_chars) +
    (need_quotes ? 2 : 0) + 1;
  formatted_arg = tor_malloc_zero(formatted_arg_len);

  /* Add leading quote */
  i=0;
  if (need_quotes)
    formatted_arg[i++] = '"';

  /* Add characters */
  SMARTLIST_FOREACH(arg_chars, char*, ch,
  {
    formatted_arg[i++] = *ch;
  });

  /* Add trailing quote */
  if (need_quotes)
    formatted_arg[i++] = '"';
  formatted_arg[i] = '\0';

  smartlist_free(arg_chars);
  return formatted_arg;
}

/** Format a command line for use on Windows, which takes the command as a
 * string rather than string array. Follows the rules from "Parsing C++
 * Command-Line Arguments" in MSDN. Algorithm based on list2cmdline in the
 * Python subprocess module. Returns a newly allocated string */
char *
tor_join_win_cmdline(const char *argv[])
{
  smartlist_t *argv_list;
  char *joined_argv;
  int i;

  /* Format each argument and put the result in a smartlist */
  argv_list = smartlist_new();
  for (i=0; argv[i] != NULL; i++) {
    smartlist_add(argv_list, (void *)format_win_cmdline_argument(argv[i]));
  }

  /* Join the arguments with whitespace */
  joined_argv = smartlist_join_strings(argv_list, " ", 0, NULL);

  /* Free the newly allocated arguments, and the smartlist */
  SMARTLIST_FOREACH(argv_list, char *, arg,
  {
    tor_free(arg);
  });
  smartlist_free(argv_list);

  return joined_argv;
}

#ifndef _WIN32
/** Format <b>child_state</b> and <b>saved_errno</b> as a hex string placed in
 * <b>hex_errno</b>.  Called between fork and _exit, so must be signal-handler
 * safe.
 *
 * <b>hex_errno</b> must have at least HEX_ERRNO_SIZE+1 bytes available.
 *
 * The format of <b>hex_errno</b> is: "CHILD_STATE/ERRNO\n", left-padded
 * with spaces. CHILD_STATE indicates where
 * in the process of starting the child process did the failure occur (see
 * CHILD_STATE_* macros for definition), and SAVED_ERRNO is the value of
 * errno when the failure occurred.
 *
 * On success return the number of characters added to hex_errno, not counting
 * the terminating NUL; return -1 on error.
 */
STATIC int
format_helper_exit_status(unsigned char child_state, int saved_errno,
                          char *hex_errno)
{
  unsigned int unsigned_errno;
  int written, left;
  char *cur;
  size_t i;
  int res = -1;

  /* Fill hex_errno with spaces, and a trailing newline (memset may
     not be signal handler safe, so we can't use it) */
  for (i = 0; i < (HEX_ERRNO_SIZE - 1); i++)
    hex_errno[i] = ' ';
  hex_errno[HEX_ERRNO_SIZE - 1] = '\n';

  /* Convert errno to be unsigned for hex conversion */
  if (saved_errno < 0) {
    // Avoid overflow on the cast to unsigned int when result is INT_MIN
    // by adding 1 to the signed int negative value,
    // then, after it has been negated and cast to unsigned,
    // adding the original 1 back (the double-addition is intentional).
    // Otherwise, the cast to signed could cause a temporary int
    // to equal INT_MAX + 1, which is undefined.
    unsigned_errno = ((unsigned int) -(saved_errno + 1)) + 1;
  } else {
    unsigned_errno = (unsigned int) saved_errno;
  }

  /*
   * Count how many chars of space we have left, and keep a pointer into the
   * current point in the buffer.
   */
  left = HEX_ERRNO_SIZE+1;
  cur = hex_errno;

  /* Emit child_state */
  written = format_hex_number_sigsafe(child_state, cur, left);

  if (written <= 0)
    goto err;

  /* Adjust left and cur */
  left -= written;
  cur += written;
  if (left <= 0)
    goto err;

  /* Now the '/' */
  *cur = '/';

  /* Adjust left and cur */
  ++cur;
  --left;
  if (left <= 0)
    goto err;

  /* Need minus? */
  if (saved_errno < 0) {
    *cur = '-';
    ++cur;
    --left;
    if (left <= 0)
      goto err;
  }

  /* Emit unsigned_errno */
  written = format_hex_number_sigsafe(unsigned_errno, cur, left);

  if (written <= 0)
    goto err;

  /* Adjust left and cur */
  left -= written;
  cur += written;

  /* Check that we have enough space left for a newline and a NUL */
  if (left <= 1)
    goto err;

  /* Emit the newline and NUL */
  *cur++ = '\n';
  *cur++ = '\0';

  res = (int)(cur - hex_errno - 1);

  goto done;

 err:
  /*
   * In error exit, just write a '\0' in the first char so whatever called
   * this at least won't fall off the end.
   */
  *hex_errno = '\0';

 done:
  return res;
}
#endif /* !defined(_WIN32) */

/* Maximum number of file descriptors, if we cannot get it via sysconf() */
#define DEFAULT_MAX_FD 256

/** Terminate the process of <b>process_handle</b>, if that process has not
 * already exited.
 *
 * Return 0 if we succeeded in terminating the process (or if the process
 * already exited), and -1 if we tried to kill the process but failed.
 *
 * Based on code originally borrowed from Python's os.kill. */
int
tor_terminate_process(process_handle_t *process_handle)
{
#ifdef _WIN32
  if (tor_get_exit_code(process_handle, 0, NULL) == PROCESS_EXIT_RUNNING) {
    HANDLE handle = process_handle->pid.hProcess;

    if (!TerminateProcess(handle, 0))
      return -1;
    else
      return 0;
  }
#else /* !(defined(_WIN32)) */
  if (process_handle->waitpid_cb) {
    /* We haven't got a waitpid yet, so we can just kill off the process. */
    return kill(process_handle->pid, SIGTERM);
  }
#endif /* defined(_WIN32) */

  return 0; /* We didn't need to kill the process, so report success */
}

/** Return the Process ID of <b>process_handle</b>. */
int
tor_process_get_pid(process_handle_t *process_handle)
{
#ifdef _WIN32
  return (int) process_handle->pid.dwProcessId;
#else
  return (int) process_handle->pid;
#endif
}

#ifdef _WIN32
HANDLE
tor_process_get_stdout_pipe(process_handle_t *process_handle)
{
  return process_handle->stdout_pipe;
}
#else /* !(defined(_WIN32)) */
/* DOCDOC tor_process_get_stdout_pipe */
int
tor_process_get_stdout_pipe(process_handle_t *process_handle)
{
  return process_handle->stdout_pipe;
}
#endif /* defined(_WIN32) */

/* DOCDOC process_handle_new */
static process_handle_t *
process_handle_new(void)
{
  process_handle_t *out = tor_malloc_zero(sizeof(process_handle_t));

#ifdef _WIN32
  out->stdin_pipe = INVALID_HANDLE_VALUE;
  out->stdout_pipe = INVALID_HANDLE_VALUE;
  out->stderr_pipe = INVALID_HANDLE_VALUE;
#else
  out->stdin_pipe = -1;
  out->stdout_pipe = -1;
  out->stderr_pipe = -1;
#endif /* defined(_WIN32) */

  return out;
}

#ifndef _WIN32
/** Invoked when a process that we've launched via tor_spawn_background() has
 * been found to have terminated.
 */
static void
process_handle_waitpid_cb(int status, void *arg)
{
  process_handle_t *process_handle = arg;

  process_handle->waitpid_exit_status = status;
  clear_waitpid_callback(process_handle->waitpid_cb);
  if (process_handle->status == PROCESS_STATUS_RUNNING)
    process_handle->status = PROCESS_STATUS_NOTRUNNING;
  process_handle->waitpid_cb = 0;
}
#endif /* !defined(_WIN32) */

/**
 * @name child-process states
 *
 * Each of these values represents a possible state that a child process can
 * be in.  They're used to determine what to say when telling the parent how
 * far along we were before failure.
 *
 * @{
 */
#define CHILD_STATE_INIT 0
#define CHILD_STATE_PIPE 1
#define CHILD_STATE_MAXFD 2
#define CHILD_STATE_FORK 3
#define CHILD_STATE_DUPOUT 4
#define CHILD_STATE_DUPERR 5
#define CHILD_STATE_DUPIN 6
#define CHILD_STATE_CLOSEFD 7
#define CHILD_STATE_EXEC 8
#define CHILD_STATE_FAILEXEC 9
/** @} */
/**
 * Boolean.  If true, then Tor may call execve or CreateProcess via
 * tor_spawn_background.
 **/
static int may_spawn_background_process = 1;
/**
 * Turn off may_spawn_background_process, so that all future calls to
 * tor_spawn_background are guaranteed to fail.
 **/
void
tor_disable_spawning_background_processes(void)
{
  may_spawn_background_process = 0;
}
/** Start a program in the background. If <b>filename</b> contains a '/', then
 * it will be treated as an absolute or relative path.  Otherwise, on
 * non-Windows systems, the system path will be searched for <b>filename</b>.
 * On Windows, only the current directory will be searched. Here, to search the
 * system path (as well as the application directory, current working
 * directory, and system directories), set filename to NULL.
 *
 * The strings in <b>argv</b> will be passed as the command line arguments of
 * the child program (following convention, argv[0] should normally be the
 * filename of the executable, and this must be the case if <b>filename</b> is
 * NULL). The last element of argv must be NULL. A handle to the child process
 * will be returned in process_handle (which must be non-NULL). Read
 * process_handle.status to find out if the process was successfully launched.
 * For convenience, process_handle.status is returned by this function.
 *
 * Some parts of this code are based on the POSIX subprocess module from
 * Python, and example code from
 * http://msdn.microsoft.com/en-us/library/ms682499%28v=vs.85%29.aspx.
 */
int
tor_spawn_background(const char *const filename, const char **argv,
                     process_environment_t *env,
                     process_handle_t **process_handle_out)
{
  if (BUG(may_spawn_background_process == 0)) {
    /* We should never reach this point if we're forbidden to spawn
     * processes. Instead we should have caught the attempt earlier. */
    return PROCESS_STATUS_ERROR;
  }

#ifdef _WIN32
  HANDLE stdout_pipe_read = NULL;
  HANDLE stdout_pipe_write = NULL;
  HANDLE stderr_pipe_read = NULL;
  HANDLE stderr_pipe_write = NULL;
  HANDLE stdin_pipe_read = NULL;
  HANDLE stdin_pipe_write = NULL;
  process_handle_t *process_handle;
  int status;

  STARTUPINFOA siStartInfo;
  BOOL retval = FALSE;

  SECURITY_ATTRIBUTES saAttr;
  char *joined_argv;

  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  /* TODO: should we set explicit security attributes? (#2046, comment 5) */
  saAttr.lpSecurityDescriptor = NULL;

  /* Assume failure to start process */
  status = PROCESS_STATUS_ERROR;

  /* Set up pipe for stdout */
  if (!CreatePipe(&stdout_pipe_read, &stdout_pipe_write, &saAttr, 0)) {
    log_warn(LD_GENERAL,
      "Failed to create pipe for stdout communication with child process: %s",
      format_win32_error(GetLastError()));
    return status;
  }
  if (!SetHandleInformation(stdout_pipe_read, HANDLE_FLAG_INHERIT, 0)) {
    log_warn(LD_GENERAL,
      "Failed to configure pipe for stdout communication with child "
      "process: %s", format_win32_error(GetLastError()));
    return status;
  }

  /* Set up pipe for stderr */
  if (!CreatePipe(&stderr_pipe_read, &stderr_pipe_write, &saAttr, 0)) {
    log_warn(LD_GENERAL,
      "Failed to create pipe for stderr communication with child process: %s",
      format_win32_error(GetLastError()));
    return status;
  }
  if (!SetHandleInformation(stderr_pipe_read, HANDLE_FLAG_INHERIT, 0)) {
    log_warn(LD_GENERAL,
      "Failed to configure pipe for stderr communication with child "
      "process: %s", format_win32_error(GetLastError()));
    return status;
  }

  /* Set up pipe for stdin */
  if (!CreatePipe(&stdin_pipe_read, &stdin_pipe_write, &saAttr, 0)) {
    log_warn(LD_GENERAL,
      "Failed to create pipe for stdin communication with child process: %s",
      format_win32_error(GetLastError()));
    return status;
  }
  if (!SetHandleInformation(stdin_pipe_write, HANDLE_FLAG_INHERIT, 0)) {
    log_warn(LD_GENERAL,
      "Failed to configure pipe for stdin communication with child "
      "process: %s", format_win32_error(GetLastError()));
    return status;
  }

  /* Create the child process */

  /* Windows expects argv to be a whitespace delimited string, so join argv up
   */
  joined_argv = tor_join_win_cmdline(argv);

  process_handle = process_handle_new();
  process_handle->status = status;

  ZeroMemory(&(process_handle->pid), sizeof(PROCESS_INFORMATION));
  ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
  siStartInfo.cb = sizeof(STARTUPINFO);
  siStartInfo.hStdError = stderr_pipe_write;
  siStartInfo.hStdOutput = stdout_pipe_write;
  siStartInfo.hStdInput = stdin_pipe_read;
  siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

  /* Create the child process */

  retval = CreateProcessA(filename,      // module name
                 joined_argv,   // command line
  /* TODO: should we set explicit security attributes? (#2046, comment 5) */
                 NULL,          // process security attributes
                 NULL,          // primary thread security attributes
                 TRUE,          // handles are inherited
  /*(TODO: set CREATE_NEW CONSOLE/PROCESS_GROUP to make GetExitCodeProcess()
   * work?) */
                 CREATE_NO_WINDOW,             // creation flags
                 (env==NULL) ? NULL : env->windows_environment_block,
                 NULL,          // use parent's current directory
                 &siStartInfo,  // STARTUPINFO pointer
                 &(process_handle->pid));  // receives PROCESS_INFORMATION

  tor_free(joined_argv);

  if (!retval) {
    log_warn(LD_GENERAL,
      "Failed to create child process %s: %s", filename?filename:argv[0],
      format_win32_error(GetLastError()));
    tor_free(process_handle);
  } else  {
    /* TODO: Close hProcess and hThread in process_handle->pid? */
    process_handle->stdout_pipe = stdout_pipe_read;
    process_handle->stderr_pipe = stderr_pipe_read;
    process_handle->stdin_pipe = stdin_pipe_write;
    status = process_handle->status = PROCESS_STATUS_RUNNING;
  }

  /* TODO: Close pipes on exit */
  *process_handle_out = process_handle;
  return status;
#else /* !(defined(_WIN32)) */
  pid_t pid;
  int stdout_pipe[2];
  int stderr_pipe[2];
  int stdin_pipe[2];
  int fd, retval;
  process_handle_t *process_handle;
  int status;

  const char *error_message = SPAWN_ERROR_MESSAGE;
  size_t error_message_length;

  /* Represents where in the process of spawning the program is;
     this is used for printing out the error message */
  unsigned char child_state = CHILD_STATE_INIT;

  char hex_errno[HEX_ERRNO_SIZE + 2]; /* + 1 should be sufficient actually */

  static int max_fd = -1;

  status = PROCESS_STATUS_ERROR;

  /* We do the strlen here because strlen() is not signal handler safe,
     and we are not allowed to use unsafe functions between fork and exec */
  error_message_length = strlen(error_message);

  // child_state = CHILD_STATE_PIPE;

  /* Set up pipe for redirecting stdout, stderr, and stdin of child */
  retval = pipe(stdout_pipe);
  if (-1 == retval) {
    log_warn(LD_GENERAL,
      "Failed to set up pipe for stdout communication with child process: %s",
       strerror(errno));
    return status;
  }

  retval = pipe(stderr_pipe);
  if (-1 == retval) {
    log_warn(LD_GENERAL,
      "Failed to set up pipe for stderr communication with child process: %s",
      strerror(errno));

    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    return status;
  }

  retval = pipe(stdin_pipe);
  if (-1 == retval) {
    log_warn(LD_GENERAL,
      "Failed to set up pipe for stdin communication with child process: %s",
       strerror(errno));

    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);

    return status;
  }

  // child_state = CHILD_STATE_MAXFD;

#ifdef _SC_OPEN_MAX
  if (-1 == max_fd) {
    max_fd = (int) sysconf(_SC_OPEN_MAX);
    if (max_fd == -1) {
      max_fd = DEFAULT_MAX_FD;
      log_warn(LD_GENERAL,
               "Cannot find maximum file descriptor, assuming %d", max_fd);
    }
  }
#else /* !(defined(_SC_OPEN_MAX)) */
  max_fd = DEFAULT_MAX_FD;
#endif /* defined(_SC_OPEN_MAX) */

  // child_state = CHILD_STATE_FORK;

  pid = fork();
  if (0 == pid) {
    /* In child */

#if defined(HAVE_SYS_PRCTL_H) && defined(__linux__)
    /* Attempt to have the kernel issue a SIGTERM if the parent
     * goes away. Certain attributes of the binary being execve()ed
     * will clear this during the execve() call, but it's better
     * than nothing.
     */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif /* defined(HAVE_SYS_PRCTL_H) && defined(__linux__) */

    child_state = CHILD_STATE_DUPOUT;

    /* Link child stdout to the write end of the pipe */
    retval = dup2(stdout_pipe[1], STDOUT_FILENO);
    if (-1 == retval)
        goto error;

    child_state = CHILD_STATE_DUPERR;

    /* Link child stderr to the write end of the pipe */
    retval = dup2(stderr_pipe[1], STDERR_FILENO);
    if (-1 == retval)
        goto error;

    child_state = CHILD_STATE_DUPIN;

    /* Link child stdin to the read end of the pipe */
    retval = dup2(stdin_pipe[0], STDIN_FILENO);
    if (-1 == retval)
      goto error;

    // child_state = CHILD_STATE_CLOSEFD;

    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);

    /* Close all other fds, including the read end of the pipe */
    /* XXX: We should now be doing enough FD_CLOEXEC setting to make
     * this needless. */
    for (fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
      close(fd);
    }

    // child_state = CHILD_STATE_EXEC;

    /* Call the requested program. We need the cast because
       execvp doesn't define argv as const, even though it
       does not modify the arguments */
    if (env)
      execve(filename, (char *const *) argv, env->unixoid_environment_block);
    else {
      static char *new_env[] = { NULL };
      execve(filename, (char *const *) argv, new_env);
    }

    /* If we got here, the exec or open(/dev/null) failed */

    child_state = CHILD_STATE_FAILEXEC;

  error:
    {
      /* XXX: are we leaking fds from the pipe? */
      int n, err=0;
      ssize_t nbytes;

      n = format_helper_exit_status(child_state, errno, hex_errno);

      if (n >= 0) {
        /* Write the error message. GCC requires that we check the return
           value, but there is nothing we can do if it fails */
        /* TODO: Don't use STDOUT, use a pipe set up just for this purpose */
        nbytes = write(STDOUT_FILENO, error_message, error_message_length);
        err = (nbytes < 0);
        nbytes = write(STDOUT_FILENO, hex_errno, n);
        err += (nbytes < 0);
      }

      _exit(err?254:255); // exit ok: in child.
    }

    /* Never reached, but avoids compiler warning */
    return status; // LCOV_EXCL_LINE
  }

  /* In parent */

  if (-1 == pid) {
    log_warn(LD_GENERAL, "Failed to fork child process: %s", strerror(errno));
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    return status;
  }

  process_handle = process_handle_new();
  process_handle->status = status;
  process_handle->pid = pid;

  /* TODO: If the child process forked but failed to exec, waitpid it */

  /* Return read end of the pipes to caller, and close write end */
  process_handle->stdout_pipe = stdout_pipe[0];
  retval = close(stdout_pipe[1]);

  if (-1 == retval) {
    log_warn(LD_GENERAL,
            "Failed to close write end of stdout pipe in parent process: %s",
            strerror(errno));
  }

  process_handle->waitpid_cb = set_waitpid_callback(pid,
                                                    process_handle_waitpid_cb,
                                                    process_handle);

  process_handle->stderr_pipe = stderr_pipe[0];
  retval = close(stderr_pipe[1]);

  if (-1 == retval) {
    log_warn(LD_GENERAL,
            "Failed to close write end of stderr pipe in parent process: %s",
            strerror(errno));
  }

  /* Return write end of the stdin pipe to caller, and close the read end */
  process_handle->stdin_pipe = stdin_pipe[1];
  retval = close(stdin_pipe[0]);

  if (-1 == retval) {
    log_warn(LD_GENERAL,
            "Failed to close read end of stdin pipe in parent process: %s",
            strerror(errno));
  }

  status = process_handle->status = PROCESS_STATUS_RUNNING;
  /* Set stdin/stdout/stderr pipes to be non-blocking */
  if (fcntl(process_handle->stdout_pipe, F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(process_handle->stderr_pipe, F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(process_handle->stdin_pipe, F_SETFL, O_NONBLOCK) < 0) {
    log_warn(LD_GENERAL, "Failed to set stderror/stdout/stdin pipes "
             "nonblocking in parent process: %s", strerror(errno));
  }

  *process_handle_out = process_handle;
  return status;
#endif /* defined(_WIN32) */
}

/** Destroy all resources allocated by the process handle in
 *  <b>process_handle</b>.
 *  If <b>also_terminate_process</b> is true, also terminate the
 *  process of the process handle. */
MOCK_IMPL(void,
tor_process_handle_destroy,(process_handle_t *process_handle,
                            int also_terminate_process))
{
  if (!process_handle)
    return;

  if (also_terminate_process) {
    if (tor_terminate_process(process_handle) < 0) {
      const char *errstr =
#ifdef _WIN32
        format_win32_error(GetLastError());
#else
        strerror(errno);
#endif
      log_notice(LD_GENERAL, "Failed to terminate process with "
                 "PID '%d' ('%s').", tor_process_get_pid(process_handle),
                 errstr);
    } else {
      log_info(LD_GENERAL, "Terminated process with PID '%d'.",
               tor_process_get_pid(process_handle));
    }
  }

  process_handle->status = PROCESS_STATUS_NOTRUNNING;

#ifdef _WIN32
  if (process_handle->stdout_pipe)
    CloseHandle(process_handle->stdout_pipe);

  if (process_handle->stderr_pipe)
    CloseHandle(process_handle->stderr_pipe);

  if (process_handle->stdin_pipe)
    CloseHandle(process_handle->stdin_pipe);
#else /* !(defined(_WIN32)) */
  close(process_handle->stdout_pipe);
  close(process_handle->stderr_pipe);
  close(process_handle->stdin_pipe);

  clear_waitpid_callback(process_handle->waitpid_cb);
#endif /* defined(_WIN32) */

  memset(process_handle, 0x0f, sizeof(process_handle_t));
  tor_free(process_handle);
}

/** Get the exit code of a process specified by <b>process_handle</b> and store
 * it in <b>exit_code</b>, if set to a non-NULL value.  If <b>block</b> is set
 * to true, the call will block until the process has exited.  Otherwise if
 * the process is still running, the function will return
 * PROCESS_EXIT_RUNNING, and exit_code will be left unchanged. Returns
 * PROCESS_EXIT_EXITED if the process did exit. If there is a failure,
 * PROCESS_EXIT_ERROR will be returned and the contents of exit_code (if
 * non-NULL) will be undefined. N.B. Under *nix operating systems, this will
 * probably not work in Tor, because waitpid() is called in main.c to reap any
 * terminated child processes.*/
int
tor_get_exit_code(process_handle_t *process_handle,
                  int block, int *exit_code)
{
#ifdef _WIN32
  DWORD retval;
  BOOL success;

  if (block) {
    /* Wait for the process to exit */
    retval = WaitForSingleObject(process_handle->pid.hProcess, INFINITE);
    if (retval != WAIT_OBJECT_0) {
      log_warn(LD_GENERAL, "WaitForSingleObject() failed (%d): %s",
              (int)retval, format_win32_error(GetLastError()));
      return PROCESS_EXIT_ERROR;
    }
  } else {
    retval = WaitForSingleObject(process_handle->pid.hProcess, 0);
    if (WAIT_TIMEOUT == retval) {
      /* Process has not exited */
      return PROCESS_EXIT_RUNNING;
    } else if (retval != WAIT_OBJECT_0) {
      log_warn(LD_GENERAL, "WaitForSingleObject() failed (%d): %s",
               (int)retval, format_win32_error(GetLastError()));
      return PROCESS_EXIT_ERROR;
    }
  }

  if (exit_code != NULL) {
    success = GetExitCodeProcess(process_handle->pid.hProcess,
                                 (PDWORD)exit_code);
    if (!success) {
      log_warn(LD_GENERAL, "GetExitCodeProcess() failed: %s",
               format_win32_error(GetLastError()));
      return PROCESS_EXIT_ERROR;
    }
  }
#else /* !(defined(_WIN32)) */
  int stat_loc;
  int retval;

  if (process_handle->waitpid_cb) {
    /* We haven't processed a SIGCHLD yet. */
    retval = waitpid(process_handle->pid, &stat_loc, block?0:WNOHANG);
    if (retval == process_handle->pid) {
      clear_waitpid_callback(process_handle->waitpid_cb);
      process_handle->waitpid_cb = NULL;
      process_handle->waitpid_exit_status = stat_loc;
    }
  } else {
    /* We already got a SIGCHLD for this process, and handled it. */
    retval = process_handle->pid;
    stat_loc = process_handle->waitpid_exit_status;
  }

  if (!block && 0 == retval) {
    /* Process has not exited */
    return PROCESS_EXIT_RUNNING;
  } else if (retval != process_handle->pid) {
    log_warn(LD_GENERAL, "waitpid() failed for PID %d: %s",
             (int)process_handle->pid, strerror(errno));
    return PROCESS_EXIT_ERROR;
  }

  if (!WIFEXITED(stat_loc)) {
    log_warn(LD_GENERAL, "Process %d did not exit normally",
             (int)process_handle->pid);
    return PROCESS_EXIT_ERROR;
  }

  if (exit_code != NULL)
    *exit_code = WEXITSTATUS(stat_loc);
#endif /* defined(_WIN32) */

  return PROCESS_EXIT_EXITED;
}

/** Helper: return the number of characters in <b>s</b> preceding the first
 * occurrence of <b>ch</b>. If <b>ch</b> does not occur in <b>s</b>, return
 * the length of <b>s</b>. Should be equivalent to strspn(s, "ch"). */
static inline size_t
str_num_before(const char *s, char ch)
{
  const char *cp = strchr(s, ch);
  if (cp)
    return cp - s;
  else
    return strlen(s);
}

/** Return non-zero iff getenv would consider <b>s1</b> and <b>s2</b>
 * to have the same name as strings in a process's environment. */
int
environment_variable_names_equal(const char *s1, const char *s2)
{
  size_t s1_name_len = str_num_before(s1, '=');
  size_t s2_name_len = str_num_before(s2, '=');

  return (s1_name_len == s2_name_len &&
          tor_memeq(s1, s2, s1_name_len));
}

/** Free <b>env</b> (assuming it was produced by
 * process_environment_make). */
void
process_environment_free_(process_environment_t *env)
{
  if (env == NULL) return;

  /* As both an optimization hack to reduce consing on Unixoid systems
   * and a nice way to ensure that some otherwise-Windows-specific
   * code will always get tested before changes to it get merged, the
   * strings which env->unixoid_environment_block points to are packed
   * into env->windows_environment_block. */
  tor_free(env->unixoid_environment_block);
  tor_free(env->windows_environment_block);

  tor_free(env);
}

/** Make a process_environment_t containing the environment variables
 * specified in <b>env_vars</b> (as C strings of the form
 * "NAME=VALUE"). */
process_environment_t *
process_environment_make(struct smartlist_t *env_vars)
{
  process_environment_t *env = tor_malloc_zero(sizeof(process_environment_t));
  int n_env_vars = smartlist_len(env_vars);
  int i;
  size_t total_env_length;
  smartlist_t *env_vars_sorted;

  tor_assert(n_env_vars + 1 != 0);
  env->unixoid_environment_block = tor_calloc(n_env_vars + 1, sizeof(char *));
  /* env->unixoid_environment_block is already NULL-terminated,
   * because we assume that NULL == 0 (and check that during compilation). */

  total_env_length = 1; /* terminating NUL of terminating empty string */
  for (i = 0; i < n_env_vars; ++i) {
    const char *s = smartlist_get(env_vars, (int)i);
    size_t slen = strlen(s);

    tor_assert(slen + 1 != 0);
    tor_assert(slen + 1 < SIZE_MAX - total_env_length);
    total_env_length += slen + 1;
  }

  env->windows_environment_block = tor_malloc_zero(total_env_length);
  /* env->windows_environment_block is already
   * (NUL-terminated-empty-string)-terminated. */

  /* Some versions of Windows supposedly require that environment
   * blocks be sorted.  Or maybe some Windows programs (or their
   * runtime libraries) fail to look up strings in non-sorted
   * environment blocks.
   *
   * Also, sorting strings makes it easy to find duplicate environment
   * variables and environment-variable strings without an '=' on all
   * OSes, and they can cause badness.  Let's complain about those. */
  env_vars_sorted = smartlist_new();
  smartlist_add_all(env_vars_sorted, env_vars);
  smartlist_sort_strings(env_vars_sorted);

  /* Now copy the strings into the environment blocks. */
  {
    char *cp = env->windows_environment_block;
    const char *prev_env_var = NULL;

    for (i = 0; i < n_env_vars; ++i) {
      const char *s = smartlist_get(env_vars_sorted, (int)i);
      size_t slen = strlen(s);
      size_t s_name_len = str_num_before(s, '=');

      if (s_name_len == slen) {
        log_warn(LD_GENERAL,
                 "Preparing an environment containing a variable "
                 "without a value: %s",
                 s);
      }
      if (prev_env_var != NULL &&
          environment_variable_names_equal(s, prev_env_var)) {
        log_warn(LD_GENERAL,
                 "Preparing an environment containing two variables "
                 "with the same name: %s and %s",
                 prev_env_var, s);
      }

      prev_env_var = s;

      /* Actually copy the string into the environment. */
      memcpy(cp, s, slen+1);
      env->unixoid_environment_block[i] = cp;
      cp += slen+1;
    }

    tor_assert(cp == env->windows_environment_block + total_env_length - 1);
  }

  smartlist_free(env_vars_sorted);

  return env;
}

/** Return a newly allocated smartlist containing every variable in
 * this process's environment, as a NUL-terminated string of the form
 * "NAME=VALUE".  Note that on some/many/most/all OSes, the parent
 * process can put strings not of that form in our environment;
 * callers should try to not get crashed by that.
 *
 * The returned strings are heap-allocated, and must be freed by the
 * caller. */
struct smartlist_t *
get_current_process_environment_variables(void)
{
  smartlist_t *sl = smartlist_new();

  char **environ_tmp; /* Not const char ** ? Really? */
  for (environ_tmp = get_environment(); *environ_tmp; ++environ_tmp) {
    smartlist_add_strdup(sl, *environ_tmp);
  }

  return sl;
}

/** For each string s in <b>env_vars</b> such that
 * environment_variable_names_equal(s, <b>new_var</b>), remove it; if
 * <b>free_p</b> is non-zero, call <b>free_old</b>(s).  If
 * <b>new_var</b> contains '=', insert it into <b>env_vars</b>. */
void
set_environment_variable_in_smartlist(struct smartlist_t *env_vars,
                                      const char *new_var,
                                      void (*free_old)(void*),
                                      int free_p)
{
  SMARTLIST_FOREACH_BEGIN(env_vars, const char *, s) {
    if (environment_variable_names_equal(s, new_var)) {
      SMARTLIST_DEL_CURRENT(env_vars, s);
      if (free_p) {
        free_old((void *)s);
      }
    }
  } SMARTLIST_FOREACH_END(s);

  if (strchr(new_var, '=') != NULL) {
    smartlist_add(env_vars, (void *)new_var);
  }
}

#ifdef _WIN32
/** Read from a handle <b>h</b> into <b>buf</b>, up to <b>count</b> bytes.  If
 * <b>hProcess</b> is NULL, the function will return immediately if there is
 * nothing more to read. Otherwise <b>hProcess</b> should be set to the handle
 * to the process owning the <b>h</b>. In this case, the function will exit
 * only once the process has exited, or <b>count</b> bytes are read. Returns
 * the number of bytes read, or -1 on error. */
ssize_t
tor_read_all_handle(HANDLE h, char *buf, size_t count,
                    const process_handle_t *process)
{
  size_t numread = 0;
  BOOL retval;
  DWORD byte_count;
  BOOL process_exited = FALSE;

  if (count > SIZE_T_CEILING || count > SSIZE_MAX)
    return -1;

  while (numread < count) {
    /* Check if there is anything to read */
    retval = PeekNamedPipe(h, NULL, 0, NULL, &byte_count, NULL);
    if (!retval) {
      log_warn(LD_GENERAL,
        "Failed to peek from handle: %s",
        format_win32_error(GetLastError()));
      return -1;
    } else if (0 == byte_count) {
      /* Nothing available: process exited or it is busy */

      /* Exit if we don't know whether the process is running */
      if (NULL == process)
        break;

      /* The process exited and there's nothing left to read from it */
      if (process_exited)
        break;

      /* If process is not running, check for output one more time in case
         it wrote something after the peek was performed. Otherwise keep on
         waiting for output */
      tor_assert(process != NULL);
      byte_count = WaitForSingleObject(process->pid.hProcess, 0);
      if (WAIT_TIMEOUT != byte_count)
        process_exited = TRUE;

      continue;
    }

    /* There is data to read; read it */
    retval = ReadFile(h, buf+numread, count-numread, &byte_count, NULL);
    tor_assert(byte_count + numread <= count);
    if (!retval) {
      log_warn(LD_GENERAL, "Failed to read from handle: %s",
        format_win32_error(GetLastError()));
      return -1;
    } else if (0 == byte_count) {
      /* End of file */
      break;
    }
    numread += byte_count;
  }
  return (ssize_t)numread;
}
#else /* !(defined(_WIN32)) */
/** Read from a handle <b>fd</b> into <b>buf</b>, up to <b>count</b> bytes.  If
 * <b>process</b> is NULL, the function will return immediately if there is
 * nothing more to read. Otherwise data will be read until end of file, or
 * <b>count</b> bytes are read.  Returns the number of bytes read, or -1 on
 * error. Sets <b>eof</b> to true if <b>eof</b> is not NULL and the end of the
 * file has been reached. */
ssize_t
tor_read_all_handle(int fd, char *buf, size_t count,
                    const process_handle_t *process,
                    int *eof)
{
  size_t numread = 0;
  ssize_t result;

  if (eof)
    *eof = 0;

  if (count > SIZE_T_CEILING || count > SSIZE_MAX)
    return -1;

  while (numread < count) {
    result = read(fd, buf+numread, count-numread);

    if (result == 0) {
      log_debug(LD_GENERAL, "read() reached end of file");
      if (eof)
        *eof = 1;
      break;
    } else if (result < 0 && errno == EAGAIN) {
      if (process)
        continue;
      else
        break;
    } else if (result < 0) {
      log_warn(LD_GENERAL, "read() failed: %s", strerror(errno));
      return -1;
    }

    numread += result;
  }

  log_debug(LD_GENERAL, "read() read %d bytes from handle", (int)numread);
  return (ssize_t)numread;
}
#endif /* defined(_WIN32) */

/** Read from stdout of a process until the process exits. */
ssize_t
tor_read_all_from_process_stdout(const process_handle_t *process_handle,
                                 char *buf, size_t count)
{
#ifdef _WIN32
  return tor_read_all_handle(process_handle->stdout_pipe, buf, count,
                             process_handle);
#else
  return tor_read_all_handle(process_handle->stdout_pipe, buf, count,
                             process_handle, NULL);
#endif /* defined(_WIN32) */
}

/** Read from stdout of a process until the process exits. */
ssize_t
tor_read_all_from_process_stderr(const process_handle_t *process_handle,
                                 char *buf, size_t count)
{
#ifdef _WIN32
  return tor_read_all_handle(process_handle->stderr_pipe, buf, count,
                             process_handle);
#else
  return tor_read_all_handle(process_handle->stderr_pipe, buf, count,
                             process_handle, NULL);
#endif /* defined(_WIN32) */
}

/** Split buf into lines, and add to smartlist. The buffer <b>buf</b> will be
 * modified. The resulting smartlist will consist of pointers to buf, so there
 * is no need to free the contents of sl. <b>buf</b> must be a NUL-terminated
 * string. <b>len</b> should be set to the length of the buffer excluding the
 * NUL. Non-printable characters (including NUL) will be replaced with "." */
int
tor_split_lines(smartlist_t *sl, char *buf, int len)
{
  /* Index in buf of the start of the current line */
  int start = 0;
  /* Index in buf of the current character being processed */
  int cur = 0;
  /* Are we currently in a line */
  char in_line = 0;

  /* Loop over string */
  while (cur < len) {
    /* Loop until end of line or end of string */
    for (; cur < len; cur++) {
      if (in_line) {
        if ('\r' == buf[cur] || '\n' == buf[cur]) {
          /* End of line */
          buf[cur] = '\0';
          /* Point cur to the next line */
          cur++;
          /* Line starts at start and ends with a nul */
          break;
        } else {
          if (!TOR_ISPRINT(buf[cur]))
            buf[cur] = '.';
        }
      } else {
        if ('\r' == buf[cur] || '\n' == buf[cur]) {
          /* Skip leading vertical space */
          ;
        } else {
          in_line = 1;
          start = cur;
          if (!TOR_ISPRINT(buf[cur]))
            buf[cur] = '.';
        }
      }
    }
    /* We are at the end of the line or end of string. If in_line is true there
     * is a line which starts at buf+start and ends at a NUL. cur points to
     * the character after the NUL. */
    if (in_line)
      smartlist_add(sl, (void *)(buf+start));
    in_line = 0;
  }
  return smartlist_len(sl);
}

/** Return a string corresponding to <b>stream_status</b>. */
const char *
stream_status_to_string(enum stream_status stream_status)
{
  switch (stream_status) {
    case IO_STREAM_OKAY:
      return "okay";
    case IO_STREAM_EAGAIN:
      return "temporarily unavailable";
    case IO_STREAM_TERM:
      return "terminated";
    case IO_STREAM_CLOSED:
      return "closed";
    default:
      tor_fragile_assert();
      return "unknown";
  }
}

#ifdef _WIN32

/** Return a smartlist containing lines outputted from
 *  <b>handle</b>. Return NULL on error, and set
 *  <b>stream_status_out</b> appropriately. */
MOCK_IMPL(smartlist_t *,
tor_get_lines_from_handle, (HANDLE *handle,
                            enum stream_status *stream_status_out))
{
  int pos;
  char stdout_buf[600] = {0};
  smartlist_t *lines = NULL;

  tor_assert(stream_status_out);

  *stream_status_out = IO_STREAM_TERM;

  pos = tor_read_all_handle(handle, stdout_buf, sizeof(stdout_buf) - 1, NULL);
  if (pos < 0) {
    *stream_status_out = IO_STREAM_TERM;
    return NULL;
  }
  if (pos == 0) {
    *stream_status_out = IO_STREAM_EAGAIN;
    return NULL;
  }

  /* End with a null even if there isn't a \r\n at the end */
  /* TODO: What if this is a partial line? */
  stdout_buf[pos] = '\0';

  /* Split up the buffer */
  lines = smartlist_new();
  tor_split_lines(lines, stdout_buf, pos);

  /* Currently 'lines' is populated with strings residing on the
     stack. Replace them with their exact copies on the heap: */
  SMARTLIST_FOREACH(lines, char *, line,
                    SMARTLIST_REPLACE_CURRENT(lines, line, tor_strdup(line)));

  *stream_status_out = IO_STREAM_OKAY;

  return lines;
}

#else /* !(defined(_WIN32)) */

/** Return a smartlist containing lines outputted from
 *  <b>fd</b>. Return NULL on error, and set
 *  <b>stream_status_out</b> appropriately. */
MOCK_IMPL(smartlist_t *,
tor_get_lines_from_handle, (int fd, enum stream_status *stream_status_out))
{
  enum stream_status stream_status;
  char stdout_buf[400];
  smartlist_t *lines = NULL;

  while (1) {
    memset(stdout_buf, 0, sizeof(stdout_buf));

    stream_status = get_string_from_pipe(fd,
                                         stdout_buf, sizeof(stdout_buf) - 1);
    if (stream_status != IO_STREAM_OKAY)
      goto done;

    if (!lines) lines = smartlist_new();
    smartlist_split_string(lines, stdout_buf, "\n", 0, 0);
  }

 done:
  *stream_status_out = stream_status;
  return lines;
}

#endif /* defined(_WIN32) */

/** Reads from <b>fd</b> and stores input in <b>buf_out</b> making
 *  sure it's below <b>count</b> bytes.
 *  If the string has a trailing newline, we strip it off.
 *
 * This function is specifically created to handle input from managed
 * proxies, according to the pluggable transports spec. Make sure it
 * fits your needs before using it.
 *
 * Returns:
 * IO_STREAM_CLOSED: If the stream is closed.
 * IO_STREAM_EAGAIN: If there is nothing to read and we should check back
 *  later.
 * IO_STREAM_TERM: If something is wrong with the stream.
 * IO_STREAM_OKAY: If everything went okay and we got a string
 *  in <b>buf_out</b>. */
enum stream_status
get_string_from_pipe(int fd, char *buf_out, size_t count)
{
  ssize_t ret;

  tor_assert(count <= INT_MAX);

  ret = read(fd, buf_out, count);

  if (ret == 0)
    return IO_STREAM_CLOSED;
  else if (ret < 0 && errno == EAGAIN)
    return IO_STREAM_EAGAIN;
  else if (ret < 0)
    return IO_STREAM_TERM;

  if (buf_out[ret - 1] == '\n') {
    /* Remove the trailing newline */
    buf_out[ret - 1] = '\0';
  } else
    buf_out[ret] = '\0';

  return IO_STREAM_OKAY;
}

/** Initialize the insecure RNG <b>rng</b> from a seed value <b>seed</b>. */
void
tor_init_weak_random(tor_weak_rng_t *rng, unsigned seed)
{
  rng->state = (uint32_t)(seed & 0x7fffffff);
}

/** Return a randomly chosen value in the range 0..TOR_WEAK_RANDOM_MAX based
 * on the RNG state of <b>rng</b>.  This entropy will not be cryptographically
 * strong; do not rely on it for anything an adversary should not be able to
 * predict. */
int32_t
tor_weak_random(tor_weak_rng_t *rng)
{
  /* Here's a linear congruential generator. OpenBSD and glibc use these
   * parameters; they aren't too bad, and should have maximal period over the
   * range 0..INT32_MAX. We don't want to use the platform rand() or random(),
   * since some platforms have bad weak RNGs that only return values in the
   * range 0..INT16_MAX, which just isn't enough. */
  rng->state = (rng->state * 1103515245 + 12345) & 0x7fffffff;
  return (int32_t) rng->state;
}

/** Return a random number in the range [0 , <b>top</b>). {That is, the range
 * of integers i such that 0 <= i < top.}  Chooses uniformly.  Requires that
 * top is greater than 0. This randomness is not cryptographically strong; do
 * not rely on it for anything an adversary should not be able to predict. */
int32_t
tor_weak_random_range(tor_weak_rng_t *rng, int32_t top)
{
  /* We don't want to just do tor_weak_random() % top, since random() is often
   * implemented with an LCG whose modulus is a power of 2, and those are
   * cyclic in their low-order bits. */
  int divisor, result;
  tor_assert(top > 0);
  divisor = TOR_WEAK_RANDOM_MAX / top;
  do {
    result = (int32_t)(tor_weak_random(rng) / divisor);
  } while (result >= top);
  return result;
}

/** Cast a given double value to a int64_t. Return 0 if number is NaN.
 * Returns either INT64_MIN or INT64_MAX if number is outside of the int64_t
 * range. */
int64_t
clamp_double_to_int64(double number)
{
  int exponent;

#if defined(MINGW_ANY) && GCC_VERSION >= 409
/*
  Mingw's math.h uses gcc's __builtin_choose_expr() facility to declare
  isnan, isfinite, and signbit.  But as implemented in at least some
  versions of gcc, __builtin_choose_expr() can generate type warnings
  even from branches that are not taken.  So, suppress those warnings.
*/
#define PROBLEMATIC_FLOAT_CONVERSION_WARNING
DISABLE_GCC_WARNING(float-conversion)
#endif /* defined(MINGW_ANY) && GCC_VERSION >= 409 */

/*
  With clang 4.0 we apparently run into "double promotion" warnings here,
  since clang thinks we're promoting a double to a long double.
 */
#if defined(__clang__)
#if __has_warning("-Wdouble-promotion")
#define PROBLEMATIC_DOUBLE_PROMOTION_WARNING
DISABLE_GCC_WARNING(double-promotion)
#endif
#endif /* defined(__clang__) */

  /* NaN is a special case that can't be used with the logic below. */
  if (isnan(number)) {
    return 0;
  }

  /* Time to validate if result can overflows a int64_t value. Fun with
   * float! Find that exponent exp such that
   *    number == x * 2^exp
   * for some x with abs(x) in [0.5, 1.0). Note that this implies that the
   * magnitude of number is strictly less than 2^exp.
   *
   * If number is infinite, the call to frexp is legal but the contents of
   * are exponent unspecified. */
  frexp(number, &exponent);

  /* If the magnitude of number is strictly less than 2^63, the truncated
   * version of number is guaranteed to be representable. The only
   * representable integer for which this is not the case is INT64_MIN, but
   * it is covered by the logic below. */
  if (isfinite(number) && exponent <= 63) {
    return (int64_t)number;
  }

  /* Handle infinities and finite numbers with magnitude >= 2^63. */
  return signbit(number) ? INT64_MIN : INT64_MAX;

#ifdef PROBLEMATIC_DOUBLE_PROMOTION_WARNING
ENABLE_GCC_WARNING(double-promotion)
#endif
#ifdef PROBLEMATIC_FLOAT_CONVERSION_WARNING
ENABLE_GCC_WARNING(float-conversion)
#endif
}

/** Return a uint64_t value from <b>a</b> in network byte order. */
uint64_t
tor_htonll(uint64_t a)
{
#ifdef WORDS_BIGENDIAN
  /* Big endian. */
  return a;
#else /* WORDS_BIGENDIAN */
  /* Little endian. The worst... */
  return htonl((uint32_t)(a>>32)) |
    (((uint64_t)htonl((uint32_t)a))<<32);
#endif /* defined(WORDS_BIGENDIAN) */
}

/** Return a uint64_t value from <b>a</b> in host byte order. */
uint64_t
tor_ntohll(uint64_t a)
{
  return tor_htonll(a);
}
