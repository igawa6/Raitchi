# Compatibility reporting

Compatibility is recorded per game version and per runtime build, as tested.
Records live in `compatibility/reports/*.json` and follow
`compatibility/schema.json`. `tools/check_compatibility.py` validates them and
renders the table below.

## States

| State | Meaning |
|---|---|
| `untested` | No report for this game version and a current build |
| `fails-startup` | Rejected during preparation, or stops before the first frame |
| `boots` | Guest code runs past startup; nothing or nothing correct is presented |
| `renders` | Menus or scenes are presented; input or progression not yet confirmed |
| `interactive` | Responds to input in the tested section |
| `playable-tested-section` | The tested route can be played at usable speed with the listed limitations |
| `completed` | The game was finished on this build; list the limitations that remain |

A state applies only to the game version, app/runtime build, device and route
in the report. Do not convert a state into a percentage, and do not promote an
opening or title screen to the whole game.

## Required fields

Game: title, program ID, game version, update/DLC (base only is supported today).
Build: app version, runtime `libmain.so` packaged SHA-256, source commit.
Device: model, SoC/GPU, GPU driver and version, Android version, relevant
settings (resolution, driver, diagnostics).
Test: date, tester, route (what was done), observed frame rate if measured,
session length, whether a save was loaded or created.
Result: state, known limitations, links to issues.

Each record also says whether the result was **observed** in that run or
**carried over** from an older build. Carried-over results are shown as stale
until retested.

## Current table

Generated from `compatibility/reports/` by
`python3 tools/check_compatibility.py --table`. See
[the current table](../compatibility/README.md).
