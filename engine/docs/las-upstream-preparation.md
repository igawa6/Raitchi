# LAS upstream shader preparation

The optional APK catalog includes 1,803 exact metadata recipes for the verified
Link's Awakening 1.0.0 original-module identity. It combines the 1,585 records
in pinned LAS Recomp Android Plus `runtime/nvn/shader_variants.txt` with 221
locally observed records. Three exact duplicates are removed. The catalog is
925,059 bytes before APK compression (about 104 KB compressed).

This is the complete imported list, not proof of complete-game coverage. The
upstream tour contains 1,569 graphics records and 16 compute records. Its
sampled-texture integer masks are unspecified. Imports use the explicit
`tex_uint=0` specialization used by upstream preparation; actual draws with
different complete keys retain ordinary compilation. Observed metadata remains
in the union even when it shares shader hashes with an upstream record.

`tools/import_shader_variants.py` reads upstream text, original sources, a
verified game identity, and optional observed metadata. It generates its parser
and key mapping from the pinned production parser/formatter and current
`renderer_prepare` implementation. Strict bounds and canonical round trips
reject ignored, malformed, overflowing, duplicated-field and embedded-NUL
input. Derived vertex-output masks remain zero in metadata; replay derives them
from current translation. Original code and constants are never written by the
tool. Existing valid output metadata is preserved through an atomic union.

Example with an extracted original source:

```sh
python3 tools/import_shader_variants.py \
  --variants sources/las-android/runtime/nvn/shader_variants.txt \
  --game-id VERIFIED_GAME_ID \
  --main-nso ORIGINAL/exefs/main --romfs ORIGINAL/romfs \
  --observed-profile OBSERVED_PROFILE --output OUTPUT_PROFILE
```

For an original NSP, replace `--romfs` with `--package ORIGINAL_NSP --keys KEYS`.
Game identity must come from verified immutable module metadata, not the title
name or an arbitrary hash. The tool requires exact stage/code/constants hashes
and partitions, reconstructs the complete input union through the production
profile merger, then verifies a warm merge performs zero resolver reads.
The existing catalog packager separately validates identity, count and digest.

Host validation reconstructed all 1,803 recipes from the logical NSP reader and
verified original main NSO, with zero missing/rejected entries. It indexed
8,796 stage entries; a warm merge reused all 1,803 recipes without source reads.
This proves source resolution and metadata validity. It does not prove shader
translation, Vulkan pipeline creation, gameplay coverage or device load time.

Candidate56 was subsequently installed and checked on Thor with the selected
Balemuni driver. A completely empty derived cache and metadata store prepared
the 1,803 recipes in 71.173 seconds: 3.137 seconds for profile/source work and
68.036 seconds for replay. It built 1,061 programs, 1,505 graphics pipelines and
16 compute pipelines, with no reported failures or skipped recipes. Reusing the
same diagnostic cache took 4.933 seconds and required no source scan or new
portable jobs. Vulkan objects are still created from cached data; this is not a
claim that the driver performs zero work.

Cold and warm Mysterious Forest save/movement checks were visually verified at
30 fps with zero compilation in the sampled settled gameplay. They do not prove
all areas work or that asset/CPU stalls are eliminated. The first cold log rolled
out of Android's buffer; the exact cold figure above comes from a fully captured
preparation-only repeat, not an estimate from the truncated log.

The normal owner environment was also prepared and restarted with byte-identical
environment files and no forced Mesa/diagnostic flags. Initial normal preparation
took 64.303 seconds and the subsequent launch 5.059 seconds. The prepared cache
was retained for ordinary play, with the previous cache kept as a private backup.
It occupies approximately 441.5 MiB including retained earlier cache entries.
Driver preferences were restored exactly and all 49 save files were unchanged.

Echoes already uses the same bounded source scanner and preparation machinery.
Its actual shader hashes, state and specializations differ: the LAS list and
current Echoes catalog have only one matching vertex/fragment hash pair, which
alone does not prove equivalent bytes and rendering inputs. Echoes needs its
own recipe inventory. Recomp runtime ancestry is not proof of identical original
game-engine revisions.

A separate original-source trial found that five of the 1,585 LAS records resolve
against Echoes. Their vertex and fragment code/constants are byte-identical
between the two games, but the pair and all compilation specializations already
exist in Echoes' 475-record catalog. Five extra rendering-state candidates form
an Echoes-scoped 480-record trial. On Thor this built four additional graphics
pipelines and no new programs, then passed a Castle Town/rift save and movement
check at 30 fps. Actual draw use of the new states and a hitch benefit remain
unproven. The experiment used a separate explicit profile; the bundled Echoes
catalog remains 475 records. Most LAS recipes cannot be applied to Echoes.

The importer gate runs optimized and with ASan/UBSan:

```sh
python3 tools/check_import_shader_variants.py
```

No CPU runtime changes are required for this catalog addition. Continuous NCE
and existing renderer defaults remain unchanged. First preparation still pays
source indexing and pipeline compilation; Continue can release the existing
startup wait while preparation proceeds with the bounded gameplay worker pool.
