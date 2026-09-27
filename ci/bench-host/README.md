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

1. **Machine.** Ubuntu 24.04 bare metal, ideally with a second NVMe drive for
   `/bench` (the measured volume); otherwise leave a spare partition when
   installing. Don't use it for anything else.
2. **Runner token.** Repository Settings → Actions → Runners → New
   self-hosted runner; copy the token from the `config.sh` line (valid 1 h).
3. **Provision** (as root, from a checkout of this repository):
   ```bash
   sudo BENCH_DEVICE=/dev/nvme1n1 RUNNER_TOKEN=<token> ci/bench-host/provision.sh
   sudo reboot
   ```
   It formats `BENCH_DEVICE` as ext4 on `/bench` (refusing one that holds a
   filesystem unless `BENCH_FORMAT=yes`), creates the unprivileged `gh-runner`
   user, installs the runner as `gh-runner.service`, and tunes the machine:
   `performance` governor, turbo off, swap off, update timers masked,
   housekeeping on cpu0 (and its SMT sibling) with every other CPU reserved
   for the runner's `bench.slice`. `HOUSEKEEPING_CPUS=0-1` widens that;
   `BENCH_SMT=off` in `/etc/default/bench-tune` turns SMT off at boot.
4. **Bencher.** Sign in to [bencher.dev](https://bencher.dev) with GitHub,
   create a public project, and create a project API key. In the repository:
   - secret `BENCHER_API_KEY`: the key
   - variable `BENCHER_PROJECT`: the project slug, if it isn't `remmy`
   - variable `BENCHER_TESTBED`: optional, default `vultr-bare-metal`
5. **GitHub.** Settings → Actions → General → *Require approval for all
   external contributors*. Create the label: `gh label create bench`.
6. **Seed the baseline.** Actions → Bench → Run workflow, once with `demo`
   and once with `full`. A PR's comparison needs a few `main` runs before the
   t-test has a history to judge against.

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
  tuning in this directory), set `BENCHER_TESTBED` to a new name so old and
  new numbers never share a series.
- A run refuses to time on an unfit machine (`tests/bench/environment.py`: the
  governor isn't `performance`, swap in use, benchmark CPUs busy). Check
  `systemctl status bench-tune` and what else is running.
