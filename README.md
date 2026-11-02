# Gemini Dictation

A small background app for Windows: press a global hotkey to start recording (a floating bar at the bottom of the screen shows the waveform), press it again and the transcript is pasted into the text box you were in. Esc cancels. Recordings stop automatically after 6 minutes and are transcribed as usual.

With a service account, audio is streamed to the Gemini Live API (`gemini-3.5-transcribe-live`) while you speak, so the text arrives about half a second after you stop. With an API key, or if the live session fails, the whole recording is sent to `generateContent` instead (2–3 s).

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Works with Visual Studio (MSVC) and MinGW. `GeminiDictation.exe` has no DLL dependencies.

## Use

1. Put `GeminiDictation.exe` in a permanent folder and run it.
2. On first run it creates `dictation.ini` next to the exe and opens it in Notepad. Fill in `ServiceAccount=` (recommended, see below) or `ApiKey=` and save. The first run also turns on "Start with Windows".
3. In any text box, press `Ctrl+Alt+Space` and start talking; press it again to finish.

Right-click the microphone icon in the tray to edit settings, reload settings (needed after changing the hotkey), toggle start with Windows, or exit.

## Settings (dictation.ini)

| Key | Meaning |
|---|---|
| `ApiKey` | API key from Google AI Studio |
| `ServiceAccount` | Path to a service account JSON key for Vertex AI (a bare file name means next to the exe). Takes precedence over `ApiKey` and enables streaming |
| `Location` | Vertex AI location, default `global` |
| `LiveModel` | Streaming transcription model, default `gemini-3.5-transcribe-live`; empty disables streaming |
| `LiveMode` | `SMART` (default) removes filler words and adds punctuation; `VERBATIM` keeps everything as spoken |
| `Languages` | Languages you speak, comma separated, default `zh-CN,en-US` |
| `Model` / `FallbackModel` | Models for non-streaming transcription; the fallback is tried when the main one errors |
| `Hotkey` | Global hotkey, e.g. `Ctrl+Alt+Space`, `Ctrl+Shift+H`, `F9` |
| `Vocabulary` | Names and terms you often say, comma separated, to help recognition |
| `Proxy` | e.g. `127.0.0.1:7890`; empty uses the system proxy |
| `RestoreClipboard` | Put the previous clipboard contents back after pasting |
| `GenerationConfig` | generationConfig for non-streaming transcription |
| `Prompt` | Custom instructions for non-streaming transcription; empty uses the built-in ones |

Keep the file's encoding (UTF-16) when saving in Notepad, or non-ASCII text breaks.

The service account needs the **Vertex AI User** role (`roles/aiplatform.user`), and the Vertex AI API must be enabled in its project.

Each transcription is logged to `dictation.log` next to the exe with timings, which helps when something is slow. Past 512 KB it is renamed to `dictation.old.log`, replacing the previous one.

## Notes

- With a paid (billing enabled) key, Google doesn't use your content to train models; free-tier content may be used to improve products.
- Windows running as administrator don't receive the paste (Windows privilege isolation). To dictate into them, run this app as administrator too.
- If the microphone can't be opened, check Settings > Privacy & security > Microphone allows desktop apps to use it.
- A Live session is opened when you press the hotkey (about 0.6–1.3 s) and closed after the transcript arrives; the audio recorded meanwhile is sent once it's ready, so this overlaps with speaking. Only the access token is fetched ahead of time. Nothing is kept connected while idle.
