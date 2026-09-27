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
  <!-- TODO(markovejnovic): Better benchmarks -->
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/res/time-dark.svg">
    <img src=".github/res/time.svg" alt="Median time to delete a 58,500-file tree: remmy with 4 threads 0.41 s, find | xargs rm with 4 jobs 0.73 s, bfs, GNU rm and find about 1.2 s, /bin/rm 1.27 s" width="720">
  </picture>
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/res/scaling-dark.svg">
    <img src=".github/res/scaling.svg" alt="Files deleted per second: remmy rises from 49k at 1 thread to 142k at 4 threads and falls to 106k at 8; xargs peaks at 80k at 4 jobs; rm is 46k" width="640">
  </picture>
</p>

## Quickstart

You can grab a `remmy` version from the [Releases](TODO) page:

```bash
curl TODO
```

`remmy` has the exact-same interface and semantics as your macOS `rm` and you
can run:

```bash
remmy -r node_modules/
```

If you trust `remmy` enough, you can add an alias in your `.bashrc` to use
`remmy` instead of `rm`:

```bash
echo "alias rm='/usr/local/bin/remmy'" >> ~/.bashrc
source ~/.bashrc
```

## Compatibility

My goal is to ensure that `remmy` is one-for-one compatible with `rm`. I've
worked at Bun for a brief period and I really know how annoying it is if tools
aren't compatible with the standard. `remmy` prioritizes compatibility over
performance.

**If your `rm` and `remmy` output are not the same, yell at me.**

## Performance

I've spent a lot of time optimizing `remmy`. Here's how it compares to other
options for removing directories:

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/res/speedup-by-tree-dark.svg">
    <img src=".github/res/speedup-by-tree.svg" alt="Speedup over /bin/rm by tree shape at 4 threads: remmy 2.5 to 3.1 times on trees of 51k to 333k files, 1.5 times on 1 MiB files, and 0.48 times on a 125-file tree" width="720">
  </picture>
</p>

| Tool                          | Time    | Files/s | vs. `rm` |
| ----------------------------- | ------: | ------: | -------: |
| **remmy** (4 threads)         | 0.41 s  |   142k  | **3.1×** |
| `find \| xargs -P4 rm`        | 0.73 s  |    80k  |    1.7×  |
| `bfs -delete`                 | 1.17 s  |    50k  |    1.09× |
| remmy (1 thread)              | 1.19 s  |    49k  |    1.07× |
| GNU `rm -rf`                  | 1.19 s  |    49k  |    1.07× |
| `find -delete`                | 1.22 s  |    48k  |    1.04× |
| `/bin/rm -rf`                 | 1.27 s  |    46k  |    1.0×  |
| `find \| xargs rm`            | 1.68 s  |    35k  |    0.76× |

Here's what remmy does (or doesn't do) to speed stuff up.

1. **Avoids calling `stat`** It reads raw directory entries with
   [`getdirentries64`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/getdirentries.2.html).
2. **Walks in parallel.** Each directory is a task on a [Chase–Lev
   work-stealing scheduler](https://inria.hal.science/hal-00802885/document).
3. **Deletes relative to open directories.** Every unlink is an `unlinkat`
   against a descriptor it already holds, so the kernel never looks up a
   full path.
4. **Avoids allocations**. Almost every spot where a naive application would
   allocate, remmy tries really hard not to. Even CLI parsing skips
   allocations.

### Detailed Benchmarks

TODO

## Contributing

Thank you for wanting to contribute! Here are some quick notes:

### AI Disclosure

**I have used AI in this project.** `remmy`, for the most-part, is
hand-written. Areas where code has not been hand-written and/or have had a
cursory, rudimentary review, are marked as such. These are generally low-risk
areas. **None of the tests are hand-written**.

**Note that I expect all PRs to use no more AI than I have.**

### Some Contributing Rules

- Be nice and respectful in the issues/PRs. If you are mean to people, I will
  immediately ban you from the project.
- **Note the license is not open-source. It is very close to GPLv3, but it is
  not GPLv3.** For the most part, this is due to an exclusion for the Omarchy
  project to use `remmy`.
- You are very welcome to open issues and/or PRs. I will do my **absolute
  best** to help you push your PRs across the finish line.
- I will not accept AI PRs. _I get to use AI because I maintain the project and
  I understand what my agents are doing. I do not understand what your agents
  are doing._ You are welcome to use AI as much as you want, but AI-generated
  PRs, overly-documented slop PRs will be immediately closed. I expect you to
  understand your code as if you wrote it by hand. I will label delinquent PRs
  as such and you are **more than welcome to ping me if you think I
  misclassified your PR**. You are welcome to use aggressive AI for tests, and
  I will perform only a cursory review on those as the risk is minor.

## License

Source-available, not open source. See [`LICENSE`](LICENSE).
