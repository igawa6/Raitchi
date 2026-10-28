# Direct package loading with NCE

Status: 2026-10-26. Raitchi can prepare a supported base NSP/XCI in its Android launcher without generating game C or extracting the RomFS tree. Real SAF selection and ExeFS-only preparation have passed on Thor for Echoes of Wisdom. The package-backed runtime mounts and indexes the original RomFS, renders the title movie, loads the existing save and responds to movement on Thor.

## Preparation and launch

In Raitchi, choose **Add game → the supported title → Game file**, select the base NSP/XCI, then select your **prod.keys**. Keep Raitchi open until it reports **ready to play**, open that game's card and press **Play**. This path does not require the C compilation step. Keep the selected package available; moving/deleting it or revoking its document permission requires selecting it again.

Android retains permission to the game document. A standalone `libraitchiprepare.so` reads the package, verifies the selected title and original module builds, validates NSO/LZ4 images against the actual adapter, and prepares only ExeFS plus `native.manifest`. It runs no guest code and loads no renderer.

Preparation publishes an immutable app-private generation and changes its selection atomically. Existing extracted games, compiled libraries and saves remain intact. The tested Echoes generation contains eight files and occupies approximately 39.3 MiB; it contains no RomFS directory, raw RomFS image, generated C or game DSO. Its original-module manifest is byte-identical to the established native preparation tool's output.

**Play** starts a fresh game process using continuous NCE and original-module metadata. That process reopens the selected document through Android's content resolver, verifies its program and prepared ExeFS identity, and reads RomFS through the package provider. Credentials and native/AOT selectors are replaced explicitly when changing titles. Temporary research flags are removed from ordinary launcher sessions.

The shared package reader performs bounded AES-XTS/CTR and BucketTree/LZ4 reads on demand. Keys stay in memory during package reading; the launcher retains the owner's imported keys privately. No decrypted-key `romfs.src` file is written. Each movie stream owns a separate reader, file descriptor, compression metadata and cache, allowing FFmpeg to seek and decode directly from encrypted/compressed package storage.

## Supported scope

Recognition and native metadata policies currently cover the pinned base versions of Link's Awakening, Let's Go Eevee and Echoes of Wisdom. Recognition is not a claim that every title's direct-package gameplay is device-qualified. Echoes uses NCE; older LAS/Eevee folder and compiled-code paths remain available.

NSZ/NCZ, patch/delta layouts, updates and packages containing multiple program candidates reject explicitly. Selected documents must support seeking and remain accessible. The provider does not verify Nintendo signatures or the complete NCA integrity hierarchy. Original NSO hashes and adapter build policies are independently checked.

The picker imports only `prod.keys`, which must contain the keys required by the package's generation. A rights-managed NSP also needs its matching embedded ticket. Importing a separate `title.keys` file is not available in this UI; placing title-key entries in the selected `prod.keys` does not replace that requirement. Packages using key-area encryption do not require a ticket.

## Evidence and remaining checks

Host and Android compilation checks cover encryption vectors, bounded package tables, LZ4/BucketTree, concurrent reads, independently owned clones, actual native manifest/NSO preparation, atomic publication, title/session switching and filesystem retries. Actual Echoes package checks matched all six ExeFS hashes and sampled resources against the prior extracted tree.

The real title movie's demux, seeking and first-frame decoding matched the existing extracted reference and raw-image AVIO through the actual FFmpeg callbacks, including after parent readers closed. Thor subsequently rendered the title movie through the package-backed path.

Device acceptance has passed package-backed title video, save selection and initial Suthorn Ruins gameplay at an observed 30 fps. Movement, bomb use, the first door, jumping and stairs responded. A new manual save produced a colored thumbnail and committed three individually hash-verified files; that save subsequently loaded through the real menu in a fresh process and movement responded. Two ordinary launches with the same runtime and environment passed new-cache preparation followed by replay, manual-save loading and steady gameplay. Broader progression and repeated-session reliability remain unqualified. Wider graphics and audio accuracy remain separate from successful package loading. Original CPU execution still requires GPU shader translation and pipeline compilation.

Implementation retains the pinned EoWRecomp MIT license in `native/runtime/LICENSE-EoWRecomp`, included in the APK. Game packages, keys, saves, decrypted programs and detailed local evidence remain unpublished.

Wonder base 1.0.0 has an experimental native adapter. Its measured bootstrap
requires the NVN 55.15 interface version and uses the existing opaque 64-byte
device builder and 0x3000-byte device allocation. The adapter declares that ABI
version; the other adapters retain 53.207. This is a bounded compatibility
experiment, not qualification of every NVN 55 procedure. Unsupported actual API
invocations retain fatal diagnostics. Initial loading and version negotiation
do not establish Wonder gameplay, and historical captures from a different
Wonder update do not qualify this base build.
