# Raitchi 0.0.1 bounded launch results

58/58 entries completed in the current snapshot. This document separates current launch-only observations from earlier saved-game evidence.

No current sweep title is marked playable solely from presentation or FPS. Earlier known-title initial scenes are limited historical gameplay evidence. Identical shared runtime bytes do not imply identical compatibility. Unknown package IDs were tracked by catalog index rather than merged as zero.

| Game | Program ID | Current launch check | Earlier gameplay evidence |
|---|---|---|---|
| The Legend of Zelda: Echoes of Wisdom | 01008CF01BAAC000 | Main screen | Playable in earlier tested initial scenes; incomplete compatibility |
| The Legend of Zelda: Link's Awakening | 01006BB00C6F0000 | Intro/demo | Playable in earlier tested initial scenes; incomplete compatibility |
| Super Mario Bros. Wonder | 010015100B514000 | Main screen | Playable in earlier tested initial scenes; incomplete compatibility |
| Pokémon: Let's Go, Eevee! | 0100187003A36000 | Controller selection | Playable in earlier tested initial scenes; incomplete compatibility |
| 13 Sentinels: Aegis Rim | 01003FC01670C000 | Main screen | No verified gameplay |
| AI: THE SOMNIUM FILES - nirvanA Initiative | Unparsed | Blocked before launch | No verified gameplay |
| Animal Crossing: New Horizons | 01006F8002326000 | Runtime error dialog | No verified gameplay |
| ASTRAL CHAIN | 01007300020FA000 | Black screen | No verified gameplay |
| CAPTAIN TSUBASA: RISE OF NEW CHAMPIONS | 0100EAE010560000 | Black screen | No verified gameplay |
| Castlevania Belmont's Curse | Unparsed | Blocked before launch | No verified gameplay |
| Chained Echoes | 0100C510166F0000 | Returned to launcher | No verified gameplay |
| Crash™ Team Racing Nitro-Fueled | 0100F9F00C696000 | Runtime error dialog | No verified gameplay |
| Diablo III: Eternal Collection | 01001B300B9BE000 | Startup notice/logo | No verified gameplay |
| Dispatch | 01008BA02525A000 | Exited before first frame (candidate57) | No verified gameplay |
| DRAGON BALL: Sparking! ZERO | 010035F022078000 | Runtime error dialog | No verified gameplay |
| DRAGON QUEST III HD-2D Remake | 01003E601E324000 | Closed before visible progress | No verified gameplay |
| DRAGON QUEST VII Reimagined | 0100A9D01C446000 | Exited before first frame (candidate57) | No verified gameplay |
| DRAGON QUEST XI S: Echoes of an Elusive Age – Definitive Edition | 01006C300E9F0000 | Returned to launcher | No verified gameplay |
| FANTASY LIFE i: The Girl Who Steals Time | 0100755017EE0000 | Runtime error dialog | No verified gameplay |
| Farming Simulator Nintendo Switch Edition | 0100D04004176000 | Blocked before launch | No verified gameplay |
| Fire Emblem: Three Houses | 010055D009F78000 | Intro/demo | Prior class-demo/battle evidence; not a playable-title qualification |
| Ghost Trick: Phantom Detective | 010029B018432000 | Blocked before launch | No verified gameplay |
| Guacamelee! 2 | 01007E100456C000 | Startup notice/logo | No verified gameplay |
| Hades | 0100535012974000 | Runtime error dialog | No verified gameplay |
| Kirby and the Forgotten Land | 01004D300C5AE000 | Menu/setup | No verified gameplay |
| Mario Kart 8 Deluxe | 0100152000022000 | Blocked before launch | No verified gameplay |
| MARVEL Cosmic Invasion | 010059D020C26000 | Runtime error dialog | No verified gameplay |
| Master Detective Archives: RAIN CODE | 0100149019460000 | Returned to launcher | No verified gameplay |
| Mega Man 11 | 0100B0C0086B0000 | Returned to launcher | No verified gameplay |
| Mega Man Legacy Collection | 01002D4007AE0000 | Returned to launcher | No verified gameplay |
| Mega Man Legacy Collection 2 | 0100842008EC4000 | Blocked before launch | No verified gameplay |
| Mega Man X Legacy Collection | 01005C60086BE000 | Runtime error dialog | No verified gameplay |
| Mega Man X Legacy Collection 2 | 01005250086C4000 | Runtime error dialog | No verified gameplay |
| Metroid Dread | 010093801237C000 | Blocked before launch | No verified gameplay |
| Mina the Hollower | 0100DCD01F306000 | Runtime error before first frame (candidate57) | No verified gameplay |
| No More Heroes 3 | 01007C600EB42000 | Black screen | No verified gameplay |
| ONE PIECE ODYSSEY Deluxe Edition | 0100D9601A994000 | Black screen | No verified gameplay |
| ONE PIECE: PIRATE WARRIORS 4 | 0100BA400E2F4000 | Menu/setup | No verified gameplay |
| Ori and the Blind Forest: Definitive Edition | 010061D00DB74000 | Runtime error dialog | No verified gameplay |
| Ori and the Will of the Wisps | 01008DD013200000 | Runtime error dialog | No verified gameplay |
| Persona 5 Royal | 01005CA01580E000 | Runtime error dialog | No verified gameplay |
| Plants vs. Zombies: Replanted for Nintendo Switch™ | 01008C50239BE000 | Runtime error dialog | No verified gameplay |
| Pokémon Brilliant Diamond | 0100000011D90000 | Runtime error dialog | No verified gameplay |
| Pokémon Shining Pearl | 010018E011D92000 | Runtime error dialog | No verified gameplay |
| Pokémon Sword | 0100ABF008968000 | Runtime error dialog | No verified gameplay |
| STEINS;GATE ELITE | 010040000C596000 | Returned to launcher | No verified gameplay |
| STORY OF SEASONS: Friends of Mineral Town | 0100ED400EEC2000 | Blocked before launch | No verified gameplay |
| SUPER MARIO ODYSSEY | 0100000000010000 | Black screen | No verified gameplay |
| The Binding of Isaac: Afterbirth+ | 010021C000B6A000 | Blocked before launch | No verified gameplay |
| The Great Ace Attorney Chronicles | 010036E00FB20000 | Runtime error dialog | No verified gameplay |
| The House in Fata Morgana - Dreams of the Revenants Edition | 010016101100A000 | Runtime error dialog | No verified gameplay |
| The Legend of Zelda: Breath of the Wild | 01007EF00011E000 | Blocked before launch | No verified gameplay |
| Tunic | 0100DA801624E000 | Returned to launcher | No verified gameplay |
| Unavowed | 0100C18014DC0000 | Runtime error dialog | No verified gameplay |
| Xenoblade Chronicles Definitive Edition | 0100FF500E34A000 | Runtime error dialog | No verified gameplay |
| Ys IX: Monstrum Nox | 0100E390124D8000 | Menu/setup | No verified gameplay |
| ŌKAMI HD | 0100276009872000 | Blank screen | No verified gameplay |
| 夜、灯す | 0100C2901153C000 | Blocked before launch | No verified gameplay |

Some native failures moved beyond older loader/recorder boundaries. Those changes are not automatically attributed to the Fire Emblem fix. New visible startup is not a gameplay pass, and a launch-only retest does not exercise earlier input-triggered failures.

## Build and regression checks

The complete library sweep used candidate56: 58 entries, 47 runtime trials and
11 package-preparation blockers. Candidate57 added eight targeted checks, not
another full-library sweep. All were fresh-process launches without gameplay
input:

| Game | Candidate57 observation |
|---|---|
| Fire Emblem: Three Houses | Colored automatic class demo, about 30 fps |
| Pokémon: Let's Go, Eevee! | Controller selection, about 30 fps |
| Super Mario Bros. Wonder | Title screen, about 60 fps |
| The Legend of Zelda: Echoes of Wisdom | Title screen, about 60 fps |
| The Legend of Zelda: Link's Awakening | Beach intro, about 30 fps |
| Dispatch | Passes the missing GetInfo28 boundary; exits before rendering |
| Dragon Quest VII Reimagined | Passes GetInfo28; guest virtual-address reservation fails before rendering |
| Mina the Hollower | Passes GetInfo28; later guest break before rendering |

68 host checks passed, including focused reruns after three stale-check repairs.
21 Android renderer checks, debug/release lint and four Android shell unit tests
passed. Optimized and sanitizer checks cover queue scheduling, allocation/thread
startup failure injection, GetInfo ABI and directory/settings regressions.
All 49 save files were byte-identical after both test rounds; the original
settings and selected driver preferences were restored.

The prepared source's 457 shell build inputs match the APK receipt. The source
overlay was restored on its actual upstream base with matching hashes/ancestry.
A complete fresh-checkout native/APK build remains unverified.

## Interpretation and remaining limits

Earlier saved-game movement in four titles qualifies only those tested initial
scenes. First-use shader stalls, unsupported services and rendering work remain.
Music recovery in Fire Emblem/Wonder is unconfirmed; residual graphics artifacts
remain possible. Boot menus, notices, demos and their FPS counters do not establish
whole-game playability or performance. Black-screen and failed entries remain in
this full report even though they are omitted from the README's booted-game table.
