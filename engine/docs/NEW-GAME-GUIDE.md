# Investigating a new game

Supporting a game usually means finding the shared runtime feature it is
missing: a service command, a kernel behavior, an NVN entry point, a shader
instruction or a texture format. Fix it once in the shared runtime and every
game benefits. A new title adapter is a small table, not a copy of the runtime.

You need your own legally obtained game package (NSP/XCI) and your own
`prod.keys`. Never upload them, never attach them to an issue, and never commit
anything extracted from them.

## 1. Claim the investigation

Open a **Game investigation** issue (one game, one version) before starting, or
comment on the existing one. Record the program ID, game version, device, GPU
and driver. If someone else is already working on the same game or on the same
renderer area, coordinate in that issue first.

## 2. Identify the build

The library screen reads the package metadata and shows why a game cannot
start. To work on an unknown build you need its program ID, version and module
build IDs. `tools/check_package_metadata.py` and the preparation library report
them without running guest code.

## 3. Add a draft adapter

Copy the smallest existing adapter in `native/adapters/` and fill in only what
you measured from your own package: program ID, version, module names, build-ID
prefixes, startup order and NVN interface version. Do not copy addresses or
data from a game into the repository beyond these identifiers. Add the title to
`tests/title_adapter_test.c`.

## 4. Run it and find the first failure

Build a debug APK ([building](BUILDING.md)), prepare your package and start it.
The first fatal diagnostic usually names an unimplemented service command, an
unsupported NVN call or an untranslated shader instruction. Do not keep going
past a failure by returning fake success: that hides the real problem and
causes harder bugs later.

Useful diagnostics are off by default and listed in [diagnostics](DIAGNOSTICS.md).
They print metadata (handles, hashes, sizes, formats), never pixels or game data.

## 5. Compare with references

When behavior is unclear, compare with public references before guessing:
- Horizon service and NVN behavior documented by switchbrew and implemented by
  open-source emulators and recomp projects (see `sources.lock.json`, and use
  only code with compatible licenses).
- Another supported title that exercises the same API.

Record which reference you used and at which commit in the pull request.

## 6. Make the smallest shared fix

- Implement the missing behavior generally, with its documented validation.
- If a title truly needs different behavior, add an adapter policy flag and
  explain the evidence. Do not branch on game names in shared code.
- Add a synthetic host test that fails before your change and passes after it.
  Tests must not need game files; build inputs from the API contract instead.

## 7. Validate and report limits

- Run the host check set ([building](BUILDING.md#host-checks)).
- Retest your game and at least one already-supported title on device, or say
  explicitly that you could not.
- Report the result with the states in [compatibility](COMPATIBILITY.md):
  reaching a title screen is "boots" or "renders", not "playable".

## What to leave out of issues and pull requests

Game files, keys, firmware, saves, extracted assets or shaders, raw memory
dumps, screenshots that show device identifiers, logs with private paths or
serial numbers. Describe the trigger (game, version, scene, steps) so another
contributor with their own copy can reproduce it.
