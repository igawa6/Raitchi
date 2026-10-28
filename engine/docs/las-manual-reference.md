# LAS manual gameplay reference

The owner controls the AYN Thor during this reference. The active marker is
`evidence/manual-session.json`; it blocks scripted input and timed scenario runs.
Do not stop, reinstall or update the test app during owner play. Read-only log
collection is allowed. No automatic session cutoff is configured.

## Current evidence, 2026-10-24

- Stock Adreno 740 driver reached file selection with invisible menu glyphs.
  The same input fixture with the pinned bundled Turnip rendered readable text.
- The isolated test app created a new File 1, Link, Normal, and a subsequent
  fresh process loaded it into the opening house with readable dialogue.
- In a combined scripted/owner-assisted run, the owner completed dialogue and
  approached/exited the house. Logs show movement, a level-jump request and a
  committed autosave. These are not yet proof that the outdoor scene rendered.
- The process was alive at the 100.14-second checkpoint. The test harness then
  stopped it in cleanup. That interruption was a harness action, not evidence
  of a game crash. Owner-controlled sessions must never use that cutoff.
- An uncapped, unscripted manual session was opened with the verified Turnip
  selection. The latest read-only observation found the process alive at its
  frontend; gameplay still requires selecting Play and the saved file.
- The owner subsequently reported: "its running fine". Record this as a positive
  manual gameplay report; it does not separately confirm every route, audio,
  save-persistence or sustained-performance check below. Continue host-side
  Let's Go bring-up without interrupting the manual session.
- On 2026-10-25 local time, the owner explicitly finished this session. Its
  marker was ended before installing/launching the separate Let's Go test app.
  Future owner-controlled sessions retain the same protection rule.

## Short owner route

1. Select Play and resume the latest Autosave, or File 1 if needed.
2. Leave the opening house and move around the village for a few minutes.
3. Open and close an in-game menu. Check readable text, controller response,
   music and sound effects.
4. Save when available. At a convenient stopping point, exit and reopen through
   the app normally, then load the same save and check progress persists.

Report the furthest location reached, whether Thor controls and audio worked,
and any visible issue. A freeze/crash report should include the action just
before it. The coordinator correlates reports with logs; audio and control feel
need owner observation. No automated input should overlap this route.

## Remaining engineering gates

This section described the initial 2026-10-24 reference gate. Shared loading and
both on-device builds subsequently executed identical runtime bytes on Thor.
The shared LAS app passed a short Mabe Village controller/menu/save/reload and
home/sleep-resume route on 2026-10-25; see [current progress](progress.md).
Longer gameplay, audible output, later scenes and matched efficiency remain
separate qualification work. The original reference installation and saves remain preserved.
