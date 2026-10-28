# Generic native loading

The experimental generic adapter prepares original NSO modules and runs them
through continuous NCE. It does not generate game C, load a game DSO or establish
game compatibility merely by accepting a package. Original CPU execution still
uses the shared kernel/services and NVN-to-Vulkan renderer.

## ELF metadata

The common loader supports the existing AArch64 RELA path and ELF64 RELR
relative relocations. RELR address entries and 63-bit bitmaps are decoded over
immutable initialized metadata. Implicit addends are frozen from the original
owned image before any relocation writes. Counts, table backing, target
alignment/ranges, initialized/BSS ownership, arithmetic and destination overlaps
are checked before committing any module to guest memory. Unsupported REL and
packed formats still stop with the exact dynamic tag, value and entry offset.

SysV hash metadata retains its existing symbol-count policy. When DT_HASH is
absent, bounded GNU ELF64 hash metadata supplies the dynamic-symbol count.
Header/bloom/bucket dimensions, initialized bounds, symbol capacity and chain
termination are validated. Duplicate conflicting GNU pointers reject. Symbol
names, relocation indices and strong imports keep their existing validation;
a missing API is not replaced with a success stub.

## NCE placement and preparation migration

Generic layouts reserve an additional owned 16 MiB below the main module.
Every game module and fixed-region address remains unchanged. NCE can place a
bank before an image or use the existing following gaps. The full bank must be
inside the owned reservation, exclude every image/region/prior bank, and pass
all signed B26 endpoint checks from the complete text interval. Validation
failures precede guest writes or protection changes; protection/publication
failures retain rollback.

The generic selection token `lead-in-bank-v2` prevents old prepared metadata
from being reused with the changed reservation. Legacy selections retain their
source, program/version and save identity. The next package preparation uses a
fresh UUID generation and atomically publishes its selection. Old prepared
NSOs/manifests and saves are preserved. The four specific adapters keep their
existing selection format and layouts.

## Verification

The format and layout checks use synthetic fixtures:

```sh
python3 tools/check_shared_loader.py
python3 tools/check_continuous_image.py
python3 tools/check_generic_adapter.py
python3 tools/check_native_package_ui.py
python3 tools/check_raitchi_shell_bridge.py
python3 tools/check_raitchi_settings.py
```

Host optimized/sanitized checks and Android compilation/linking establish
component behavior. Separate device trials are required to establish original
module execution, rendering, controls and gameplay. A live process, a cleared
loader rejection or a title/menu frame is insufficient for gameplay acceptance.
