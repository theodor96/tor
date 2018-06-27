/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef TOR_WIN32ERR_H
#define TOR_WIN32ERR_H

#include "orconfig.h"

/* Platform-specific helpers. */
#ifdef _WIN32
#include <windef.h>
char *format_win32_error(DWORD err);
#endif

#endif
