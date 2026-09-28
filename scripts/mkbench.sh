#!/usr/bin/env bash
# shellcheck disable=SC2016 # single-quoted commands expand on the server
# Sets up (or re-checks) the benchmark server over SSH and the repository's
# side of it, from a workstation with `gh` logged in as a repository admin.
# USER must be root or have passwordless sudo.
#
#   [BENCHER_API_KEY=...] scripts/mkbench.sh [options] USER@HOST
#   [BENCHER_API_KEY=...] scripts/mkbench.sh [options] USER HOST
#
#   -p, --port PORT         SSH port (default 22)
#   -i, --identity FILE     SSH private key
#   -d, --device DEV        block device for /bench, the measured volume
#                           (default: a blank disk if the server has one, else
#                           a directory on the root filesystem)
#       --no-device         always use a directory on the root filesystem
#       --format            allow erasing a DEV that holds another filesystem
#       --housekeeping CPUS OS CPUs, e.g. 0-1 (default: cpu0 and its SMT sibling)
#   -R, --repo OWNER/NAME   default markovejnovic/remmy
#       --trust USER        GitHub user whose jobs may run on the server;
#                           repeat for several (default: the repo's owner)
#       --no-reboot         don't reboot even if a change needs it
#       --force             reboot at once, even while the runner runs a job
#                           (by default a needed reboot waits for the job)
#   -h, --help
#
# Safe to run any number of times: it copies ci/bench-host to the server, runs
# provision.sh (which only changes what differs), registers the runner only if
# GitHub doesn't already know it, reboots only when a change needs it, and
# verifies the server. Then it configures the repository: approval for every
# outside contributor's workflow runs, the BENCHER_API_KEY
# secret (from the environment: a user API key from bencher.dev > API Keys),
# and a first benchmark run on main if there has never been one.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO=markovejnovic/remmy
PORT=22
IDENTITY=""
DEVICE=""
FORMAT=""
HOUSEKEEPING=""
REBOOT=yes
FORCE=""
TRUST=()
REMOTE_DIR=.cache/remmy-bench-host
MARKER=/run/remmy-bench-reboot-required

usage() { sed -n '3,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; }
log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'mkbench: %s\n' "$*" >&2; exit 1; }

target=()
while (($#)); do
	case "$1" in
	-p | --port) PORT="${2:?}"; shift ;;
	-i | --identity) IDENTITY="${2:?}"; shift ;;
	-d | --device) DEVICE="${2:?}"; shift ;;
	--no-device) DEVICE=none ;;
	--format) FORMAT=yes ;;
	--housekeeping) HOUSEKEEPING="${2:?}"; shift ;;
	-R | --repo) REPO="${2:?}"; shift ;;
	--trust) TRUST+=("${2:?}"); shift ;;
	--no-reboot) REBOOT="" ;;
	--force) FORCE=yes ;;
	-h | --help) usage; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) target+=("$1") ;;
	esac
	shift
done
case ${#target[@]} in
1) [[ ${target[0]} == *@* ]] || die "expected USER@HOST or USER HOST"; DEST="${target[0]}" ;;
2) DEST="${target[0]}@${target[1]}" ;;
*) usage >&2; exit 64 ;;
esac
[[ $REPO =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "bad --repo '$REPO'"
((${#TRUST[@]})) || TRUST=("${REPO%%/*}")
for user in "${TRUST[@]}"; do [[ $user =~ ^[A-Za-z0-9-]+$ ]] || die "bad --trust '$user'"; done
[[ -z $HOUSEKEEPING || $HOUSEKEEPING =~ ^[0-9,-]+$ ]] || die "bad --housekeeping '$HOUSEKEEPING'"
[[ -z $DEVICE || $DEVICE == none || $DEVICE =~ ^/dev/[A-Za-z0-9/_.-]+$ ]] || die "bad --device '$DEVICE'"

for tool in ssh tar gh jq; do command -v "$tool" >/dev/null || die "$tool is required"; done
gh auth status >/dev/null 2>&1 || die "gh is not logged in: run gh auth login"
[[ -f $ROOT/ci/bench-host/provision.sh ]] || die "run from a remmy checkout"

# One multiplexed connection for every step; it survives until we exit.
# Short path: a socket path over ~100 bytes (macOS's $TMPDIR is long) fails.
CTL="$(mktemp -d /tmp/mkbench.XXXXXX)"
SSH=(ssh -p "$PORT" -o ControlMaster=auto -o "ControlPath=$CTL/%C" -o ControlPersist=120
	-o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4
	-o StrictHostKeyChecking=accept-new)
[[ -n $IDENTITY ]] && SSH+=(-i "$IDENTITY")
cleanup() {
	"${SSH[@]}" -O exit "$DEST" &>/dev/null || true
	rm -rf "$CTL"
}
trap cleanup EXIT
remote() { "${SSH[@]}" "$DEST" "$@"; }

# field NAME JSON: one field of a JSON object, empty if absent or no object.
field() { jq -r ".$1 // empty" <<<"${2:-null}"; }

# Our runner as GitHub sees it: {"status","busy"} or nothing.
github_runner() {
	gh api "repos/$REPO/actions/runners" --paginate -q ".runners[] | select(.name == \"$1\") | {status, busy}" |
		head -n1
}

log "connecting to $DEST"
remote true || die "cannot reach $DEST over SSH (see the error above)"
read -r uid host home configured < <(remote 'printf "%s %s %s %s\n" "$(id -u)" "$(hostname)" "$HOME" \
	"$(test -f /opt/actions-runner/.runner && echo yes || echo no)"')
remote 'grep -q "VERSION_ID=\"24.04\"" /etc/os-release' || die "$DEST is not Ubuntu 24.04"
if [[ $uid == 0 ]]; then
	SUDO=""
elif remote 'sudo -n true' &>/dev/null; then
	SUDO="sudo -n"
else
	die "$DEST needs passwordless sudo (or connect as root)"
fi
# These come from the server: check them before they reach a local command.
[[ $uid =~ ^[0-9]+$ ]] || die "unexpected uid from the server: '$uid'"
[[ $host =~ ^[A-Za-z0-9][A-Za-z0-9.-]{0,62}$ ]] || die "unexpected hostname from the server: '$host'"
[[ $home =~ ^/[A-Za-z0-9/_.-]+$ ]] || die "unexpected home directory from the server: '$home'"
[[ $configured == yes || $configured == no ]] || die "unexpected runner state from the server: '$configured'"
REMOTE_DIR="$home/$REMOTE_DIR"
RUNNER_NAME="$host-bench"
echo "host $host, runner $RUNNER_NAME, runner configured: $configured"

log "checking the runner on GitHub"
state="$(github_runner "$RUNNER_NAME")"
reset=""
token_needed=""
if [[ $configured == no ]]; then
	token_needed=yes
elif [[ -z $state ]]; then
	echo "$RUNNER_NAME is configured on $host but unknown to GitHub: registering it again"
	token_needed=yes reset=yes
else
	echo "$RUNNER_NAME: $state"
fi

log "copying ci/bench-host"
remote "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
tar -C "$ROOT/ci/bench-host" -cf - . | remote "tar -xf - -C '$REMOTE_DIR'"
if [[ -n $token_needed ]]; then
	# Through a private file, never a command line other users could read.
	gh api -X POST "repos/$REPO/actions/runners/registration-token" -q .token |
		remote "umask 077 && cat > '$REMOTE_DIR/token'"
fi

log "provisioning"
env_args=("REPO_URL=https://github.com/$REPO" "TRUSTED_ACTORS='${TRUST[*]}'")
[[ -n $DEVICE ]] && env_args+=("BENCH_DEVICE=$DEVICE")
[[ -n $FORMAT ]] && env_args+=(BENCH_FORMAT=yes)
[[ -n $HOUSEKEEPING ]] && env_args+=("HOUSEKEEPING_CPUS=$HOUSEKEEPING")
[[ -n $reset ]] && env_args+=(RUNNER_RESET=yes)
[[ -n $token_needed ]] && env_args+=("RUNNER_TOKEN_FILE=$REMOTE_DIR/token")
status=0
tty=()
[[ -t 0 && -t 1 ]] && tty=(-t) # live, unbuffered output when run from a terminal
"${SSH[@]}" "${tty[@]}" "$DEST" "$SUDO env ${env_args[*]} bash '$REMOTE_DIR/provision.sh'" || status=$?
remote "rm -f '$REMOTE_DIR/token'" || true
((status == 0)) || die "provision.sh failed (exit $status); fix the cause and run this again"

log "configuring $REPO"
# bench.yml's plan job reads the same list, to skip (not fail) other runs.
if [[ "${TRUST[*]}" == "${REPO%%/*}" ]]; then
	gh variable delete BENCH_TRUSTED_ACTORS -R "$REPO" &>/dev/null || true
else
	gh variable set BENCH_TRUSTED_ACTORS -R "$REPO" --body "${TRUST[*]}"
fi
echo "  ok    trusted: ${TRUST[*]}"
# Fork PRs never reach the server (bench-job-gate refuses them); the label that
# once let a maintainer opt one in is gone.
if gh label list -R "$REPO" --json name -q '.[].name' | grep -qx bench; then
	gh label delete bench -R "$REPO" --yes >/dev/null
fi
echo "  ok    no bench label"
# A second guard: GitHub holds an outside contributor's runs for approval.
policy=all_external_contributors
if [[ "$(gh api "repos/$REPO/actions/permissions/fork-pr-contributor-approval" -q .approval_policy)" != "$policy" ]]; then
	gh api -X PUT "repos/$REPO/actions/permissions/fork-pr-contributor-approval" -f approval_policy="$policy"
fi
echo "  ok    fork PR runs need approval ($policy)"
if [[ -n ${BENCHER_API_KEY:-} ]]; then
	printf '%s' "$BENCHER_API_KEY" | gh secret set BENCHER_API_KEY -R "$REPO"
	echo "  ok    secret BENCHER_API_KEY set"
elif gh secret list -R "$REPO" --json name -q '.[].name' | grep -qx BENCHER_API_KEY; then
	echo "  ok    secret BENCHER_API_KEY present"
else
	printf '  TODO  results are not tracked until BENCHER_API_KEY is set: sign in at\n'
	printf '        https://bencher.dev, create a user API key, and run this again with\n'
	printf '        BENCHER_API_KEY=<key> in the environment\n'
fi

if remote "test -e $MARKER"; then
	if [[ -z $REBOOT ]]; then
		echo "A reboot is needed; skipped (--no-reboot). Run this again without it later."
	else
		if [[ -z $FORCE ]]; then
			# A reboot mid-job would fail that job: wait for it to finish.
			waited=""
			while [[ $(field busy "$(github_runner "$RUNNER_NAME")") == true ]]; do
				[[ -n $waited ]] || echo "A reboot is needed; waiting for the runner's job to finish (Ctrl-C and run again later is fine)"
				waited=yes
				sleep 30
			done
			# Stop it so no new job starts between this check and the reboot.
			remote "$SUDO systemctl stop gh-runner.service" || true
		fi
		log "rebooting $host"
		boot_id="$(remote cat /proc/sys/kernel/random/boot_id)"
		remote "$SUDO systemctl reboot" &>/dev/null || true
		"${SSH[@]}" -O exit "$DEST" &>/dev/null || true
		sleep 10
		for ((i = 0; i < 60; i++)); do
			now="$(remote cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)"
			[[ -n $now && $now != "$boot_id" ]] && break
			sleep 10
		done
		[[ -n $now && $now != "$boot_id" ]] || die "$host did not come back within 10 minutes"
	fi
fi

log "verifying"
fail=0
check() {
	if remote "$2" &>/dev/null; then printf '  ok    %s\n' "$1"; else printf '  FAIL  %s\n' "$1"; fail=1; fi
}
check "bench-tune ran" 'systemctl is-active --quiet bench-tune.service'
pending=""
[[ -z $REBOOT ]] && remote "test -e $MARKER" && pending=yes
if [[ -n $pending ]]; then
	printf '  skip  runner service active (starts after the pending reboot)\n'
else
	check "runner service active" 'systemctl is-active --quiet gh-runner.service'
fi
check "runner in bench.slice" 'systemctl show -p Slice --value gh-runner.service | grep -qx bench.slice'
check "/bench ready, owned by gh-runner" '
	test "$(stat -c %U /bench)" = gh-runner && { mountpoint -q /bench || test -e /var/lib/remmy-bench/bench-on-rootfs; }'
if remote 'mountpoint -q /bench'; then
	printf '        /bench is a dedicated disk: %s\n' "$(remote 'findmnt -no SOURCE /bench')"
else
	printf '        /bench is a directory on the root filesystem (pass --device when a spare disk exists)\n'
fi
check "no swap" 'test -z "$(swapon --noheadings)"'
check "governor is performance (or none)" \
	'! grep -hv performance /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | grep -q .'
check "gh-runner may drop caches" "sudo -n -u gh-runner sudo -n -l /usr/local/sbin/bench-ctl drop-caches"
check "job gate wired (root-owned .env)" '
	grep -qx "ACTIONS_RUNNER_HOOK_JOB_STARTED=/usr/local/sbin/bench-job-gate" /opt/actions-runner/.env &&
	test "$(stat -c %U /opt/actions-runner/.env /usr/local/sbin/bench-job-gate | sort -u)" = root'
# The gate only binds a runner that started after .env named it; a stale
# Runner.Listener (or two) would take jobs without it.
if [[ -z $pending ]]; then
	check "one runner process, started after the gate" '
		pids="$(pgrep -u gh-runner -f "bin/Runner.Listener")"
		test "$(wc -w <<<"$pids")" = 1 &&
		test "$(date -d "$(ps -o lstart= -p "$pids")" +%s)" -ge "$(stat -c %Y /opt/actions-runner/.env)"'
fi
check "SSH is key-only" "$SUDO sshd -T 2>/dev/null | grep -qx 'passwordauthentication no'"
check "firewall loaded (inbound SSH only)" "$SUDO nft list table inet remmy_bench"
check "security updates scheduled" 'systemctl is-enabled --quiet bench-patch.timer && systemctl is-active --quiet bench-patch.timer'
# Through a new connection: the shared one would survive a firewall lockout.
if "${SSH[@]}" -o ControlPath=none -o BatchMode=yes "$DEST" true &>/dev/null; then
	printf '  ok    a new SSH connection gets in\n'
else
	printf '  FAIL  a new SSH connection is refused: fix it from the Vultr web console\n'
	fail=1
fi
image="$(sed -n 's/^GCC_IMAGE=//p' "$ROOT/ci/bench-host/provision.sh")"
check "toolchain image present" "sudo -n -u gh-runner -H sh -c 'cd && podman image exists $image'"
if [[ -n $pending ]]; then
	printf '  skip  CPU split (reboot pending)\n'
else
	# The runner may use exactly the bench CPUs, and sshd (system.slice) none of them.
	check "CPU split applied" '
		bench="$(cat /sys/fs/cgroup/bench.slice/cpuset.cpus.effective)"
		pid="$(systemctl show -p MainPID --value gh-runner.service)"
		test -n "$bench" &&
		test "$(awk "/^Cpus_allowed_list/ {print \$2}" "/proc/$pid/status")" = "$bench" &&
		test "$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective)" != "$bench"'
fi
online=""
for ((i = 0; i < 12; i++)); do
	state="$(github_runner "$RUNNER_NAME")"
	[[ $(field status "$state") == online ]] && { online=yes; break; }
	sleep 5
done
if [[ -n $online ]]; then printf '  ok    GitHub sees %s online\n' "$RUNNER_NAME"; else
	printf '  FAIL  GitHub does not see %s online\n' "$RUNNER_NAME"
	fail=1
fi
((fail == 0)) || die "some checks failed (see above); running this again is safe"

log "first benchmark run"
# Before the workflow is merged there is nothing to dispatch; after, seed main's
# history once, unless a run already has (or is about to).
if gh api "repos/$REPO/contents/.github/workflows/bench.yml" &>/dev/null; then
	runs="$(gh run list -R "$REPO" -w bench.yml -b main -L 50 --json status,conclusion,event \
		-q '[.[] | select(.event != "pull_request") | select(.conclusion == "success" or .status != "completed")] | length')"
	if [[ $runs == 0 ]]; then
		gh workflow run bench.yml -R "$REPO" --ref main -f suite=demo
		echo "  ok    dispatched a first demo run on main (full runs nightly)"
	else
		echo "  ok    main already has benchmark runs"
	fi
else
	echo "  skip  first run: bench.yml is not on the default branch yet (merge the PR, then run this again)"
fi

log "$host is ready: jobs labelled remmy-bench will run there"
