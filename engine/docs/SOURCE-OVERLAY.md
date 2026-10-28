# Restoring the Eden-derived source overlay

The release preparation contains `engine/`, `shell/` and `shell-overlay.json`.
The manifest records the actual pinned Eden base, every retained shell file's
SHA-256, and explicit upstream file deletions. It contains no owner paths or
repository URL invented for a future Raitchi fork. Upstream Git history must be
preserved; copying the files into a new empty repository is not equivalent.

From the prepared engine source directory, restore an isolated shell checkout:

```sh
python3 tools/restore_shell_overlay.py \
  --snapshot ../shell \
  --manifest ../shell-overlay.json \
  --destination ../shell-with-history
```

The tool clones Eden without shallow history, checks out the exact manifest base,
verifies snapshot hashes, applies only declared files/deletions, and verifies
all copied hashes and base ancestry. The destination must not exist. It does not
initialize a replacement repository, create a commit, push, install an app, or
change a canonical checkout. An optional `--clone-source` can supply a local Git
cache containing the same base; the resulting origin is reset to the declared
public upstream URL. Keep the manifest adjacent to the source directories when
creating source archives.

The prepared release's `shell.lock.json` records the actual public distribution
URL, the exact upstream base commit and the hashed uncommitted overlay. It does
not assert that the distribution repository is a standalone Eden fork containing
that base or that a Raitchi integration commit has been published. Restore the
shell using the manifest's real upstream URL. The restored HEAD equals the
upstream base pin; working-tree changes remain uncommitted.

After the native bootstrap, build the restored checkout:

```sh
python3 tools/fetch_sources.py
python3 tools/verify_sources.py
python3 tools/verify_sources.py --lock echoes-source.lock.json
# Run the complete native dependency/bootstrap sequence in BUILDING.md first.
python3 tools/build_eden_shell.py --shell ../shell-with-history \
  --build --allow-downloads
```

The development builder also supports `--allow-unpinned-shell` when using a
separate development lock whose integration HEAD differs from this reconstructed
base. That bypasses only HEAD equality; it still checks Eden ancestry and input
hashes. It is unnecessary for the supplied base-plus-overlay release metadata.
A maintained fork should preserve upstream history and commit a reviewed overlay;
its real integration commit can then be locked. An uncommitted snapshot is not
represented as such a commit.

Validation scope: an isolated local-cache clone successfully restored the pinned
base and exact overlay hashes, and the existing builder's `shell_inputs()` could
read the restored sources. Native dependencies, Gradle downloads, compilation,
APK signing and device execution were not run in this overlay validation.
A new checkout full build remains a separate required validation.
