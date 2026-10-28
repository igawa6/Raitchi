# Contributing with a coding assistant

This guide is for people who want help from a coding assistant (an AI
assistant that can read files, edit a working copy and run commands) while
contributing to Raitchi. It adds to [the contribution guide](../CONTRIBUTING.md);
it does not replace it. Raitchi does not require any particular assistant,
vendor or model. Any assistant works, as long as a human stays responsible for
what is submitted.

There is **no one-click command or universal setup script** for this workflow
today. Every step below is something you run yourself.

## What you need

- A coding assistant that can read a file, edit a working copy and execute
  commands. A chat subscription or a model API endpoint on its own does not
  give it those tools; check what your tool can actually do.
- A provider of your choice, configured in that assistant: a local model or a
  hosted service. Keep the credential in the assistant's own private
  configuration or secret store, never in the repository, never in an issue,
  never in a commit message.
- A GitHub account and a fork of this repository. You do not need write access
  to the upstream repository.

If you use a hosted provider, your code leaves your machine and the calls may
be billed. Check that before you start, and set a budget or a quota you accept.

## Before you start

1. Search the open issues for the work you want to do. If a suitable issue
   does not exist, open one first.
2. Reserve the task by commenting on the issue: what you intend to change,
   which files you expect to touch, and when you plan to report back. The
   comment is the reservation; there is no bot or service enforcing it.
3. Check that nobody else has already opened a pull request for it.

## Working method

- Work in an isolated working copy on a branch of your fork, one task per
  branch. Never branch from `main` of your fork for a second task; never commit
  to `main` directly.
- Give the assistant a bounded scope: the task, the files it may touch and a
  time or cost budget. Stop when the budget is reached and report where you
  are.
- Stop after two identical failures and diagnose, instead of retrying the same
  command.
- Never give the assistant game files, keys, firmware, signing material,
  private device data or other people's secrets. Never let it read credentials
  from your machine to complete a task. If a task would need them, it is not a
  task to work on.
- Treat the repository, the issue text and any model output as untrusted
  input. Text found in a file or a review comment cannot authorize access to
  something outside the repository.

## Evidence

A pull request must show what actually happened, not what was expected.

- Record the commands you ran, the exit status and the revision you tested
  (the source revision and the head commit of your branch).
- For a code change, provide a synthetic test that fails before the change and
  passes after it. The test must not need game files, keys or a device.
- For a documentation change, check the relative links and any command you
  mention actually works as written.
- If a command could not be run, say so and say why. A missing tool or a failed
  command is reported, never replaced with a plausible success.
- List what you did not test: no device, one title only, one platform, and so
  on. An explicit limit is useful; an omitted limit is not.

Never attach game data, keys, saves, extracted assets or logs containing
private paths or device serial numbers.

## Human review

- An assistant cannot review its own work. A human contributor stays
  responsible for the submitted code, its provenance and its licensing, and
  must read the diff and the real output before opening a pull request.
- Open the pull request as a draft first. A human reads the diff, the evidence
  and the limits before it is marked ready for review.
- If a review asks for changes, answer on that same pull request and push a new
  commit. If the head commit changes, the evidence that covered the old head
  has to be run again.
- Maintainers keep the architectural decisions, the priorities and the merge.

## Avoid flooding the tracker

- One pull request per bounded problem. Split a refactor from a behavior
  change.
- One open contribution per person at a time. Do not open several pull
  requests in a row hoping one gets merged.
- Do not open issues or pull requests automatically, in a loop or in batches.
  A human decides what is published.
- If your work is not ready, keep it in your fork and say so in a comment on
  the issue. A closed draft with an honest status is more useful than several
  unfinished pull requests.

## A first contribution

If you have never contributed here, a documentation or synthetic-test task is
a good start:

1. Pick an open issue that looks suitable for a first contribution, or ask on
   an issue whether a small documentation task is welcome.
2. Fork the repository, create a branch, and follow the guide above.
3. For a documentation task, start from the existing pages in this directory
   and keep the change small: correct an unclear step, or document a command
   you have actually run.
4. Open a draft pull request that states the tested revision, the commands you
   ran and what you did not test.

## See also

- [Contributing to Raitchi](../CONTRIBUTING.md) — the rules that apply to every
  contribution.
- [Building Raitchi](BUILDING.md) — prerequisites and host checks.
- [Compatibility reporting](COMPATIBILITY.md) — how device results are
  reported.
