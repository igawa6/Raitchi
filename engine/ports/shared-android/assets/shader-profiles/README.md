This optional catalog contains exact game/version shader recipe metadata only.
No game binaries, shader payloads, keys or saves belong here. The catalog
contains two exact verified game/version catalogs:

- Echoes of Wisdom 1.0.0: 475 observed recipes covering recorded startup and
  Castle Town save loading. All resolve from original sources. A bounded Thor
  run prepared them in 22.82 seconds; this is not whole-game coverage.
- Link's Awakening 1.0.0: 1,803 recipes combining the 1,585 upstream records
  with 221 locally observed records (three exact duplicates). All reconstruct
  from the original main NSO and logical NSP RomFS. No whole-game completeness
  or device preparation time is implied. Upstream records do not encode sampled
  texture integer masks: these import the explicit `tex_uint=0` specialization.
  Actual draws with different keys retain ordinary compilation.

The LAS importer derives fields from the production renderer and uses the
upstream parser/formatter under stricter bounds and canonical validation.
Upstream list: LAS Recomp Android Plus, commit
`cca791a3e7218e1dac95e7d4a11b8f97dba3ea51`,
`runtime/nvn/shader_variants.txt`; MIT attribution is preserved in the app's
LAS Recomp notices. The catalog contains hashes and state only, not that game's
original shader code.

Export a locally learned profile with `tools/shader_profile_tool.py export`, then
name it `<game_version_identity>.bin`. Add an explicit manifest entry containing
`game_version_identity` (64 lowercase hex), `file`, `sha256`, and `records` from
the tool's inspect output. Packaging validates the actual metadata codec,
identity, size, digest and record count and rejects any unlisted files. These
profiles describe observed recipes, not complete coverage of every game scene.

The runtime loads only the selected exact identity and resolves original shader
inputs from the owner's game. Its atomic metadata union retains unresolved
recipes for later preparation alongside locally learned recipes.
