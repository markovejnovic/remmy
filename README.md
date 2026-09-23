<div align="center">

# remmy

**`rm -rf`, 3× faster on macOS.**

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange?style=flat-square)](#results)
[![CI](https://img.shields.io/github/actions/workflow/status/markovejnovic/remmy/ci.yml?branch=main&style=flat-square&logo=githubactions&logoColor=white&label=CI)](https://github.com/markovejnovic/remmy/actions/workflows/ci.yml)
![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![GCC 16](https://img.shields.io/badge/GCC-16-A42E2B?style=flat-square&logo=gnu&logoColor=white)
[![License](https://img.shields.io/badge/license-source--available-lightgrey?style=flat-square)](LICENSE)

</div>

<p align="center">
  <img src=".github/res/time.png" alt="Median time to delete a 58,500-file tree: remmy with 4 threads 0.44 s, /bin/rm 1.3 s, find | xargs rm with 4 jobs 5.6 s" width="720">
</p>

<p align="center">
  <img src=".github/res/scaling.png" alt="Files deleted per second: remmy rises from 49k at 1 thread to 133k at 4 threads; xargs stays near 10k; rm is 44k" width="640">
</p>

Deleting a 58,500-file tree takes `/bin/rm` **1.3 s**. remmy does it in
**0.44 s**. It is a drop-in for `rm -r`.

```sh
remmy -r node_modules target build
```

If you have an `rm` that beats it on macOS, please open an issue. I want to
see it.

## Results

| Tool                          | Time    | Files/s | vs. `rm` |
| ----------------------------- | ------: | ------: | -------: |
| **remmy** (4 threads)         | 0.44 s  |   133k  | **3.0×** |
| remmy (1 thread)              | 1.2 s   |    49k  |    1.1×  |
| `/bin/rm -rf`                 | 1.3 s   |    44k  |    1.0×  |
| `find \| xargs -P4 rm`        | 5.6 s   |    10k  |    0.24× |
| `find \| xargs rm`            | 8.9 s   |   6.6k  |    0.15× |

Even on one thread remmy is faster than `rm`. With four it is three times
faster. Remmy achieves this by:

1. **Never stats.** It reads raw directory entries with
   [`getdirentries64`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getdirentries.2.html).
2. **Deletes relative to open directories.** Every unlink is an `unlinkat`
   against a descriptor it already holds, so the kernel never looks up a
   full path.
3. **Walks in parallel.** Each directory is a task on a [Chase–Lev
   work-stealing scheduler](https://inria.hal.science/hal-00802885/document).
4. **Avoids allocations**. Almost every spot where a naive application would
   allocate, remmy tries really hard not to. Even CLI parsing skips
   allocations.

Remmy is aggresively tested to ensure compatibility with `rm`. **Note that
remmy is currently not completely compatible with `rm` and is not considered
production-ready.**

## Install

remmy is written in C++26 and needs GCC 16.

```sh
brew install gcc ninja cmake
cmake --preset release
cmake --build --preset release
cp build/gcc-release/remmy /usr/local/bin/
```

## Usage

```sh
remmy file1 file2           # remove files
remmy -r dir1 dir2          # remove directories recursively
REMMY_THREADS=8 remmy -r d  # override the worker count (default: 4)
```

**Note that overriding the thread-count is likely to give you worse results
than the default.**

## License

Source-available, not open source. See [`LICENSE`](LICENSE).
