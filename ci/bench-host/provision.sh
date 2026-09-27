#!/usr/bin/env bash
# Turns a fresh Ubuntu 24.04 bare-metal machine into the benchmark runner.
# Idempotent: re-run it after editing anything in this directory.
#
#   sudo BENCH_DEVICE=/dev/nvme1n1 RUNNER_TOKEN=... ./provision.sh
#
# BENCH_DEVICE      block device (or partition) for /bench, the measured volume.
#                   Only needed until /bench is mounted. A device that already
#                   holds a filesystem is refused unless BENCH_FORMAT=yes.
# RUNNER_TOKEN      registration token from the repository's Settings > Actions >
#                   Runners > New self-hosted runner. Only needed until the
#                   runner is configured.
# HOUSEKEEPING_CPUS CPUs left to the OS (default: cpu0 and its SMT siblings).
#                   Every other CPU belongs to the runner and the benchmarks.
# REPO_URL          default https://github.com/markovejnovic/remmy
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_URL="${REPO_URL:-https://github.com/markovejnovic/remmy}"
RUNNER_USER=gh-runner
RUNNER_DIR=/opt/actions-runner
RUNNER_LABELS=remmy-bench
# Keep in sync with GCC_IMAGE in .github/workflows/release.yml and bench.yml.
GCC_IMAGE=gcc:16.2.0-trixie@sha256:ef558a40d1f13115293feee01526dbdb9aaad7c9c5a00da05f471ce042e855c1

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'provision: %s\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root"
grep -q 'VERSION_ID="24.04"' /etc/os-release || die "written for Ubuntu 24.04"

# Expands a cpulist ("0-3,8") to one CPU per line.
expand_cpus() {
	local part
	tr ',' '\n' <<<"$1" | while read -r part; do
		[[ -z $part ]] && continue
		if [[ $part == *-* ]]; then seq "${part%-*}" "${part#*-}"; else echo "$part"; fi
	done
}

log "packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# build-essential: tests/bench compiles mktree.cpp. podman: rootless builds in
# the release toolchain image, so the runner never needs root or a docker group.
apt-get install -y -qq --no-install-recommends \
	build-essential bfs ca-certificates curl e2fsprogs fuse-overlayfs git hyperfine jq \
	podman python3 slirp4netns tar uidmap util-linux

log "runner user"
id "$RUNNER_USER" &>/dev/null || useradd --create-home --shell /bin/bash "$RUNNER_USER"
grep -q "^$RUNNER_USER:" /etc/subuid || usermod --add-subuids 100000-165535 "$RUNNER_USER"
grep -q "^$RUNNER_USER:" /etc/subgid || usermod --add-subgids 100000-165535 "$RUNNER_USER"
RUNNER_HOME="$(getent passwd "$RUNNER_USER" | cut -d: -f6)"
install -d -o "$RUNNER_USER" -g "$RUNNER_USER" "$RUNNER_HOME/.config/containers"
# The runner is a system service, not a login session: no systemd user manager.
cat >"$RUNNER_HOME/.config/containers/containers.conf" <<'EOF'
[engine]
cgroup_manager = "cgroupfs"
events_logger = "file"
EOF
chown "$RUNNER_USER:" "$RUNNER_HOME/.config/containers/containers.conf"

log "root helpers"
install -m 0755 -o root -g root "$HERE/bench-ctl" /usr/local/sbin/bench-ctl
install -m 0755 -o root -g root "$HERE/bench-tune" /usr/local/sbin/bench-tune
install -m 0440 -o root -g root "$HERE/sudoers" /etc/sudoers.d/bench
visudo -cf /etc/sudoers.d/bench >/dev/null || die "sudoers.d/bench does not parse"

log "measured volume /bench"
if ! mountpoint -q /bench; then
	[[ -n ${BENCH_DEVICE:-} ]] || die "set BENCH_DEVICE: /bench is not mounted"
	[[ -b $BENCH_DEVICE ]] || die "$BENCH_DEVICE is not a block device"
	if [[ -n "$(blkid -o value -s TYPE "$BENCH_DEVICE" || true)" ]]; then
		[[ ${BENCH_FORMAT:-} == yes ]] || die "$BENCH_DEVICE already holds a filesystem; BENCH_FORMAT=yes erases it"
	fi
	mkfs.ext4 -q -F -L bench "$BENCH_DEVICE"
	uuid="$(blkid -o value -s UUID "$BENCH_DEVICE")"
	install -d /bench
	# noatime: reads during a delete must not become writes. No `discard`:
	# bench-ctl fstrim trims before each suite instead of during timed runs.
	echo "UUID=$uuid /bench ext4 noatime,nofail 0 2" >>/etc/fstab
	systemctl daemon-reload
	mount /bench
fi
chown "$RUNNER_USER:" /bench

log "quiet machine"
# Nothing may wake up mid-run. Patch by hand: sudo apt-get upgrade && sudo reboot.
for unit in apt-daily.timer apt-daily-upgrade.timer man-db.timer fstrim.timer motd-news.timer \
	e2scrub_all.timer unattended-upgrades.service; do
	systemctl disable --now "$unit" 2>/dev/null || true
	systemctl mask "$unit" 2>/dev/null || true
done
if command -v snap >/dev/null; then snap refresh --hold >/dev/null || true; fi
sed -i -E 's|^([^#].*[[:space:]]swap[[:space:]].*)$|# \1|' /etc/fstab
install -m 0644 "$HERE/bench-tune.service" /etc/systemd/system/bench-tune.service
systemctl daemon-reload
systemctl enable bench-tune.service
systemctl restart bench-tune.service

log "CPU split"
all_cpus="$(cat /sys/devices/system/cpu/online)"
HOUSEKEEPING_CPUS="${HOUSEKEEPING_CPUS:-$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list)}"
bench_cpus="$(comm -23 <(expand_cpus "$all_cpus" | sort) <(expand_cpus "$HOUSEKEEPING_CPUS" | sort) |
	sort -n | paste -sd, -)"
[[ -n $bench_cpus ]] || die "no CPUs left for benchmarks after HOUSEKEEPING_CPUS=$HOUSEKEEPING_CPUS"
echo "housekeeping: $HOUSEKEEPING_CPUS  benchmarks: $bench_cpus"
for unit in system.slice user.slice init.scope; do
	install -d "/etc/systemd/system/$unit.d"
	printf '[%s]\nAllowedCPUs=%s\n' "$([[ $unit == *.scope ]] && echo Scope || echo Slice)" "$HOUSEKEEPING_CPUS" \
		>"/etc/systemd/system/$unit.d/50-housekeeping.conf"
done
printf '[Unit]\nDescription=Benchmark runner and everything it starts\n\n[Slice]\nAllowedCPUs=%s\n' "$bench_cpus" \
	>/etc/systemd/system/bench.slice

log "GitHub Actions runner"
if [[ ! -f $RUNNER_DIR/.runner ]]; then
	[[ -n ${RUNNER_TOKEN:-} ]] || die "set RUNNER_TOKEN: the runner is not configured"
	case "$(uname -m)" in x86_64) arch=x64 ;; aarch64) arch=arm64 ;; *) die "unsupported $(uname -m)" ;; esac
	version="$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest | jq -r .tag_name)"
	version="${version#v}"
	install -d -o "$RUNNER_USER" -g "$RUNNER_USER" "$RUNNER_DIR"
	curl -fsSL "https://github.com/actions/runner/releases/download/v$version/actions-runner-linux-$arch-$version.tar.gz" |
		sudo -u "$RUNNER_USER" tar -xz -C "$RUNNER_DIR"
	"$RUNNER_DIR/bin/installdependencies.sh" >/dev/null
	sudo -u "$RUNNER_USER" "$RUNNER_DIR/config.sh" --unattended --replace \
		--url "$REPO_URL" --token "$RUNNER_TOKEN" --name "$(hostname)-bench" \
		--labels "$RUNNER_LABELS" --work "$RUNNER_HOME/work"
fi
cat >/etc/systemd/system/gh-runner.service <<EOF
[Unit]
Description=GitHub Actions runner (remmy benchmarks)
After=network-online.target bench-tune.service
Wants=network-online.target

[Service]
User=$RUNNER_USER
WorkingDirectory=$RUNNER_DIR
ExecStart=$RUNNER_DIR/run.sh
Slice=bench.slice
KillMode=process
KillSignal=SIGTERM
TimeoutStopSec=5min
Restart=always

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable gh-runner.service
systemctl restart gh-runner.service

log "toolchain image"
(cd /tmp && sudo -u "$RUNNER_USER" podman pull -q "$GCC_IMAGE" >/dev/null)

log "done"
echo "Reboot once so the CPU split applies to every process: sudo reboot"
