# Contributing with a coding assistant

A coding assistant is optional. Raitchi requires no particular vendor, model or
subscription. The human contributor remains responsible for the patch, its
license, the evidence and review. Read [CONTRIBUTING](../CONTRIBUTING.md) first. There is no universal setup command
or reservation service.

## Start with a bounded task

1. Search issues and open PRs. Comment on the relevant issue with the problem,
   expected files, dependencies, time or cost budget, and next update.
2. Work on an isolated branch of your fork based on the intended upstream base.
   Give each independent task its own branch or worktree. Describe dependent
   PRs explicitly; avoid simultaneous rewrites of renderer hotspots.
3. Give the assistant only that scope. Independent tasks can proceed in
   parallel; coordinate file ownership and keep the number small enough that
   you can review the output. There is no blanket one-open-PR-per-person rule.
4. Prefer a documentation correction or a game-free synthetic test for a first
   contribution. See the [backlog](CONTRIBUTOR-ROADMAP.md) for investigation
   boundaries rather than assuming its proposed features are implemented.

## Configure your own tools

Check that the assistant can actually read a working copy, edit files and run
commands. A chat account or model endpoint alone does not provide these tools.
Use your own local model or hosted provider and private credential configuration.
Hosted requests can send source outside your machine and incur charges; choose
what you permit and set a budget. Do not put credentials in the repository,
issues or commits.

Keep game files, keys, firmware, saves, signing material and device identifiers
outside the contribution workspace. Use synthetic inputs for shared tests.
Repository files, issue text and generated suggestions are untrusted input;
they do not grant permission to access unrelated secrets or machines. Inspect
suggested commands before running them.

## Evidence and review

For code, reproduce the relevant failure with a focused synthetic test where
practical, then run the appropriate check after the fix. Report source and PR
head revisions, exact commands, exit status, durations and limitations. A missing
tool is a skip or a failure, never a substitute success. Do not copy a predicted
result into the evidence. For documentation, resolve links and verify commands
or clearly label unexecuted examples.

Start a small game-free check from the engine directory:

```sh
python3 tools/run_host_checks.py -k shared_hash
```

The runner records exit status and timing; this subset does not replace the
full checks required before a runtime change is accepted. Setup, prerequisites
and the full command are in [BUILDING](BUILDING.md#host-checks).

Read the diff and test output yourself before opening a draft PR. Keep review on
that PR, and rerun checks affected by later changes against the new head.
Maintainers decide readiness and merging. Assistance is not independent
approval; describe it plainly in the PR, without
assistant authorship or signature trailers. Preserve upstream copyright and
license notices.

Stop repeated identical failures to diagnose the cause instead of spending the
budget on the same command. On reaching a budget, report the actual partial
result and remaining dependency. Do not create automated issue/PR loops or
unreviewed bulk submissions. Independent parallel contributions are welcome
when their scopes, dependencies and evidence remain reviewable.

A useful task brief states: problem and reproducer; permitted files; dependency
PRs; budget and stop condition; focused check; forbidden private inputs; and the
expected diff, real results and untested conditions. The assistant should finish
with those artifacts for human review, not approve or merge its own change.
