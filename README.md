# Pebble Wrist AI 🤖⌚

> An intelligent, voice-powered AI assistant living right on your Pebble smartwatch. 

**Pebble Wrist AI** allows you to talk to your watch, send the transcribed voice to modern Large Language Models (like Claude, GPT-4, Llama, etc.) via OpenRouter, and read streaming responses right on your wrist. No extra apps or servers required after setup—your phone acts as the bridge!

![Platforms](https://img.shields.io/badge/platform-Basalt%20%7C%20Chalk%20%7C%20Diorite%20%7C%20Emery-blue)
![License](https://img.shields.io/badge/license-CC%20BY--NC%204.0-green)

---

## ✨ Features

- **🗣️ Voice Input**: Uses Pebble's built-in dictation interface (supported via Rebble Dictation).
- **⚡ Real-time Streaming**: Watch the AI type out its response in real-time, with `<think>` block filtering for reasoning models.
- **📚 Multi-Conversation Support**: Manage up to 20 concurrent chats. The system auto-generates smart titles using the LLM and keeps history for contextual follow-ups.
- **🧠 Multi-Model Management**: Switch between up to 5 models (e.g., GPT-4o, Claude, Gemini) instantly from your watch or manage the list in the config page.
- **⭕ Full Round Support**: Optimized UI for Pebble Time Round (Chalk), featuring centered layouts and text-wrap adaptation.
- **📱 Serverless Architecture**: Everything runs locally between your Pebble and your phone's Companion App (PebbleKit JS). Total privacy.
- **⚙️ Modern Config Page**: Tweak system prompts, manage conversations, and add/remove models from a sleek, dark-themed interface.
- **🎲 Surprise Me**: Long-press DOWN to get a random AI-generated joke, fact, recipe, or thought-provoking question.

---

## ⌚ Compatibility

| Platform | Supported Watches |
|----------|-------|
| **Basalt** | Pebble Time, Pebble Time Steel |
| **Chalk** | Pebble Time Round |
| **Diorite** | Pebble 2, Pebble 2 HR |
| **Emery** | Pebble Time 2 |

---

## 🚀 Setup & Installation

1. **Install the App**: Install the `.pbw` file via the Pebble app (sideload) or from the Rebble Appstore.
2. **Access Settings**: Open the app settings in the Pebble mobile app on your phone.
3. **API Key Setup**: Get an API key from [OpenRouter](https://openrouter.ai), and paste it into the configuration page.
4. **Model List**: Add your favorite models (e.g., `google/gemma-4-31b-it`) to the model list.
5. **Start Chatting**: Launch the app on your watch and press **SELECT** to start the dictation session!

---

## 🎮 Button Controls

| Button | State | Action |
|--------|-------|--------|
| **SELECT** (short) | Idle | Start voice dictation |
| **SELECT** (short) | Response | Return to main circle (ready for follow-up) |
| **SELECT** (long) | Any | Start a new conversation |
| **UP** (short) | Response | Scroll up |
| **UP** (short) | Other | Open chat list menu |
| **UP** (long) | Any | Open chat list menu |
| **DOWN** (short) | Response | Scroll down |
| **DOWN** (short) | Other | Open chat list menu |
| **DOWN** (long) | Idle | Send a random "Surprise Me" prompt |

### 📂 Chat List Menu
Accessible via UP or DOWN buttons:
- **Row 0: Current Model** — tap to open the **Model Select** sub-menu
- **Row 1: + Start New Chat** — clears history and starts fresh
- **Row 2+: Chat History** — select a previous conversation to resume it with full context

### 🤖 Model Select Sub-menu
- Lists all configured models (up to 5)
- Active model is highlighted with a dot indicator
- Tap any model to switch instantly (no conversation history is cleared)

---

## 🛠️ Architecture Overview

```text
[ Pebble Watch ]  <---- AppMessage ---->  [ PebbleKit JS (Phone) ]  <---- HTTPS ---->  [ OpenRouter API ]
- Hardware UI                             - LocalStorage DB                            - AI processing
- DictationSession                        - Network Requests                           - Streaming SSE
- Animation State Machine                 - Config Page (Pebble Tooling)
- Multi-chat / Model UI
```

---

## 📦 Building from Source

To compile the watchapp yourself, you need the [Rebble SDK / pebble tool](https://developer.rebble.io).

```bash
# Compile the project
pebble build

# Install to your watch (replace IP with your phone's IP running Pebble Developer Mode)
pebble install --phone 192.168.x.x
```

---

## 💖 Support the Project

If you find this app useful and want to support its ongoing development, consider buying me a coffee! Your support helps keep legacy smartwatches alive and smart!

☕ [**Donate via PayPal (paypal.me/Asterwyn)**](https://paypal.me/Asterwyn)

---

## 📜 License

[CC BY-NC 4.0](LICENSE) — Free for personal and non-commercial use.
