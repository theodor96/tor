/* Copyright (c) 2001, Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file torerr.h
 *
 * \brief Headers for torerr.c.
 **/

#ifndef TOR_TORERR_H
#define TOR_TORERR_H

/* The raw_assert...() variants are for use within code that can't call
 * tor_assertion_failed_() because of call circularity issues. */
#define raw_assert(expr) do {                                           \
    if (!(expr)) {                                                      \
      tor_raw_assertion_failed_msg_(__FILE__, __LINE__, #expr, NULL);   \
      abort();                                                          \
    }                                                                   \
  } while (0)
#define raw_assert_unreached(expr) raw_assert(0)
#define raw_assert_unreached_msg(msg) do {                          \
    tor_raw_assertion_failed_msg_(__FILE__, __LINE__, "0", (msg));  \
    abort();                                                        \
  } while (0)

void tor_raw_assertion_failed_msg_(const char *file, int line,
                                   const char *expr,
                                   const char *msg);

/** Maximum number of fds that will get notifications if we crash */
#define TOR_SIGSAFE_LOG_MAX_FDS 8

void tor_log_err_sigsafe(const char *m, ...);
int tor_log_get_sigsafe_err_fds(const int **out);
void tor_log_set_sigsafe_err_fds(const int *fds, int n);
void tor_log_sigsafe_err_set_granularity(int ms);

int format_hex_number_sigsafe(unsigned long x, char *buf, int max_len);
int format_dec_number_sigsafe(unsigned long x, char *buf, int max_len);

#endif /* !defined(TOR_TORLOG_H) */
