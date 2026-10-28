# Source validation before publishing

The prepared distribution targets the Raitchi Android modules. Original Eden
emulator/platform sources containing unrelated network defaults are omitted;
the Android modules use the shared engine's staged native libraries instead.
This snapshot does not promise that Eden's other platform targets build.
Copyright, license texts, upstream credits and the real upstream base are retained.
Internal instructions, development policies and host inventory tools are omitted.
Public documentation uses generic data-directory and address placeholders.
No functional constant in the canonical repositories is changed by preparation.

1. Verify the source files against `SOURCE-MANIFEST.json` and the shell snapshot
   against `shell-overlay.json`; both use relative names and SHA-256 only.
2. Run `tools/check_source_privacy.py` on each source root, supplying an external
   private-pattern file. Scan all payload bytes and filenames independently too.
   Do not package the private pattern file or local preparation audit directory.
3. Restore the shell using [the overlay workflow](SOURCE-OVERLAY.md). Verify the
   exact upstream base, copied hashes and source inputs before compiling.
4. Fetch and verify locked sources, bootstrap all dependencies in
   [the build instructions](BUILDING.md), then perform the native and app builds.
   Preserve real upstream history when preparing a maintained fork.
5. Inspect the final release APK and source archive, keep signing material out
   of both, and verify the published artifact hashes match the reviewed files.

Metadata/hash restoration has been validated in an isolated clone. A complete
new-checkout native/APK build is a separate check and is not established by that
restoration test or by a successful development-tree build.
