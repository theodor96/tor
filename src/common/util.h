/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file util.h
 * \brief Headers for util.c
 **/

#ifndef TOR_UTIL_H
#define TOR_UTIL_H

#include "orconfig.h"
#include "lib/cc/torint.h"
#include "common/compat.h"
#include "lib/ctime/di_ops.h"
#include "lib/testsupport/testsupport.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
/* for the correct alias to struct stat */
#include <sys/stat.h>
#endif
#include "lib/err/torerr.h"
#include "lib/malloc/util_malloc.h"
#include "lib/wallclock/approx_time.h"
#include "lib/string/util_string.h"
#include "lib/string/parse_int.h"
#include "lib/string/scanf.h"
#include "lib/intmath/bits.h"
#include "lib/intmath/addsub.h"
#include "lib/intmath/muldiv.h"
#include "lib/intmath/cmp.h"
#include "lib/log/ratelim.h"
#include "lib/log/util_bug.h"
#include "lib/log/escape.h"
#include "lib/fs/dir.h"
#include "lib/fs/files.h"
#include "lib/fs/path.h"
#include "lib/encoding/time_fmt.h"
#include "lib/encoding/cstring.h"

void tor_log_mallinfo(int severity);

/** Macro: yield a pointer to an enclosing structure given a pointer to
 * a substructure at offset <b>off</b>. Example:
 * <pre>
 *   struct base { ... };
 *   struct subtype { int x; struct base b; } x;
 *   struct base *bp = &x.base;
 *   struct *sp = SUBTYPE_P(bp, struct subtype, b);
 * </pre>
 */
#define SUBTYPE_P(p, subtype, basemember) \
  ((void*) ( ((char*)(p)) - offsetof(subtype, basemember) ))

/* Logic */
/** Macro: true if two values have the same boolean value. */
#define bool_eq(a,b) (!(a)==!(b))
/** Macro: true if two values have different boolean values. */
#define bool_neq(a,b) (!(a)!=!(b))

/* Math functions */

/* String manipulation */

/* Time helpers */

/* File helpers */

#define write_all(fd, buf, count, isSock) \
  ((isSock) ? write_all_to_socket((fd), (buf), (count)) \
            : write_all_to_fd((int)(fd), (buf), (count)))
#define read_all(fd, buf, count, isSock) \
  ((isSock) ? read_all_from_socket((fd), (buf), (count)) \
            : read_all_from_fd((int)(fd), (buf), (count)))

#ifdef _WIN32
HANDLE load_windows_system_library(const TCHAR *library_name);
#endif

#endif /* !defined(TOR_UTIL_H) */
