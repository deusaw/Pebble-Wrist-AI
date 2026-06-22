# Wrist AI

> A flexible LLM client for Pebble smartwatches.

**Wrist AI** lets you talk to any Large Language Model directly from your Pebble. Speak your question, and the response appears on your wrist. Your phone acts as a silent bridge — no server, no cloud relay, no extra apps.

Works with OpenRouter (GPT, Claude, Gemini, Gemma, Llama, Qwen, and more) or your own custom API endpoint.

![Platforms](https://img.shields.io/badge/platform-Basalt%20%7C%20Chalk%20%7C%20Diorite%20%7C%20Emery-blue)
![License](https://img.shields.io/badge/license-CC%20BY--NC%204.0-green)

---

## Features

- **Voice Input** — Uses Pebble's built-in dictation (via Rebble services). Press SELECT, speak, done.
- **Multi-Model Switching** — Configure up to 5 models and switch between them directly from your watch. No need to open settings.
- **Multi-Conversation** — Manage up to 20 concurrent chats. The LLM auto-generates a smart title for each conversation. Switch between them from the watch menu with full context preserved.
- **Custom API Support** — Use OpenRouter out of the box, or point the app to your own backend / local model server with a custom API URL.
- **Conversation Export** — Export any conversation as a structured JSON file (with messages, timestamps, and model info) from the config page.
- **Font Size & Bold** — Choose from three text sizes (Normal / Large / Extra Large) and toggle bold text for better readability on your wrist.
- **"Surprise Me" Toggle** — Long-press DOWN for a random prompt, or disable it in settings if you prefer to avoid accidental triggers.
- **Text-to-Speech (Emery only)** — Long-press DOWN on a reply to have it read aloud via Google Cloud TTS, with automatic Chinese/English voice detection. Adjustable volume. *Beta.*
- **Health Context** — Optionally include your daily step count and active minutes in the AI's context. Off by default. *Beta.*
- **Location Context** — Optionally include your approximate GPS location in the AI's context. Off by default. *Beta.*
- **Web Search** — Optionally let models search the web for up-to-date information (OpenRouter only, on by default). Toggle from the config page or right from the watch menu. *Beta.*
- **Auto Read Aloud (Emery only)** — Optionally have every AI reply read aloud automatically when it arrives. Off by default. *Beta.*
- **Cross-Platform** — Supports all Pebble hardware including Pebble Time Round (Chalk) with fully optimized centered layouts.
- **Serverless & Private** — Your phone talks directly to the API. Conversations are stored locally on your phone's localStorage. Nothing leaves your device except the API request itself — and the optional Health/Location toggles, which are off by default and only include that data in the request when you enable them.
- **Dark-Themed Config Page** — A modern settings interface inside the Pebble companion app for API keys, system prompts, model lists, and conversation management.

---

## Compatibility

| Platform | Watches |
|----------|---------|
| **Basalt** | Pebble Time, Pebble Time Steel |
| **Chalk** | Pebble Time Round |
| **Diorite** | Pebble 2, Pebble 2 HR |
| **Emery** | Pebble Time 2 |

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
| **SELECT** (short) | TTS playing (Emery) | Stop reading aloud |
| **SELECT** (long) | Any | Start a new conversation |
| **UP** (short) | Response | Scroll up |
| **UP** (short) | Other | Open chat list menu |
| **UP** (long) | Any | Open chat list menu |
| **UP** (long) | TTS playing (Emery) | Volume up |
| **DOWN** (short) | Response | Scroll down |
| **DOWN** (short) | Other | Open chat list menu |
| **DOWN** (long) | Idle | Send a random "Surprise Me" prompt (can be disabled in settings) |
| **DOWN** (long) | Response (Emery) | Read the current reply aloud |
| **DOWN** (long) | TTS playing (Emery) | Volume down |

### Chat List Menu

Accessible via UP or DOWN button:

- **Row 0: Current Model** — tap to open the model select sub-menu
- **Row 1: + Start New Chat** — clears context and starts fresh
- **Row 2+: Chat History** — select a previous conversation to resume with full context

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
| `src/pkjs/index.js` | JavaScript (ES5) | API calls, conversation storage, config, chunked Bluetooth transfer |
| `config/index.html` | HTML/CSS/JS | Settings page (hosted on GitHub Pages) |

The watch communicates with the phone over Bluetooth using Pebble's AppMessage protocol. Long responses are chunked into ~256-byte segments and reassembled on the watch. The entire codebase is two files: ~1,700 lines of C and ~750 lines of JS.

---

## Version History

### v1.3.0
- **Text-to-Speech** — Read AI replies aloud on Emery (Google Cloud TTS, auto Chinese/English). Long-press DOWN to read; long-press UP/DOWN to adjust volume; short-press SELECT to stop. *Beta.*
- **Auto Read Aloud** — Optionally read every reply automatically (Emery only). *Beta.*
- **Health Context** — Optional toggle to include daily steps and active minutes in AI context. Off by default. *Beta.*
- **Location Context** — Optional toggle to include approximate GPS in AI context. Off by default. *Beta.*
- **Web Search** — Optional toggle to let models search the web for current information (OpenRouter only, on by default). Configurable from the config page or the watch menu. *Beta.*
- **Reply starts at top** — Replies now scroll to the top so you read from the beginning, instead of jumping to the bottom.
- **Bug fixes** — Fixed TTS error messages corrupting AI replies and freezing buttons; removed the "Voice" API mode trap that hid the API key field; cleaned up residual "Surprise" display when sending fails; aligned TTS audio length with on-screen text length; rounded active minutes (<60s no longer counts as 0); added TTS loading indicator.
- **Pre-release hardening** — Declared `health` capability (health data was silently failing without it); added TTS cancel protocol so Stop actually stops; added TTS backpressure to prevent long replies from turning into noise; TTS HTTP/network errors now surface to the user; Auto TTS no longer fires when no TTS key is set; TTS watchdog prevents permanent button freeze if a message is dropped; health data now correctly reports legitimate zero values; TTS playback stays controllable until audio fully drains.

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
- TTS is Emery-only (the only Pebble with a speaker). On severe Bluetooth jitter, a dropped audio chunk may corrupt the rest of that sentence's audio.
- **Currently unable to test microphone/dictation features** due to lack of a physical device. If you encounter any issues, please [open an issue on GitHub](https://github.com/deusaw/Pebble-Wrist-AI/issues).

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
