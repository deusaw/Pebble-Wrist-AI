# Wrist AI Watch UI Plan

Status: design proposal only; not implemented in this pass.

## Direction

Wrist AI should feel like a capable Pebble application, not a phone UI compressed onto a watch. The redesign should use Pebble conventions first: system fonts, one clear focus per screen, native `MenuLayer` behavior, hardware-button actions and restrained color.

Orange remains the Wrist AI primary accent and purple the secondary accent. Answer pages stay light. Large black or dark-gray phone-style cards are not part of this direction.

v1.4.2 now supports Random or a fixed theme. Theme headers use a 26px Gothic 18 Bold bar so Chinese titles use a supported glyph size and sit visually centered.

## Navigation model

| Screen | UP | SELECT | DOWN | BACK |
|---|---|---|---|---|
| Home | Conversations | Dictate | Notes/TODOs | Exit |
| Reply | Scroll up | Follow-up/home | Scroll down | Home |
| Dictation review | — | Send | Re-record | Cancel |
| Notes list | Previous row | Open item | Next row | Home |
| Note actions | Previous row | Run action | Next row | Notes |

Long SELECT continues to create a new conversation. Long DOWN on a reply remains TTS on speaker watches. Surprise Me should eventually move out of a competing long-press path and into the UP menu.

## Screen proposals

### Home

- Small `Wi` mark rather than a dominant animated logo.
- One status line: Ready, Listening, Sending, Thinking, Offline or API Key Required.
- Bottom button legend: `UP Chats · SELECT Ask · DOWN Notes`.
- Orange or purple can fill the status band, but the page background should remain platform-native.

### Listening and thinking

- Keep animation lightweight and centered.
- Use a stable status label below it; avoid changing large background colors every state.
- Notification-interrupted Dictation should visibly say `Interrupted · retrying` before the one automatic retry.

### Reply

- Preserve the light answer page.
- Render the user's question as a compact header separated by a thin orange rule.
- Place `Hold DOWN for TTS` inside the header band rather than floating against reply text.
- Use the same 14/18/24 system-font ladder for question and reply.
- Show a small page/scroll indicator only while scrolling.

### Notes/TODOs

- Use a standard Pebble menu, not custom cards.
- Row 1: checkbox/status icon and title.
- Row 2: compact due time or `Note`.
- Detail screen should show title and content before actions.
- Actions should be state-specific: `Open Conversation`, `Complete` or `Reopen`, then `Delete`.
- Delete should use a two-step confirmation screen on the watch.

### Menus

- UP always means conversations/settings.
- DOWN from Home always means Notes/TODOs.
- Menu highlight follows the Home theme selected for the current launch, always with white text; monochrome watches use black highlight with white text.
- Avoid centered text for ordinary menu rows; Pebble menus are easier to scan left-aligned.

## Platform palette

| Role | Color watches | Monochrome fallback |
|---|---|---|
| Page | White / very light warm neutral | White |
| Primary accent | Orange | Black |
| Menu selection | Current Home theme + white text | Black + white text |
| Main text | Black / dark plum | Black |
| Secondary text | Dark gray | Black with smaller font |
| Destructive | Red only where supported | Black + explicit wording |

Chalk layouts need centered content and larger side insets. Diorite and Flint must not depend on color to communicate state.

## Implementation order

1. Normalize button mapping and footer legends.
2. Rebuild Home, Listening and Thinking around one native layout system.
3. Refine Reply header, TTS hint and scroll indication.
4. Add Note detail and delete confirmation.
5. Apply platform palettes and Chalk geometry.
6. Build all five targets, then test Basalt/Chalk screenshots and Emery/Flint memory.

## Non-goals

- No phone-style card stack on the watch.
- No animated transitions that consume TTS or reply-buffer memory.
- No icon font or large bitmap library.
- No dependency on Timeline developer actions until Core Devices implements them.
