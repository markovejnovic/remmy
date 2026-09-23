// See LICENSE in the repository root.

// mktree: build a synthetic directory tree fast (parallel over top-level
// subtrees). Files are empty (0 bytes) unless --size given.
//   usage: mktree ROOT [--depth D] [--fanout F] [--files M] [--size BYTES]
//                 [--threads T]
// Tree: ROOT has F subdirs; each subdir recursively has F subdirs down to
// depth D; every directory (incl. ROOT) has M files. Prints "dirs,files,secs".
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int depth = 3, fanout = 8, files = 100, threads = 0;
static long fsize = 0;
static std::atomic<long> ndirs{0}, nfiles{0};
static std::vector<char> filebuf;

static void die(const char* m) {
  perror(m);
  _exit(1);
}

static void fill(int dfd) {
  char name[32];
  for (int i = 0; i < files; ++i) {
    snprintf(name, sizeof name, "f%06d", i);
    int fd = openat(dfd, name, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (fd < 0) die("openat(file)");
    if (fsize > 0 && write(fd, filebuf.data(), (size_t)fsize) != fsize)
      die("write");
    close(fd);
  }
  nfiles += files;
}

static void build(int dfd, int d) {
  fill(dfd);
  if (d >= depth) return;
  char name[32];
  for (int i = 0; i < fanout; ++i) {
    snprintf(name, sizeof name, "d%03d", i);
    if (mkdirat(dfd, name, 0755) != 0) die("mkdirat");
    int c = openat(dfd, name, O_RDONLY | O_DIRECTORY);
    if (c < 0) die("openat(dir)");
    ++ndirs;
    build(c, d + 1);
    close(c);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: mktree ROOT [--depth D] [--fanout F] [--files M] [--size "
            "B] [--threads T]\n");
    return 2;
  }
  const char* root = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--depth"))
      depth = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--fanout"))
      fanout = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--files"))
      files = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--size"))
      fsize = atol(argv[i + 1]);
    else if (!strcmp(argv[i], "--threads"))
      threads = atoi(argv[i + 1]);
    else {
      fprintf(stderr, "bad arg %s\n", argv[i]);
      return 2;
    }
  }
  if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
  if (fsize > 0) filebuf.assign((size_t)fsize, 'x');
  auto t0 = std::chrono::steady_clock::now();
  if (mkdir(root, 0755) != 0) die("mkdir(root)");
  int rfd = open(root, O_RDONLY | O_DIRECTORY);
  if (rfd < 0) die("open(root)");
  ++ndirs;
  fill(rfd);
  // Pre-create top-level dirs, then hand each to a thread round-robin.
  std::vector<int> tops;
  char name[32];
  for (int i = 0; i < fanout && depth >= 1; ++i) {
    snprintf(name, sizeof name, "d%03d", i);
    if (mkdirat(rfd, name, 0755) != 0) die("mkdirat(top)");
    int c = openat(rfd, name, O_RDONLY | O_DIRECTORY);
    if (c < 0) die("openat(top)");
    ++ndirs;
    tops.push_back(c);
  }
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t)
    pool.emplace_back(
        [t](const std::vector<int>& tv) {
          for (size_t i = (size_t)t; i < tv.size(); i += (size_t)threads) {
            build(tv[i], 1);
            close(tv[i]);
          }
        },
        std::cref(tops));
  for (auto& th : pool) th.join();
  close(rfd);
  double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  printf("%ld,%ld,%.3f\n", ndirs.load(), nfiles.load(), s);
  return 0;
}
