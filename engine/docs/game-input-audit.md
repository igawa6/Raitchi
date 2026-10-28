# Imported-input static audit

The private baseline imports now provide concrete module and symbol evidence. This audit reads NSO headers, dynamic symbol tables and relocations; it does not execute or compile game code. Exact module IDs, hashes, layouts, dependency names, symbols and candidate providers remain only in ignored `evidence/game-input-audit.json`.

Reproduce with `python3 tools/audit_game_inputs.py`. The utility uses the unchanged pinned LAS Android NSO parser, Python LZ4 when available, and a system liblz4 fallback otherwise. It preserves all 32 build-ID header bytes, including trailing zeroes. Its source commits match the [runtime audit](runtime-audit.md). Output is restricted to the ignored evidence directory.

| Selected runtime profile | Inventoried NSOs | Selected NSOs | Relocation-referenced import entries | Literal HLE candidates | Guest-export candidates | Weak undefined → zero | Unaccounted strong imports |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| LAS Android | 5 | 2 | 756 | 68 | 671 | 17 | 0 |
| Let’s Go | 5 | 4 | 1581 | 74 | 1490 | 17 | 0 |

Counts are imported symbol-table entries, summed over selected modules, not unique names or dynamic calls. Every counted import has at least one supported symbol relocation. “Candidate” means source-level resolution under the corresponding profile, following host-HLE preference and then selected guest definitions. Compilation conditions, retained linker registrations, generated function coverage, and implementation correctness still need verification.

Both imports contain main, SDK, two subsdk modules and rtld. LAS’s pinned runtime selects main/SDK; Let’s Go selects all four non-rtld modules. The excluded files were inventoried without assuming they should be loaded. Both source runtimes replace rtld startup duties. The actual selected images fit their declared canonical slots without overlap and end before the HLE region. Subtracting the proposed Android shift preserves those arithmetic properties; successful Android address reservation remains untested by this audit.

All inventoried relocations use the four types already handled by the pinned loaders. This establishes relocation-format coverage for these inputs, not parser hardening or successful runtime relocation.

The port priorities are now narrower:

1. **Preserve module and constructor ordering.** Eight Let’s Go main imports resolve exclusively to subsdk0 under its source profile; a two-module table cannot supply those definitions. Its guest SDK and subsdk modules also import main exports, confirming that all modules must be loaded before relocation. Subsdk1 remains selected and initialized by the source runtime even though main’s static candidate resolution does not require one of its guest exports; that absence does not establish safe omission.
2. **Review 19 changed Let’s Go import resolutions.** Against the Android source registry, 17 entries fall back to guest definitions where Let’s Go has explicit host overrides: 14 input-related, one display-related and two movie-related. Two exception-related entries change in the other direction. Eighteen changes occur in main and one in subsdk1. These are concrete semantic-review priorities, not 19 proven unsupported functions: every entry still has a selected guest provider.
3. **Preserve dynamic registration evidence.** The scanner recognizes the source’s `NVN(name)` registration macro separately from literal HLE declarations. None of these static import entries needs an NVN macro match, but NVN procedures are also obtained by name at runtime. Static closure therefore cannot establish NVN request coverage, service command coverage, motion behavior or movie correctness. Record actual bootstrap/proc requests and HLE calls during baseline sessions.
4. **Make the manifest cover the complete selected module set.** Retain complete header IDs, file hashes, segment/BSS sizes, relocation types and defined-export providers per module. Verify the shifted generated tables and host callbacks against that manifest. A main-only prefix match is insufficient for interchangeable runtime artifacts.

The existing acceptance gate can now require zero unexpected strong static imports and the observed two/four-module inventories for these exact private inputs. This evidence does not close the remaining ABI, Android loading, graphics, gameplay, save-isolation or identical-runtime-artifact gates.
