/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2017, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "or.h"
#include "buffers.h"
#include "proto_control0.h"

/** Return 1 iff buf looks more like it has an (obsolete) v0 controller
 * command on it than any valid v1 controller command. */
int
peek_buf_has_control0_command(buf_t *buf)
{
  if (buf_datalen(buf) >= 4) {
    char header[4];
    uint16_t cmd;
    peek_from_buf(header, sizeof(header), buf);
    cmd = ntohs(get_uint16(header+2));
    if (cmd <= 0x14)
      return 1; /* This is definitely not a v1 control command. */
  }
  return 0;
}

