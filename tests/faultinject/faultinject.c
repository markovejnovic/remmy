// See LICENSE in the repository root.
//
// Syscall fault injection for remmy's end-to-end tests (macOS and Linux).
//
// macOS: loaded with DYLD_INSERT_LIBRARIES; replaces libc entry points through
// dyld's __interpose section. Calls made from inside this image are not
// interposed, so the replacements reach the real functions by calling them by
// name.
//
// Linux: loaded with LD_PRELOAD; exports the libc names itself and reaches the
// real functions through dlsym(RTLD_NEXT). Calls from inside this image would
// bind to its own exports, so they go through REAL() as well.
//
// Configuration (read once, on first intercepted call):
//
//   REMMY_FAULTS      Semicolon-separated rules: FN:ERRNO:SELECTOR
//                       FN        open openat unlink unlinkat rmdir lstat
//                                 fstatat getdirentries (getdents64 on Linux)
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

#if defined(__linux__)
#define _GNU_SOURCE
#include <dlfcn.h>
#endif

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

#if defined(__APPLE__)
// Private libSystem entry points remmy links against.
extern int __unlinkat(int, const char*, int);
extern ssize_t __getdirentries64(int, char*, size_t, off_t*);

typedef struct dirent RawDirent;
#define REAL(fn) fn
#elif defined(__linux__)
typedef struct dirent64 RawDirent;

static int (*real_open)(const char*, int, ...);
static int (*real_openat)(int, const char*, int, ...);
static int (*real_unlink)(const char*);
static int (*real_unlinkat)(int, const char*, int);
static int (*real_rmdir)(const char*);
static int (*real_lstat)(const char*, struct stat*);
static int (*real_fstatat)(int, const char*, struct stat*, int);
static ssize_t (*real_getdents64)(int, void*, size_t);

static pthread_once_t real_once = PTHREAD_ONCE_INIT;

// Resolved on first use: another image's constructor may call in before ours
// would run.
static void ResolveReal(void) {
#define RESOLVE(fn) real_##fn = (__typeof__(real_##fn))dlsym(RTLD_NEXT, #fn)
  RESOLVE(open);
  RESOLVE(openat);
  RESOLVE(unlink);
  RESOLVE(unlinkat);
  RESOLVE(rmdir);
  RESOLVE(lstat);
  RESOLVE(fstatat);
  RESOLVE(getdents64);
#undef RESOLVE
}

#define REAL(fn) (pthread_once(&real_once, ResolveReal), real_##fn)
#else
#error "faultinject.c: requires Darwin or Linux"
#endif

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
    log_fd = REAL(open)(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
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
    snprintf(out, PATH_MAX, "%s", name);
    return;
  }
  char base[PATH_MAX];
  if (dirfd == AT_FDCWD) {
    if (getcwd(base, sizeof base) == NULL) return;
  } else {
#if defined(__APPLE__)
    if (fcntl(dirfd, F_GETPATH, base) == -1) return;
#else
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
    const ssize_t len = readlink(link, base, sizeof base - 1);
    if (len == -1) return;
    base[len] = '\0';
#endif
  }
  if (name != NULL && name[0] != '\0') {
    // Truncation is fine for a log line.
    if (snprintf(out, PATH_MAX, "%s/%s", base, name) < 0) out[0] = '\0';
  } else {
    snprintf(out, PATH_MAX, "%s", base);
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
      int len =
          snprintf(line, sizeof line, "%s\t%d\t%s", kFnNames[fn], r->err, path);
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
  return REAL(open)(path, flags, mode);
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
  return REAL(openat)(dirfd, path, flags, mode);
}

static int FiUnlink(const char* path) {
  const int err = Decide(kUnlink, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return REAL(unlink)(path);
}

static int FiUnlinkat(int dirfd, const char* path, int flags) {
  const int err = Decide(kUnlinkat, dirfd, path);
  if (err) FAIL_WITH(err);
  return REAL(unlinkat)(dirfd, path, flags);
}

#if defined(__APPLE__)
static int FiPrivateUnlinkat(int dirfd, const char* path, int flags) {
  const int err = Decide(kUnlinkat, dirfd, path);
  if (err) FAIL_WITH(err);
  return __unlinkat(dirfd, path, flags);
}
#endif

static int FiRmdir(const char* path) {
  const int err = Decide(kRmdir, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return REAL(rmdir)(path);
}

static int FiLstat(const char* path, struct stat* st) {
  const int err = Decide(kLstat, AT_FDCWD, path);
  if (err) FAIL_WITH(err);
  return REAL(lstat)(path, st);
}

static int FiFstatat(int dirfd, const char* path, struct stat* st, int flags) {
  const int err = Decide(kFstatat, dirfd, path);
  if (err) FAIL_WITH(err);
  return REAL(fstatat)(dirfd, path, st, flags);
}

static ssize_t FiGetdirentries(int fd, char* buf, size_t nbytes, off_t* basep) {
  const int err = Decide(kGetdirentries, fd, NULL);
  if (err) FAIL_WITH(err);
#if defined(__APPLE__)
  const ssize_t n = __getdirentries64(fd, buf, nbytes, basep);
#else
  (void)basep;
  const ssize_t n = REAL(getdents64)(fd, buf, nbytes);
#endif
  if (n > 0 && dt_unknown) {
    // Byte-wise: records are not guaranteed to be aligned for struct dirent.
    for (ssize_t off = 0; off < n;) {
      uint16_t reclen;
      memcpy(&reclen, buf + off + offsetof(RawDirent, d_reclen), sizeof reclen);
      if (reclen == 0) break;
      buf[off + (ssize_t)offsetof(RawDirent, d_type)] = DT_UNKNOWN;
      off += reclen;
    }
  }
  return n;
}

#if defined(__APPLE__)
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
#else
// Exported under the libc names so the dynamic linker binds remmy to these.
// A mode is only read for O_CREAT, like libc's own open.
int open(const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
  va_end(ap);
  return FiOpen(path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
  va_end(ap);
  return FiOpenat(dirfd, path, flags, mode);
}

int unlink(const char* path) { return FiUnlink(path); }

int unlinkat(int dirfd, const char* path, int flags) {
  return FiUnlinkat(dirfd, path, flags);
}

int rmdir(const char* path) { return FiRmdir(path); }

int lstat(const char* path, struct stat* st) { return FiLstat(path, st); }

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
  return FiFstatat(dirfd, path, st, flags);
}

ssize_t getdents64(int fd, void* buf, size_t nbytes) {
  return FiGetdirentries(fd, buf, nbytes, NULL);
}
#endif
