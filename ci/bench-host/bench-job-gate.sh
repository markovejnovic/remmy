#!/usr/bin/env bash
# The runner's job-started hook (ACTIONS_RUNNER_HOOK_JOB_STARTED; the runner
# insists on a .sh name). It runs before any step of every job, and a non-zero
# exit fails the job before the repository is even checked out.
#
# External pull requests never run on this machine: no review, approval,
# label or re-run lets one through. The only jobs admitted are the owner's:
# push, schedule and workflow_dispatch triggered and run by OWNER, and
# pull_request from a branch of REPO that OWNER opened and OWNER pushed to.
# Everything else (forks, anyone else's branch or push, any other event) is
# refused.
#
# A pull request can't get around this: it brings its own copy of bench.yml,
# but not this file or its config (both root-owned), and the event data comes
# from GitHub. /etc/remmy-bench/gate.conf (written by provision.sh) sets REPO
# and OWNER, the repository's owner.
set -euo pipefail

refuse() {
	echo "bench-job-gate: refusing this job: $*" >&2
	echo "Only the repository owner's own code runs on the benchmark machine; external PRs never do." >&2
	exit 1
}

# shellcheck source=/dev/null
. /etc/remmy-bench/gate.conf
[[ -n ${REPO:-} && -n ${OWNER:-} ]] || refuse "gate.conf is incomplete"

[[ ${GITHUB_REPOSITORY:-} == "$REPO" ]] || refuse "repository '${GITHUB_REPOSITORY:-}' is not $REPO"
[[ ${GITHUB_ACTOR:-} == "$OWNER" ]] || refuse "triggered by '${GITHUB_ACTOR:-}', not $OWNER"
# A re-run is started by whoever clicks it, not by the original actor.
[[ ${GITHUB_TRIGGERING_ACTOR:-$GITHUB_ACTOR} == "$OWNER" ]] ||
	refuse "run by '${GITHUB_TRIGGERING_ACTOR:-}', not $OWNER"

case "${GITHUB_EVENT_NAME:-}" in
push | schedule | workflow_dispatch) ;;
pull_request)
	event="${GITHUB_EVENT_PATH:?}"
	head="$(jq -r '.pull_request.head.repo.full_name // ""' "$event")"
	author="$(jq -r '.pull_request.user.login // ""' "$event")"
	[[ $head == "$REPO" ]] || refuse "external pull request (from '$head')"
	[[ $author == "$OWNER" ]] || refuse "pull request opened by '$author', not $OWNER"
	;;
*) refuse "event '${GITHUB_EVENT_NAME:-}' is not one the benchmarks use" ;;
esac

echo "bench-job-gate: admitted ${GITHUB_EVENT_NAME} by ${GITHUB_ACTOR} on ${GITHUB_REF:-?}"
