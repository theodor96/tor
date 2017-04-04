/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2017, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "compat.h"
#include "confline.h"
#include "torlog.h"
#include "util.h"

/** Helper: allocate a new configuration option mapping 'key' to 'val',
 * append it to *<b>lst</b>. */
void
config_line_append(config_line_t **lst,
                   const char *key,
                   const char *val)
{
  config_line_t *newline;

  newline = tor_malloc_zero(sizeof(config_line_t));
  newline->key = tor_strdup(key);
  newline->value = tor_strdup(val);
  newline->next = NULL;
  while (*lst)
    lst = &((*lst)->next);

  (*lst) = newline;
}

/** Return the line in <b>lines</b> whose key is exactly <b>key</b>, or NULL
 * if no such key exists. For handling commandline-only options only; other
 * options should be looked up in the appropriate data structure. */
const config_line_t *
config_line_find(const config_line_t *lines,
                 const char *key)
{
  const config_line_t *cl;
  for (cl = lines; cl; cl = cl->next) {
    if (!strcmp(cl->key, key))
      return cl;
  }
  return NULL;
}

/** Helper: parse the config string and strdup into key/value
 * strings. Set *result to the list, or NULL if parsing the string
 * failed.  Return 0 on success, -1 on failure. Warn and ignore any
 * misformatted lines.
 *
 * If <b>extended</b> is set, then treat keys beginning with / and with + as
 * indicating "clear" and "append" respectively. */
int
config_get_lines(const char *string, config_line_t **result, int extended)
{
  config_line_t *list = NULL, **next;
  char *k, *v;
  const char *parse_err;

  next = &list;
  do {
    k = v = NULL;
    string = parse_config_line_from_str_verbose(string, &k, &v, &parse_err);
    if (!string) {
      log_warn(LD_CONFIG, "Error while parsing configuration: %s",
               parse_err?parse_err:"<unknown>");
      config_free_lines(list);
      tor_free(k);
      tor_free(v);
      return -1;
    }
    if (k && v) {
      unsigned command = CONFIG_LINE_NORMAL;
      if (extended) {
        if (k[0] == '+') {
          char *k_new = tor_strdup(k+1);
          tor_free(k);
          k = k_new;
          command = CONFIG_LINE_APPEND;
        } else if (k[0] == '/') {
          char *k_new = tor_strdup(k+1);
          tor_free(k);
          k = k_new;
          tor_free(v);
          v = tor_strdup("");
          command = CONFIG_LINE_CLEAR;
        }
      }
      /* This list can get long, so we keep a pointer to the end of it
       * rather than using config_line_append over and over and getting
       * n^2 performance. */
      *next = tor_malloc_zero(sizeof(config_line_t));
      (*next)->key = k;
      (*next)->value = v;
      (*next)->next = NULL;
      (*next)->command = command;
      next = &((*next)->next);
    } else {
      tor_free(k);
      tor_free(v);
    }
  } while (*string);

  *result = list;
  return 0;
}

/**
 * Free all the configuration lines on the linked list <b>front</b>.
 */
void
config_free_lines(config_line_t *front)
{
  config_line_t *tmp;

  while (front) {
    tmp = front;
    front = tmp->next;

    tor_free(tmp->key);
    tor_free(tmp->value);
    tor_free(tmp);
  }
}

/** Return a newly allocated deep copy of the lines in <b>inp</b>. */
config_line_t *
config_lines_dup(const config_line_t *inp)
{
  return config_lines_dup_and_filter(inp, NULL);
}

/** Return a newly allocated deep copy of the lines in <b>inp</b>,
 * but only the ones that match <b>key</b>. */
config_line_t *
config_lines_dup_and_filter(const config_line_t *inp,
                            const char *key)
{
  config_line_t *result = NULL;
  config_line_t **next_out = &result;
  while (inp) {
    if (key && strcasecmpstart(inp->key, key)) {
      inp = inp->next;
      continue;
    }
    *next_out = tor_malloc_zero(sizeof(config_line_t));
    (*next_out)->key = tor_strdup(inp->key);
    (*next_out)->value = tor_strdup(inp->value);
    inp = inp->next;
    next_out = &((*next_out)->next);
  }
  (*next_out) = NULL;
  return result;
}

/** Return true iff a and b contain identical keys and values in identical
 * order. */
int
config_lines_eq(config_line_t *a, config_line_t *b)
{
  while (a && b) {
    if (strcasecmp(a->key, b->key) || strcmp(a->value, b->value))
      return 0;
    a = a->next;
    b = b->next;
  }
  if (a || b)
    return 0;
  return 1;
}

/** Return the number of lines in <b>a</b> whose key is <b>key</b>. */
int
config_count_key(const config_line_t *a, const char *key)
{
  int n = 0;
  while (a) {
    if (!strcasecmp(a->key, key)) {
      ++n;
    }
    a = a->next;
  }
  return n;
}

/** Given a string containing part of a configuration file or similar format,
 * advance past comments and whitespace and try to parse a single line.  If we
 * parse a line successfully, set *<b>key_out</b> to a new string holding the
 * key portion and *<b>value_out</b> to a new string holding the value portion
 * of the line, and return a pointer to the start of the next line.  If we run
 * out of data, return a pointer to the end of the string.  If we encounter an
 * error, return NULL and set *<b>err_out</b> (if provided) to an error
 * message.
 */
const char *
parse_config_line_from_str_verbose(const char *line, char **key_out,
                                   char **value_out,
                                   const char **err_out)
{
  /*
    See torrc_format.txt for a description of the (silly) format this parses.
   */
  const char *key, *val, *cp;
  int continuation = 0;

  tor_assert(key_out);
  tor_assert(value_out);

  *key_out = *value_out = NULL;
  key = val = NULL;
  /* Skip until the first keyword. */
  while (1) {
    while (TOR_ISSPACE(*line))
      ++line;
    if (*line == '#') {
      while (*line && *line != '\n')
        ++line;
    } else {
      break;
    }
  }

  if (!*line) { /* End of string? */
    *key_out = *value_out = NULL;
    return line;
  }

  /* Skip until the next space or \ followed by newline. */
  key = line;
  while (*line && !TOR_ISSPACE(*line) && *line != '#' &&
         ! (line[0] == '\\' && line[1] == '\n'))
    ++line;
  *key_out = tor_strndup(key, line-key);

  /* Skip until the value. */
  while (*line == ' ' || *line == '\t')
    ++line;

  val = line;

  /* Find the end of the line. */
  if (*line == '\"') { // XXX No continuation handling is done here
    if (!(line = unescape_string(line, value_out, NULL))) {
      if (err_out)
        *err_out = "Invalid escape sequence in quoted string";
      return NULL;
    }
    while (*line == ' ' || *line == '\t')
      ++line;
    if (*line == '\r' && *(++line) == '\n')
      ++line;
    if (*line && *line != '#' && *line != '\n') {
      if (err_out)
        *err_out = "Excess data after quoted string";
      return NULL;
    }
  } else {
    /* Look for the end of the line. */
    while (*line && *line != '\n' && (*line != '#' || continuation)) {
      if (*line == '\\' && line[1] == '\n') {
        continuation = 1;
        line += 2;
      } else if (*line == '#') {
        do {
          ++line;
        } while (*line && *line != '\n');
        if (*line == '\n')
          ++line;
      } else {
        ++line;
      }
    }

    if (*line == '\n') {
      cp = line++;
    } else {
      cp = line;
    }
    /* Now back cp up to be the last nonspace character */
    while (cp>val && TOR_ISSPACE(*(cp-1)))
      --cp;

    tor_assert(cp >= val);

    /* Now copy out and decode the value. */
    *value_out = tor_strndup(val, cp-val);
    if (continuation) {
      char *v_out, *v_in;
      v_out = v_in = *value_out;
      while (*v_in) {
        if (*v_in == '#') {
          do {
            ++v_in;
          } while (*v_in && *v_in != '\n');
          if (*v_in == '\n')
            ++v_in;
        } else if (v_in[0] == '\\' && v_in[1] == '\n') {
          v_in += 2;
        } else {
          *v_out++ = *v_in++;
        }
      }
      *v_out = '\0';
    }
  }

  if (*line == '#') {
    do {
      ++line;
    } while (*line && *line != '\n');
  }
  while (TOR_ISSPACE(*line)) ++line;

  return line;
}

