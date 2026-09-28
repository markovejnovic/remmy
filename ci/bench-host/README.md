# Benchmark host

The bare-metal Linux machine that runs `tests/bench` in CI, and how its results
are tracked.

```
push / PR / nightly
  └─ Bench (.github/workflows/bench.yml)
       plan   (GitHub runner)  demo or full? skip?
       bench  (this machine)   build the static release binary in the release
                               toolchain image, time it, run.json -> bmf.json
                               -> artifact "bench-results"
  └─ Bench track (.github/workflows/bench-track.yml, GitHub runner)
       artifact -> Bencher: history, PR comparison against main, PR comment,
       fails on a remmy time-ratio regression
```

| Suite | When | Takes |
| --- | --- | --- |
| `demo` | every push to `main`, every same-repo PR push, a fork PR when a maintainer adds the `bench` label | minutes |
| `full` | nightly at 03:17 UTC if `main` moved since the last nightly; manual dispatch | hours |

Only remmy's **time-ratio** (its median over GNU `rm`'s, measured in the same
run) raises alerts: a slower disk or a new kernel slows every tool alike, and
the ratio cancels that. Every tool's latency and throughput are tracked too; if
`rm`'s latency jumps, the machine changed, not remmy.

## Setting up

1. **Machine.** Ubuntu 24.04 bare metal that does nothing else. A spare,
   empty NVMe drive for `/bench` (the measured volume) gives the steadiest
   timings; without one, `/bench` is a directory on the root filesystem.
2. **Bencher key** (optional now, needed for tracking): sign in to
   [bencher.dev](https://bencher.dev) with GitHub and create a user API key.
   The project is created on the first upload.
3. **Run**, from a checkout, with `gh` logged in as a repository admin:
   ```bash
   BENCHER_API_KEY=<key> scripts/mkbench.sh root@<ip>
   ```
   That's all. Connect as root or a user with passwordless sudo. The script:
   - copies this directory to the server and runs `provision.sh`, which picks
     `/bench` (a blank disk, meaning no filesystem, partitions, RAID or LVM, so
     formatting it loses nothing; else a directory on the root filesystem;
     `--device DEV` or `--no-device` to choose), creates the unprivileged
     `gh-runner` user, installs the runner, and tunes the machine:
     `performance` governor, turbo off, swap off, update timers masked,
     housekeeping on cpu0 (and its SMT sibling) with every other CPU in the
     runner's `bench.slice` (`--housekeeping 0-1` widens that;
     `BENCH_SMT=off` in `/etc/default/bench-tune` turns SMT off at boot);
   - registers the runner if GitHub doesn't know it, reboots if a change
     needs it (never while a job runs), and verifies the server;
   - creates the `bench` label, requires approval for every outside
     contributor's workflow runs, stores `BENCHER_API_KEY` as a secret, and
     once `bench.yml` is on `main`, dispatches a first run if there has never
     been one.

   Run it again any time (after merging, after editing this directory, after
   a reboot): every step changes only what differs, and a runner is never
   restarted mid-job. `scripts/mkbench.sh --help` lists the options.

## Security

The bench job runs a PR's code on this machine, so:

- fork PRs run only when a maintainer adds `bench`, and each push after that
  needs the label removed and added again, after reading the new code;
- the runner is an unprivileged user whose only root access is
  `bench-ctl drop-caches` and `bench-ctl fstrim` (`sudoers`); builds use
  rootless podman, never a docker group;
- the bench workflow has a read-only token and no secrets; the Bencher key
  lives in `bench-track.yml`, which runs from `main` on GitHub's runners and
  trusts nothing in the artifact beyond numbers and, for non-PR runs, the
  suite name.

## Maintenance

- **Updates** are off so nothing runs mid-benchmark. Patch in a quiet window:
  `sudo apt-get update && sudo apt-get upgrade && sudo reboot`.
- **After changing anything that moves timings** (kernel, hardware, BIOS,
  tuning in this directory, moving `/bench` to a disk), set `BENCHER_TESTBED` to a new name so old and
  new numbers never share a series.
- A run refuses to time on an unfit machine (`tests/bench/environment.py`: the
  governor isn't `performance`, swap in use, benchmark CPUs busy). Check
  `systemctl status bench-tune` and what else is running.
