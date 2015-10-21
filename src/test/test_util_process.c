/* Copyright (c) 2010-2015, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#define UTIL_PROCESS_PRIVATE
#include "orconfig.h"
#include "or.h"

#include "test.h"

#include "util_process.h"

#include "log_test_helpers.h"

#define NS_MODULE util_process

static void
temp_callback(int r, void *s)
{
  (void)r;
  (void)s;
}

static void
test_util_process_set_waitpid_callback(void *ignored)
{
  (void)ignored;
  waitpid_callback_t *res1 = NULL, *res2 = NULL;
  int previous_log = setup_capture_of_logs(LOG_WARN);
  pid_t pid = (pid_t)42;

  res1 = set_waitpid_callback(pid, temp_callback, NULL);
  tt_assert(res1);

  res2 = set_waitpid_callback(pid, temp_callback, NULL);
  tt_assert(res2);
  tt_str_op(mock_saved_log_at(0), OP_EQ,
            "Replaced a waitpid monitor on pid 42. That should be "
            "impossible.\n");

 done:
  teardown_capture_of_logs(previous_log);
  clear_waitpid_callback(res1);
  clear_waitpid_callback(res2);
}

static void
test_util_process_clear_waitpid_callback(void *ignored)
{
  (void)ignored;
  waitpid_callback_t *res;
  int previous_log = setup_capture_of_logs(LOG_WARN);
  pid_t pid = (pid_t)43;

  clear_waitpid_callback(NULL);

  res = set_waitpid_callback(pid, temp_callback, NULL);
  clear_waitpid_callback(res);
  tt_int_op(mock_saved_log_number(), OP_EQ, 0);

#if 0
  /* No.  This is use-after-free.  We don't _do_ that. XXXX */
  clear_waitpid_callback(res);
  tt_str_op(mock_saved_log_at(0), OP_EQ,
            "Couldn't remove waitpid monitor for pid 43.\n");
#endif

 done:
  teardown_capture_of_logs(previous_log);
}

struct testcase_t util_process_tests[] = {
  { "set_waitpid_callback", test_util_process_set_waitpid_callback, 0,
    NULL, NULL },
  { "clear_waitpid_callback", test_util_process_clear_waitpid_callback, 0,
    NULL, NULL },
  END_OF_TESTCASES
};

