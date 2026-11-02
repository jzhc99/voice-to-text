// GeminiDictation: press a global hotkey to start recording (a floating bar shows the waveform), press it again
// to transcribe with Gemini, and the text is pasted into the input box that had focus. Esc cancels.
// Settings live in dictation.ini next to the exe (created on first run).
//
// With a service account, audio is streamed to the Gemini Live API while you speak, over a session that is
// opened and authenticated ahead of time, so the text arrives about a second after you stop. Without one (or
// if the live session fails) the whole recording is sent to generateContent instead.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef _WIN32_IE
#define _WIN32_IE 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#define APP_TITLE   L"Gemini Dictation"
#define RUN_VALUE   L"GeminiDictation"
#define API_HOST    L"generativelanguage.googleapis.com"
#define LIVE_PATH   L"/ws/google.cloud.aiplatform.v1.LlmBidiService/BidiGenerateContent"

#define IDI_APP         1               // must match app.rc

#define WM_TRAY         (WM_APP + 1)
#define WM_TRANSCRIBED  (WM_APP + 2)
#define WM_LIVE_DONE    (WM_APP + 3)

enum { HOTKEY_TOGGLE = 1, HOTKEY_ESC = 2 };
enum { TIMER_HIDE = 1, TIMER_RESTORE_CLIP, TIMER_TICK, TIMER_LIVE_WATCHDOG, TIMER_STOP_TAIL };
enum { IDM_SETTINGS = 100, IDM_RELOAD, IDM_AUTOSTART, IDM_EXIT };
enum { ST_IDLE, ST_RECORDING, ST_TRANSCRIBING };
enum { LIVE_IDLE, LIVE_RECORDING, LIVE_FINISHING };

#define SAMPLE_RATE    16000
#define CHUNK_SAMPLES  800              // 50 ms per buffer
#define NUM_BUFS       8
#define MAX_SECONDS    360              // 6 minutes max; then recording stops and is transcribed as usual
#define MIN_SAMPLES    (SAMPLE_RATE * 3 / 10)
#define NUM_BARS       56
#define STOP_TAIL_MS   300              // keeps recording after the stop key: the last words are still in the audio driver

// While idle the access token is kept fresh; after a failure (e.g. offline) it is retried this often
#define TOKEN_RETRY_MS      (30 * 1000)

typedef struct { char *p; size_t len, cap; } Buf;
typedef struct { UINT fmt; HGLOBAL data; } ClipItem;
typedef struct { char *body; size_t bodyLen; wchar_t model[128], fallback[128]; int liveFailed; } Job;
typedef struct { wchar_t *text, *error; int fallback; } JobResult;

// Everything the live thread needs, copied from the settings under g_liveLock
typedef struct {
    int enabled;
    wchar_t saFile[MAX_PATH], location[64], model[128], mode[32], proxy[256], languages[256], vocabulary[4096];
} LiveConfig;

typedef struct {
    LiveConfig cfg;
    HINTERNET ses, con, ws;
    HANDLE thread, ready, done;
    volatile LONG ok, dead, ended, complete, expiring;
    DWORD openedAt;
    Buf text;                           // final transcript (UTF-8)
    char error[300];
} LiveSession;

static HINSTANCE g_inst;
static HWND g_main, g_overlay;
static UINT g_taskbarCreated;
static NOTIFYICONDATAW g_nid;
static HFONT g_font;
static int g_dpi = 96;

// Settings
static wchar_t g_hotkey[64];
static wchar_t g_apiKey[256];
static wchar_t g_serviceAccount[MAX_PATH];
static wchar_t g_location[64];
static wchar_t g_liveModel[128];
static wchar_t g_liveMode[32];
static wchar_t g_languages[256];
static wchar_t g_model[128];
static wchar_t g_fallbackModel[128];
static wchar_t g_proxy[256];
static wchar_t g_vocabulary[4096];
static wchar_t g_prompt[8192];
static wchar_t g_genConfig[1024];
static int g_restoreClipboard;
static int g_hotkeyOk;

// State
static int g_state = ST_IDLE;
static HWND g_target;
static DWORD g_recordStart, g_stopTick;
static int g_stopping; // the stop key was pressed; recording ends when TIMER_STOP_TAIL fires
static int g_useLive, g_waitLive;

// Recording
static HWAVEIN g_wi;
static WAVEHDR g_hdr[NUM_BUFS];
static short g_bufMem[NUM_BUFS][CHUNK_SAMPLES];
static int g_nextHdr;
static short *g_pcm;
static size_t g_pcmLen, g_pcmCap;
static float g_bars[NUM_BARS];
static int g_barPos;

// Live thread. g_liveLock guards g_pcm/g_pcmLen (the live thread reads them while recording) and the fields below
static CRITICAL_SECTION g_liveLock;
static HANDLE g_liveWake;
static LiveConfig g_liveCfg;
static LONG g_liveGen, g_livePhase;
static volatile LONG g_liveQuit;

// Service account token cache
static CRITICAL_SECTION g_tokenLock;
static char g_token[2048];
static time_t g_tokenExpire;
static wchar_t g_tokenFor[MAX_PATH];
static wchar_t g_tokenProject[128];

// Overlay text (the waveform is drawn while recording)
static wchar_t g_overlayText[512];
static int g_overlayError;

// Clipboard backup
static ClipItem g_clip[64];
static int g_clipCount, g_clipPending;

// ---------------------------------------------------------------- Helpers

static void BufAppend(Buf *b, const void *data, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        char *p;
        while (cap < b->len + n + 1) cap *= 2;
        p = (char *)realloc(b->p, cap);
        if (!p) return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void BufStr(Buf *b, const char *s) { BufAppend(b, s, strlen(s)); }

static char *WideToUtf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? n : 1);
    if (!s) return NULL;
    if (n <= 0) { s[0] = 0; return s; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static wchar_t *Utf8ToWide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((n > 0 ? n : 1) * sizeof(wchar_t));
    if (!w) return NULL;
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static wchar_t *DupW(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

static void TrimW(wchar_t *s)
{
    size_t n, start = 0;
    if (!s) return;
    while (s[start] && iswspace(s[start])) start++;
    if (start) memmove(s, s + start, (wcslen(s + start) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n > 0 && iswspace(s[n - 1])) s[--n] = 0;
}

// A file in the exe's folder; an absolute name is returned as is
static void ExeDirFile(wchar_t *out, size_t cap, const wchar_t *name)
{
    wchar_t *slash;
    if (name[0] == L'\\' || (name[0] && name[1] == L':')) {
        wcsncpy(out, name, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    GetModuleFileNameW(NULL, out, (DWORD)cap);
    slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
    wcsncat(out, name, cap - wcslen(out) - 1);
}

static void IniPath(wchar_t *out, size_t cap) { ExeDirFile(out, cap, L"dictation.ini"); }

static wchar_t *Fmt(const wchar_t *fmt, const wchar_t *a, DWORD n)
{
    wchar_t buf[512];
    _snwprintf(buf, 512, fmt, a, n);
    buf[511] = 0;
    return DupW(buf);
}

// Appends a line (UTF-8) to dictation.log next to the exe, to see where the time goes. Past LOG_MAX_BYTES the
// file becomes dictation.old.log (replacing the previous one), so the two together stay under 2 × LOG_MAX_BYTES
#define LOG_MAX_BYTES (512 * 1024)
static void Log(const char *fmt, ...)
{
    wchar_t path[MAX_PATH], old[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    char line[1024];
    int n;
    va_list ap;
    SYSTEMTIME t;
    HANDLE f;
    DWORD w;
    GetLocalTime(&t);
    n = _snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    n += _vsnprintf(line + n, sizeof line - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    ExeDirFile(path, MAX_PATH, L"dictation.log");
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > LOG_MAX_BYTES)) {
        ExeDirFile(old, MAX_PATH, L"dictation.old.log");
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, line, (DWORD)n, &w, NULL);
    CloseHandle(f);
}

// ---------------------------------------------------------------- JSON (only the parts needed here)

static void JsonEscape(Buf *b, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') BufStr(b, "\\\"");
        else if (c == '\\') BufStr(b, "\\\\");
        else if (c == '\n') BufStr(b, "\\n");
        else if (c == '\r') BufStr(b, "\\r");
        else if (c == '\t') BufStr(b, "\\t");
        else if (c < 0x20) { char e[8]; _snprintf(e, sizeof e, "\\u%04x", c); BufAppend(b, e, 6); }
        else BufAppend(b, &c, 1);
    }
}

static void JsonEscapeW(Buf *b, const wchar_t *s)
{
    char *u = WideToUtf8(s);
    JsonEscape(b, u ? u : "");
    free(u);
}

static int Hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void PutUtf8(Buf *b, unsigned cp)
{
    char o[4];
    size_t n;
    if (cp < 0x80) { o[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    BufAppend(b, o, n);
}

// Finds the next "key": "string" from p and appends the decoded UTF-8 to out; returns the position after the
// value, or NULL if not found
static const char *JsonNextString(const char *p, const char *key, Buf *out)
{
    char pat[64];
    _snprintf(pat, sizeof pat, "\"%s\"", key);
    pat[sizeof pat - 1] = 0;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '"') continue;
        p++;
        while (*p && *p != '"') {
            if (*p != '\\') { BufAppend(out, p, 1); p++; continue; }
            p++;
            switch (*p) {
            case 'n': BufStr(out, "\n"); p++; break;
            case 'r': p++; break;
            case 't': BufStr(out, "\t"); p++; break;
            case 'b': case 'f': p++; break;
            case 'u': {
                unsigned cp, lo;
                if (!Hex4(p + 1, &cp)) { p++; break; }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u' && Hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                PutUtf8(out, cp);
                break;
            }
            case 0: break;
            default: BufAppend(out, p, 1); p++; break;
            }
        }
        return *p ? p + 1 : p;
    }
    return NULL;
}

// "key": number in s, or -1
static int JsonInt(const char *s, const char *key)
{
    char pat[64];
    const char *p;
    _snprintf(pat, sizeof pat, "\"%s\"", key);
    pat[sizeof pat - 1] = 0;
    if (!s || (p = strstr(s, pat)) == NULL || (p = strchr(p, ':')) == NULL) return -1;
    return atoi(p + 1);
}

// Appends a comma separated setting as a JSON string array; returns how many items were written
static int JsonList(Buf *b, const wchar_t *csv)
{
    // Besides , and ; also splits on the fullwidth comma and semicolon and the ideographic comma (Chinese IME input)
    static const wchar_t seps[] = L",\xFF0C;\xFF1B\x3001";
    wchar_t item[512];
    int n = 0;
    while (*csv) {
        size_t len = 0;
        while (*csv && wcschr(seps, *csv)) csv++;
        while (csv[len] && !wcschr(seps, csv[len])) len++;
        if (len == 0) break;
        if (len > 511) len = 511;
        wcsncpy(item, csv, len);
        item[len] = 0;
        csv += len;
        TrimW(item);
        if (!item[0]) continue;
        BufStr(b, n++ ? ",\"" : "[\"");
        JsonEscapeW(b, item);
        BufStr(b, "\"");
    }
    if (n) BufStr(b, "]");
    return n;
}

// ---------------------------------------------------------------- Settings

// The \x escapes are the Chinese filler words "en", "a" and "nage"
static const wchar_t DEFAULT_PROMPT[] =
    L"Transcribe this recording word for word. Write Chinese in Simplified Chinese. Keep English words, terms, product "
    L"names and abbreviations in English exactly as spoken; do not translate them. Add proper punctuation. Remove filler "
    L"words (\x55EF, \x554A, \x90A3\x4E2A, um, uh) and obvious stutters or repetitions. Do not summarize, rephrase, or answer questions "
    L"in the recording. Output only the transcribed text, with no explanations. If nobody speaks, output nothing.";

static void WriteDefaultIni(const wchar_t *path)
{
    static const wchar_t text[] =
        L"; Gemini Dictation settings. Changes apply the next time you press the hotkey (after changing Hotkey, use\r\n"
        L"; \"Reload settings\" in the tray menu). Keep this file's encoding (UTF-16) when saving, or non-ASCII text breaks.\r\n"
        L"[dictation]\r\n"
        L"; API key from Google AI Studio (https://aistudio.google.com/apikey)\r\n"
        L"ApiKey=\r\n"
        L"; Or a service account for Vertex AI: path to its JSON key file (a bare file name means next to the exe).\r\n"
        L"; Takes precedence over ApiKey and enables streaming transcription\r\n"
        L"ServiceAccount=\r\n"
        L"; Vertex AI location\r\n"
        L"Location=global\r\n"
        L"; Streaming transcription model (Live API, service account only); the text arrives about a second after you\r\n"
        L"; stop. Leave empty to always send the whole recording to Model instead\r\n"
        L"LiveModel=gemini-3.5-transcribe-live\r\n"
        L"; SMART removes filler words and adds punctuation; VERBATIM keeps everything as spoken\r\n"
        L"LiveMode=SMART\r\n"
        L"; Languages you speak, comma separated\r\n"
        L"Languages=zh-CN,en-US\r\n"
        L"; Model for non-streaming transcription (API key, or when the live session fails); FallbackModel is tried\r\n"
        L"; when Model returns an error (quota, model not found, ...)\r\n"
        L"Model=gemini-3.5-flash\r\n"
        L"FallbackModel=gemini-3.5-flash-lite\r\n"
        L"; Global hotkey to start/stop recording, e.g. Ctrl+Alt+Space, Ctrl+Shift+H, F9. Esc cancels while recording\r\n"
        L"Hotkey=Ctrl+Alt+Space\r\n"
        L"; Names, product names and terms you often say, comma separated, to help recognition\r\n"
        L"Vocabulary=\r\n"
        L"; Proxy, e.g. 127.0.0.1:7890; leave empty to use the system proxy settings\r\n"
        L"Proxy=\r\n"
        L"; Put the previous clipboard contents back after pasting (1 yes, 0 no)\r\n"
        L"RestoreClipboard=1\r\n"
        L"; generationConfig (JSON) for non-streaming transcription; usually leave as is\r\n"
        L"GenerationConfig={\"temperature\":0,\"maxOutputTokens\":8192}\r\n"
        L"; Custom instructions for non-streaming transcription; empty uses the built-in ones\r\n"
        L"; (mixed Chinese/English, punctuation, no filler words)\r\n"
        L"Prompt=\r\n";
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    WCHAR bom = 0xFEFF;
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, &bom, sizeof bom, &w, NULL);
    WriteFile(f, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &w, NULL);
    CloseHandle(f);
}

static void LiveUpdateConfig(void);

static void LoadConfig(void)
{
    wchar_t ini[MAX_PATH];
    IniPath(ini, MAX_PATH);
    GetPrivateProfileStringW(L"dictation", L"Hotkey", L"Ctrl+Alt+Space", g_hotkey, 64, ini);
    GetPrivateProfileStringW(L"dictation", L"ApiKey", L"", g_apiKey, 256, ini);
    GetPrivateProfileStringW(L"dictation", L"ServiceAccount", L"", g_serviceAccount, MAX_PATH, ini);
    GetPrivateProfileStringW(L"dictation", L"Location", L"global", g_location, 64, ini);
    GetPrivateProfileStringW(L"dictation", L"LiveModel", L"gemini-3.5-transcribe-live", g_liveModel, 128, ini);
    GetPrivateProfileStringW(L"dictation", L"LiveMode", L"SMART", g_liveMode, 32, ini);
    GetPrivateProfileStringW(L"dictation", L"Languages", L"zh-CN,en-US", g_languages, 256, ini);
    GetPrivateProfileStringW(L"dictation", L"Model", L"gemini-3.5-flash", g_model, 128, ini);
    GetPrivateProfileStringW(L"dictation", L"FallbackModel", L"", g_fallbackModel, 128, ini);
    GetPrivateProfileStringW(L"dictation", L"Proxy", L"", g_proxy, 256, ini);
    GetPrivateProfileStringW(L"dictation", L"Vocabulary", L"", g_vocabulary, 4096, ini);
    GetPrivateProfileStringW(L"dictation", L"Prompt", L"", g_prompt, 8192, ini);
    GetPrivateProfileStringW(L"dictation", L"GenerationConfig", L"{\"temperature\":0,\"maxOutputTokens\":8192}", g_genConfig, 1024, ini);
    g_restoreClipboard = (int)GetPrivateProfileIntW(L"dictation", L"RestoreClipboard", 1, ini);
    TrimW(g_apiKey);
    TrimW(g_serviceAccount);
    TrimW(g_location);
    if (!g_location[0]) wcscpy(g_location, L"global");
    TrimW(g_liveModel);
    TrimW(g_liveMode);
    TrimW(g_model);
    TrimW(g_fallbackModel);
    TrimW(g_proxy);
    TrimW(g_genConfig);
    LiveUpdateConfig();
}

static void OpenSettings(void)
{
    wchar_t ini[MAX_PATH];
    IniPath(ini, MAX_PATH);
    ShellExecuteW(NULL, L"open", L"notepad.exe", ini, NULL, SW_SHOWNORMAL);
}

static int ParseHotkey(const wchar_t *text, UINT *mods, UINT *vk)
{
    wchar_t buf[64];
    wchar_t *tok, *ctx = NULL;
    *mods = 0;
    *vk = 0;
    wcsncpy(buf, text, 63);
    buf[63] = 0;
    for (tok = wcstok(buf, L"+", &ctx); tok; tok = wcstok(NULL, L"+", &ctx)) {
        wchar_t *end;
        while (*tok == L' ') tok++;
        end = tok + wcslen(tok);
        while (end > tok && end[-1] == L' ') *--end = 0;
        if (!_wcsicmp(tok, L"ctrl") || !_wcsicmp(tok, L"control")) *mods |= MOD_CONTROL;
        else if (!_wcsicmp(tok, L"alt")) *mods |= MOD_ALT;
        else if (!_wcsicmp(tok, L"shift")) *mods |= MOD_SHIFT;
        else if (!_wcsicmp(tok, L"win")) *mods |= MOD_WIN;
        else if (wcslen(tok) == 1 && iswalnum(tok[0])) *vk = (UINT)towupper(tok[0]);
        else if ((tok[0] == L'F' || tok[0] == L'f') && _wtoi(tok + 1) >= 1 && _wtoi(tok + 1) <= 24) *vk = VK_F1 + (UINT)_wtoi(tok + 1) - 1;
        else if (!_wcsicmp(tok, L"space")) *vk = VK_SPACE;
        else if (!_wcsicmp(tok, L"insert")) *vk = VK_INSERT;
        else if (!_wcsicmp(tok, L"pause")) *vk = VK_PAUSE;
        else if (!_wcsicmp(tok, L"scrolllock")) *vk = VK_SCROLL;
        else if (!_wcsicmp(tok, L"`") || !_wcsicmp(tok, L"backtick")) *vk = VK_OEM_3;
        else return 0;
    }
    return *vk != 0;
}

static void RegisterToggleHotkey(void)
{
    UINT mods, vk;
    if (g_hotkeyOk) UnregisterHotKey(g_main, HOTKEY_TOGGLE);
    g_hotkeyOk = ParseHotkey(g_hotkey, &mods, &vk) && RegisterHotKey(g_main, HOTKEY_TOGGLE, mods | MOD_NOREPEAT, vk);
}

// ---------------------------------------------------------------- Start with Windows

static int IsAutostart(void)
{
    HKEY k;
    int on = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void SetAutostart(int on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
        cmd[MAX_PATH + 3] = 0;
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- Overlay

static void HideLater(UINT ms) { SetTimer(g_main, TIMER_HIDE, ms, NULL); }

static void PlaceOverlay(void)
{
    HMONITOR mon = MonitorFromWindow(g_target ? g_target : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    int w = MulDiv(480, g_dpi, 96), h = MulDiv(52, g_dpi, 96);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    KillTimer(g_main, TIMER_HIDE);
    SetWindowPos(g_overlay, HWND_TOPMOST,
        mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
        mi.rcWork.bottom - h - MulDiv(80, g_dpi, 96), w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_overlay, NULL, FALSE);
}

static void ShowOverlay(const wchar_t *text, int isError)
{
    wcsncpy(g_overlayText, text, 511);
    g_overlayText[511] = 0;
    g_overlayError = isError;
    PlaceOverlay();
}

static void PaintOverlay(HWND h, HDC dc)
{
    RECT rc;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ oldBmp, oldFont;
    HBRUSH bg;
    int pad = MulDiv(16, g_dpi, 96), wdt, hgt;

    GetClientRect(h, &rc);
    wdt = rc.right;
    hgt = rc.bottom;
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, wdt, hgt);
    oldBmp = SelectObject(mem, bmp);

    bg = CreateSolidBrush(g_overlayError ? RGB(150, 40, 36) : RGB(32, 33, 36));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));

    if (g_state == ST_RECORDING) {
        // Left: red dot + elapsed time; right: scrolling level meter
        int dot = MulDiv(10, g_dpi, 96), cy = hgt / 2;
        int x = pad, barArea, barW, i;
        DWORD secs = (DWORD)(g_pcmLen / SAMPLE_RATE);
        wchar_t t[32];
        RECT tr;
        HBRUSH red = CreateSolidBrush(RGB(234, 67, 53)), wave = CreateSolidBrush(RGB(138, 180, 248));
        HGDIOBJ oldBrush = SelectObject(mem, red), oldPen = SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, x, cy - dot / 2, x + dot, cy + dot / 2);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        x += dot + MulDiv(8, g_dpi, 96);

        _snwprintf(t, 32, L"%u:%02u", secs / 60, secs % 60);
        t[31] = 0;
        oldFont = SelectObject(mem, g_font);
        // The time turns yellow near the limit
        if (secs >= MAX_SECONDS - 30) SetTextColor(mem, RGB(253, 214, 99));
        tr.left = x; tr.top = 0; tr.right = x + MulDiv(52, g_dpi, 96); tr.bottom = hgt;
        DrawTextW(mem, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        SelectObject(mem, oldFont);
        x = tr.right + MulDiv(4, g_dpi, 96);

        barArea = wdt - pad - x;
        barW = barArea / NUM_BARS;
        if (barW < 2) barW = 2;
        for (i = 0; i < NUM_BARS; i++) {
            float level = g_bars[(g_barPos + i) % NUM_BARS]; // oldest to newest
            int bh = (int)(level * (hgt - MulDiv(16, g_dpi, 96)));
            RECT r;
            if (bh < MulDiv(2, g_dpi, 96)) bh = MulDiv(2, g_dpi, 96);
            r.left = x + i * barW;
            r.right = r.left + barW - (barW > 3 ? MulDiv(2, g_dpi, 96) : 1);
            r.top = cy - bh / 2;
            r.bottom = r.top + bh;
            FillRect(mem, &r, wave);
        }
        DeleteObject(red);
        DeleteObject(wave);
    } else {
        RECT tr = rc;
        tr.left += pad;
        tr.right -= pad;
        oldFont = SelectObject(mem, g_font);
        DrawTextW(mem, g_overlayText, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, oldFont);
    }

    BitBlt(dc, 0, 0, wdt, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintOverlay(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- Recording

static void AppendPcm(const short *s, size_t n)
{
    size_t i, max = (size_t)MAX_SECONDS * SAMPLE_RATE;
    int peak = 0;
    if (n == 0) return;
    if (g_pcmLen + n > max) n = max - g_pcmLen;
    EnterCriticalSection(&g_liveLock);
    if (g_pcmLen + n > g_pcmCap) {
        size_t cap = g_pcmCap ? g_pcmCap : SAMPLE_RATE * 30;
        short *p;
        while (cap < g_pcmLen + n) cap *= 2;
        if (cap > max) cap = max;
        p = (short *)realloc(g_pcm, cap * sizeof(short));
        if (!p) { LeaveCriticalSection(&g_liveLock); return; }
        g_pcm = p;
        g_pcmCap = cap;
    }
    memcpy(g_pcm + g_pcmLen, s, n * sizeof(short));
    g_pcmLen += n;
    LeaveCriticalSection(&g_liveLock);
    if (g_liveWake) SetEvent(g_liveWake);

    for (i = 0; i < n; i++) {
        int v = s[i] < 0 ? -(int)s[i] : s[i];
        if (v > peak) peak = v;
    }
    // Square root so quiet speech still moves the meter
    g_bars[g_barPos] = (float)sqrt(peak / 32768.0);
    if (g_bars[g_barPos] > 1.0f) g_bars[g_barPos] = 1.0f;
    g_barPos = (g_barPos + 1) % NUM_BARS;
}

// The driver fills buffers in the order they were queued, so collect them in ring order
static void Harvest(int requeue)
{
    for (;;) {
        WAVEHDR *h = &g_hdr[g_nextHdr];
        if (!(h->dwFlags & WHDR_DONE) || h->dwUser) break;
        AppendPcm((const short *)h->lpData, h->dwBytesRecorded / sizeof(short));
        h->dwUser = 1;
        if (requeue) {
            h->dwUser = 0;
            waveInAddBuffer(g_wi, h, sizeof *h);
        }
        g_nextHdr = (g_nextHdr + 1) % NUM_BUFS;
    }
}

static int StartRecording(void)
{
    WAVEFORMATEX wf;
    int i;
    ZeroMemory(&wf, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = SAMPLE_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = SAMPLE_RATE * 2;
    if (waveInOpen(&g_wi, WAVE_MAPPER, &wf, (DWORD_PTR)g_main, 0, CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
        g_wi = NULL;
        return 0;
    }
    EnterCriticalSection(&g_liveLock);
    g_pcmLen = 0;
    LeaveCriticalSection(&g_liveLock);
    g_nextHdr = 0;
    g_barPos = 0;
    memset(g_bars, 0, sizeof g_bars);
    for (i = 0; i < NUM_BUFS; i++) {
        ZeroMemory(&g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_bufMem[i];
        g_hdr[i].dwBufferLength = sizeof g_bufMem[i];
        waveInPrepareHeader(g_wi, &g_hdr[i], sizeof g_hdr[i]);
        waveInAddBuffer(g_wi, &g_hdr[i], sizeof g_hdr[i]);
    }
    if (waveInStart(g_wi) != MMSYSERR_NOERROR) {
        waveInReset(g_wi);
        for (i = 0; i < NUM_BUFS; i++) waveInUnprepareHeader(g_wi, &g_hdr[i], sizeof g_hdr[i]);
        waveInClose(g_wi);
        g_wi = NULL;
        return 0;
    }
    return 1;
}

static void StopRecording(void)
{
    int i;
    if (!g_wi) return;
    waveInReset(g_wi); // marks every buffer done, including the partly filled one
    Harvest(0);
    for (i = 0; i < NUM_BUFS; i++) waveInUnprepareHeader(g_wi, &g_hdr[i], sizeof g_hdr[i]);
    waveInClose(g_wi);
    g_wi = NULL;
}

// ---------------------------------------------------------------- HTTP

static char *Base64(const unsigned char *d, size_t n, size_t *outLen)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = 4 * ((n + 2) / 3), i, j = 0;
    char *o = (char *)malloc(len + 1);
    if (!o) return NULL;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)d[i] << 16) | ((unsigned)d[i + 1] << 8) | d[i + 2];
        o[j++] = t[(v >> 18) & 63]; o[j++] = t[(v >> 12) & 63]; o[j++] = t[(v >> 6) & 63]; o[j++] = t[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)d[i] << 16;
        if (i + 1 < n) v |= (unsigned)d[i + 1] << 8;
        o[j++] = t[(v >> 18) & 63];
        o[j++] = t[(v >> 12) & 63];
        o[j++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        o[j++] = '=';
    }
    o[j] = 0;
    *outLen = j;
    return o;
}

static HINTERNET OpenWinHttp(const wchar_t *proxy)
{
    HINTERNET ses;
    if (proxy[0])
        return WinHttpOpen(L"GeminiDictation/1.0", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy, WINHTTP_NO_PROXY_BYPASS, 0);
    ses = WinHttpOpen(L"GeminiDictation/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"GeminiDictation/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return ses;
}

// Returns the HTTP status, or 0 on a network failure (winErr holds the error code)
static DWORD HttpPost(const wchar_t *proxy, const wchar_t *host, const wchar_t *path, const wchar_t *headers,
                      const char *body, size_t bodyLen, Buf *resp, DWORD *winErr)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sz = sizeof status;

    *winErr = 0;
    ses = OpenWinHttp(proxy);
    if (!ses) goto fail;
    // 6 minutes of audio is about 15 MB: allow time for the upload and the answer
    WinHttpSetTimeouts(ses, 15000, 15000, 120000, 120000);
    con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) goto fail;
    req = WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto fail;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, (LPVOID)body, (DWORD)bodyLen, (DWORD)bodyLen, 0)) goto fail;
    if (!WinHttpReceiveResponse(req, NULL)) goto fail;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD avail = 0, got = 0;
        char chunk[16384];
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (avail > sizeof chunk) avail = sizeof chunk;
        if (!WinHttpReadData(req, chunk, avail, &got) || got == 0) break;
        BufAppend(resp, chunk, got);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return status;

fail:
    *winErr = GetLastError();
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return 0;
}

// ---------------------------------------------------------------- Service account (Vertex AI)

static char *ReadFileUtf8(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    char *s;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    s = (size == INVALID_FILE_SIZE || size > (1u << 20)) ? NULL : (char *)malloc(size + 1);
    if (s && ReadFile(f, s, size, &got, NULL)) s[got] = 0;
    else { free(s); s = NULL; }
    CloseHandle(f);
    return s;
}

static char *Base64Url(const void *d, size_t n)
{
    size_t len, i;
    char *s = Base64((const unsigned char *)d, n, &len);
    if (!s) return NULL;
    for (i = 0; i < len; i++) {
        if (s[i] == '+') s[i] = '-';
        else if (s[i] == '/') s[i] = '_';
        else if (s[i] == '=') { s[i] = 0; break; }
    }
    return s;
}

// RS256 signature with a PEM (PKCS#8) private key; returns the signature length, 0 on failure
static DWORD SignRs256(const char *pem, const char *data, size_t len, unsigned char *sig, DWORD sigCap)
{
    BYTE *der = NULL, *rsa = NULL;
    DWORD derLen = 0, rsaLen = 0, infoLen = 0;
    ULONG sigLen = 0;
    CRYPT_PRIVATE_KEY_INFO *info = NULL;
    BCRYPT_ALG_HANDLE sha = NULL, alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
    unsigned char hash[32];
    int hashed = 0;

    if (!CryptStringToBinaryA(pem, 0, CRYPT_STRING_BASE64HEADER, NULL, &derLen, NULL, NULL)) goto done;
    der = (BYTE *)malloc(derLen);
    if (!der || !CryptStringToBinaryA(pem, 0, CRYPT_STRING_BASE64HEADER, der, &derLen, NULL, NULL)) goto done;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, der, derLen,
                             CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &infoLen)) goto done;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, info->PrivateKey.pbData,
                             info->PrivateKey.cbData, CRYPT_DECODE_ALLOC_FLAG, NULL, &rsa, &rsaLen)) goto done;

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(sha, &hh, NULL, 0, NULL, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hh, (PUCHAR)data, (ULONG)len, 0)) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hh, hash, sizeof hash, 0)))
        hashed = 1;
    if (!hashed) goto done;
    // PKCS_RSA_PRIVATE_KEY decodes to a CAPI private key blob
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, NULL, 0))) goto done;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(alg, NULL, LEGACY_RSAPRIVATE_BLOB, &key, rsa, rsaLen, 0))) goto done;
    if (!BCRYPT_SUCCESS(BCryptSignHash(key, &pad, hash, sizeof hash, sig, sigCap, &sigLen, BCRYPT_PAD_PKCS1))) sigLen = 0;

done:
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (hh) BCryptDestroyHash(hh);
    if (sha) BCryptCloseAlgorithmProvider(sha, 0);
    if (rsa) { SecureZeroMemory(rsa, rsaLen); LocalFree(rsa); }
    if (info) { SecureZeroMemory(info, infoLen); LocalFree(info); }
    if (der) { SecureZeroMemory(der, derLen); free(der); }
    return sigLen;
}

// Exchanges the service account's JSON key for an access token (valid for an hour; renewed only when close to
// expiry). Caller holds g_tokenLock. Returns an error message on failure
static wchar_t *RefreshToken(const wchar_t *saFile, const wchar_t *proxy)
{
    static const char header[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    wchar_t path[MAX_PATH], *err = NULL;
    char *json, *part, claims[1024];
    unsigned char sig[512];
    DWORD sigLen, status, winErr;
    Buf email = { 0 }, key = { 0 }, project = { 0 }, jwt = { 0 }, body = { 0 }, resp = { 0 }, tok = { 0 };
    time_t now = time(NULL);
    const char *p;
    int expiresIn = 3600;

    ExeDirFile(path, MAX_PATH, saFile);
    if (g_token[0] && now < g_tokenExpire && !wcscmp(path, g_tokenFor)) return NULL;
    g_token[0] = 0;

    json = ReadFileUtf8(path);
    if (!json) return Fmt(L"Can't read the service account file: %ls (error %u)", path, GetLastError());
    JsonNextString(json, "client_email", &email);
    JsonNextString(json, "private_key", &key);
    JsonNextString(json, "project_id", &project);
    SecureZeroMemory(json, strlen(json));
    free(json);
    if (!email.p || !key.p || !project.p) { err = DupW(L"The service account file is invalid: expected a JSON key file"); goto done; }

    // JWT: header.claims.signature, each part base64url
    part = Base64Url(header, sizeof header - 1);
    BufStr(&jwt, part ? part : "");
    free(part);
    _snprintf(claims, sizeof claims,
              "{\"iss\":\"%s\",\"scope\":\"https://www.googleapis.com/auth/cloud-platform\","
              "\"aud\":\"https://oauth2.googleapis.com/token\",\"iat\":%lld,\"exp\":%lld}",
              email.p, (long long)now - 60, (long long)now + 3540);
    claims[sizeof claims - 1] = 0;
    part = Base64Url(claims, strlen(claims));
    BufStr(&jwt, ".");
    BufStr(&jwt, part ? part : "");
    free(part);
    sigLen = jwt.p ? SignRs256(key.p, jwt.p, jwt.len, sig, sizeof sig) : 0;
    if (!sigLen) { err = DupW(L"Can't use the private_key in the service account file"); goto done; }
    part = Base64Url(sig, sigLen);
    BufStr(&jwt, ".");
    BufStr(&jwt, part ? part : "");
    free(part);

    BufStr(&body, "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=");
    BufStr(&body, jwt.p);
    status = HttpPost(proxy, L"oauth2.googleapis.com", L"/token", L"Content-Type: application/x-www-form-urlencoded\r\n",
                      body.p, body.len, &resp, &winErr);
    if (status == 0) { err = Fmt(L"Can't reach Google sign-in (check network or proxy settings)%ls [%u]", L"", winErr); goto done; }
    if (resp.p) JsonNextString(resp.p, "access_token", &tok);
    if (status != 200 || !tok.p || tok.len >= sizeof g_token) {
        Buf msg = { 0 };
        wchar_t *wmsg;
        if (resp.p && !JsonNextString(resp.p, "error_description", &msg)) JsonNextString(resp.p, "error", &msg);
        wmsg = Utf8ToWide(msg.p ? msg.p : "");
        err = Fmt(L"Service account sign-in failed: %ls (HTTP %u)", wmsg ? wmsg : L"", status);
        free(wmsg);
        free(msg.p);
        goto done;
    }
    p = strstr(resp.p, "\"expires_in\"");
    if (p && (p = strchr(p, ':')) != NULL && atoi(p + 1) > 600) expiresIn = atoi(p + 1);
    memcpy(g_token, tok.p, tok.len + 1);
    g_tokenExpire = now + expiresIn - 300;
    wcscpy(g_tokenFor, path);
    MultiByteToWideChar(CP_UTF8, 0, project.p, -1, g_tokenProject, 128);
    g_tokenProject[127] = 0;

done:
    if (key.p) SecureZeroMemory(key.p, key.len);
    free(email.p);
    free(key.p);
    free(project.p);
    free(jwt.p);
    free(body.p);
    free(resp.p);
    free(tok.p);
    return err;
}

// Thread-safe: copies a valid access token and the key's project id out. Returns an error message on failure
static wchar_t *GetAccessToken(const wchar_t *saFile, const wchar_t *proxy, char *token, size_t tokenCap,
                               wchar_t *project, size_t projectCap)
{
    wchar_t *err;
    EnterCriticalSection(&g_tokenLock);
    err = RefreshToken(saFile, proxy);
    if (!err) {
        strncpy(token, g_token, tokenCap - 1);
        token[tokenCap - 1] = 0;
        wcsncpy(project, g_tokenProject, projectCap - 1);
        project[projectCap - 1] = 0;
    }
    LeaveCriticalSection(&g_tokenLock);
    return err;
}

static void InvalidateToken(void)
{
    EnterCriticalSection(&g_tokenLock);
    g_token[0] = 0;
    LeaveCriticalSection(&g_tokenLock);
}

static void VertexHost(const wchar_t *location, wchar_t *host, size_t cap)
{
    if (!_wcsicmp(location, L"global")) wcsncpy(host, L"aiplatform.googleapis.com", cap - 1);
    else _snwprintf(host, cap, L"%ls-aiplatform.googleapis.com", location);
    host[cap - 1] = 0;
}

// ---------------------------------------------------------------- Non-streaming transcription (generateContent)

static void PutLE32(unsigned char *p, DWORD v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void PutLE16(unsigned char *p, WORD v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static Job *BuildJob(void)
{
    DWORD dataBytes = (DWORD)(g_pcmLen * sizeof(short));
    unsigned char *wav = (unsigned char *)malloc(44 + dataBytes);
    char *b64, *prompt8, *gen8;
    size_t b64Len;
    Buf body = { 0 };
    Buf prompt = { 0 };
    Job *job;
    if (!wav) return NULL;

    memcpy(wav, "RIFF", 4); PutLE32(wav + 4, 36 + dataBytes); memcpy(wav + 8, "WAVEfmt ", 8);
    PutLE32(wav + 16, 16); PutLE16(wav + 20, 1); PutLE16(wav + 22, 1);
    PutLE32(wav + 24, SAMPLE_RATE); PutLE32(wav + 28, SAMPLE_RATE * 2); PutLE16(wav + 32, 2); PutLE16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); PutLE32(wav + 40, dataBytes);
    memcpy(wav + 44, g_pcm, dataBytes);
    b64 = Base64(wav, 44 + dataBytes, &b64Len);
    free(wav);
    if (!b64) return NULL;

    prompt8 = WideToUtf8(g_prompt[0] ? g_prompt : DEFAULT_PROMPT);
    BufStr(&prompt, prompt8 ? prompt8 : "");
    free(prompt8);
    if (g_vocabulary[0]) {
        char *v = WideToUtf8(g_vocabulary);
        BufStr(&prompt, "\nThese words may come up in the recording; write them exactly like this: ");
        BufStr(&prompt, v ? v : "");
        free(v);
    }

    BufStr(&body, "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"");
    JsonEscape(&body, prompt.p ? prompt.p : "");
    BufStr(&body, "\"},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
    BufAppend(&body, b64, b64Len);
    BufStr(&body, "\"}}]}]");
    free(b64);
    free(prompt.p);
    gen8 = WideToUtf8(g_genConfig);
    if (gen8 && gen8[0] == '{') {
        BufStr(&body, ",\"generationConfig\":");
        BufStr(&body, gen8);
    }
    free(gen8);
    BufStr(&body, "}");
    if (!body.p) return NULL;

    job = (Job *)calloc(1, sizeof *job);
    if (!job) { free(body.p); return NULL; }
    job->body = body.p;
    job->bodyLen = body.len;
    wcsncpy(job->model, g_model, 127);
    wcsncpy(job->fallback, g_fallbackModel, 127);
    return job;
}

static DWORD GeminiPost(const wchar_t *model, const char *token, const wchar_t *project, const char *body, size_t bodyLen,
                        Buf *resp, DWORD *winErr)
{
    wchar_t host[128], path[512], headers[2304];
    if (token) {
        VertexHost(g_location, host, 128);
        _snwprintf(path, 512, L"/v1/projects/%ls/locations/%ls/publishers/google/models/%ls:generateContent",
                   project, g_location, model);
        _snwprintf(headers, 2304, L"Content-Type: application/json\r\nAuthorization: Bearer %hs\r\n", token);
    } else {
        wcscpy(host, API_HOST);
        _snwprintf(path, 512, L"/v1beta/models/%ls:generateContent", model);
        _snwprintf(headers, 2304, L"Content-Type: application/json\r\nx-goog-api-key: %ls\r\n", g_apiKey);
    }
    path[511] = headers[2303] = 0;
    return HttpPost(g_proxy, host, path, headers, body, bodyLen, resp, winErr);
}

// Runs only while the state is ST_TRANSCRIBING, when the main thread does not touch the settings
static DWORD WINAPI TranscribeThread(LPVOID param)
{
    Job *job = (Job *)param;
    JobResult *res = (JobResult *)calloc(1, sizeof *res);
    const wchar_t *models[2];
    int i, count = 0;
    char token[2048] = "";
    wchar_t project[128] = L"";
    DWORD start = GetTickCount(), t;

    models[count++] = job->model;
    if (job->fallback[0] && wcscmp(job->fallback, job->model)) models[count++] = job->fallback;
    if (res && g_serviceAccount[0] && (res->error = GetAccessToken(g_serviceAccount, g_proxy, token, sizeof token, project, 128)) != NULL)
        count = 0;
    Log("generateContent: %u KB request (about %.1f s of audio), token %u ms", (unsigned)(job->bodyLen / 1024),
        job->bodyLen / (SAMPLE_RATE * 2 * 4 / 3.0), GetTickCount() - start);

    for (i = 0; i < count && res; i++) {
        Buf resp = { 0 };
        DWORD winErr, status;
        t = GetTickCount();
        status = GeminiPost(models[i], g_serviceAccount[0] ? token : NULL, project, job->body, job->bodyLen, &resp, &winErr);
        {
            char *m = WideToUtf8(models[i]);
            Log("  %s: HTTP %u, %u ms, tokens in %d / thinking %d / out %d", m ? m : "", status, GetTickCount() - t,
                JsonInt(resp.p, "promptTokenCount"), JsonInt(resp.p, "thoughtsTokenCount"), JsonInt(resp.p, "candidatesTokenCount"));
            if (status != 200 && resp.p) Log("    %.300s", resp.p);
            free(m);
        }
        free(res->error);
        res->error = NULL;

        if (status == 200 && resp.p) {
            // Join the text of every part in candidates
            Buf text = { 0 };
            const char *p = strstr(resp.p, "\"candidates\"");
            while (p && (p = JsonNextString(p, "text", &text)) != NULL) { }
            res->text = Utf8ToWide(text.p ? text.p : "");
            res->fallback = job->liveFailed || i > 0;
            free(text.p);
            free(resp.p);
            break;
        }

        if (status == 0) {
            res->error = Fmt(L"Can't reach Gemini (check network or proxy settings)%ls [%u]", L"", winErr);
            free(resp.p);
            break; // another model won't help with a network problem
        } else {
            Buf msg = { 0 };
            wchar_t *wmsg;
            if (resp.p) JsonNextString(resp.p, "message", &msg);
            wmsg = Utf8ToWide(msg.p ? msg.p : "");
            res->error = Fmt(L"Gemini error: %ls (HTTP %u)", wmsg ? wmsg : L"", status);
            free(wmsg);
            free(msg.p);
            free(resp.p);
            // Wrong key or no permission: another model won't help either
            if (status == 401) InvalidateToken();
            if (status == 401 || status == 403) break;
        }
    }

    SecureZeroMemory(token, sizeof token);
    free(job->body);
    free(job);
    if (res && !PostMessageW(g_main, WM_TRANSCRIBED, 0, (LPARAM)res)) {
        free(res->text);
        free(res->error);
        free(res);
    }
    return 0;
}

// ---------------------------------------------------------------- Streaming transcription (Live API)
//
// Each dictation opens its own session when the hotkey is pressed: connect (~0.3 s), setup (~0.4 s),
// activityStart, the audio as it is recorded, activityEnd, then the final inputTranscription. The audio recorded
// while connecting is sent once the session is ready, so connecting overlaps with speaking. The access token is
// fetched ahead of time (it lasts an hour) so the hotkey doesn't wait on it.

// Called on the main thread after the settings are (re)loaded
static void LiveUpdateConfig(void)
{
    LiveConfig c;
    ZeroMemory(&c, sizeof c); // compared with memcmp, so unused bytes must be zero
    c.enabled = g_serviceAccount[0] && g_liveModel[0];
    ExeDirFile(c.saFile, MAX_PATH, g_serviceAccount);
    wcsncpy(c.location, g_location, 63);
    wcsncpy(c.model, g_liveModel, 127);
    wcsncpy(c.mode, g_liveMode, 31);
    wcsncpy(c.proxy, g_proxy, 255);
    wcsncpy(c.languages, g_languages, 255);
    wcsncpy(c.vocabulary, g_vocabulary, 4095);
    if (!g_liveWake) { g_liveCfg = c; return; }
    EnterCriticalSection(&g_liveLock);
    g_liveCfg = c;
    LeaveCriticalSection(&g_liveLock);
    SetEvent(g_liveWake);
}

static void LiveSetPhase(LONG phase, int newDictation)
{
    EnterCriticalSection(&g_liveLock);
    if (newDictation) g_liveGen++;
    g_livePhase = phase;
    LeaveCriticalSection(&g_liveLock);
    SetEvent(g_liveWake);
}

static char *LiveSetupJson(const LiveConfig *c, const wchar_t *project)
{
    Buf b = { 0 };
    BufStr(&b, "{\"setup\":{\"model\":\"projects/");
    JsonEscapeW(&b, project);
    BufStr(&b, "/locations/");
    JsonEscapeW(&b, c->location);
    BufStr(&b, "/publishers/google/models/");
    JsonEscapeW(&b, c->model);
    BufStr(&b, "\",\"generationConfig\":{\"responseModalities\":[\"TEXT\"]},\"inputAudioTranscription\":{");
    if (c->mode[0]) {
        BufStr(&b, "\"mode\":\"");
        JsonEscapeW(&b, c->mode);
        BufStr(&b, "\"");
    }
    {
        Buf list = { 0 };
        if (JsonList(&list, c->languages)) {
            BufStr(&b, c->mode[0] ? ",\"languageCodes\":" : "\"languageCodes\":");
            BufStr(&b, list.p);
        }
        free(list.p);
    }
    {
        Buf list = { 0 };
        if (JsonList(&list, c->vocabulary)) {
            BufStr(&b, b.p[b.len - 1] == '{' ? "\"customVocabulary\":" : ",\"customVocabulary\":");
            BufStr(&b, list.p);
        }
        free(list.p);
    }
    // We say when speech starts and ends (activityStart/activityEnd) instead of the server detecting it
    BufStr(&b, "},\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}}}}");
    return b.p;
}

static int LiveSend(LiveSession *s, const char *json, size_t len)
{
    return s && s->ws && !s->dead &&
           WinHttpWebSocketSend(s->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json, (DWORD)len) == NO_ERROR;
}

static void LiveOnMessage(LiveSession *s, const char *m)
{
    const char *p;
    if (strstr(m, "\"setupComplete\"")) {
        InterlockedExchange(&s->ok, 1);
        SetEvent(s->ready);
    }
    // The final text; partial results come as "interimInputTranscription" and are ignored
    if ((p = strstr(m, "\"inputTranscription\"")) != NULL) JsonNextString(p, "text", &s->text);
    if (strstr(m, "\"goAway\"")) InterlockedExchange(&s->expiring, 1);
    if (s->ended && (strstr(m, "\"generationComplete\"") || strstr(m, "\"turnComplete\""))) {
        InterlockedExchange(&s->complete, 1);
        SetEvent(s->done);
    }
}

static DWORD WINAPI LiveRecvThread(LPVOID param)
{
    LiveSession *s = (LiveSession *)param;
    Buf msg = { 0 };
    BYTE chunk[8192];
    for (;;) {
        DWORD got = 0, err;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        err = WinHttpWebSocketReceive(s->ws, chunk, sizeof chunk, &got, &type);
        if (err != NO_ERROR) {
            if (!s->error[0]) _snprintf(s->error, sizeof s->error - 1, "connection lost (%lu)", err);
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code = 0;
            char reason[124];
            DWORD rlen = 0;
            if (WinHttpWebSocketQueryCloseStatus(s->ws, &code, reason, sizeof reason - 1, &rlen) != NO_ERROR) rlen = 0;
            reason[rlen] = 0;
            _snprintf(s->error, sizeof s->error - 1, "closed by the server (%u) %s", code, reason);
            break;
        }
        BufAppend(&msg, chunk, got);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
        if (msg.p) LiveOnMessage(s, msg.p);
        msg.len = 0;
    }
    free(msg.p);
    InterlockedExchange(&s->dead, 1);
    SetEvent(s->ready);
    SetEvent(s->done);
    return 0;
}

static void LiveClose(LiveSession *s)
{
    if (!s) return;
    if (s->ws) {
        WinHttpWebSocketShutdown(s->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(s->ws); // also ends the blocked receive
    }
    if (s->thread) {
        if (WaitForSingleObject(s->thread, 5000) != WAIT_OBJECT_0) return; // still running: leak rather than free under it
        CloseHandle(s->thread);
    }
    if (s->con) WinHttpCloseHandle(s->con);
    if (s->ses) WinHttpCloseHandle(s->ses);
    if (s->ready) CloseHandle(s->ready);
    if (s->done) CloseHandle(s->done);
    free(s->text.p);
    free(s);
}

// Connects, authenticates and sends the setup; blocks until the session is ready. Never returns NULL unless out
// of memory; on failure the session has dead set and error filled in
static LiveSession *LiveOpen(const LiveConfig *cfg)
{
    LiveSession *s = (LiveSession *)calloc(1, sizeof *s);
    char token[2048], *setup;
    wchar_t project[128], host[128], headers[2304], *err;
    HINTERNET req = NULL;
    DWORD status = 0, sz = sizeof status;

    if (!s) return NULL;
    s->cfg = *cfg;
    s->openedAt = GetTickCount();
    s->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->done = CreateEventW(NULL, TRUE, FALSE, NULL);

    err = GetAccessToken(cfg->saFile, cfg->proxy, token, sizeof token, project, 128);
    if (err) {
        WideCharToMultiByte(CP_UTF8, 0, err, -1, s->error, sizeof s->error - 1, NULL, NULL);
        free(err);
        goto fail;
    }
    VertexHost(cfg->location, host, 128);
    s->ses = OpenWinHttp(cfg->proxy);
    if (!s->ses) goto netfail;
    // No receive timeout: the socket is quiet while the user talks; LiveFinish bounds the wait for the result
    WinHttpSetTimeouts(s->ses, 15000, 15000, 15000, 0);
    s->con = WinHttpConnect(s->ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!s->con) goto netfail;
    req = WinHttpOpenRequest(s->con, L"GET", LIVE_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req || !WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) goto netfail;
    _snwprintf(headers, 2304, L"Authorization: Bearer %hs\r\n", token);
    headers[2303] = 0;
    SecureZeroMemory(token, sizeof token);
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto netfail;
    SecureZeroMemory(headers, sizeof headers);
    if (!WinHttpReceiveResponse(req, NULL)) goto netfail;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        if (status == 401) InvalidateToken();
        _snprintf(s->error, sizeof s->error - 1, "WebSocket upgrade refused (HTTP %lu)", status);
        goto fail;
    }
    s->ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (!s->ws) goto netfail;
    WinHttpCloseHandle(req);
    req = NULL;
    s->thread = CreateThread(NULL, 0, LiveRecvThread, s, 0, NULL);
    if (!s->thread) goto netfail;

    setup = LiveSetupJson(cfg, project);
    if (!setup || !LiveSend(s, setup, strlen(setup))) {
        free(setup);
        if (!s->error[0]) strcpy(s->error, "can't send the setup");
        goto fail;
    }
    free(setup);
    if (WaitForSingleObject(s->ready, 20000) != WAIT_OBJECT_0 || !s->ok) {
        if (!s->error[0]) strcpy(s->error, "no setupComplete from the server");
        goto fail;
    }
    return s;

netfail:
    if (!s->error[0]) _snprintf(s->error, sizeof s->error - 1, "can't connect (check network or proxy settings) [%lu]", GetLastError());
fail:
    SecureZeroMemory(token, sizeof token);
    if (req) WinHttpCloseHandle(req);
    InterlockedExchange(&s->dead, 1);
    Log("live: can't open a session: %s", s->error);
    return s;
}

// Sends everything recorded so far for dictation gen; returns 0 if the connection failed
static int LiveSendPcm(LiveSession *s, LONG gen, size_t *sent)
{
    static short chunk[SAMPLE_RATE]; // up to 1 s per message (only the live thread uses this)
    static const char head[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    for (;;) {
        size_t n = 0, b64Len;
        char *b64;
        Buf msg = { 0 };
        int ok;
        EnterCriticalSection(&g_liveLock);
        if (g_liveGen == gen && g_livePhase != LIVE_IDLE && g_pcmLen > *sent) {
            n = g_pcmLen - *sent;
            if (n > SAMPLE_RATE) n = SAMPLE_RATE;
            memcpy(chunk, g_pcm + *sent, n * sizeof(short));
        }
        LeaveCriticalSection(&g_liveLock);
        if (!n) return 1;
        b64 = Base64((const unsigned char *)chunk, n * sizeof(short), &b64Len);
        BufStr(&msg, head);
        if (b64) BufAppend(&msg, b64, b64Len);
        BufStr(&msg, "\"}}}");
        ok = b64 && msg.p && LiveSend(s, msg.p, msg.len);
        free(b64);
        free(msg.p);
        if (!ok) return 0;
        *sent += n;
    }
}

// Ends the dictation and waits for the final transcript. A result without text means "fall back"
static JobResult *LiveFinish(LiveSession *s, int failed, size_t samples)
{
    static const char end[] = "{\"realtimeInput\":{\"activityEnd\":{}}}";
    JobResult *res = (JobResult *)calloc(1, sizeof *res);
    DWORD t = GetTickCount();
    if (!res) return NULL;
    if (!failed && s && s->ok && !s->dead) {
        InterlockedExchange(&s->ended, 1);
        if (LiveSend(s, end, sizeof end - 1))
            WaitForSingleObject(s->done, 10000 + (DWORD)(samples / SAMPLE_RATE) * 100);
    }
    if (s && s->complete) {
        res->text = Utf8ToWide(s->text.p ? s->text.p : "");
        Log("live: transcript %u ms after activityEnd (%.1f s of audio)", GetTickCount() - t, samples / (double)SAMPLE_RATE);
    } else {
        const char *why = s && s->error[0] ? s->error : (failed ? "the session failed" : "timed out");
        res->error = Utf8ToWide(why);
        Log("live: failed (%s)", why);
    }
    return res;
}

static DWORD WINAPI LiveThread(LPVOID unused)
{
    LiveSession *active = NULL;
    LiveConfig *cfg = (LiveConfig *)malloc(sizeof *cfg);
    LONG gen, phase, cur = 0, finished = 0;
    size_t sent = 0;
    int failed = 0, tokenOk = 1;
    DWORD tokenTry = 0;
    (void)unused;
    if (!cfg) return 0;

    for (;;) {
        WaitForSingleObject(g_liveWake, cur ? 50 : 1000);
        if (g_liveQuit) break;
        EnterCriticalSection(&g_liveLock);
        *cfg = g_liveCfg;
        gen = g_liveGen;
        phase = g_livePhase;
        LeaveCriticalSection(&g_liveLock);

        // A new dictation: open a session now, while the audio piles up
        if (!cur && phase != LIVE_IDLE && gen != finished) {
            static const char start[] = "{\"realtimeInput\":{\"activityStart\":{}}}";
            cur = gen;
            sent = 0;
            failed = 0;
            active = LiveOpen(cfg);
            if (active && active->ok) Log("live: session opened in %u ms", GetTickCount() - active->openedAt);
            if (!active || active->dead || !LiveSend(active, start, sizeof start - 1)) failed = 1;
            continue; // opening can take seconds: read the phase again
        }

        if (cur) {
            if (gen != cur || phase == LIVE_IDLE) { // cancelled
                LiveClose(active);
                active = NULL;
                finished = cur;
                cur = 0;
                continue;
            }
            if (!failed && !LiveSendPcm(active, cur, &sent)) failed = 1;
            if (phase == LIVE_FINISHING) {
                // The main thread stopped the recording before setting FINISHING, so all audio has been sent
                JobResult *res = LiveFinish(active, failed, sent);
                if (res && !PostMessageW(g_main, WM_LIVE_DONE, (WPARAM)cur, (LPARAM)res)) {
                    free(res->text);
                    free(res->error);
                    free(res);
                }
                LiveClose(active);
                active = NULL;
                finished = cur;
                cur = 0;
            }
            continue;
        }

        // Idle: keep the access token fresh (a no-op while the cached one has more than 5 minutes left)
        if (cfg->enabled && (tokenOk || GetTickCount() - tokenTry > TOKEN_RETRY_MS)) {
            char token[2048];
            wchar_t project[128], *err = GetAccessToken(cfg->saFile, cfg->proxy, token, sizeof token, project, 128);
            SecureZeroMemory(token, sizeof token);
            if (err && tokenOk) Log("token: can't refresh: %ls", err);
            if (!err && !tokenOk) Log("token: refreshed");
            tokenOk = !err;
            tokenTry = GetTickCount();
            free(err);
        }
    }
    LiveClose(active);
    free(cfg);
    return 0;
}

// ---------------------------------------------------------------- Clipboard + paste

static int OpenClipboardRetry(void)
{
    int i;
    for (i = 0; i < 20; i++) {
        if (OpenClipboard(g_main)) return 1;
        Sleep(25);
    }
    return 0;
}

static int IsHandleFormat(UINT f)
{
    // These formats hold GDI handles and the like, not HGLOBALs, so they can't be copied byte for byte
    switch (f) {
    case CF_BITMAP: case CF_DSPBITMAP: case CF_PALETTE: case CF_METAFILEPICT: case CF_DSPMETAFILEPICT:
    case CF_ENHMETAFILE: case CF_DSPENHMETAFILE: case CF_OWNERDISPLAY:
        return 1;
    }
    return (f >= CF_PRIVATEFIRST && f <= CF_PRIVATELAST) || (f >= CF_GDIOBJFIRST && f <= CF_GDIOBJLAST);
}

static void BackupClipboard(void)
{
    UINT f = 0;
    SIZE_T total = 0;
    if (!OpenClipboardRetry()) return;
    while ((f = EnumClipboardFormats(f)) != 0 && g_clipCount < 64) {
        HANDLE h;
        SIZE_T sz;
        void *src;
        HGLOBAL copy;
        if (IsHandleFormat(f)) continue;
        h = GetClipboardData(f);
        if (!h) continue;
        sz = GlobalSize(h);
        if (sz == 0 || total + sz > 64u * 1024 * 1024) continue;
        src = GlobalLock(h);
        if (!src) continue;
        copy = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (copy) {
            memcpy(GlobalLock(copy), src, sz);
            GlobalUnlock(copy);
            g_clip[g_clipCount].fmt = f;
            g_clip[g_clipCount].data = copy;
            g_clipCount++;
            total += sz;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static void RestoreClipboard(void)
{
    int i;
    g_clipPending = 0;
    if (g_clipCount == 0) return;
    if (OpenClipboardRetry()) {
        EmptyClipboard();
        for (i = 0; i < g_clipCount; i++)
            if (!SetClipboardData(g_clip[i].fmt, g_clip[i].data)) GlobalFree(g_clip[i].data);
        CloseClipboard();
    } else {
        for (i = 0; i < g_clipCount; i++) GlobalFree(g_clip[i].data);
    }
    g_clipCount = 0;
}

static int SetClipboardText(const wchar_t *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes), ex;
    UINT exclude;
    if (!h) return 0;
    memcpy(GlobalLock(h), text, bytes);
    GlobalUnlock(h);
    if (!OpenClipboardRetry()) { GlobalFree(h); return 0; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) { GlobalFree(h); CloseClipboard(); return 0; }
    // Keep it out of Win+V clipboard history and cloud sync
    exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    ex = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (ex) {
        *(DWORD *)GlobalLock(ex) = 0;
        GlobalUnlock(ex);
        if (!SetClipboardData(exclude, ex)) GlobalFree(ex);
    }
    CloseClipboard();
    return 1;
}

static void KeyEvent(INPUT *in, WORD vk, DWORD flags)
{
    ZeroMemory(in, sizeof *in);
    in->type = INPUT_KEYBOARD;
    in->ki.wVk = vk;
    in->ki.dwFlags = flags;
}

static void WaitModifiersReleased(void)
{
    // The user may still be holding the hotkey's Ctrl/Alt/Shift; sending Ctrl+V now would become another shortcut
    static const int mods[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
    DWORD until = GetTickCount() + 1500;
    int i, down;
    while (GetTickCount() < until) {
        down = 0;
        for (i = 0; i < 5; i++) if (GetAsyncKeyState(mods[i]) & 0x8000) down = 1;
        if (!down) return;
        Sleep(20);
    }
    for (i = 0; i < 3; i++) {
        if (GetAsyncKeyState(mods[i]) & 0x8000) {
            INPUT in;
            KeyEvent(&in, (WORD)mods[i], KEYEVENTF_KEYUP);
            SendInput(1, &in, sizeof in);
        }
    }
}

static int Paste(const wchar_t *text)
{
    INPUT in[4];
    WaitModifiersReleased();
    if (g_target && GetForegroundWindow() != g_target) SetForegroundWindow(g_target);

    // If the previous clipboard hasn't been restored yet, don't back up again (it would save the dictated text)
    if (g_restoreClipboard && !g_clipPending) BackupClipboard();
    if (!SetClipboardText(text)) return 0;

    KeyEvent(&in[0], VK_CONTROL, 0);
    KeyEvent(&in[1], 'V', 0);
    KeyEvent(&in[2], 'V', KEYEVENTF_KEYUP);
    KeyEvent(&in[3], VK_CONTROL, KEYEVENTF_KEYUP);
    SendInput(4, in, sizeof(INPUT));

    if (g_clipCount > 0) {
        // Give the target app a moment to read the clipboard, then put the old contents back
        g_clipPending = 1;
        SetTimer(g_main, TIMER_RESTORE_CLIP, 800, NULL);
    }
    return 1;
}

// ---------------------------------------------------------------- Flow

static void EndRecordingUi(void)
{
    UnregisterHotKey(g_main, HOTKEY_ESC);
    KillTimer(g_main, TIMER_TICK);
}

static void StartRestJob(int liveFailed)
{
    Job *job = BuildJob();
    if (!job) {
        g_state = ST_IDLE;
        ShowOverlay(L"Out of memory", 1);
        HideLater(3000);
        return;
    }
    job->liveFailed = liveFailed;
    CloseHandle(CreateThread(NULL, 0, TranscribeThread, job, 0, NULL));
}

static void FinishRecording(void)
{
    KillTimer(g_main, TIMER_STOP_TAIL);
    g_stopping = 0;
    StopRecording();
    EndRecordingUi();
    if (g_pcmLen < MIN_SAMPLES) {
        if (g_useLive) LiveSetPhase(LIVE_IDLE, 0);
        g_state = ST_IDLE;
        ShowOverlay(L"Too short, not sent", 0);
        HideLater(1000);
        return;
    }
    g_state = ST_TRANSCRIBING;
    ShowOverlay(L"… Transcribing", 0);
    if (g_useLive) {
        g_waitLive = 1;
        // In case the live thread is stuck (e.g. a dead connection), fall back on our own
        SetTimer(g_main, TIMER_LIVE_WATCHDOG, 30000 + (UINT)(g_pcmLen / SAMPLE_RATE) * 200, NULL);
        LiveSetPhase(LIVE_FINISHING, 0);
    } else {
        StartRestJob(0);
    }
}

static void OnToggle(void)
{
    if (g_state == ST_IDLE) {
        LoadConfig();
        g_target = GetForegroundWindow();
        if (!g_apiKey[0] && !g_serviceAccount[0]) {
            ShowOverlay(L"No API key yet: the settings file is open, fill in ApiKey or ServiceAccount and save", 1);
            HideLater(5000);
            OpenSettings();
            return;
        }
        if (!StartRecording()) {
            ShowOverlay(L"Can't open the microphone: check Settings > Privacy > Microphone allows desktop apps", 1);
            HideLater(5000);
            return;
        }
        g_state = ST_RECORDING;
        g_overlayError = 0;
        g_recordStart = GetTickCount();
        g_useLive = g_serviceAccount[0] && g_liveModel[0];
        if (g_useLive) LiveSetPhase(LIVE_RECORDING, 1);
        RegisterHotKey(g_main, HOTKEY_ESC, MOD_NOREPEAT, VK_ESCAPE);
        SetTimer(g_main, TIMER_TICK, 250, NULL);
        PlaceOverlay();
    } else if (g_state == ST_RECORDING && !g_stopping) {
        g_stopping = 1;
        g_stopTick = GetTickCount();
        SetTimer(g_main, TIMER_STOP_TAIL, STOP_TAIL_MS, NULL);
    }
    // A hotkey press while transcribing does nothing
}

static void OnCancel(void)
{
    if (g_state != ST_RECORDING) return;
    KillTimer(g_main, TIMER_STOP_TAIL);
    g_stopping = 0;
    StopRecording();
    EndRecordingUi();
    if (g_useLive) LiveSetPhase(LIVE_IDLE, 0);
    g_state = ST_IDLE;
    ShowOverlay(L"Cancelled", 0);
    HideLater(800);
}

static void OnTranscribed(JobResult *res)
{
    g_state = ST_IDLE;
    if (res->error) {
        ShowOverlay(res->error, 1);
        HideLater(6000);
    } else {
        TrimW(res->text);
        if (!res->text || !res->text[0]) {
            ShowOverlay(L"Nothing recognized", 0);
            HideLater(1200);
        } else if (!Paste(res->text)) {
            ShowOverlay(L"Couldn't write to the clipboard", 1);
            HideLater(3000);
        } else {
            // The text itself is the feedback: just get out of the way
            KillTimer(g_main, TIMER_HIDE);
            ShowWindow(g_overlay, SW_HIDE);
            Log("pasted %u ms after stop%s", GetTickCount() - g_stopTick, res->fallback ? " (fallback)" : "");
        }
    }
    free(res->text);
    free(res->error);
    free(res);
}

static void OnLiveDone(LONG gen, JobResult *res)
{
    if (!g_waitLive || gen != g_liveGen) { // late result after the watchdog already fell back
        free(res->text);
        free(res->error);
        free(res);
        return;
    }
    g_waitLive = 0;
    KillTimer(g_main, TIMER_LIVE_WATCHDOG);
    LiveSetPhase(LIVE_IDLE, 0);
    if (res->text) {
        OnTranscribed(res);
        return;
    }
    free(res->error);
    free(res);
    StartRestJob(1);
}

static void OnLiveWatchdog(void)
{
    KillTimer(g_main, TIMER_LIVE_WATCHDOG);
    if (!g_waitLive) return;
    g_waitLive = 0;
    Log("live: no result in time, falling back to generateContent");
    LiveSetPhase(LIVE_IDLE, 0);
    StartRestJob(1);
}

// ---------------------------------------------------------------- Tray

// app.ico embedded through app.rc (white microphone on blue)
static HICON AppIcon(int metricX, int metricY)
{
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                             GetSystemMetrics(metricX), GetSystemMetrics(metricY), LR_SHARED);
}

static void AddTray(int balloon)
{
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = AppIcon(SM_CXSMICON, SM_CYSMICON);
    _snwprintf(g_nid.szTip, 128, L"Gemini Dictation (%ls)", g_hotkey);
    g_nid.szTip[127] = 0;
    if (balloon) {
        g_nid.uFlags |= NIF_INFO;
        wcsncpy(g_nid.szInfoTitle, L"Gemini Dictation is running", 63);
        _snwprintf(g_nid.szInfo, 256, L"Fill in ApiKey or ServiceAccount in the settings, then press %ls in any text box "
                   L"to start talking, press it again to finish, Esc to cancel.", g_hotkey);
        g_nid.szInfo[255] = 0;
        g_nid.dwInfoFlags = NIIF_INFO;
    }
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowTrayMenu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    UINT cmd;
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Edit settings…");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"Reload settings");
    AppendMenuW(menu, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(menu);
    PostMessageW(g_main, WM_NULL, 0, 0);

    switch (cmd) {
    case IDM_SETTINGS:
        OpenSettings();
        break;
    case IDM_RELOAD:
        if (g_state != ST_IDLE) break; // the transcription thread reads the settings
        LoadConfig();
        RegisterToggleHotkey();
        ShowOverlay(g_hotkeyOk ? L"Settings reloaded" : L"Hotkey unavailable (invalid or used by another program)", !g_hotkeyOk);
        HideLater(g_hotkeyOk ? 1200 : 4000);
        break;
    case IDM_AUTOSTART:
        SetAutostart(!IsAutostart());
        break;
    case IDM_EXIT:
        DestroyWindow(g_main);
        break;
    }
}

// ---------------------------------------------------------------- Main window (hidden, only receives messages)

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_HOTKEY:
        if (w == HOTKEY_TOGGLE) OnToggle();
        else if (w == HOTKEY_ESC) OnCancel();
        return 0;
    case MM_WIM_DATA:
        if (g_state == ST_RECORDING && g_wi && (HWAVEIN)w == g_wi) {
            Harvest(1);
            InvalidateRect(g_overlay, NULL, FALSE);
            if (g_pcmLen >= (size_t)MAX_SECONDS * SAMPLE_RATE) OnToggle(); // limit reached: stop and transcribe
        }
        return 0;
    case WM_TRANSCRIBED:
        OnTranscribed((JobResult *)l);
        return 0;
    case WM_LIVE_DONE:
        OnLiveDone((LONG)w, (JobResult *)l);
        return 0;
    case WM_TIMER:
        if (w == TIMER_HIDE) {
            KillTimer(h, TIMER_HIDE);
            if (g_state == ST_IDLE) ShowWindow(g_overlay, SW_HIDE);
        } else if (w == TIMER_RESTORE_CLIP) {
            KillTimer(h, TIMER_RESTORE_CLIP);
            RestoreClipboard();
        } else if (w == TIMER_TICK) {
            InvalidateRect(g_overlay, NULL, FALSE);
        } else if (w == TIMER_STOP_TAIL) {
            if (g_stopping) FinishRecording();
        } else if (w == TIMER_LIVE_WATCHDOG) {
            OnLiveWatchdog();
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) ShowTrayMenu();
        else if (LOWORD(l) == WM_LBUTTONDBLCLK) OpenSettings();
        return 0;
    case WM_DESTROY:
        if (g_state == ST_RECORDING) StopRecording();
        if (g_clipPending) RestoreClipboard();
        InterlockedExchange(&g_liveQuit, 1);
        SetEvent(g_liveWake);
        UnregisterHotKey(h, HOTKEY_TOGGLE);
        UnregisterHotKey(h, HOTKEY_ESC);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (g_taskbarCreated && m == g_taskbarCreated) {
        AddTray(0); // the tray icon has to be added again after Explorer restarts
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show)
{
    WNDCLASSEXW wc;
    HDC screen;
    MSG msg;
    HMODULE dwm;
    wchar_t ini[MAX_PATH];
    int firstRun;
    (void)prev;
    (void)cmdLine;
    (void)show;
    g_inst = inst;

    CreateMutexW(NULL, TRUE, L"Local\\GeminiDictation");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Already running: look for the microphone icon in the system tray.", APP_TITLE, MB_ICONINFORMATION);
        return 0;
    }

    SetProcessDPIAware();
    screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);

    InitializeCriticalSection(&g_liveLock);
    InitializeCriticalSection(&g_tokenLock);
    IniPath(ini, MAX_PATH);
    firstRun = GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES;
    if (firstRun) WriteDefaultIni(ini);
    LoadConfig();

    g_font = CreateFontW(-MulDiv(11, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = AppIcon(SM_CXICON, SM_CYICON);
    wc.hIconSm = AppIcon(SM_CXSMICON, SM_CYSMICON);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"GeminiDictationMain";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = L"GeminiDictationOverlay";
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(0, L"GeminiDictationMain", APP_TITLE, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    // Topmost, never takes focus, click-through, not on the taskbar
    g_overlay = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                L"GeminiDictationOverlay", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    SetLayeredWindowAttributes(g_overlay, 0, 240, LWA_ALPHA);
    dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT(WINAPI * DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
        DwmSetAttr set = (DwmSetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        int round = 2; // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND (rounded corners on Windows 11)
        if (set) set(g_overlay, 33, &round, sizeof round);
    }

    // Opens and authenticates the first live session right away
    g_liveWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, LiveThread, NULL, 0, NULL));

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray(firstRun);
    RegisterToggleHotkey();
    if (!g_hotkeyOk) {
        wchar_t t[160];
        _snwprintf(t, 160, L"Hotkey %ls is unavailable (invalid or in use); right-click the tray icon to change it", g_hotkey);
        t[159] = 0;
        ShowOverlay(t, 1);
        HideLater(6000);
    }
    if (firstRun) {
        SetAutostart(1);
        OpenSettings();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
