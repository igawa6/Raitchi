# Security policy

## Supported versions

AC-NativeNX is experimental. Only the **latest commit of `main`** and the **latest published release** (if
any) get fixes. Problems in upstream projects (Raitchi by igawa6, Eden, yuzu) belong to their own
maintainers; we do not report to or on behalf of them.

## Reporting a vulnerability

**Do not open a public issue.**

Use GitHub's private vulnerability reporting: open the **Security** tab of this repository and click
**Report a vulnerability**. If that is not available, open a public issue that says only "I need to
report a security problem privately" with no details, and a maintainer will set up a private channel.

Please include:

- the affected commit or release, and the component (importer, runtime, APK, CI, build scripts);
- how to reproduce it, using **synthetic files only**. Never attach game files, keys or firmware, even
  to show a parser bug: describe the structure or build a minimal crafted file;
- the impact you expect.

We aim to acknowledge reports within 7 days. This is a volunteer project, so there is no guaranteed fix
time. We credit reporters unless they ask us not to.

## In scope

- Memory-safety or parsing bugs in the importers (NSP / XCI / NCA / archive / mod parsing) that a crafted
  file can trigger.
- Anything that could leak user files, keys or saves outside the app's storage.
- Supply chain: CI workflows, release signing, bundled binaries (for example GPU driver packages),
  dependency pinning.
- Secrets committed to the repository or its history.

## Out of scope

- Bugs that need a modified device or root access the user already granted.
- Game glitches, crashes without a security impact, and performance problems: use the bug report form.

## Legal and rights-holder requests

If you are a rights holder and believe something in this repository infringes your rights, report it
privately through the same **Security** tab ("Report a vulnerability") with the exact file paths or URLs.
We review requests promptly and remove content that does not belong here. You can also use GitHub's
DMCA process. This project contains and distributes no game, key or firmware (see
[CONTRIBUTING.md](CONTRIBUTING.md)).
