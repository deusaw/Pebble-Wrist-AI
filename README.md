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
- **"Surprise Me"** — Long-press DOWN for a random prompt: jokes, fun facts, recipes, or thought-provoking questions.
- **Custom Wi Logo** — A hand-drawn "Wi" logo rendered natively in C, with pulse animation, morph transitions between states, and a shadow layer for depth.
- **Random Color Theme** — Each app launch picks a random background color from a 12-color palette, with a 5-stripe color bar accent on the response page. Every session looks a little different.
- **Smooth Animations** — State-driven animation system: 3-dot bounce while listening, arrow animation while sending, spinning circle with orbiting dot while thinking, and ease-out morph transitions between the main circle and response view.
- **Round Screen Support** — Fully optimized layouts for Pebble Time Round (Chalk) with centered positioning and text-wrap adaptation.
- **Serverless & Private** — Your phone talks directly to the API. Conversations are stored locally on your phone's localStorage. Nothing leaves your device except the API request itself.
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
| **SELECT** (long) | Any | Start a new conversation |
| **UP** (short) | Response | Scroll up |
| **UP** (short) | Other | Open chat list menu |
| **UP** (long) | Any | Open chat list menu |
| **DOWN** (short) | Response | Scroll down |
| **DOWN** (short) | Other | Open chat list menu |
| **DOWN** (long) | Idle | Send a random "Surprise Me" prompt |

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

The watch communicates with the phone over Bluetooth using Pebble's AppMessage protocol. Long responses are chunked into ~256-byte segments and reassembled on the watch. The entire codebase is two files: ~1,400 lines of C and ~640 lines of JS.

---

## Known Limitations

- Voice dictation only supports English (Pebble system limitation)
- Text only — no images or emoji rendering on Pebble's display
- Conversation content is not persisted on the watch after restart (full history lives on the phone)
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

[**Donate via PayPal (paypal.me/Asterwyn)**](https://paypal.me/Asterwyn)

---

## License

[CC BY-NC 4.0](LICENSE) — Free for personal and non-commercial use.
