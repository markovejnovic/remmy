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
| `demo` | every push to `main`, every push to one of your own same-repo PRs | minutes |
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
   - requires approval for every outside contributor's workflow runs,
     stores `BENCHER_API_KEY` as a secret, and
     once `bench.yml` is on `main`, dispatches a first run if there has never
     been one.

   Run it again any time (after merging, after editing this directory, after
   a reboot): every step changes only what differs, and a runner is never
   restarted mid-job. `scripts/mkbench.sh --help` lists the options.

## Security

**Only code you reviewed runs on this machine.** Never a fork's PR, and
never anyone else's, even if you approve its workflow run.

- **The gate.** `bench-job-gate` is the runner's job-started hook. It runs
  before every job's first step, and refuses (failing the job before checkout)
  anything that isn't this repository, on `push`, `schedule` or
  `workflow_dispatch`, started and re-run by a trusted user, or a
  `pull_request` from a branch of this repository by a trusted author. Trusted
  means the repository's owner unless `mkbench.sh --trust USER` says
  otherwise. A PR can't get around it: it brings its own `bench.yml`, but
  the hook, its config (`/etc/remmy-bench/gate.conf`) and the runner's `.env`
  are root-owned, and the event data comes from GitHub. `bench.yml`'s plan
  job applies the same rule so that other runs skip instead of failing.
- **What the gate can't stop.** Code you merged that turns out to be hostile,
  such as a compromised dependency, runs as `gh-runner`, which owns the runner
  and could tamper with it. So everything the machine runs is pinned: Python
  packages by hash (`uv.lock`), uv by version, the toolchain image by
  digest, the runner by version and SHA-256, and actions by commit.
- **What the machine can reach.** Its jobs get a read-only token and no
  secrets. The Bencher key lives in `bench-track.yml`, which runs from
  `main` on GitHub's runners and reads only numbers from the artifact, plus
  the suite name for non-PR runs. The machine writes nothing to GitHub's
  Actions cache, and releases restore no cache, so nothing from it reaches a
  release.
- **The machine itself.** `gh-runner` is unprivileged. Its only root access
  is `bench-ctl drop-caches` and `bench-ctl fstrim` (`sudoers`). Builds use
  rootless podman, never a docker group. SSH is key-only. Protect the Vultr
  account with 2FA: it is the machine's root of trust.

## Maintenance

- **Updates** are off so nothing runs mid-benchmark. Patch in a quiet window:
  `sudo apt-get update && sudo apt-get upgrade && sudo reboot`.
- **After changing anything that moves timings** (kernel, hardware, BIOS,
  tuning in this directory, moving `/bench` to a disk), set `BENCHER_TESTBED` to a new name so old and
  new numbers never share a series.
- A run refuses to time on an unfit machine (`tests/bench/environment.py`: the
  governor isn't `performance`, swap in use, benchmark CPUs busy). Check
  `systemctl status bench-tune` and what else is running.
