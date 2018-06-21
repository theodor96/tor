/* Copyright (c) 2001, Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file crypto_rand.c
 *
 * \brief Functions for initialising and seeding (pseudo-)random
 * number generators, and working with randomness.
 **/

#ifndef CRYPTO_RAND_PRIVATE
#define CRYPTO_RAND_PRIVATE

#include "lib/crypt_ops/crypto_rand.h"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif /* defined(_WIN32) */

#include "lib/container/smartlist.h"
#include "common/compat.h"
#include "lib/crypt_ops/compat_openssl.h"
#include "lib/crypt_ops/crypto_util.h"
#include "common/sandbox.h"
#include "lib/testsupport/testsupport.h"
#include "common/torlog.h"
#include "common/util.h"
#include "common/util_format.h"

DISABLE_GCC_WARNING(redundant-decls)
#include <openssl/rand.h>
ENABLE_GCC_WARNING(redundant-decls)

#if __GNUC__ && GCC_VERSION >= 402
#if GCC_VERSION >= 406
#pragma GCC diagnostic pop
#else
#pragma GCC diagnostic warning "-Wredundant-decls"
#endif
#endif /* __GNUC__ && GCC_VERSION >= 402 */

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif

/**
 * How many bytes of entropy we add at once.
 *
 * This is how much entropy OpenSSL likes to add right now, so maybe it will
 * work for us too.
 **/
#define ADD_ENTROPY 32

/**
 * Longest recognized DNS query.
 **/
#define MAX_DNS_LABEL_SIZE 63

/**
 * Largest strong entropy request permitted.
 **/
#define MAX_STRONGEST_RAND_SIZE 256

/**
 * Set the seed of the weak RNG to a random value.
 **/
void
crypto_seed_weak_rng(tor_weak_rng_t *rng)
{
  unsigned seed;
  crypto_rand((void*)&seed, sizeof(seed));
  tor_init_weak_random(rng, seed);
}

#ifdef TOR_UNIT_TESTS
int break_strongest_rng_syscall = 0;
int break_strongest_rng_fallback = 0;
#endif

/**
 * Try to get <b>out_len</b> bytes of the strongest entropy we can generate,
 * via system calls, storing it into <b>out</b>. Return 0 on success, -1 on
 * failure.  A maximum request size of 256 bytes is imposed.
 **/
static int
crypto_strongest_rand_syscall(uint8_t *out, size_t out_len)
{
  tor_assert(out_len <= MAX_STRONGEST_RAND_SIZE);

  /* We only log at notice-level here because in the case that this function
   * fails the crypto_strongest_rand_raw() caller will log with a warning-level
   * message and let crypto_strongest_rand() error out and finally terminating
   * Tor with an assertion error.
   */

#ifdef TOR_UNIT_TESTS
  if (break_strongest_rng_syscall)
    return -1;
#endif

#if defined(_WIN32)
  static int provider_set = 0;
  static HCRYPTPROV provider;

  if (!provider_set) {
    if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT)) {
      log_notice(LD_CRYPTO, "Unable to set Windows CryptoAPI provider [1].");
      return -1;
    }
    provider_set = 1;
  }
  if (!CryptGenRandom(provider, out_len, out)) {
    log_notice(LD_CRYPTO, "Unable get entropy from the Windows CryptoAPI.");
    return -1;
  }

  return 0;
#elif defined(__linux__) && defined(SYS_getrandom)
  static int getrandom_works = 1; /* Be optimistic about our chances... */

  /* getrandom() isn't as straightforward as getentropy(), and has
   * no glibc wrapper.
   *
   * As far as I can tell from getrandom(2) and the source code, the
   * requests we issue will always succeed (though it will block on the
   * call if /dev/urandom isn't seeded yet), since we are NOT specifying
   * GRND_NONBLOCK and the request is <= 256 bytes.
   *
   * The manpage is unclear on what happens if a signal interrupts the call
   * while the request is blocked due to lack of entropy....
   *
   * We optimistically assume that getrandom() is available and functional
   * because it is the way of the future, and 2 branch mispredicts pale in
   * comparison to the overheads involved with failing to open
   * /dev/srandom followed by opening and reading from /dev/urandom.
   */
  if (PREDICT_LIKELY(getrandom_works)) {
    long ret;
    /* A flag of '0' here means to read from '/dev/urandom', and to
     * block if insufficient entropy is available to service the
     * request.
     */
    const unsigned int flags = 0;
    do {
      ret = syscall(SYS_getrandom, out, out_len, flags);
    } while (ret == -1 && ((errno == EINTR) ||(errno == EAGAIN)));

    if (PREDICT_UNLIKELY(ret == -1)) {
      /* LCOV_EXCL_START we can't actually make the syscall fail in testing. */
      tor_assert(errno != EAGAIN);
      tor_assert(errno != EINTR);

      /* Useful log message for errno. */
      if (errno == ENOSYS) {
        log_notice(LD_CRYPTO, "Can't get entropy from getrandom()."
                   " You are running a version of Tor built to support"
                   " getrandom(), but the kernel doesn't implement this"
                   " function--probably because it is too old?"
                   " Trying fallback method instead.");
      } else {
        log_notice(LD_CRYPTO, "Can't get entropy from getrandom(): %s."
                              " Trying fallback method instead.",
                   strerror(errno));
      }

      getrandom_works = 0; /* Don't bother trying again. */
      return -1;
      /* LCOV_EXCL_STOP */
    }

    tor_assert(ret == (long)out_len);
    return 0;
  }

  return -1; /* getrandom() previously failed unexpectedly. */
#elif defined(HAVE_GETENTROPY)
  /* getentropy() is what Linux's getrandom() wants to be when it grows up.
   * the only gotcha is that requests are limited to 256 bytes.
   */
  return getentropy(out, out_len);
#else
  (void) out;
#endif /* defined(_WIN32) || ... */

  /* This platform doesn't have a supported syscall based random. */
  return -1;
}

/**
 * Try to get <b>out_len</b> bytes of the strongest entropy we can generate,
 * via the per-platform fallback mechanism, storing it into <b>out</b>.
 * Return 0 on success, -1 on failure.  A maximum request size of 256 bytes
 * is imposed.
 **/
static int
crypto_strongest_rand_fallback(uint8_t *out, size_t out_len)
{
#ifdef TOR_UNIT_TESTS
  if (break_strongest_rng_fallback)
    return -1;
#endif

#ifdef _WIN32
  /* Windows exclusively uses crypto_strongest_rand_syscall(). */
  (void)out;
  (void)out_len;
  return -1;
#else /* !(defined(_WIN32)) */
  static const char *filenames[] = {
    "/dev/srandom", "/dev/urandom", "/dev/random", NULL
  };
  int fd, i;
  size_t n;

  for (i = 0; filenames[i]; ++i) {
    log_debug(LD_FS, "Considering %s as entropy source", filenames[i]);
    fd = open(sandbox_intern_string(filenames[i]), O_RDONLY, 0);
    if (fd<0) continue;
    log_info(LD_CRYPTO, "Reading entropy from \"%s\"", filenames[i]);
    n = read_all(fd, (char*)out, out_len, 0);
    close(fd);
    if (n != out_len) {
      /* LCOV_EXCL_START
       * We can't make /dev/foorandom actually fail. */
      log_notice(LD_CRYPTO,
                 "Error reading from entropy source %s (read only %lu bytes).",
                 filenames[i],
                 (unsigned long)n);
      return -1;
      /* LCOV_EXCL_STOP */
    }

    return 0;
  }

  return -1;
#endif /* defined(_WIN32) */
}

/**
 * Try to get <b>out_len</b> bytes of the strongest entropy we can generate,
 * storing it into <b>out</b>. Return 0 on success, -1 on failure.  A maximum
 * request size of 256 bytes is imposed.
 **/
STATIC int
crypto_strongest_rand_raw(uint8_t *out, size_t out_len)
{
  static const size_t sanity_min_size = 16;
  static const int max_attempts = 3;
  tor_assert(out_len <= MAX_STRONGEST_RAND_SIZE);

  /* For buffers >= 16 bytes (128 bits), we sanity check the output by
   * zero filling the buffer and ensuring that it actually was at least
   * partially modified.
   *
   * Checking that any individual byte is non-zero seems like it would
   * fail too often (p = out_len * 1/256) for comfort, but this is an
   * "adjust according to taste" sort of check.
   */
  memwipe(out, 0, out_len);
  for (int i = 0; i < max_attempts; i++) {
    /* Try to use the syscall/OS favored mechanism to get strong entropy. */
    if (crypto_strongest_rand_syscall(out, out_len) != 0) {
      /* Try to use the less-favored mechanism to get strong entropy. */
      if (crypto_strongest_rand_fallback(out, out_len) != 0) {
        /* Welp, we tried.  Hopefully the calling code terminates the process
         * since we're basically boned without good entropy.
         */
        log_warn(LD_CRYPTO,
                 "Cannot get strong entropy: no entropy source found.");
        return -1;
      }
    }

    if ((out_len < sanity_min_size) || !tor_mem_is_zero((char*)out, out_len))
      return 0;
  }

  /* LCOV_EXCL_START
   *
   * We tried max_attempts times to fill a buffer >= 128 bits long,
   * and each time it returned all '0's.  Either the system entropy
   * source is busted, or the user should go out and buy a ticket to
   * every lottery on the planet.
   */
  log_warn(LD_CRYPTO, "Strong OS entropy returned all zero buffer.");

  return -1;
  /* LCOV_EXCL_STOP */
}

/**
 * Try to get <b>out_len</b> bytes of the strongest entropy we can generate,
 * storing it into <b>out</b>.
 **/
void
crypto_strongest_rand(uint8_t *out, size_t out_len)
{
#define DLEN SHA512_DIGEST_LENGTH
  /* We're going to hash DLEN bytes from the system RNG together with some
   * bytes from the openssl PRNG, in order to yield DLEN bytes.
   */
  uint8_t inp[DLEN*2];
  uint8_t tmp[DLEN];
  tor_assert(out);
  while (out_len) {
    crypto_rand((char*) inp, DLEN);
    if (crypto_strongest_rand_raw(inp+DLEN, DLEN) < 0) {
      // LCOV_EXCL_START
      log_err(LD_CRYPTO, "Failed to load strong entropy when generating an "
              "important key. Exiting.");
      /* Die with an assertion so we get a stack trace. */
      tor_assert(0);
      // LCOV_EXCL_STOP
    }
    if (out_len >= DLEN) {
      SHA512(inp, sizeof(inp), out);
      out += DLEN;
      out_len -= DLEN;
    } else {
      SHA512(inp, sizeof(inp), tmp);
      memcpy(out, tmp, out_len);
      break;
    }
  }
  memwipe(tmp, 0, sizeof(tmp));
  memwipe(inp, 0, sizeof(inp));
#undef DLEN
}

/**
 * Seed OpenSSL's random number generator with bytes from the operating
 * system.  Return 0 on success, -1 on failure.
 **/
int
crypto_seed_rng(void)
{
  int rand_poll_ok = 0, load_entropy_ok = 0;
  uint8_t buf[ADD_ENTROPY];

  /* OpenSSL has a RAND_poll function that knows about more kinds of
   * entropy than we do.  We'll try calling that, *and* calling our own entropy
   * functions.  If one succeeds, we'll accept the RNG as seeded. */
  rand_poll_ok = RAND_poll();
  if (rand_poll_ok == 0)
    log_warn(LD_CRYPTO, "RAND_poll() failed."); // LCOV_EXCL_LINE

  load_entropy_ok = !crypto_strongest_rand_raw(buf, sizeof(buf));
  if (load_entropy_ok) {
    RAND_seed(buf, sizeof(buf));
  }

  memwipe(buf, 0, sizeof(buf));

  if ((rand_poll_ok || load_entropy_ok) && RAND_status() == 1)
    return 0;
  else
    return -1;
}

/**
 * Write <b>n</b> bytes of strong random data to <b>to</b>. Supports mocking
 * for unit tests.
 *
 * This function is not allowed to fail; if it would fail to generate strong
 * entropy, it must terminate the process instead.
 **/
MOCK_IMPL(void,
crypto_rand, (char *to, size_t n))
{
  crypto_rand_unmocked(to, n);
}

/**
 * Write <b>n</b> bytes of strong random data to <b>to</b>.  Most callers
 * will want crypto_rand instead.
 *
 * This function is not allowed to fail; if it would fail to generate strong
 * entropy, it must terminate the process instead.
 **/
void
crypto_rand_unmocked(char *to, size_t n)
{
  int r;
  if (n == 0)
    return;

  tor_assert(n < INT_MAX);
  tor_assert(to);
  r = RAND_bytes((unsigned char*)to, (int)n);
  /* We consider a PRNG failure non-survivable. Let's assert so that we get a
   * stack trace about where it happened.
   */
  tor_assert(r >= 0);
}

/**
 * Return a pseudorandom integer, chosen uniformly from the values
 * between 0 and <b>max</b>-1 inclusive.  <b>max</b> must be between 1 and
 * INT_MAX+1, inclusive.
 */
int
crypto_rand_int(unsigned int max)
{
  unsigned int val;
  unsigned int cutoff;
  tor_assert(max <= ((unsigned int)INT_MAX)+1);
  tor_assert(max > 0); /* don't div by 0 */

  /* We ignore any values that are >= 'cutoff,' to avoid biasing the
   * distribution with clipping at the upper end of unsigned int's
   * range.
   */
  cutoff = UINT_MAX - (UINT_MAX%max);
  while (1) {
    crypto_rand((char*)&val, sizeof(val));
    if (val < cutoff)
      return val % max;
  }
}

/**
 * Return a pseudorandom integer, chosen uniformly from the values i such
 * that min <= i < max.
 *
 * <b>min</b> MUST be in range [0, <b>max</b>).
 * <b>max</b> MUST be in range (min, INT_MAX].
 **/
int
crypto_rand_int_range(unsigned int min, unsigned int max)
{
  tor_assert(min < max);
  tor_assert(max <= INT_MAX);

  /* The overflow is avoided here because crypto_rand_int() returns a value
   * between 0 and (max - min) inclusive. */
  return min + crypto_rand_int(max - min);
}

/**
 * As crypto_rand_int_range, but supports uint64_t.
 **/
uint64_t
crypto_rand_uint64_range(uint64_t min, uint64_t max)
{
  tor_assert(min < max);
  return min + crypto_rand_uint64(max - min);
}

/**
 * As crypto_rand_int_range, but supports time_t.
 **/
time_t
crypto_rand_time_range(time_t min, time_t max)
{
  tor_assert(min < max);
  return min + (time_t)crypto_rand_uint64(max - min);
}

/**
 * Return a pseudorandom 64-bit integer, chosen uniformly from the values
 * between 0 and <b>max</b>-1 inclusive.
 **/
uint64_t
crypto_rand_uint64(uint64_t max)
{
  uint64_t val;
  uint64_t cutoff;
  tor_assert(max < UINT64_MAX);
  tor_assert(max > 0); /* don't div by 0 */

  /* We ignore any values that are >= 'cutoff,' to avoid biasing the
   * distribution with clipping at the upper end of unsigned int's
   * range.
   */
  cutoff = UINT64_MAX - (UINT64_MAX%max);
  while (1) {
    crypto_rand((char*)&val, sizeof(val));
    if (val < cutoff)
      return val % max;
  }
}

/**
 * Return a pseudorandom double d, chosen uniformly from the range
 * 0.0 <= d < 1.0.
 **/
double
crypto_rand_double(void)
{
  /* We just use an unsigned int here; we don't really care about getting
   * more than 32 bits of resolution */
  unsigned int u;
  crypto_rand((char*)&u, sizeof(u));
#if SIZEOF_INT == 4
#define UINT_MAX_AS_DOUBLE 4294967296.0
#elif SIZEOF_INT == 8
#define UINT_MAX_AS_DOUBLE 1.8446744073709552e+19
#else
#error SIZEOF_INT is neither 4 nor 8
#endif /* SIZEOF_INT == 4 || ... */
  return ((double)u) / UINT_MAX_AS_DOUBLE;
}

/**
 * Generate and return a new random hostname starting with <b>prefix</b>,
 * ending with <b>suffix</b>, and containing no fewer than
 * <b>min_rand_len</b> and no more than <b>max_rand_len</b> random base32
 * characters. Does not check for failure.
 *
 * Clip <b>max_rand_len</b> to MAX_DNS_LABEL_SIZE.
 **/
char *
crypto_random_hostname(int min_rand_len, int max_rand_len, const char *prefix,
                       const char *suffix)
{
  char *result, *rand_bytes;
  int randlen, rand_bytes_len;
  size_t resultlen, prefixlen;

  if (max_rand_len > MAX_DNS_LABEL_SIZE)
    max_rand_len = MAX_DNS_LABEL_SIZE;
  if (min_rand_len > max_rand_len)
    min_rand_len = max_rand_len;

  randlen = crypto_rand_int_range(min_rand_len, max_rand_len+1);

  prefixlen = strlen(prefix);
  resultlen = prefixlen + strlen(suffix) + randlen + 16;

  rand_bytes_len = ((randlen*5)+7)/8;
  if (rand_bytes_len % 5)
    rand_bytes_len += 5 - (rand_bytes_len%5);
  rand_bytes = tor_malloc(rand_bytes_len);
  crypto_rand(rand_bytes, rand_bytes_len);

  result = tor_malloc(resultlen);
  memcpy(result, prefix, prefixlen);
  base32_encode(result+prefixlen, resultlen-prefixlen,
                rand_bytes, rand_bytes_len);
  tor_free(rand_bytes);
  strlcpy(result+prefixlen+randlen, suffix, resultlen-(prefixlen+randlen));

  return result;
}

/**
 * Return a randomly chosen element of <b>sl</b>; or NULL if <b>sl</b>
 * is empty.
 **/
void *
smartlist_choose(const smartlist_t *sl)
{
  int len = smartlist_len(sl);
  if (len)
    return smartlist_get(sl,crypto_rand_int(len));
  return NULL; /* no elements to choose from */
}

/**
 * Scramble the elements of <b>sl</b> into a random order.
 **/
void
smartlist_shuffle(smartlist_t *sl)
{
  int i;
  /* From the end of the list to the front, choose at random from the
     positions we haven't looked at yet, and swap that position into the
     current position.  Remember to give "no swap" the same probability as
     any other swap. */
  for (i = smartlist_len(sl)-1; i > 0; --i) {
    int j = crypto_rand_int(i+1);
    smartlist_swap(sl, i, j);
  }
}

/** Make sure that openssl is using its default PRNG. Return 1 if we had to
 * adjust it; 0 otherwise. */
int
crypto_force_rand_ssleay(void)
{
  RAND_METHOD *default_method;
  default_method = RAND_OpenSSL();
  if (RAND_get_rand_method() != default_method) {
    log_notice(LD_CRYPTO, "It appears that one of our engines has provided "
               "a replacement the OpenSSL RNG. Resetting it to the default "
               "implementation.");
    RAND_set_rand_method(default_method);
    return 1;
  }
  return 0;
}

#endif /* !defined(CRYPTO_RAND_PRIVATE) */
