// See LICENSE in the repository root.
//
// Syscall fault injection for remmy's end-to-end tests (macOS).
//
// Loaded with DYLD_INSERT_LIBRARIES; replaces libc entry points through dyld's
// __interpose section. Calls made from inside this image are not interposed,
// so the replacements reach the real functions by calling them by name.
//
// Configuration (read once, on first intercepted call):
//
//   REMMY_FAULTS      Semicolon-separated rules: FN:ERRNO:SELECTOR
//                       FN        open openat unlink unlinkat rmdir lstat
//                                 fstatat getdirentries
//                       ERRNO     positive integer
//                       SELECTOR  nth=N   fail only the Nth call to FN
//                       (1-based)
//                                 from=N  fail the Nth call to FN and all later
//                                 name=S  fail every call whose final path
//                                         component is exactly S
//                                 all     fail every call
//   REMMY_FAULT_LOG   File that gets one NUL-terminated record per injected
//                     failure (a path may hold any byte but NUL):
//                       FN <TAB> ERRNO <TAB> absolute path <NUL>
//   REMMY_FAULT_DT_UNKNOWN
//                     If "1", rewrite every d_type returned by getdirentries
//                     to DT_UNKNOWN, as some filesystems do.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Private libSystem entry points remmy links against.
extern int __unlinkat(int, const char*, int);
extern ssize_t __getdirentries64(int, char*, size_t, off_t*);

enum Fn {
  kOpen,
  kOpenat,
  kUnlink,
  kUnlinkat,
  kRmdir,
  kLstat,
  kFstatat,
  kGetdirentries,
  kFnCount,
};

static const char* const kFnNames[kFnCount] = {
    "open",  "openat", "unlink",  "unlinkat",
    "rmdir", "lstat",  "fstatat", "getdirentries",
};

enum Selector { kNth, kFrom, kName, kAll };

struct Rule {
  enum Fn fn;
  int err;
  enum Selector selector;
  long n;
  char name[NAME_MAX + 1];
};

enum { kMaxRules = 16 };

static struct Rule rules[kMaxRules];
static int rule_count;
static atomic_long calls[kFnCount];
static int log_fd = -1;
static bool dt_unknown;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void Die(const char* what) {
  // Misconfiguration is a test bug: fail loudly instead of silently injecting
  // nothing.
  (void)write(STDERR_FILENO, "faultinject: bad REMMY_FAULTS: ", 31);
  (void)write(STDERR_FILENO, what, strlen(what));
  (void)write(STDERR_FILENO, "\n", 1);
  _exit(125);
}

static void ParseRule(char* spec) {
  if (rule_count == kMaxRules) Die("too many rules");
  struct Rule* r = &rules[rule_count++];

  char* fn = strsep(&spec, ":");
  char* err = strsep(&spec, ":");
  char* sel = spec;
  if (fn == NULL || err == NULL || sel == NULL)
    Die("expected FN:ERRNO:SELECTOR");

  r->fn = kFnCount;
  for (int i = 0; i < kFnCount; ++i) {
    if (strcmp(fn, kFnNames[i]) == 0) r->fn = (enum Fn)i;
  }
  if (r->fn == kFnCount) Die(fn);

  r->err = atoi(err);
  if (r->err <= 0) Die(err);

  if (strcmp(sel, "all") == 0) {
    r->selector = kAll;
  } else if (strncmp(sel, "nth=", 4) == 0) {
    r->selector = kNth;
    r->n = atol(sel + 4);
  } else if (strncmp(sel, "from=", 5) == 0) {
    r->selector = kFrom;
    r->n = atol(sel + 5);
  } else if (strncmp(sel, "name=", 5) == 0) {
    r->selector = kName;
    if (strlen(sel + 5) > NAME_MAX) Die("name too long");
    strcpy(r->name, sel + 5);
  } else {
    Die(sel);
  }
  if ((r->selector == kNth || r->selector == kFrom) && r->n <= 0) Die(sel);
}

static void Init(void) {
  const char* spec = getenv("REMMY_FAULTS");
  if (spec != NULL && *spec != '\0') {
    char* copy = strdup(spec);
    if (copy == NULL) Die("out of memory");
    char* cursor = copy;
    for (char* item; (item = strsep(&cursor, ";")) != NULL;) {
      if (*item != '\0') ParseRule(item);
    }
    free(copy);
  }

  const char* log = getenv("REMMY_FAULT_LOG");
  if (log != NULL && *log != '\0') {
    log_fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  }

  const char* dt = getenv("REMMY_FAULT_DT_UNKNOWN");
  dt_unknown = dt != NULL && strcmp(dt, "1") == 0;
}

static const char* Basename(const char* path) {
  if (path == NULL) return "";
  const char* slash = strrchr(path, '/');
  // "dir/" names "dir": skip trailing slashes.
  if (slash != NULL && slash[1] == '\0') {
    const char* end = slash;
    while (end > path && end[-1] == '/') --end;
    const char* start = end;
    while (start > path && start[-1] != '/') --start;
    static _Thread_local char buf[NAME_MAX + 1];
    size_t len = (size_t)(end - start);
    if (len > NAME_MAX) len = NAME_MAX;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
  }
  return slash == NULL ? path : slash + 1;
}

// Absolute path of `name` relative to `dirfd`, for the log. Best effort.
static void Resolve(int dirfd, const char* name, char out[PATH_MAX]) {
  out[0] = '\0';
  if (name != NULL && name[0] == '/') {
    strlcpy(out, name, PATH_MAX);
    return;
  }
  char base[PATH_MAX];
  if (dirfd == AT_FDCWD) {
    if (getcwd(base, sizeof base) == NULL) return;
  } else if (fcntl(dirfd, F_GETPATH, base) == -1) {
    return;
  }
  strlcpy(out, base, PATH_MAX);
  if (name != NULL && name[0] != '\0') {
    strlcat(out, "/", PATH_MAX);
    strlcat(out, name, PATH_MAX);
  }
}

// Returns the errno to inject for this call, or 0 to let it through.
static int Decide(enum Fn fn, int dirfd, const char* name) {
  pthread_once(&once, Init);
  const long nth =
      atomic_fetch_add_explicit(&calls[fn], 1, memory_order_relaxed) + 1;

  for (int i = 0; i < rule_count; ++i) {
    const struct Rule* r = &rules[i];
    if (r->fn != fn) continue;
    bool hit = false;
    switch (r->selector) {
      case kAll:
        hit = true;
        break;
      case kNth:
        hit = nth == r->n;
        break;
      case kFrom:
        hit = nth >= r->n;
        break;
      case kName:
        hit = strcmp(Basename(name), r->name) == 0;
        break;
    }
    if (!hit) continue;

    if (log_fd != -1) {
      char path[PATH_MAX];
      Resolve(dirfd, name, path);
      char line[PATH_MAX + 64];
      int len = snprintf(line, sizeof line, "%s\t%d\t%s", kFnNames[fn],
                         r->err, path);
      // Write through the terminating NUL, which ends the record.
      if (len > 0)
        (void)write(log_fd, line,
                    (size_t)len < sizeof line ? (size_t)len + 1 : sizeof line);
    }
    return r->err;
  }
  return 0;
}

#define FAIL_WITH(err) \
  do {                 \
    errno = (err);     \
    return -1;         \
  } while (0)

static int FiOpen(const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  const int err = Decide(kOpen, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return open(path, flags, mode);
}

static int FiOpenat(int dirfd, const char* path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  const int err = Decide(kOpenat, dirfd, path);
  if (err) FAIL_WITH(err);
  return openat(dirfd, path, flags, mode);
}

static int FiUnlink(const char* path) {
  const int err = Decide(kUnlink, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return unlink(path);
}

static int FiUnlinkat(int dirfd, const char* path, int flags) {
  const int err = Decide(kUnlinkat, dirfd, path);
  if (err) FAIL_WITH(err);
  return unlinkat(dirfd, path, flags);
}

static int FiPrivateUnlinkat(int dirfd, const char* path, int flags) {
  const int err = Decide(kUnlinkat, dirfd, path);
  if (err) FAIL_WITH(err);
  return __unlinkat(dirfd, path, flags);
}

static int FiRmdir(const char* path) {
  const int err = Decide(kRmdir, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return rmdir(path);
}

static int FiLstat(const char* path, struct stat* st) {
  const int err = Decide(kLstat, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return lstat(path, st);
}

static int FiFstatat(int dirfd, const char* path, struct stat* st, int flags) {
  const int err = Decide(kFstatat, dirfd, path);
  if (err) FAIL_WITH(err);
  return fstatat(dirfd, path, st, flags);
}

static ssize_t FiGetdirentries(int fd, char* buf, size_t nbytes, off_t* basep) {
  const int err = Decide(kGetdirentries, fd, NULL);
  if (err) FAIL_WITH(err);
  const ssize_t n = __getdirentries64(fd, buf, nbytes, basep);
  if (n > 0 && dt_unknown) {
    // Byte-wise: records are not guaranteed to be aligned for struct dirent.
    for (ssize_t off = 0; off < n;) {
      uint16_t reclen;
      memcpy(&reclen, buf + off + offsetof(struct dirent, d_reclen),
             sizeof reclen);
      if (reclen == 0) break;
      buf[off + (ssize_t)offsetof(struct dirent, d_type)] = DT_UNKNOWN;
      off += reclen;
    }
  }
  return n;
}

#define INTERPOSE(replacement, original)                                    \
  __attribute__((used)) static const struct {                               \
    const void* replacement_fn;                                             \
    const void* original_fn;                                                \
  } interpose_##original __attribute__((section("__DATA,__interpose"))) = { \
      (const void*)(uintptr_t)&replacement, (const void*)(uintptr_t)&original}

INTERPOSE(FiOpen, open);
INTERPOSE(FiOpenat, openat);
INTERPOSE(FiUnlink, unlink);
INTERPOSE(FiUnlinkat, unlinkat);
INTERPOSE(FiPrivateUnlinkat, __unlinkat);
INTERPOSE(FiRmdir, rmdir);
INTERPOSE(FiLstat, lstat);
INTERPOSE(FiFstatat, fstatat);
INTERPOSE(FiGetdirentries, __getdirentries64);
