#!/usr/bin/env bash
# Turns an Ubuntu 24.04 bare-metal machine into the benchmark runner. Usually
# run through scripts/mkbench.sh from a workstation; see README.md.
#
#   sudo RUNNER_TOKEN_FILE=token ./provision.sh
#
# Idempotent: every step checks the machine and changes only what differs, so
# re-running is always safe. A re-run never restarts a runner that is working,
# and never formats a disk that holds data (unless BENCH_FORMAT=yes).
#
# BENCH_DEVICE      block device or partition for /bench, the measured volume.
#                   Default: a blank disk (no filesystem, partitions, RAID or
#                   LVM: formatting it loses nothing) if there is one, else a
#                   directory on the root filesystem. `none` forces the latter.
#                   Only read until /bench is set up.
# BENCH_FORMAT=yes  allow formatting a BENCH_DEVICE that holds another filesystem.
# RUNNER_TOKEN_FILE file holding a runner registration token (Settings > Actions
#                   > Runners > New self-hosted runner). Only needed until the
#                   runner is configured. RUNNER_TOKEN works too.
# RUNNER_RESET=yes  drop the local runner configuration and register again, for
#                   a runner that was deleted on GitHub. Needs a token.
# HOUSEKEEPING_CPUS CPUs left to the OS (default: cpu0 and its SMT siblings).
#                   Every other CPU belongs to the runner and the benchmarks.
# REPO_URL          default https://github.com/markovejnovic/remmy
#
# When a change needs a reboot to apply, /run/remmy-bench-reboot-required exists
# afterwards (a reboot clears it).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_URL="${REPO_URL:-https://github.com/markovejnovic/remmy}"
RUNNER_USER=gh-runner
RUNNER_DIR=/opt/actions-runner
RUNNER_LABELS=remmy-bench
REBOOT_MARKER=/run/remmy-bench-reboot-required
# Records that /bench is a plain directory by choice (it lives outside /bench,
# which every bench job empties).
ROOTFS_MARKER=/var/lib/remmy-bench/bench-on-rootfs
# Keep in sync with GCC_IMAGE in .github/workflows/bench.yml (and release.yml).
# Fully qualified: podman, unlike docker, has no default registry.
GCC_IMAGE=docker.io/library/gcc:16.2.0-trixie@sha256:ef558a40d1f13115293feee01526dbdb9aaad7c9c5a00da05f471ce042e855c1

log() { printf '\n==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { printf 'provision: %s\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root"
grep -q 'VERSION_ID="24.04"' /etc/os-release || die "written for Ubuntu 24.04"

# put MODE OWNER DEST: writes stdin to DEST only if it differs. Returns 0 when
# it changed something, 1 when DEST was already right.
put() {
	local mode="$1" owner="$2" dest="$3" tmp
	tmp="$(mktemp)"
	cat >"$tmp"
	if [[ -f $dest ]] && cmp -s "$tmp" "$dest" &&
		[[ "$(stat -c '%a %U:%G' "$dest")" == "${mode#0} $owner" ]]; then
		rm -f "$tmp"
		return 1
	fi
	install -D -m "$mode" -o "${owner%:*}" -g "${owner#*:}" "$tmp" "$dest"
	rm -f "$tmp"
	note "wrote $dest"
}

# Expands a cpulist ("0-3,8") to one CPU per line.
expand_cpus() {
	local part
	tr ',' '\n' <<<"$1" | while read -r part; do
		[[ -z $part ]] && continue
		if [[ $part == *-* ]]; then seq "${part%-*}" "${part#*-}"; else echo "$part"; fi
	done
}

# Disks that hold nothing: no filesystem or partition-table signature, no
# partitions, unmounted, not a RAID or LVM member, writable.
blank_disks() {
	local dev size type ro name
	lsblk -dpnbo NAME,SIZE,TYPE,RO | while read -r dev size type ro; do
		name="${dev##*/}"
		[[ $type == disk && $ro == 0 && $size -gt 0 && $name != zram* ]] || continue
		[[ -z "$(ls -A "/sys/block/$name/holders" 2>/dev/null)" ]] || continue
		[[ "$(lsblk -nro NAME "$dev" | wc -l)" -eq 1 ]] || continue
		lsblk -nro MOUNTPOINT "$dev" | grep -q . && continue
		blkid -p "$dev" &>/dev/null && continue
		echo "$dev"
	done
}

runner_busy() { pgrep -u "$RUNNER_USER" -f Runner.Worker >/dev/null; }

# A fresh cloud machine may still be running cloud-init and its apt jobs.
if command -v cloud-init >/dev/null; then cloud-init status --wait >/dev/null 2>&1 || true; fi

log "packages"
export DEBIAN_FRONTEND=noninteractive
APT=(apt-get -qq -o DPkg::Lock::Timeout=600 -o Acquire::Retries=5)
# build-essential: tests/bench compiles mktree.cpp. podman: rootless builds in
# the release toolchain image, so the runner never needs root or a docker group.
packages=(build-essential bfs ca-certificates curl e2fsprogs fuse-overlayfs git hyperfine jq
	podman python3 slirp4netns sudo tar uidmap util-linux)
if dpkg-query -W -f='${Status}\n' "${packages[@]}" 2>/dev/null | grep -qv 'install ok installed' ||
	[[ "$(dpkg-query -W -f='${Status}\n' "${packages[@]}" 2>/dev/null | wc -l)" -ne ${#packages[@]} ]]; then
	apt_log=/var/log/remmy-bench-apt.log
	if ! { "${APT[@]}" update && "${APT[@]}" install -y --no-install-recommends "${packages[@]}"; } &>"$apt_log"; then
		tail -n 40 "$apt_log" >&2
		die "apt-get failed (full log: $apt_log)"
	fi
	note "installed ${packages[*]}"
else
	note "all installed"
fi

log "runner user"
id "$RUNNER_USER" &>/dev/null || useradd --create-home --shell /bin/bash "$RUNNER_USER"
grep -q "^$RUNNER_USER:" /etc/subuid || usermod --add-subuids 100000-165535 "$RUNNER_USER"
grep -q "^$RUNNER_USER:" /etc/subgid || usermod --add-subgids 100000-165535 "$RUNNER_USER"
RUNNER_HOME="$(getent passwd "$RUNNER_USER" | cut -d: -f6)"
install -d -o "$RUNNER_USER" -g "$RUNNER_USER" "$RUNNER_HOME/.config" "$RUNNER_HOME/.config/containers"
# The runner is a system service, not a login session: no systemd user manager.
put 0644 "$RUNNER_USER:$RUNNER_USER" "$RUNNER_HOME/.config/containers/containers.conf" <<'EOF' || true
[engine]
cgroup_manager = "cgroupfs"
events_logger = "file"
EOF

log "root helpers"
put 0755 root:root /usr/local/sbin/bench-ctl <"$HERE/bench-ctl" || true
put 0755 root:root /usr/local/sbin/bench-tune <"$HERE/bench-tune" || true
visudo -cf "$HERE/sudoers" >/dev/null || die "sudoers does not parse"
put 0440 root:root /etc/sudoers.d/bench <"$HERE/sudoers" || true

log "measured volume /bench"
install -d /bench
if mountpoint -q /bench; then
	note "dedicated disk, mounted"
elif grep -qE '^[^#]\S*\s+/bench\s' /etc/fstab; then
	# Set up before; just not mounted (nofail lets a boot continue without it).
	mount /bench || die "/bench is in /etc/fstab but does not mount; fix or remove that line"
	note "dedicated disk, mounted from fstab"
else
	device="${BENCH_DEVICE:-}"
	if [[ -z $device && -e $ROOTFS_MARKER ]]; then
		device=none
	elif [[ -z $device ]]; then
		mapfile -t blank < <(blank_disks)
		if ((${#blank[@]})); then
			device="${blank[0]}"
			note "picked blank disk $device (blank: ${blank[*]})"
		else
			device=none
		fi
	fi
	if [[ $device == none ]]; then
		install -d "${ROOTFS_MARKER%/*}"
		touch "$ROOTFS_MARKER"
		note "a directory on the root filesystem (no blank disk; timings are noisier)"
	else
		[[ -b $device ]] || die "$device is not a block device"
		if findmnt -rno TARGET --source "$device" >/dev/null ||
			lsblk -nro MOUNTPOINT "$device" | grep -q .; then
			die "$device (or a partition on it) is mounted; pick an unused device"
		fi
		fstype="$(blkid -o value -s TYPE "$device" || true)"
		label="$(blkid -o value -s LABEL "$device" || true)"
		if [[ $fstype == ext4 && $label == bench ]]; then
			note "reusing the bench filesystem already on $device"
		elif blkid -p "$device" &>/dev/null || [[ "$(lsblk -nro NAME "$device" | wc -l)" -gt 1 ]]; then
			[[ ${BENCH_FORMAT:-} == yes ]] || die "$device holds data (${fstype:-partitions}); BENCH_FORMAT=yes erases it"
			mkfs.ext4 -q -F -L bench "$device"
			note "formatted $device"
		else
			mkfs.ext4 -q -F -L bench "$device"
			note "formatted blank $device"
		fi
		uuid="$(blkid -o value -s UUID "$device")"
		# The directory's scratch contents would only hide under the mount.
		find /bench -mindepth 1 -delete
		rm -f "$ROOTFS_MARKER"
		# noatime: reads during a delete must not become writes. No `discard`:
		# bench-ctl fstrim trims before each suite instead of during timed runs.
		echo "UUID=$uuid /bench ext4 noatime,nofail 0 2" >>/etc/fstab
		systemctl daemon-reload
		mount /bench
		note "dedicated disk $device, mounted"
	fi
fi
chown "$RUNNER_USER:" /bench

log "quiet machine"
# Nothing may wake up mid-run. Patch by hand: sudo apt-get upgrade && sudo reboot.
for unit in apt-daily.timer apt-daily-upgrade.timer man-db.timer fstrim.timer motd-news.timer \
	e2scrub_all.timer dpkg-db-backup.timer fwupd-refresh.timer podman-auto-update.timer \
	sysstat-collect.timer sysstat-summary.timer unattended-upgrades.service; do
	if [[ "$(systemctl is-enabled "$unit" 2>/dev/null || true)" != masked ]]; then
		systemctl disable --now "$unit" &>/dev/null || true
		systemctl mask "$unit" &>/dev/null || true
		note "masked $unit"
	fi
done
if command -v snap >/dev/null; then snap refresh --hold &>/dev/null || true; fi
sed -i -E 's|^([^#].*[[:space:]]swap[[:space:]].*)$|# \1|' /etc/fstab
changed=0
put 0644 root:root /etc/systemd/system/bench-tune.service <"$HERE/bench-tune.service" && changed=1
[[ $changed == 1 ]] && systemctl daemon-reload
systemctl enable --quiet bench-tune.service
# Re-applying the tuning is harmless, and catches a knob something reset.
systemctl restart bench-tune.service

log "CPU split"
all_cpus="$(cat /sys/devices/system/cpu/online)"
HOUSEKEEPING_CPUS="${HOUSEKEEPING_CPUS:-$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list)}"
[[ $HOUSEKEEPING_CPUS =~ ^[0-9,-]+$ ]] || die "bad HOUSEKEEPING_CPUS '$HOUSEKEEPING_CPUS'"
bench_cpus="$(comm -23 <(expand_cpus "$all_cpus" | sort) <(expand_cpus "$HOUSEKEEPING_CPUS" | sort) |
	sort -n | paste -sd, -)"
[[ -n $bench_cpus ]] || die "no CPUs left for benchmarks after HOUSEKEEPING_CPUS=$HOUSEKEEPING_CPUS"
note "housekeeping: $HOUSEKEEPING_CPUS  benchmarks: $bench_cpus"
split_changed=0
for unit in system.slice user.slice init.scope; do
	section="$([[ $unit == *.scope ]] && echo Scope || echo Slice)"
	printf '[%s]\nAllowedCPUs=%s\n' "$section" "$HOUSEKEEPING_CPUS" |
		put 0644 root:root "/etc/systemd/system/$unit.d/50-housekeeping.conf" && split_changed=1
done
printf '[Unit]\nDescription=Benchmark runner and everything it starts\n\n[Slice]\nAllowedCPUs=%s\n' "$bench_cpus" |
	put 0644 root:root /etc/systemd/system/bench.slice && split_changed=1
if [[ $split_changed == 1 ]]; then
	systemctl daemon-reload
	# Running processes keep their old placement until a reboot.
	touch "$REBOOT_MARKER"
	note "CPU split changed: reboot to apply it everywhere"
fi

log "GitHub Actions runner"
token="${RUNNER_TOKEN:-}"
if [[ -z $token && -n ${RUNNER_TOKEN_FILE:-} ]]; then token="$(<"$RUNNER_TOKEN_FILE")"; fi
if [[ -f $RUNNER_DIR/.runner && ${RUNNER_RESET:-} == yes ]]; then
	runner_busy && die "the runner is running a job; retry when it finishes"
	systemctl stop gh-runner.service 2>/dev/null || true
	rm -f "$RUNNER_DIR/.runner" "$RUNNER_DIR/.credentials" "$RUNNER_DIR/.credentials_rsaparams"
	note "dropped the old registration"
fi
if [[ ! -x $RUNNER_DIR/config.sh ]]; then
	case "$(uname -m)" in x86_64) arch=x64 ;; aarch64) arch=arm64 ;; *) die "unsupported $(uname -m)" ;; esac
	version="$(curl -fsSL --retry 5 https://api.github.com/repos/actions/runner/releases/latest | jq -r .tag_name)"
	version="${version#v}"
	[[ $version =~ ^[0-9.]+$ ]] || die "could not find the latest runner release"
	install -d -o "$RUNNER_USER" -g "$RUNNER_USER" "$RUNNER_DIR"
	curl -fsSL --retry 5 -o /tmp/actions-runner.tar.gz \
		"https://github.com/actions/runner/releases/download/v$version/actions-runner-linux-$arch-$version.tar.gz"
	sudo -u "$RUNNER_USER" tar -xzf /tmp/actions-runner.tar.gz -C "$RUNNER_DIR"
	rm -f /tmp/actions-runner.tar.gz
	note "installed runner $version"
fi
chown -R "$RUNNER_USER:" "$RUNNER_DIR"
if [[ ! -f $RUNNER_DIR/.runner ]]; then
	[[ -n $token ]] || die "the runner is not configured: pass RUNNER_TOKEN_FILE or RUNNER_TOKEN"
	if [[ -x $RUNNER_DIR/bin/installdependencies.sh ]]; then "$RUNNER_DIR/bin/installdependencies.sh" &>/dev/null; fi
	sudo -u "$RUNNER_USER" "$RUNNER_DIR/config.sh" --unattended --replace \
		--url "$REPO_URL" --token "$token" --name "$(hostname)-bench" \
		--labels "$RUNNER_LABELS" --work "$RUNNER_HOME/work"
	runner_registered=1
fi
runner_changed=0
put 0644 root:root /etc/systemd/system/gh-runner.service <<EOF && runner_changed=1
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
[[ $runner_changed == 1 ]] && systemctl daemon-reload
systemctl enable --quiet gh-runner.service
if ! systemctl is-active --quiet gh-runner.service; then
	systemctl start gh-runner.service
	note "started the runner"
elif [[ $runner_changed == 1 || -n ${runner_registered:-} ]]; then
	# Restarting mid-job would fail that job: leave it for the reboot instead.
	if runner_busy; then
		touch "$REBOOT_MARKER"
		note "runner is busy: its new configuration applies after a reboot"
	else
		systemctl restart gh-runner.service
		note "restarted the runner"
	fi
else
	note "running"
fi

log "toolchain image"
if sudo -u "$RUNNER_USER" -H bash -c "cd ~ && podman image exists '$GCC_IMAGE'"; then
	note "present"
else
	sudo -u "$RUNNER_USER" -H bash -c "cd ~ && podman pull -q '$GCC_IMAGE' >/dev/null"
	note "pulled"
fi

log "done"
if [[ -e $REBOOT_MARKER ]]; then echo "A reboot is needed to apply every change."; fi
