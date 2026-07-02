# Wrist AI

> A flexible LLM client for Pebble smartwatches.

**Wrist AI** lets you talk to any Large Language Model directly from your Pebble. Speak your question, and the response appears on your wrist. Your phone acts as a silent bridge — no server, no cloud relay, no extra apps.

Works with OpenRouter (GPT, Claude, Gemini, Gemma, Llama, Qwen, and more) or your own custom API endpoint.

![Platforms](https://img.shields.io/badge/platform-Basalt%20%7C%20Chalk%20%7C%20Diorite%20%7C%20Emery%20%7C%20Flint-blue)
![License](https://img.shields.io/badge/license-CC%20BY--NC%204.0-green)

---

## Features

- **Voice Input** — Uses Pebble's built-in dictation. Press SELECT to speak, or optionally start listening automatically when Wrist AI opens.
- **Multi-Model Switching** — Configure up to 5 models and switch between them directly from your watch. No need to open settings.
- **Multi-Conversation** — Manage up to 20 concurrent chats. The LLM auto-generates a smart title for each conversation. Switch between them from the watch menu with full context preserved.
- **Custom API Support** — Use OpenRouter out of the box, or point the app to your own backend / local model server with a custom API URL.
- **Conversation Export** — Export any conversation as a structured JSON file (with messages, timestamps, and model info) from the config page.
- **Font Size & Bold** — Choose from three text sizes (Normal / Large / Extra Large) and toggle bold text for better readability on your wrist.
- **"Surprise Me" Toggle** — Long-press DOWN for a random prompt, or disable it in settings if you prefer to avoid accidental triggers.
- **Text-to-Speech (Emery; experimental on Pebble 2 Duo)** — Long-press DOWN on a reply to have it read aloud via Google Cloud TTS, with automatic Chinese/English voice detection. Emery uses 16 kHz μ-law transport, native 16-bit playback, gentle mastering and sentence de-clicking. Duo remains on the conservative 8 kHz path. *Beta.*
- **Multi-Day Health Context** — Optionally include 1, 3, or 7 days of steps, activity, distance, calories, heart rate, sleep, and deep sleep in the AI's context. Off by default. *Beta.*
- **ToDo & Notes** — Create up to five items in one request. Press DOWN from the home screen to complete, reopen, delete, or return to the conversation that created an item. Completed items move to a 50-item Archive.
- **Config ToDo & Notes Management** — View, edit, complete/reopen, and delete local ToDo & Notes from Config.
- **Todoist Two-Way Sync** — Untimed ToDo & Notes and timed Timeline events synchronize directly with Todoist. Wrist AI syncs on launch, every five minutes while open, after local mutations, and through optional background Wakeups.
- **Selectable Theme** — Keep themes random or fix Orange, Violet, Indigo, Cobalt, Oxford Blue, Red, or Deep Gray across Home, headers, and menu selection.
- **Strong Local Reminders** — Explicit strong-reminder requests use one of seven persistent watch Wakeup slots and a longer vibration pattern; the eighth Wakeup slot is reserved for optional Todoist synchronization.
- **Timeline Reminders** — Create, update, and remove timed reminders, date-only all-day events, and batches of local Pebble Timeline events through natural-language conversations and Todoist synchronization. Requires a compatible Core Devices companion app. *Beta.*
- **Location Context** — Optionally include your approximate GPS location (reverse-geocoded to city name) in the AI's context. Off by default. *Beta.*
- **Web Search** — Optionally let models search the web for up-to-date information (OpenRouter only, on by default). Links and source URLs are removed from watch replies and TTS. *Beta.*
- **Cross-Platform** — Supports all Pebble hardware including Pebble Time Round (Chalk) with fully optimized centered layouts.
- **Serverless & Private** — Your phone talks directly to the configured AI, TTS, and optional Todoist APIs. Conversations and integration mappings are stored locally in the Pebble companion app. No Wrist AI backend is required.
- **Organized Config Page** — Warm orange-and-purple settings cards group connection, AI behavior, watch experience, health/planning, TTS, conversations, and destructive actions.

---

## Compatibility

| Platform | Watches |
|----------|---------|
| **Basalt** | Pebble Time, Pebble Time Steel |
| **Chalk** | Pebble Time Round |
| **Diorite** | Pebble 2, Pebble 2 HR |
| **Emery** | Pebble Time 2 |
| **Flint** | Pebble 2 Duo |

---

## Setup

1. **Install** — Sideload the `.pbw` file via the Pebble app, or install from the Rebble Appstore.
2. **Configure** — Open app settings in the Pebble mobile app.
3. **API Key** — Get a key from [OpenRouter](https://openrouter.ai) and paste it in. Or switch to Custom API mode and enter your own endpoint URL.
4. **Add Models** — Add model identifiers (e.g., `google/gemma-3-27b-it`) to the model list.
5. **Start** — Launch the app on your watch and press SELECT to begin.

---

## Button Controls

| Button | State | Action |
|--------|-------|--------|
| **SELECT** (short) | Idle | Start voice dictation |
| **SELECT** (short) | Response | Return to main screen (ready for follow-up) |
| **SELECT** (short) | TTS playing (Emery/Flint) | Stop reading aloud |
| **SELECT** (long) | Any | Start a new conversation |
| **UP** (short) | Response | Scroll up |
| **UP** (short) | TTS playing (Emery/Flint) | Scroll up without animation |
| **UP** (short) | Other | Open chat list menu |
| **UP** (long) | TTS playing/loading (Emery/Flint) | No-op with double vibration |
| **UP** (long) | Other | Open chat list menu |
| **DOWN** (short) | Response | Scroll down |
| **DOWN** (short) | TTS playing (Emery/Flint) | Scroll down without animation |
| **DOWN** (short) | Idle/main screen | Open ToDo & Notes |
| **DOWN** (long) | TTS playing/loading (Emery/Flint) | No-op with double vibration |
| **DOWN** (long) | Idle | Send a random "Surprise Me" prompt (can be disabled in settings) |
| **DOWN** (long) | Response (Emery/Flint) | Read the current reply aloud |

### Chat List Menu

Accessible via the UP button:

- **Row 0: Current Model** — tap to open the model select sub-menu
- **Row 1: Controls** — opens the built-in short/long-press reference
- **Row 2: ToDo & Notes** — opens active items and the separate Archive entrance
- **Row 3: Web Search** — toggle web search on or off
- **Row 4: Volume** (Emery/Flint) — choose TTS playback volume from 10% to 100%
- **Row 4+ (without speaker) / Row 5+ (Emery/Flint): Chat History** — select a previous conversation to resume with full context

### Model Select

- Lists all configured models (up to 5)
- Active model is marked with a dot indicator
- Tap any model to switch instantly (conversation history is preserved)

---

## Architecture

```
[Pebble Watch]  <---- Bluetooth ---->  [Phone (PebbleKit JS)]  <---- HTTPS ---->  [OpenRouter / Custom API]
```

| Component | Language | Role |
|-----------|----------|------|
| `src/c/mdbl.c` | C (Pebble SDK 3) | Watch UI, animation state machine, AppMessage handling |
| `src/pkjs/pebble-js-app.js` | JavaScript (ES5) | API calls, conversation storage, config, chunked Bluetooth transfer |
| `config/index.html` | HTML/CSS/JS | Settings page (hosted on GitHub Pages) |

The watch communicates with the phone over Bluetooth using Pebble's AppMessage protocol. Long responses are chunked into ~256-byte segments and reassembled on the watch. Emery TTS uses one-byte μ-law transport decoded to native 16-bit audio; Flint uses 8-bit PCM. Both use watch→phone flow control. ToDo & Notes, chats, Memory.md, Timeline/Todoist mappings, the incremental sync token, and the durable offline outbox are stored phone-side; strong reminder and Todoist sync Wakeups are persisted on the watch.

---

## Version History

### v1.5.0
- **ToDo & Notes** — DOWN opens active items plus a compact Archive entrance. Items can be completed, reopened, deleted, or returned to their originating conversation; the latest 50 completions are retained.
- **Timeline-linked ToDo & Notes** — Actionable Timeline requests can create one linked TODO without duplicating the event. Ordinary appointments are stored as Event items and projected to Timeline.
- **Todoist Sync** — Optional direct synchronization uses persistent task IDs, an offline mutation queue and incremental reads. It runs on launch, every five minutes while Wrist AI remains open, after local mutations, and through optional 30-minute/1-hour/3-hour Wakeups. Status appears below the conversation title; an enabled integration without a token shows `Set up Todoist`.
- **On-watch Controls Guide** — The UP menu includes a Pebble-style reference for every contextual short/long-press action. It can be hidden from Config after onboarding.
- **TTS De-clicking** — Emery high-quality audio now uses a smoothed noise gate, envelope compression, gentle high-frequency smoothing and six-millisecond sentence fades to reduce isolated crackle and μ-law grain.
- **Strong Local Reminders** — Explicit strong-reminder requests use up to seven watch Wakeup slots with a longer vibration pattern; one slot is reserved for optional synchronization.
- **Exact Sleep Intervals** — Supplies the LLM with real sleep/deep-sleep start and end times instead of aggregate minutes alone.
- **Launch-aware Timeline Integration** — Recognizes Timeline actions and launch codes, restores the Event's original conversation when available, and preserves Event context.
- **Long-term Memory** — Stable personal facts such as a birth date, identity,
  durable preferences and long-term goals are routed exclusively to a compact
  local `Memory.md`, never shadow Notes. Config can review and safely edit the
  Markdown document.
- **Simplified Chinese Normalization** — Converts Dictation and LLM text from Traditional to Simplified Chinese before display, storage, title generation, Timeline parsing, and TTS.
- **Smaller Chinese Replies** — Reply text now uses the same 14/18/24 size ladder as the question area instead of the former larger 18/24/28 ladder.
- **Dictation Review and Recovery** — After recognition, SELECT sends, DOWN re-records, and BACK cancels. Unexpected system aborts such as notification interruptions retry Dictation once.
- **Reply Header Spacing** — Adds fixed space below the TTS hint so it no longer touches scrolled response text.
- **Unified Theme Headers** — Settings, ToDo & Notes, item actions, Dictation review, and replies use compact bold headers colored from the Home theme instead of decorative stripes.
- **Reliable Direct TODO Creation** — Explicit named requests have a deterministic fallback when the model omits the hidden Note control block.

Release-ready App Store copy and the 720×320 banner are stored in
[`store-assets`](store-assets/). The full bilingual change list is in
[`V1.5.0_RELEASE_NOTES.md`](V1.5.0_RELEASE_NOTES.md).

### v1.4.0
- **Multi-day Health and Sleep** — Send 1, 3, or 7 days of activity, calories, heart rate, sleep, and deep-sleep history to the selected LLM.
- **Listen on Launch** — Optional one-shot automatic dictation when Wrist AI opens; the recognized question is sent directly to the LLM.
- **Pebble Timeline** — Create local Timeline events through conversation, including relative times, notification reminders, and up to five events in one request. Core Devices' local insert/delete bridge is used for Todoist updates and removals.
- **Timeline boundary** — Core Devices exposes local insert/delete bridges, but no local Timeline list API. Wrist AI can manage Timeline pins it created and saved locally; events from other apps remain outside its local index.
- **Pebble 2 Duo TTS** — Added conservative experimental Flint support. Disabled by default and not yet validated on physical Duo hardware.
- **Safer TTS Volume** — The watch menu remains 10%–100%, while current hardware output is capped at 50% to avoid high-volume clipping.
- **Improved Readability** — Replaced overly bright filled backgrounds while preserving the orange and purple visual style. Reply and menu pages keep their existing appearance.
- **Cleaner Web Search Replies** — URLs, domain names, source lists, and numbered citations are removed before display, storage, and TTS.
- **Config and Export Fixes** — Fixed iOS PKJS loading, bypassed stale Config WebView caches, and exported JSON with explicit UTF-8 encoding for Chinese text.

### v1.3.0
- **Text-to-Speech** — Read AI replies aloud on Emery (Google Cloud TTS, auto Chinese/English). Long-press DOWN to read; short-press SELECT to stop. TTS volume is selected from the watch UP menu (`Volume`, 10%-100%); current logical 100% maps to 50% hardware output. Config provides speaking rate and Emery high-quality/standard mode. *Beta.*
- **Health Context** — Optional toggle to include daily steps and active minutes in AI context. Off by default. *Beta.*
- **Location Context** — Optional toggle to include approximate GPS (reverse-geocoded to city name) in AI context. Off by default. *Beta.*
- **Web Search** — Optional toggle to let models search the web for current information (OpenRouter only, on by default). Configurable from the config page or the watch menu. *Beta.*
- **Reply starts at top** — Replies now scroll to the top so you read from the beginning, instead of jumping to the bottom.
- **Speaking rate** — Adjustable TTS speed (0.75× / 1.0× / 1.25× / 1.5×) from the config page.
- **Bug fixes** — Fixed TTS error messages corrupting AI replies and freezing buttons; removed the "Voice" API mode trap that hid the API key field; cleaned up residual "Surprise" display when sending fails; aligned TTS audio length with on-screen text length; rounded active minutes (<60s no longer counts as 0); added TTS loading indicator; fixed `atob` not available in PebbleKit JS; fixed TTS request failing when outbox was busy.
- **Pre-release hardening** — Declared `health` capability (health data was silently failing without it); added TTS cancel protocol so Stop actually stops; TTS HTTP/network errors now surface to the user; TTS watchdog prevents permanent button freeze if a message is dropped; health data now correctly reports legitimate zero values; TTS playback stays controllable until audio fully drains.
- **TTS audio engine overhaul** (real-device tuning) — Replaced 4-bit ADPCM, switched volume control to Pebble's speaker API, uses a 35KB Emery ring buffer with PAUSE/RESUME watermarks, large packets for reliable iOS background delivery, and guarded non-animated scrolling during playback. Emery high-quality mode adds 16 kHz μ-law transport and native 16-bit output. Reference implementation: [Pebble_Gemini](https://github.com/ericlmccormick/Pebble_Gemini).

### v1.2.0
- **Font size settings** — Choose Normal, Large, or Extra Large text in the config page
- **Bold text toggle** — Make response text bold for better readability
- **"Surprise Me" toggle** — Disable the long-press DOWN random prompt in settings
- **Improved Export button** — Better readability with darker background and clearer label

---

## Known Limitations

- Dictation language is determined by the phone-side Rebble voice service; the app cannot set it. Display of non-Latin scripts depends on the watch's installed language pack.
- Text only — no images or emoji rendering on Pebble's display
- Conversation content is not persisted on the watch after restart (full history lives on the phone)
- TTS is stable on Emery and experimental on Pebble 2 Duo (Flint). Emery uses 16 kHz μ-law/16-bit playback; Flint uses conservative 8 kHz PCM. On extreme Bluetooth jitter, minor word-level chop may still occur. Menu volume remains 10%-100%, while code caps hardware output at 50%.
- Timeline notifications depend on the watch's notification, vibration, and Do Not Disturb settings. Timeline reminders are not Pebble system alarms and do not provide alarm snooze behavior.
- Pebble 2 Duo TTS remains disabled by default because physical Duo playback has not been validated.

---

## Building from Source

Requires the [Rebble SDK / pebble tool](https://developer.rebble.io), or use [CloudPebble](https://cloudpebble.repebble.com).

```bash
pebble build
pebble install --phone 192.168.x.x
```

---

## Support

If you find this app useful, consider buying me a coffee:

https://www.paypal.com/paypalme/Asterwyn

---

## License

[CC BY-NC 4.0](LICENSE) — Free for personal and non-commercial use.
