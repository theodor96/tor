/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file compat.c
 * \brief Wrappers to make calls more portable.  This code defines
 * functions such as tor_snprintf, get/set various data types,
 * renaming, setting socket options, switching user IDs.  It is basically
 * where the non-portable items are conditionally included depending on
 * the platform.
 **/

#define COMPAT_PRIVATE
#include "common/compat.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <sys/locking.h>
#endif

#ifdef HAVE_UNAME
#include <sys/utsname.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_CRT_EXTERNS_H
#include <crt_externs.h>
#endif
#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#ifdef _WIN32
#include <conio.h>
#include <wchar.h>
/* Some mingw headers lack these. :p */
#if defined(HAVE_DECL__GETWCH) && !HAVE_DECL__GETWCH
wint_t _getwch(void);
#endif
#ifndef WEOF
#define WEOF (wchar_t)(0xFFFF)
#endif
#if defined(HAVE_DECL_SECUREZEROMEMORY) && !HAVE_DECL_SECUREZEROMEMORY
static inline void
SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
  volatile char *vcptr = (volatile char*)ptr;
  while (cnt--)
    *vcptr++ = 0;
}
#endif /* defined(HAVE_DECL_SECUREZEROMEMORY) && !HAVE_DECL_SECUREZEROMEMORY */
#elif defined(HAVE_READPASSPHRASE_H)
#include <readpassphrase.h>
#else
#include "tor_readpassphrase.h"
#endif /* defined(_WIN32) || ... */

/* Includes for the process attaching prevention */
#if defined(HAVE_SYS_PRCTL_H) && defined(__linux__)
/* Only use the linux prctl;  the IRIX prctl is totally different */
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <sys/ptrace.h>
#endif /* defined(HAVE_SYS_PRCTL_H) && defined(__linux__) || ... */

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h> /* FreeBSD needs this to know what version it is */
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#ifdef HAVE_SYS_SYSLIMITS_H
#include <sys/syslimits.h>
#endif
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif

#include "lib/log/torlog.h"
#include "common/util.h"
#include "lib/container/smartlist.h"
#include "lib/wallclock/tm_cvt.h"
#include "lib/net/address.h"
#include "lib/sandbox/sandbox.h"

/** Given <b>hlen</b> bytes at <b>haystack</b> and <b>nlen</b> bytes at
 * <b>needle</b>, return a pointer to the first occurrence of the needle
 * within the haystack, or NULL if there is no such occurrence.
 *
 * This function is <em>not</em> timing-safe.
 *
 * Requires that <b>nlen</b> be greater than zero.
 */
const void *
tor_memmem(const void *_haystack, size_t hlen,
           const void *_needle, size_t nlen)
{
#if defined(HAVE_MEMMEM) && (!defined(__GNUC__) || __GNUC__ >= 2)
  tor_assert(nlen);
  return memmem(_haystack, hlen, _needle, nlen);
#else
  /* This isn't as fast as the GLIBC implementation, but it doesn't need to
   * be. */
  const char *p, *last_possible_start;
  const char *haystack = (const char*)_haystack;
  const char *needle = (const char*)_needle;
  char first;
  tor_assert(nlen);

  if (nlen > hlen)
    return NULL;

  p = haystack;
  /* Last position at which the needle could start. */
  last_possible_start = haystack + hlen - nlen;
  first = *(const char*)needle;
  while ((p = memchr(p, first, last_possible_start + 1 - p))) {
    if (fast_memeq(p, needle, nlen))
      return p;
    if (++p > last_possible_start) {
      /* This comparison shouldn't be necessary, since if p was previously
       * equal to last_possible_start, the next memchr call would be
       * "memchr(p, first, 0)", which will return NULL. But it clarifies the
       * logic. */
      return NULL;
    }
  }
  return NULL;
#endif /* defined(HAVE_MEMMEM) && (!defined(__GNUC__) || __GNUC__ >= 2) */
}

/** Helper for tor_strtok_r_impl: Advances cp past all characters in
 * <b>sep</b>, and returns its new value. */
static char *
strtok_helper(char *cp, const char *sep)
{
  if (sep[1]) {
    while (*cp && strchr(sep, *cp))
      ++cp;
  } else {
    while (*cp && *cp == *sep)
      ++cp;
  }
  return cp;
}

/** Implementation of strtok_r for platforms whose coders haven't figured out
 * how to write one.  Hey, retrograde libc developers!  You can use this code
 * here for free! */
char *
tor_strtok_r_impl(char *str, const char *sep, char **lasts)
{
  char *cp, *start;
  tor_assert(*sep);
  if (str) {
    str = strtok_helper(str, sep);
    if (!*str)
      return NULL;
    start = cp = *lasts = str;
  } else if (!*lasts || !**lasts) {
    return NULL;
  } else {
    start = cp = *lasts;
  }

  if (sep[1]) {
    while (*cp && !strchr(sep, *cp))
      ++cp;
  } else {
    cp = strchr(cp, *sep);
  }

  if (!cp || !*cp) {
    *lasts = NULL;
  } else {
    *cp++ = '\0';
    *lasts = strtok_helper(cp, sep);
  }
  return start;
}

/** Represents a lockfile on which we hold the lock. */
struct tor_lockfile_t {
  /** Name of the file */
  char *filename;
  /** File descriptor used to hold the file open */
  int fd;
};

/** Try to get a lock on the lockfile <b>filename</b>, creating it as
 * necessary.  If someone else has the lock and <b>blocking</b> is true,
 * wait until the lock is available.  Otherwise return immediately whether
 * we succeeded or not.
 *
 * Set *<b>locked_out</b> to true if somebody else had the lock, and to false
 * otherwise.
 *
 * Return a <b>tor_lockfile_t</b> on success, NULL on failure.
 *
 * (Implementation note: because we need to fall back to fcntl on some
 *  platforms, these locks are per-process, not per-thread.  If you want
 *  to do in-process locking, use tor_mutex_t like a normal person.
 *  On Windows, when <b>blocking</b> is true, the maximum time that
 *  is actually waited is 10 seconds, after which NULL is returned
 *  and <b>locked_out</b> is set to 1.)
 */
tor_lockfile_t *
tor_lockfile_lock(const char *filename, int blocking, int *locked_out)
{
  tor_lockfile_t *result;
  int fd;
  *locked_out = 0;

  log_info(LD_FS, "Locking \"%s\"", filename);
  fd = tor_open_cloexec(filename, O_RDWR|O_CREAT|O_TRUNC, 0600);
  if (fd < 0) {
    log_warn(LD_FS,"Couldn't open \"%s\" for locking: %s", filename,
             strerror(errno));
    return NULL;
  }

#ifdef _WIN32
  _lseek(fd, 0, SEEK_SET);
  if (_locking(fd, blocking ? _LK_LOCK : _LK_NBLCK, 1) < 0) {
    if (errno != EACCES && errno != EDEADLOCK)
      log_warn(LD_FS,"Couldn't lock \"%s\": %s", filename, strerror(errno));
    else
      *locked_out = 1;
    close(fd);
    return NULL;
  }
#elif defined(HAVE_FLOCK)
  if (flock(fd, LOCK_EX|(blocking ? 0 : LOCK_NB)) < 0) {
    if (errno != EWOULDBLOCK)
      log_warn(LD_FS,"Couldn't lock \"%s\": %s", filename, strerror(errno));
    else
      *locked_out = 1;
    close(fd);
    return NULL;
  }
#else
  {
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, blocking ? F_SETLKW : F_SETLK, &lock) < 0) {
      if (errno != EACCES && errno != EAGAIN)
        log_warn(LD_FS, "Couldn't lock \"%s\": %s", filename, strerror(errno));
      else
        *locked_out = 1;
      close(fd);
      return NULL;
    }
  }
#endif /* defined(_WIN32) || ... */

  result = tor_malloc(sizeof(tor_lockfile_t));
  result->filename = tor_strdup(filename);
  result->fd = fd;
  return result;
}

/** Release the lock held as <b>lockfile</b>. */
void
tor_lockfile_unlock(tor_lockfile_t *lockfile)
{
  tor_assert(lockfile);

  log_info(LD_FS, "Unlocking \"%s\"", lockfile->filename);
#ifdef _WIN32
  _lseek(lockfile->fd, 0, SEEK_SET);
  if (_locking(lockfile->fd, _LK_UNLCK, 1) < 0) {
    log_warn(LD_FS,"Error unlocking \"%s\": %s", lockfile->filename,
             strerror(errno));
  }
#elif defined(HAVE_FLOCK)
  if (flock(lockfile->fd, LOCK_UN) < 0) {
    log_warn(LD_FS, "Error unlocking \"%s\": %s", lockfile->filename,
             strerror(errno));
  }
#else
  /* Closing the lockfile is sufficient. */
#endif /* defined(_WIN32) || ... */

  close(lockfile->fd);
  lockfile->fd = -1;
  tor_free(lockfile->filename);
  tor_free(lockfile);
}

/** Number of extra file descriptors to keep in reserve beyond those that we
 * tell Tor it's allowed to use. */
#define ULIMIT_BUFFER 32 /* keep 32 extra fd's beyond ConnLimit_ */

/** Learn the maximum allowed number of file descriptors, and tell the
 * system we want to use up to that number. (Some systems have a low soft
 * limit, and let us set it higher.)  We compute this by finding the largest
 * number that we can use.
 *
 * If the limit is below the reserved file descriptor value (ULIMIT_BUFFER),
 * return -1 and <b>max_out</b> is untouched.
 *
 * If we can't find a number greater than or equal to <b>limit</b>, then we
 * fail by returning -1 and <b>max_out</b> is untouched.
 *
 * If we are unable to set the limit value because of setrlimit() failing,
 * return 0 and <b>max_out</b> is set to the current maximum value returned
 * by getrlimit().
 *
 * Otherwise, return 0 and store the maximum we found inside <b>max_out</b>
 * and set <b>max_sockets</b> with that value as well.*/
int
set_max_file_descriptors(rlim_t limit, int *max_out)
{
  if (limit < ULIMIT_BUFFER) {
    log_warn(LD_CONFIG,
             "ConnLimit must be at least %d. Failing.", ULIMIT_BUFFER);
    return -1;
  }

  /* Define some maximum connections values for systems where we cannot
   * automatically determine a limit. Re Cygwin, see
   * http://archives.seul.org/or/talk/Aug-2006/msg00210.html
   * For an iPhone, 9999 should work. For Windows and all other unknown
   * systems we use 15000 as the default. */
#ifndef HAVE_GETRLIMIT
#if defined(CYGWIN) || defined(__CYGWIN__)
  const char *platform = "Cygwin";
  const unsigned long MAX_CONNECTIONS = 3200;
#elif defined(_WIN32)
  const char *platform = "Windows";
  const unsigned long MAX_CONNECTIONS = 15000;
#else
  const char *platform = "unknown platforms with no getrlimit()";
  const unsigned long MAX_CONNECTIONS = 15000;
#endif /* defined(CYGWIN) || defined(__CYGWIN__) || ... */
  log_fn(LOG_INFO, LD_NET,
         "This platform is missing getrlimit(). Proceeding.");
  if (limit > MAX_CONNECTIONS) {
    log_warn(LD_CONFIG,
             "We do not support more than %lu file descriptors "
             "on %s. Tried to raise to %lu.",
             (unsigned long)MAX_CONNECTIONS, platform, (unsigned long)limit);
    return -1;
  }
  limit = MAX_CONNECTIONS;
#else /* !(!defined(HAVE_GETRLIMIT)) */
  struct rlimit rlim;

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    log_warn(LD_NET, "Could not get maximum number of file descriptors: %s",
             strerror(errno));
    return -1;
  }
  if (rlim.rlim_max < limit) {
    log_warn(LD_CONFIG,"We need %lu file descriptors available, and we're "
             "limited to %lu. Please change your ulimit -n.",
             (unsigned long)limit, (unsigned long)rlim.rlim_max);
    return -1;
  }

  if (rlim.rlim_max > rlim.rlim_cur) {
    log_info(LD_NET,"Raising max file descriptors from %lu to %lu.",
             (unsigned long)rlim.rlim_cur, (unsigned long)rlim.rlim_max);
  }
  /* Set the current limit value so if the attempt to set the limit to the
   * max fails at least we'll have a valid value of maximum sockets. */
  *max_out = (int)rlim.rlim_cur - ULIMIT_BUFFER;
  set_max_sockets(*max_out);
  rlim.rlim_cur = rlim.rlim_max;

  if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    int couldnt_set = 1;
    const int setrlimit_errno = errno;
#ifdef OPEN_MAX
    uint64_t try_limit = OPEN_MAX - ULIMIT_BUFFER;
    if (errno == EINVAL && try_limit < (uint64_t) rlim.rlim_cur) {
      /* On some platforms, OPEN_MAX is the real limit, and getrlimit() is
       * full of nasty lies.  I'm looking at you, OSX 10.5.... */
      rlim.rlim_cur = MIN((rlim_t) try_limit, rlim.rlim_cur);
      if (setrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        if (rlim.rlim_cur < (rlim_t)limit) {
          log_warn(LD_CONFIG, "We are limited to %lu file descriptors by "
                   "OPEN_MAX (%lu), and ConnLimit is %lu.  Changing "
                   "ConnLimit; sorry.",
                   (unsigned long)try_limit, (unsigned long)OPEN_MAX,
                   (unsigned long)limit);
        } else {
          log_info(LD_CONFIG, "Dropped connection limit to %lu based on "
                   "OPEN_MAX (%lu); Apparently, %lu was too high and rlimit "
                   "lied to us.",
                   (unsigned long)try_limit, (unsigned long)OPEN_MAX,
                   (unsigned long)rlim.rlim_max);
        }
        couldnt_set = 0;
      }
    }
#endif /* defined(OPEN_MAX) */
    if (couldnt_set) {
      log_warn(LD_CONFIG,"Couldn't set maximum number of file descriptors: %s",
               strerror(setrlimit_errno));
    }
  }
  /* leave some overhead for logs, etc, */
  limit = rlim.rlim_cur;
#endif /* !defined(HAVE_GETRLIMIT) */

  if (limit > INT_MAX)
    limit = INT_MAX;
  tor_assert(max_out);
  *max_out = (int)limit - ULIMIT_BUFFER;
  set_max_sockets(*max_out);

  return 0;
}

#ifndef _WIN32
/** Log details of current user and group credentials. Return 0 on
 * success. Logs and return -1 on failure.
 */
static int
log_credential_status(void)
{
/** Log level to use when describing non-error UID/GID status. */
#define CREDENTIAL_LOG_LEVEL LOG_INFO
  /* Real, effective and saved UIDs */
  uid_t ruid, euid, suid;
  /* Read, effective and saved GIDs */
  gid_t rgid, egid, sgid;
  /* Supplementary groups */
  gid_t *sup_gids = NULL;
  int sup_gids_size;
  /* Number of supplementary groups */
  int ngids;

  /* log UIDs */
#ifdef HAVE_GETRESUID
  if (getresuid(&ruid, &euid, &suid) != 0 ) {
    log_warn(LD_GENERAL, "Error getting changed UIDs: %s", strerror(errno));
    return -1;
  } else {
    log_fn(CREDENTIAL_LOG_LEVEL, LD_GENERAL,
           "UID is %u (real), %u (effective), %u (saved)",
           (unsigned)ruid, (unsigned)euid, (unsigned)suid);
  }
#else /* !(defined(HAVE_GETRESUID)) */
  /* getresuid is not present on MacOS X, so we can't get the saved (E)UID */
  ruid = getuid();
  euid = geteuid();
  (void)suid;

  log_fn(CREDENTIAL_LOG_LEVEL, LD_GENERAL,
         "UID is %u (real), %u (effective), unknown (saved)",
         (unsigned)ruid, (unsigned)euid);
#endif /* defined(HAVE_GETRESUID) */

  /* log GIDs */
#ifdef HAVE_GETRESGID
  if (getresgid(&rgid, &egid, &sgid) != 0 ) {
    log_warn(LD_GENERAL, "Error getting changed GIDs: %s", strerror(errno));
    return -1;
  } else {
    log_fn(CREDENTIAL_LOG_LEVEL, LD_GENERAL,
           "GID is %u (real), %u (effective), %u (saved)",
           (unsigned)rgid, (unsigned)egid, (unsigned)sgid);
  }
#else /* !(defined(HAVE_GETRESGID)) */
  /* getresgid is not present on MacOS X, so we can't get the saved (E)GID */
  rgid = getgid();
  egid = getegid();
  (void)sgid;
  log_fn(CREDENTIAL_LOG_LEVEL, LD_GENERAL,
         "GID is %u (real), %u (effective), unknown (saved)",
         (unsigned)rgid, (unsigned)egid);
#endif /* defined(HAVE_GETRESGID) */

  /* log supplementary groups */
  sup_gids_size = 64;
  sup_gids = tor_calloc(64, sizeof(gid_t));
  while ((ngids = getgroups(sup_gids_size, sup_gids)) < 0 &&
         errno == EINVAL &&
         sup_gids_size < NGROUPS_MAX) {
    sup_gids_size *= 2;
    sup_gids = tor_reallocarray(sup_gids, sizeof(gid_t), sup_gids_size);
  }

  if (ngids < 0) {
    log_warn(LD_GENERAL, "Error getting supplementary GIDs: %s",
             strerror(errno));
    tor_free(sup_gids);
    return -1;
  } else {
    int i, retval = 0;
    char *s = NULL;
    smartlist_t *elts = smartlist_new();

    for (i = 0; i<ngids; i++) {
      smartlist_add_asprintf(elts, "%u", (unsigned)sup_gids[i]);
    }

    s = smartlist_join_strings(elts, " ", 0, NULL);

    log_fn(CREDENTIAL_LOG_LEVEL, LD_GENERAL, "Supplementary groups are: %s",s);

    tor_free(s);
    SMARTLIST_FOREACH(elts, char *, cp, tor_free(cp));
    smartlist_free(elts);
    tor_free(sup_gids);

    return retval;
  }

  return 0;
}
#endif /* !defined(_WIN32) */

/** Return true iff we were compiled with capability support, and capabilities
 * seem to work. **/
int
have_capability_support(void)
{
#ifdef HAVE_LINUX_CAPABILITIES
  cap_t caps = cap_get_proc();
  if (caps == NULL)
    return 0;
  cap_free(caps);
  return 1;
#else /* !(defined(HAVE_LINUX_CAPABILITIES)) */
  return 0;
#endif /* defined(HAVE_LINUX_CAPABILITIES) */
}

#ifdef HAVE_LINUX_CAPABILITIES
/** Helper. Drop all capabilities but a small set, and set PR_KEEPCAPS as
 * appropriate.
 *
 * If pre_setuid, retain only CAP_NET_BIND_SERVICE, CAP_SETUID, and
 * CAP_SETGID, and use PR_KEEPCAPS to ensure that capabilities persist across
 * setuid().
 *
 * If not pre_setuid, retain only CAP_NET_BIND_SERVICE, and disable
 * PR_KEEPCAPS.
 *
 * Return 0 on success, and -1 on failure.
 */
static int
drop_capabilities(int pre_setuid)
{
  /* We keep these three capabilities, and these only, as we setuid.
   * After we setuid, we drop all but the first. */
  const cap_value_t caplist[] = {
    CAP_NET_BIND_SERVICE, CAP_SETUID, CAP_SETGID
  };
  const char *where = pre_setuid ? "pre-setuid" : "post-setuid";
  const int n_effective = pre_setuid ? 3 : 1;
  const int n_permitted = pre_setuid ? 3 : 1;
  const int n_inheritable = 1;
  const int keepcaps = pre_setuid ? 1 : 0;

  /* Sets whether we keep capabilities across a setuid. */
  if (prctl(PR_SET_KEEPCAPS, keepcaps) < 0) {
    log_warn(LD_CONFIG, "Unable to call prctl() %s: %s",
             where, strerror(errno));
    return -1;
  }

  cap_t caps = cap_get_proc();
  if (!caps) {
    log_warn(LD_CONFIG, "Unable to call cap_get_proc() %s: %s",
             where, strerror(errno));
    return -1;
  }
  cap_clear(caps);

  cap_set_flag(caps, CAP_EFFECTIVE, n_effective, caplist, CAP_SET);
  cap_set_flag(caps, CAP_PERMITTED, n_permitted, caplist, CAP_SET);
  cap_set_flag(caps, CAP_INHERITABLE, n_inheritable, caplist, CAP_SET);

  int r = cap_set_proc(caps);
  cap_free(caps);
  if (r < 0) {
    log_warn(LD_CONFIG, "No permission to set capabilities %s: %s",
             where, strerror(errno));
    return -1;
  }

  return 0;
}
#endif /* defined(HAVE_LINUX_CAPABILITIES) */

/** Call setuid and setgid to run as <b>user</b> and switch to their
 * primary group.  Return 0 on success.  On failure, log and return -1.
 *
 * If SWITCH_ID_KEEP_BINDLOW is set in 'flags', try to use the capability
 * system to retain the abilitity to bind low ports.
 *
 * If SWITCH_ID_WARN_IF_NO_CAPS is set in flags, also warn if we have
 * don't have capability support.
 */
int
switch_id(const char *user, const unsigned flags)
{
#ifndef _WIN32
  const struct passwd *pw = NULL;
  uid_t old_uid;
  gid_t old_gid;
  static int have_already_switched_id = 0;
  const int keep_bindlow = !!(flags & SWITCH_ID_KEEP_BINDLOW);
  const int warn_if_no_caps = !!(flags & SWITCH_ID_WARN_IF_NO_CAPS);

  tor_assert(user);

  if (have_already_switched_id)
    return 0;

  /* Log the initial credential state */
  if (log_credential_status())
    return -1;

  log_fn(CREDENTIAL_LOG_LEVEL, LD_GENERAL, "Changing user and groups");

  /* Get old UID/GID to check if we changed correctly */
  old_uid = getuid();
  old_gid = getgid();

  /* Lookup the user and group information, if we have a problem, bail out. */
  pw = tor_getpwnam(user);
  if (pw == NULL) {
    log_warn(LD_CONFIG, "Error setting configured user: %s not found", user);
    return -1;
  }

#ifdef HAVE_LINUX_CAPABILITIES
  (void) warn_if_no_caps;
  if (keep_bindlow) {
    if (drop_capabilities(1))
      return -1;
  }
#else /* !(defined(HAVE_LINUX_CAPABILITIES)) */
  (void) keep_bindlow;
  if (warn_if_no_caps) {
    log_warn(LD_CONFIG, "KeepBindCapabilities set, but no capability support "
             "on this system.");
  }
#endif /* defined(HAVE_LINUX_CAPABILITIES) */

  /* Properly switch egid,gid,euid,uid here or bail out */
  if (setgroups(1, &pw->pw_gid)) {
    log_warn(LD_GENERAL, "Error setting groups to gid %d: \"%s\".",
             (int)pw->pw_gid, strerror(errno));
    if (old_uid == pw->pw_uid) {
      log_warn(LD_GENERAL, "Tor is already running as %s.  You do not need "
               "the \"User\" option if you are already running as the user "
               "you want to be.  (If you did not set the User option in your "
               "torrc, check whether it was specified on the command line "
               "by a startup script.)", user);
    } else {
      log_warn(LD_GENERAL, "If you set the \"User\" option, you must start Tor"
               " as root.");
    }
    return -1;
  }

  if (setegid(pw->pw_gid)) {
    log_warn(LD_GENERAL, "Error setting egid to %d: %s",
             (int)pw->pw_gid, strerror(errno));
    return -1;
  }

  if (setgid(pw->pw_gid)) {
    log_warn(LD_GENERAL, "Error setting gid to %d: %s",
             (int)pw->pw_gid, strerror(errno));
    return -1;
  }

  if (setuid(pw->pw_uid)) {
    log_warn(LD_GENERAL, "Error setting configured uid to %s (%d): %s",
             user, (int)pw->pw_uid, strerror(errno));
    return -1;
  }

  if (seteuid(pw->pw_uid)) {
    log_warn(LD_GENERAL, "Error setting configured euid to %s (%d): %s",
             user, (int)pw->pw_uid, strerror(errno));
    return -1;
  }

  /* This is how OpenBSD rolls:
  if (setgroups(1, &pw->pw_gid) || setegid(pw->pw_gid) ||
      setgid(pw->pw_gid) || setuid(pw->pw_uid) || seteuid(pw->pw_uid)) {
      setgid(pw->pw_gid) || seteuid(pw->pw_uid) || setuid(pw->pw_uid)) {
    log_warn(LD_GENERAL, "Error setting configured UID/GID: %s",
    strerror(errno));
    return -1;
  }
  */

  /* We've properly switched egid, gid, euid, uid, and supplementary groups if
   * we're here. */
#ifdef HAVE_LINUX_CAPABILITIES
  if (keep_bindlow) {
    if (drop_capabilities(0))
      return -1;
  }
#endif /* defined(HAVE_LINUX_CAPABILITIES) */

#if !defined(CYGWIN) && !defined(__CYGWIN__)
  /* If we tried to drop privilege to a group/user other than root, attempt to
   * restore root (E)(U|G)ID, and abort if the operation succeeds */

  /* Only check for privilege dropping if we were asked to be non-root */
  if (pw->pw_uid) {
    /* Try changing GID/EGID */
    if (pw->pw_gid != old_gid &&
        (setgid(old_gid) != -1 || setegid(old_gid) != -1)) {
      log_warn(LD_GENERAL, "Was able to restore group credentials even after "
               "switching GID: this means that the setgid code didn't work.");
      return -1;
    }

    /* Try changing UID/EUID */
    if (pw->pw_uid != old_uid &&
        (setuid(old_uid) != -1 || seteuid(old_uid) != -1)) {
      log_warn(LD_GENERAL, "Was able to restore user credentials even after "
               "switching UID: this means that the setuid code didn't work.");
      return -1;
    }
  }
#endif /* !defined(CYGWIN) && !defined(__CYGWIN__) */

  /* Check what really happened */
  if (log_credential_status()) {
    return -1;
  }

  have_already_switched_id = 1; /* mark success so we never try again */

#if defined(__linux__) && defined(HAVE_SYS_PRCTL_H) && \
  defined(HAVE_PRCTL) && defined(PR_SET_DUMPABLE)
  if (pw->pw_uid) {
    /* Re-enable core dumps if we're not running as root. */
    log_info(LD_CONFIG, "Re-enabling coredumps");
    if (prctl(PR_SET_DUMPABLE, 1)) {
      log_warn(LD_CONFIG, "Unable to re-enable coredumps: %s",strerror(errno));
    }
  }
#endif /* defined(__linux__) && defined(HAVE_SYS_PRCTL_H) && ... */
  return 0;

#else /* !(!defined(_WIN32)) */
  (void)user;
  (void)flags;

  log_warn(LD_CONFIG, "Switching users is unsupported on your OS.");
  return -1;
#endif /* !defined(_WIN32) */
}

/* We only use the linux prctl for now. There is no Win32 support; this may
 * also work on various BSD systems and Mac OS X - send testing feedback!
 *
 * On recent Gnu/Linux kernels it is possible to create a system-wide policy
 * that will prevent non-root processes from attaching to other processes
 * unless they are the parent process; thus gdb can attach to programs that
 * they execute but they cannot attach to other processes running as the same
 * user. The system wide policy may be set with the sysctl
 * kernel.yama.ptrace_scope or by inspecting
 * /proc/sys/kernel/yama/ptrace_scope and it is 1 by default on Ubuntu 11.04.
 *
 * This ptrace scope will be ignored on Gnu/Linux for users with
 * CAP_SYS_PTRACE and so it is very likely that root will still be able to
 * attach to the Tor process.
 */
/** Attempt to disable debugger attachment: return 1 on success, -1 on
 * failure, and 0 if we don't know how to try on this platform. */
int
tor_disable_debugger_attach(void)
{
  int r = -1;
  log_debug(LD_CONFIG,
            "Attemping to disable debugger attachment to Tor for "
            "unprivileged users.");
#if defined(__linux__) && defined(HAVE_SYS_PRCTL_H) \
  && defined(HAVE_PRCTL) && defined(PR_SET_DUMPABLE)
#define TRIED_TO_DISABLE
  r = prctl(PR_SET_DUMPABLE, 0);
#elif defined(__APPLE__) && defined(PT_DENY_ATTACH)
#define TRIED_TO_ATTACH
  r = ptrace(PT_DENY_ATTACH, 0, 0, 0);
#endif /* defined(__linux__) && defined(HAVE_SYS_PRCTL_H) ... || ... */

  // XXX: TODO - Mac OS X has dtrace and this may be disabled.
  // XXX: TODO - Windows probably has something similar
#ifdef TRIED_TO_DISABLE
  if (r == 0) {
    log_debug(LD_CONFIG,"Debugger attachment disabled for "
              "unprivileged users.");
    return 1;
  } else {
    log_warn(LD_CONFIG, "Unable to disable debugger attaching: %s",
             strerror(errno));
  }
#endif /* defined(TRIED_TO_DISABLE) */
#undef TRIED_TO_DISABLE
  return r;
}

#ifndef HAVE__NSGETENVIRON
#ifndef HAVE_EXTERN_ENVIRON_DECLARED
/* Some platforms declare environ under some circumstances, others don't. */
#ifndef RUNNING_DOXYGEN
extern char **environ;
#endif
#endif /* !defined(HAVE_EXTERN_ENVIRON_DECLARED) */
#endif /* !defined(HAVE__NSGETENVIRON) */

/** Return the current environment. This is a portable replacement for
 * 'environ'. */
char **
get_environment(void)
{
#ifdef HAVE__NSGETENVIRON
  /* This is for compatibility between OSX versions.  Otherwise (for example)
   * when we do a mostly-static build on OSX 10.7, the resulting binary won't
   * work on OSX 10.6. */
  return *_NSGetEnviron();
#else /* !(defined(HAVE__NSGETENVIRON)) */
  return environ;
#endif /* defined(HAVE__NSGETENVIRON) */
}

/** Get name of current host and write it to <b>name</b> array, whose
 * length is specified by <b>namelen</b> argument. Return 0 upon
 * successful completion; otherwise return return -1. (Currently,
 * this function is merely a mockable wrapper for POSIX gethostname().)
 */
MOCK_IMPL(int,
tor_gethostname,(char *name, size_t namelen))
{
   return gethostname(name,namelen);
}

/** Hold the result of our call to <b>uname</b>. */
static char uname_result[256];
/** True iff uname_result is set. */
static int uname_result_is_set = 0;

/** Return a pointer to a description of our platform.
 */
MOCK_IMPL(const char *,
get_uname,(void))
{
#ifdef HAVE_UNAME
  struct utsname u;
#endif
  if (!uname_result_is_set) {
#ifdef HAVE_UNAME
    if (uname(&u) != -1) {
      /* (Linux says 0 is success, Solaris says 1 is success) */
      strlcpy(uname_result, u.sysname, sizeof(uname_result));
    } else
#endif /* defined(HAVE_UNAME) */
      {
#ifdef _WIN32
        OSVERSIONINFOEX info;
        int i;
        const char *plat = NULL;
        static struct {
          unsigned major; unsigned minor; const char *version;
        } win_version_table[] = {
          { 6, 2, "Windows 8" },
          { 6, 1, "Windows 7" },
          { 6, 0, "Windows Vista" },
          { 5, 2, "Windows Server 2003" },
          { 5, 1, "Windows XP" },
          { 5, 0, "Windows 2000" },
          /* { 4, 0, "Windows NT 4.0" }, */
          { 4, 90, "Windows Me" },
          { 4, 10, "Windows 98" },
          /* { 4, 0, "Windows 95" } */
          { 3, 51, "Windows NT 3.51" },
          { 0, 0, NULL }
        };
        memset(&info, 0, sizeof(info));
        info.dwOSVersionInfoSize = sizeof(info);
        if (! GetVersionEx((LPOSVERSIONINFO)&info)) {
          strlcpy(uname_result, "Bizarre version of Windows where GetVersionEx"
                  " doesn't work.", sizeof(uname_result));
          uname_result_is_set = 1;
          return uname_result;
        }
        if (info.dwMajorVersion == 4 && info.dwMinorVersion == 0) {
          if (info.dwPlatformId == VER_PLATFORM_WIN32_NT)
            plat = "Windows NT 4.0";
          else
            plat = "Windows 95";
        } else {
          for (i=0; win_version_table[i].major>0; ++i) {
            if (win_version_table[i].major == info.dwMajorVersion &&
                win_version_table[i].minor == info.dwMinorVersion) {
              plat = win_version_table[i].version;
              break;
            }
          }
        }
        if (plat) {
          strlcpy(uname_result, plat, sizeof(uname_result));
        } else {
          if (info.dwMajorVersion > 6 ||
              (info.dwMajorVersion==6 && info.dwMinorVersion>2))
            tor_snprintf(uname_result, sizeof(uname_result),
                         "Very recent version of Windows [major=%d,minor=%d]",
                         (int)info.dwMajorVersion,(int)info.dwMinorVersion);
          else
            tor_snprintf(uname_result, sizeof(uname_result),
                         "Unrecognized version of Windows [major=%d,minor=%d]",
                         (int)info.dwMajorVersion,(int)info.dwMinorVersion);
        }
#ifdef VER_NT_SERVER
      if (info.wProductType == VER_NT_SERVER ||
          info.wProductType == VER_NT_DOMAIN_CONTROLLER) {
        strlcat(uname_result, " [server]", sizeof(uname_result));
      }
#endif /* defined(VER_NT_SERVER) */
#else /* !(defined(_WIN32)) */
        /* LCOV_EXCL_START -- can't provoke uname failure */
        strlcpy(uname_result, "Unknown platform", sizeof(uname_result));
        /* LCOV_EXCL_STOP */
#endif /* defined(_WIN32) */
      }
    uname_result_is_set = 1;
  }
  return uname_result;
}

/*
 *   Process control
 */

/** Implementation logic for compute_num_cpus(). */
static int
compute_num_cpus_impl(void)
{
#ifdef _WIN32
  SYSTEM_INFO info;
  memset(&info, 0, sizeof(info));
  GetSystemInfo(&info);
  if (info.dwNumberOfProcessors >= 1 && info.dwNumberOfProcessors < INT_MAX)
    return (int)info.dwNumberOfProcessors;
  else
    return -1;
#elif defined(HAVE_SYSCONF)
#ifdef _SC_NPROCESSORS_CONF
  long cpus_conf = sysconf(_SC_NPROCESSORS_CONF);
#else
  long cpus_conf = -1;
#endif
#ifdef _SC_NPROCESSORS_ONLN
  long cpus_onln = sysconf(_SC_NPROCESSORS_ONLN);
#else
  long cpus_onln = -1;
#endif
  long cpus = -1;

  if (cpus_conf > 0 && cpus_onln < 0) {
    cpus = cpus_conf;
  } else if (cpus_onln > 0 && cpus_conf < 0) {
    cpus = cpus_onln;
  } else if (cpus_onln > 0 && cpus_conf > 0) {
    if (cpus_onln < cpus_conf) {
      log_notice(LD_GENERAL, "I think we have %ld CPUS, but only %ld of them "
                 "are available. Telling Tor to only use %ld. You can over"
                 "ride this with the NumCPUs option",
                 cpus_conf, cpus_onln, cpus_onln);
    }
    cpus = cpus_onln;
  }

  if (cpus >= 1 && cpus < INT_MAX)
    return (int)cpus;
  else
    return -1;
#else
  return -1;
#endif /* defined(_WIN32) || ... */
}

#define MAX_DETECTABLE_CPUS 16

/** Return how many CPUs we are running with.  We assume that nobody is
 * using hot-swappable CPUs, so we don't recompute this after the first
 * time.  Return -1 if we don't know how to tell the number of CPUs on this
 * system.
 */
int
compute_num_cpus(void)
{
  static int num_cpus = -2;
  if (num_cpus == -2) {
    num_cpus = compute_num_cpus_impl();
    tor_assert(num_cpus != -2);
    if (num_cpus > MAX_DETECTABLE_CPUS) {
      /* LCOV_EXCL_START */
      log_notice(LD_GENERAL, "Wow!  I detected that you have %d CPUs. I "
                 "will not autodetect any more than %d, though.  If you "
                 "want to configure more, set NumCPUs in your torrc",
                 num_cpus, MAX_DETECTABLE_CPUS);
      num_cpus = MAX_DETECTABLE_CPUS;
      /* LCOV_EXCL_STOP */
    }
  }
  return num_cpus;
}

/** As localtime_r, but defined for platforms that don't have it:
 *
 * Convert *<b>timep</b> to a struct tm in local time, and store the value in
 * *<b>result</b>.  Return the result on success, or NULL on failure.
 */
struct tm *
tor_localtime_r(const time_t *timep, struct tm *result)
{
  char *err = NULL;
  struct tm *r = tor_localtime_r_msg(timep, result, &err);
  if (err) {
    log_warn(LD_BUG, "%s", err);
    tor_free(err);
  }
  return r;
}

/** As gmtime_r, but defined for platforms that don't have it:
 *
 * Convert *<b>timep</b> to a struct tm in UTC, and store the value in
 * *<b>result</b>.  Return the result on success, or NULL on failure.
 */
struct tm *
tor_gmtime_r(const time_t *timep, struct tm *result)
{
  char *err = NULL;
  struct tm *r = tor_gmtime_r_msg(timep, result, &err);
  if (err) {
    log_warn(LD_BUG, "%s", err);
    tor_free(err);
  }
  return r;
}

#if defined(HAVE_MLOCKALL) && HAVE_DECL_MLOCKALL && defined(RLIMIT_MEMLOCK)
#define HAVE_UNIX_MLOCKALL
#endif

#ifdef HAVE_UNIX_MLOCKALL
/** Attempt to raise the current and max rlimit to infinity for our process.
 * This only needs to be done once and can probably only be done when we have
 * not already dropped privileges.
 */
static int
tor_set_max_memlock(void)
{
  /* Future consideration for Windows is probably SetProcessWorkingSetSize
   * This is similar to setting the memory rlimit of RLIMIT_MEMLOCK
   * http://msdn.microsoft.com/en-us/library/ms686234(VS.85).aspx
   */

  struct rlimit limit;

  /* RLIM_INFINITY is -1 on some platforms. */
  limit.rlim_cur = RLIM_INFINITY;
  limit.rlim_max = RLIM_INFINITY;

  if (setrlimit(RLIMIT_MEMLOCK, &limit) == -1) {
    if (errno == EPERM) {
      log_warn(LD_GENERAL, "You appear to lack permissions to change memory "
                           "limits. Are you root?");
    }
    log_warn(LD_GENERAL, "Unable to raise RLIMIT_MEMLOCK: %s",
             strerror(errno));
    return -1;
  }

  return 0;
}
#endif /* defined(HAVE_UNIX_MLOCKALL) */

/** Attempt to lock all current and all future memory pages.
 * This should only be called once and while we're privileged.
 * Like mlockall() we return 0 when we're successful and -1 when we're not.
 * Unlike mlockall() we return 1 if we've already attempted to lock memory.
 */
int
tor_mlockall(void)
{
  static int memory_lock_attempted = 0;

  if (memory_lock_attempted) {
    return 1;
  }

  memory_lock_attempted = 1;

  /*
   * Future consideration for Windows may be VirtualLock
   * VirtualLock appears to implement mlock() but not mlockall()
   *
   * http://msdn.microsoft.com/en-us/library/aa366895(VS.85).aspx
   */

#ifdef HAVE_UNIX_MLOCKALL
  if (tor_set_max_memlock() == 0) {
    log_debug(LD_GENERAL, "RLIMIT_MEMLOCK is now set to RLIM_INFINITY.");
  }

  if (mlockall(MCL_CURRENT|MCL_FUTURE) == 0) {
    log_info(LD_GENERAL, "Insecure OS paging is effectively disabled.");
    return 0;
  } else {
    if (errno == ENOSYS) {
      /* Apple - it's 2009! I'm looking at you. Grrr. */
      log_notice(LD_GENERAL, "It appears that mlockall() is not available on "
                             "your platform.");
    } else if (errno == EPERM) {
      log_notice(LD_GENERAL, "It appears that you lack the permissions to "
                             "lock memory. Are you root?");
    }
    log_notice(LD_GENERAL, "Unable to lock all current and future memory "
                           "pages: %s", strerror(errno));
    return -1;
  }
#else /* !(defined(HAVE_UNIX_MLOCKALL)) */
  log_warn(LD_GENERAL, "Unable to lock memory pages. mlockall() unsupported?");
  return -1;
#endif /* defined(HAVE_UNIX_MLOCKALL) */
}

/**
 * On Windows, WSAEWOULDBLOCK is not always correct: when you see it,
 * you need to ask the socket for its actual errno.  Also, you need to
 * get your errors from WSAGetLastError, not errno.  (If you supply a
 * socket of -1, we check WSAGetLastError, but don't correct
 * WSAEWOULDBLOCKs.)
 *
 * The upshot of all of this is that when a socket call fails, you
 * should call tor_socket_errno <em>at most once</em> on the failing
 * socket to get the error.
 */
#if defined(_WIN32)
int
tor_socket_errno(tor_socket_t sock)
{
  int optval, optvallen=sizeof(optval);
  int err = WSAGetLastError();
  if (err == WSAEWOULDBLOCK && SOCKET_OK(sock)) {
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (void*)&optval, &optvallen))
      return err;
    if (optval)
      return optval;
  }
  return err;
}
#endif /* defined(_WIN32) */

#if defined(_WIN32)
#define E(code, s) { code, (s " [" #code " ]") }
struct { int code; const char *msg; } windows_socket_errors[] = {
  E(WSAEINTR, "Interrupted function call"),
  E(WSAEACCES, "Permission denied"),
  E(WSAEFAULT, "Bad address"),
  E(WSAEINVAL, "Invalid argument"),
  E(WSAEMFILE, "Too many open files"),
  E(WSAEWOULDBLOCK,  "Resource temporarily unavailable"),
  E(WSAEINPROGRESS, "Operation now in progress"),
  E(WSAEALREADY, "Operation already in progress"),
  E(WSAENOTSOCK, "Socket operation on nonsocket"),
  E(WSAEDESTADDRREQ, "Destination address required"),
  E(WSAEMSGSIZE, "Message too long"),
  E(WSAEPROTOTYPE, "Protocol wrong for socket"),
  E(WSAENOPROTOOPT, "Bad protocol option"),
  E(WSAEPROTONOSUPPORT, "Protocol not supported"),
  E(WSAESOCKTNOSUPPORT, "Socket type not supported"),
  /* What's the difference between NOTSUPP and NOSUPPORT? :) */
  E(WSAEOPNOTSUPP, "Operation not supported"),
  E(WSAEPFNOSUPPORT,  "Protocol family not supported"),
  E(WSAEAFNOSUPPORT, "Address family not supported by protocol family"),
  E(WSAEADDRINUSE, "Address already in use"),
  E(WSAEADDRNOTAVAIL, "Cannot assign requested address"),
  E(WSAENETDOWN, "Network is down"),
  E(WSAENETUNREACH, "Network is unreachable"),
  E(WSAENETRESET, "Network dropped connection on reset"),
  E(WSAECONNABORTED, "Software caused connection abort"),
  E(WSAECONNRESET, "Connection reset by peer"),
  E(WSAENOBUFS, "No buffer space available"),
  E(WSAEISCONN, "Socket is already connected"),
  E(WSAENOTCONN, "Socket is not connected"),
  E(WSAESHUTDOWN, "Cannot send after socket shutdown"),
  E(WSAETIMEDOUT, "Connection timed out"),
  E(WSAECONNREFUSED, "Connection refused"),
  E(WSAEHOSTDOWN, "Host is down"),
  E(WSAEHOSTUNREACH, "No route to host"),
  E(WSAEPROCLIM, "Too many processes"),
  /* Yes, some of these start with WSA, not WSAE. No, I don't know why. */
  E(WSASYSNOTREADY, "Network subsystem is unavailable"),
  E(WSAVERNOTSUPPORTED, "Winsock.dll out of range"),
  E(WSANOTINITIALISED, "Successful WSAStartup not yet performed"),
  E(WSAEDISCON, "Graceful shutdown now in progress"),
#ifdef WSATYPE_NOT_FOUND
  E(WSATYPE_NOT_FOUND, "Class type not found"),
#endif
  E(WSAHOST_NOT_FOUND, "Host not found"),
  E(WSATRY_AGAIN, "Nonauthoritative host not found"),
  E(WSANO_RECOVERY, "This is a nonrecoverable error"),
  E(WSANO_DATA, "Valid name, no data record of requested type)"),

  /* There are some more error codes whose numeric values are marked
   * <b>OS dependent</b>. They start with WSA_, apparently for the same
   * reason that practitioners of some craft traditions deliberately
   * introduce imperfections into their baskets and rugs "to allow the
   * evil spirits to escape."  If we catch them, then our binaries
   * might not report consistent results across versions of Windows.
   * Thus, I'm going to let them all fall through.
   */
  { -1, NULL },
};
/** There does not seem to be a strerror equivalent for Winsock errors.
 * Naturally, we have to roll our own.
 */
const char *
tor_socket_strerror(int e)
{
  int i;
  for (i=0; windows_socket_errors[i].code >= 0; ++i) {
    if (e == windows_socket_errors[i].code)
      return windows_socket_errors[i].msg;
  }
  return strerror(e);
}
#endif /* defined(_WIN32) */

/** Called before we make any calls to network-related functions.
 * (Some operating systems require their network libraries to be
 * initialized.) */
int
network_init(void)
{
#ifdef _WIN32
  /* This silly exercise is necessary before windows will allow
   * gethostbyname to work. */
  WSADATA WSAData;
  int r;
  r = WSAStartup(0x101,&WSAData);
  if (r) {
    log_warn(LD_NET,"Error initializing windows network layer: code was %d",r);
    return -1;
  }
  if (sizeof(SOCKET) != sizeof(tor_socket_t)) {
    log_warn(LD_BUG,"The tor_socket_t type does not match SOCKET in size; Tor "
             "might not work. (Sizes are %d and %d respectively.)",
             (int)sizeof(tor_socket_t), (int)sizeof(SOCKET));
  }
  /* WSAData.iMaxSockets might show the max sockets we're allowed to use.
   * We might use it to complain if we're trying to be a server but have
   * too few sockets available. */
#endif /* defined(_WIN32) */
  return 0;
}

#if defined(HW_PHYSMEM64)
/* This appears to be an OpenBSD thing */
#define INT64_HW_MEM HW_PHYSMEM64
#elif defined(HW_MEMSIZE)
/* OSX defines this one */
#define INT64_HW_MEM HW_MEMSIZE
#endif /* defined(HW_PHYSMEM64) || ... */

/**
 * Helper: try to detect the total system memory, and return it. On failure,
 * return 0.
 */
static uint64_t
get_total_system_memory_impl(void)
{
#if defined(__linux__)
  /* On linux, sysctl is deprecated. Because proc is so awesome that you
   * shouldn't _want_ to write portable code, I guess? */
  unsigned long long result=0;
  int fd = -1;
  char *s = NULL;
  const char *cp;
  size_t file_size=0;
  if (-1 == (fd = tor_open_cloexec("/proc/meminfo",O_RDONLY,0)))
    return 0;
  s = read_file_to_str_until_eof(fd, 65536, &file_size);
  if (!s)
    goto err;
  cp = strstr(s, "MemTotal:");
  if (!cp)
    goto err;
  /* Use the system sscanf so that space will match a wider number of space */
  if (sscanf(cp, "MemTotal: %llu kB\n", &result) != 1)
    goto err;

  close(fd);
  tor_free(s);
  return result * 1024;

  /* LCOV_EXCL_START Can't reach this unless proc is broken. */
 err:
  tor_free(s);
  close(fd);
  return 0;
  /* LCOV_EXCL_STOP */
#elif defined (_WIN32)
  /* Windows has MEMORYSTATUSEX; pretty straightforward. */
  MEMORYSTATUSEX ms;
  memset(&ms, 0, sizeof(ms));
  ms.dwLength = sizeof(ms);
  if (! GlobalMemoryStatusEx(&ms))
    return 0;

  return ms.ullTotalPhys;

#elif defined(HAVE_SYSCTL) && defined(INT64_HW_MEM)
  /* On many systems, HW_PYHSMEM is clipped to 32 bits; let's use a better
   * variant if we know about it. */
  uint64_t memsize = 0;
  size_t len = sizeof(memsize);
  int mib[2] = {CTL_HW, INT64_HW_MEM};
  if (sysctl(mib,2,&memsize,&len,NULL,0))
    return 0;

  return memsize;

#elif defined(HAVE_SYSCTL) && defined(HW_PHYSMEM)
  /* On some systems (like FreeBSD I hope) you can use a size_t with
   * HW_PHYSMEM. */
  size_t memsize=0;
  size_t len = sizeof(memsize);
  int mib[2] = {CTL_HW, HW_USERMEM};
  if (sysctl(mib,2,&memsize,&len,NULL,0))
    return 0;

  return memsize;

#else
  /* I have no clue. */
  return 0;
#endif /* defined(__linux__) || ... */
}

/**
 * Try to find out how much physical memory the system has. On success,
 * return 0 and set *<b>mem_out</b> to that value. On failure, return -1.
 */
MOCK_IMPL(int,
get_total_system_memory, (size_t *mem_out))
{
  static size_t mem_cached=0;
  uint64_t m = get_total_system_memory_impl();
  if (0 == m) {
    /* LCOV_EXCL_START -- can't make this happen without mocking. */
    /* We couldn't find our memory total */
    if (0 == mem_cached) {
      /* We have no cached value either */
      *mem_out = 0;
      return -1;
    }

    *mem_out = mem_cached;
    return 0;
    /* LCOV_EXCL_STOP */
  }

#if SIZE_MAX != UINT64_MAX
  if (m > SIZE_MAX) {
    /* I think this could happen if we're a 32-bit Tor running on a 64-bit
     * system: we could have more system memory than would fit in a
     * size_t. */
    m = SIZE_MAX;
  }
#endif /* SIZE_MAX != UINT64_MAX */

  *mem_out = mem_cached = (size_t) m;

  return 0;
}

/** Emit the password prompt <b>prompt</b>, then read up to <b>buflen</b>
 * bytes of passphrase into <b>output</b>. Return the number of bytes in
 * the passphrase, excluding terminating NUL.
 */
ssize_t
tor_getpass(const char *prompt, char *output, size_t buflen)
{
  tor_assert(buflen <= SSIZE_MAX);
  tor_assert(buflen >= 1);
#if defined(HAVE_READPASSPHRASE)
  char *pwd = readpassphrase(prompt, output, buflen, RPP_ECHO_OFF);
  if (pwd == NULL)
    return -1;
  return strlen(pwd);
#elif defined(_WIN32)
  int r = -1;
  while (*prompt) {
    _putch(*prompt++);
  }

  tor_assert(buflen <= INT_MAX);
  wchar_t *buf = tor_calloc(buflen, sizeof(wchar_t));

  wchar_t *ptr = buf, *lastch = buf + buflen - 1;
  while (ptr < lastch) {
    wint_t ch = _getwch();
    switch (ch) {
      case '\r':
      case '\n':
      case WEOF:
        goto done_reading;
      case 3:
        goto done; /* Can't actually read ctrl-c this way. */
      case '\b':
        if (ptr > buf)
          --ptr;
        continue;
      case 0:
      case 0xe0:
        ch = _getwch(); /* Ignore; this is a function or arrow key */
        break;
      default:
        *ptr++ = ch;
        break;
    }
  }
 done_reading:
  ;

#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x80
#endif

  /* Now convert it to UTF-8 */
  r = WideCharToMultiByte(CP_UTF8,
                          WC_NO_BEST_FIT_CHARS|WC_ERR_INVALID_CHARS,
                          buf, (int)(ptr-buf),
                          output, (int)(buflen-1),
                          NULL, NULL);
  if (r <= 0) {
    r = -1;
    goto done;
  }

  tor_assert(r < (int)buflen);

  output[r] = 0;

 done:
  SecureZeroMemory(buf, sizeof(wchar_t)*buflen);
  tor_free(buf);
  return r;
#else
#error "No implementation for tor_getpass found!"
#endif /* defined(HAVE_READPASSPHRASE) || ... */
}

/** Return the amount of free disk space we have permission to use, in
 * bytes. Return -1 if the amount of free space can't be determined. */
int64_t
tor_get_avail_disk_space(const char *path)
{
#ifdef HAVE_STATVFS
  struct statvfs st;
  int r;
  memset(&st, 0, sizeof(st));

  r = statvfs(path, &st);
  if (r < 0)
    return -1;

  int64_t result = st.f_bavail;
  if (st.f_frsize) {
    result *= st.f_frsize;
  } else if (st.f_bsize) {
    result *= st.f_bsize;
  } else {
    return -1;
  }

  return result;
#elif defined(_WIN32)
  ULARGE_INTEGER freeBytesAvail;
  BOOL ok;

  ok = GetDiskFreeSpaceEx(path, &freeBytesAvail, NULL, NULL);
  if (!ok) {
    return -1;
  }
  return (int64_t)freeBytesAvail.QuadPart;
#else
  (void)path;
  errno = ENOSYS;
  return -1;
#endif /* defined(HAVE_STATVFS) || ... */
}
