# Wrist AI Agent Handoff

This file is the current entry point for future coding agents.

## Current release

- Target version: `1.5.0`
- App UUID: `1c4a7728-ee62-431f-a941-e40fafab98af`
- Supported targets: Basalt, Chalk, Diorite, Emery and Flint
- Canonical plan: `V1.5.0_PLAN.md`
- Latest audit: `V1.5.0_AUDIT.md`
- Proposed watch redesign: `WATCH_UI_PLAN.md`

The Git branch is still named `v1.4.0`; trust `package.json` and the audit documents for the release version.

## Canonical files

- `src/c/mdbl.c`: watch UI, Health data, Wakeup, TTS and AppMessage.
- `src/pkjs/pebble-js-app.js`: the only supported PebbleKit JS entry point.
- `config/index.html`: hosted Config source.
- `package.json`: UUID, version, target platforms and append-only message-key order.
- `wscript`: forces the traditional single-file PKJS bundle required by iOS Core Devices.

Do not restore `src/pkjs/index.js`. The root bundle script must remain named `pebble-js-app.js`, otherwise the iOS companion app can remain on “Loading watchapp”.

`wscript` deliberately vendors `opencc-js/dist/umd/t2cn.js` into the generated
root script while keeping `enableMultiJS=false`. Do not return to copying the
source entry directly: a bare `require('opencc-js/t2cn')` makes iOS PebbleKit JS
fail before Settings is registered. Always inspect and execute the script from
the final PBW, not only `src/pkjs/pebble-js-app.js`.

## Invariants

- Never change the UUID.
- Append message keys only; reordering existing keys breaks the C/JS protocol.
- Message keys are append-only. `CONTROLS_VISIBLE` is currently last (`10041`);
  `THEME_COLOR` uses `-1` for Random and `0..6` for fixed themes.
- Do not claim Timeline synchronization succeeded. The local insert API has no completion callback.
- Timeline pins created by Wrist AI can be removed with the Core Devices
  `Pebble.deleteTimelinePin(id)` bridge; like insertion, it has no completion callback.
- Core Devices does not expose a local Timeline list/read API to PKJS. Only
  Wrist AI pins saved in `timeline_launch_map` can be matched, updated or
  removed; arbitrary watch Timeline pins cannot be enumerated.
- Without Todoist, ToDo & Notes are canonical phone-side objects and Timeline/Wakeup are
  projections. With Todoist enabled, task IDs link Todoist to those local projections.
- Ordinary reminders use Timeline Reminder. Only explicit strong/persistent vibration uses Wakeup.
- Keep visible replies at or below 1,800 UTF-8 bytes.
- Chats remain 20 × 20 messages. Active ToDo & Notes and completed Archive
  each retain up to 50; the watch snapshot contains 20 active + 10 archived.
  Wakeup allocation is seven strong-reminder slots plus one rolling Todoist sync slot.
- Health and location are opt-in and must never be persisted into normal chat history.
- Memory.md must not store secrets, authentication data, financial identifiers or exact health samples.
- Stable personal facts explicitly intended for long-term memory are Memory-only
  and must never create a shadow Note/TODO. Config edits use one
  `- key: stable fact` item per line and retain the 40-item / 12KB limits.

## TTS safety baseline

The menu remains 10%–100%, but logical 100% maps to hardware 50%.

- Emery ring: 35,840 bytes, heap allocated
- Flint ring: 12,288 bytes, heap allocated
- Decode buffer: 1,600 bytes
- Playback pump: 100 ms
- Emery high quality: 16 kHz μ-law transport, native 16-bit playback
- Emery JS chunks: 3,000 bytes / 130 ms; pause/resume: 27,000 / 21,000
- Flint JS chunks: 1,200 bytes / 90 ms; pause/resume: 8,500 / 6,500

Emery mastering uses a smoothed envelope gate/compressor, mild high-frequency
smoothing and six-millisecond sentence fades. Do not casually retune these
values. Flint has not been validated on physical Duo hardware.

Todoist sync runs on launch, after local mutations, every five minutes while
the watch app remains open, and through optional background Wakeups. The
watch-side foreground timer must defer while dictation, LLM work, TTS or a
pending Note command owns the AppMessage path.

## Config constraints

The app opens `https://deusaw.github.io/Pebble-Wrist-AI/config/` with a cache-busting version parameter. Editing `config/index.html` locally does not update users until the hosted page is deployed.

Keep existing form element IDs and the `webviewclosed` response schema stable. iOS Config has previously failed because of PKJS loading, cache and URL-size issues.

Config conversation data is passed in the URL hash, with a 500,000-character safety limit. Export explicitly creates UTF-8 bytes with BOM and uses a Base64 fallback for old WebViews.

Config also receives bounded `notes_data` in the hash. It returns only dirty
`note_updates` and `deleted_notes`. Preserve this delta protocol. Theme is
returned as `theme_color`.

## Build and checks

```sh
node --check src/pkjs/pebble-js-app.js
git diff --check
uv tool run --from pebble-tool pebble build
shasum -a 256 build/pebble-app.pbw
```

Before release, also verify:

- Config inline JavaScript parses and all static IDs are unique.
- Notes multi-create, complete, reopen, delete and conversation restoration.
- Direct named TODO fallback, Config Note deltas, and Random/fixed theme synchronization.
- Timeline all-day, relative-time, multiple-event and linked-TODO behavior.
- Todoist create/update/complete/reopen/delete, incremental import, due add/remove,
  Timeline deletion, failed-create retention, and Config save payload.
- Chinese export in the real iOS Pebble App.
- Basalt, Chalk and Diorite display; Emery TTS; Flint only when hardware becomes available.

## Working-tree warning

The current worktree contains a large uncommitted v1.4.x change set, including the deliberate deletion of `src/pkjs/index.js` and addition of `src/pkjs/pebble-js-app.js`. Preserve unrelated user changes and inspect `git status` before editing.
