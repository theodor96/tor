/* Copyright (c) 2001, Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef TOR_ESCAPE_H
#define TOR_ESCAPE_H

#include "orconfig.h"
#include "lib/cc/compat_compiler.h"
#include <stddef.h>

char *esc_for_log(const char *string) ATTR_MALLOC;
char *esc_for_log_len(const char *chars, size_t n) ATTR_MALLOC;
const char *escaped(const char *string);

#endif /* !defined(TOR_TORLOG_H) */
