# Gemini Dictation

Voice typing for Windows. Press a hotkey, talk, press it again, and the text is pasted into whatever you were typing in. Esc cancels.

With a service account, audio streams to the Gemini Live API while you speak, so the text arrives about a second after you stop. With an API key, the whole recording is sent when you stop (2–3 s).

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Works with Visual Studio (MSVC) and MinGW. The resulting `GeminiDictation.exe` has no DLL dependencies.

## Setup

1. Put `GeminiDictation.exe` in a permanent folder and run it. It adds itself to Windows startup; you can turn that off from the tray menu.
2. It creates `dictation.ini` next to the exe and opens it in Notepad. Fill in one of these and save:
   - `ServiceAccount=`: path to a Vertex AI service account JSON key (recommended, enables streaming). The account needs the **Vertex AI User** role, and the Vertex AI API must be enabled in its project.
   - `ApiKey=`: an API key from [Google AI Studio](https://aistudio.google.com/apikey).
3. In any text box, press `Ctrl+Alt+Space`, talk, and press it again.

Right-click the tray icon to edit settings, reload them, or exit.

## Settings

Every setting is explained in `dictation.ini`. The ones you're most likely to change:

| Key | Meaning |
|---|---|
| `Hotkey` | e.g. `Ctrl+Alt+Space`, `Ctrl+Shift+H`, `F9`. Use "Reload settings" in the tray menu after changing it |
| `Languages` | Languages you speak, default `zh-CN,en-US` |
| `Vocabulary` | Names and terms you often say, comma separated |
| `Proxy` | e.g. `127.0.0.1:7890`; empty uses the system proxy |

## Troubleshooting

- **Nothing is pasted into a program:** programs running as administrator don't accept the paste unless this app also runs as administrator.
- **The microphone won't open:** check that Settings > Privacy & security > Microphone allows desktop apps.
- **It's slow or failing:** `dictation.log` next to the exe records timings and errors.
