/* Portable install support.

   Everything the program writes lives next to the executable: saves/ (game
   saves, quicksaves, settings, log) and game.ini, which holds:
       rom=<disc image file or the folder that contains it>
       check_updates=1        (0: never look for a newer release)
       auto_update=0          (1: install a newer release without asking)
       update_repo=owner/name (optional: check another GitHub repository)
   The installer writes rom= (it asks for the disc image); a portable copy
   asks with a file dialog the first time the image is not found.

   Update check: on start the program asks GitHub for the latest release of
   GAME_UPDATE_REPO (game.json "update_repo").  If it is newer than
   GAME_VERSION and has a "*-portable.zip" asset, the zip is downloaded to
   update/, update.ps1 is started to replace the program files once this
   process has exited (saves/, rom/ and game.ini are kept), and the program
   exits; update.ps1 starts the new version. */
#include "runtime.h"
#include "game_config.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comdlg32.lib")

extern int g_headless;

/* ---------------------------------------------------------------- game.ini */
static char ini_path[600];

void portable_init(const char *exe_dir) {
    snprintf(ini_path, sizeof ini_path, "%s\\game.ini", exe_dir);
}

/* value of key in game.ini, or def */
static void ini_get(const char *key, char *out, size_t n, const char *def) {
    snprintf(out, n, "%s", def);
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[1024];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (!_strnicmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1;
            v[strcspn(v, "\r\n")] = 0;
            snprintf(out, n, "%s", v);
            break;
        }
    }
    fclose(f);
}

/* set key=value in game.ini, keeping the other lines */
static void ini_set(const char *key, const char *value) {
    char *lines[64];
    int nl = 0, done = 0;
    size_t kl = strlen(key);
    FILE *f = fopen(ini_path, "r");
    char line[1024];
    while (f && nl < 63 && fgets(line, sizeof line, f)) {
        if (!_strnicmp(line, key, kl) && line[kl] == '=') {
            snprintf(line, sizeof line, "%s=%s\n", key, value);
            done = 1;
        }
        lines[nl++] = _strdup(line);
    }
    if (f) fclose(f);
    if (!done) {
        snprintf(line, sizeof line, "%s=%s\n", key, value);
        lines[nl++] = _strdup(line);
    }
    f = fopen(ini_path, "w");
    for (int i = 0; i < nl; i++) {
        if (f) fputs(lines[i], f);
        free(lines[i]);
    }
    if (f) fclose(f);
}

/* the disc image configured in game.ini (file or folder), "" if none */
void portable_rom_path(char *out, size_t n) { ini_get("rom", out, n, ""); }

/* first run of a portable copy: ask where the disc image is */
int portable_ask_rom(char *out, size_t n) {
    if (g_headless) return -1;
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "CD-i disc image (*.cue, *.bin)\0*.cue;*.bin\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrTitle = "Where is the " GAME_TITLE " disc image (.cue or .bin)?";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return -1;
    snprintf(out, n, "%s", file);
    return 0;
}

void portable_save_rom(const char *path) { ini_set("rom", path); }

/* ---------------------------------------------------------------- update check */
static int http_get(const wchar_t *host, const wchar_t *path, char **body, size_t *len, size_t max) {
    int rc = -1;
    *body = NULL;
    *len = 0;
    HINTERNET s = WinHttpOpen(L"cdi-recomp-updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"cdi-recomp-updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 4000, 4000, 4000, 30000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            size_t cap = 1 << 16;
            char *buf = (char *)malloc(cap + 1);
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                if (*len + avail > max) { free(buf); buf = NULL; break; }
                if (*len + avail > cap) {
                    while (*len + avail > cap) cap *= 2;
                    buf = (char *)realloc(buf, cap + 1);
                }
                if (!WinHttpReadData(r, buf + *len, avail, &got)) break;
                *len += got;
            }
            if (buf) { buf[*len] = 0; *body = buf; rc = 0; }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return rc;
}

/* "x.y.z" (optionally "v"-prefixed) -> comparable number */
static long long version_key(const char *v) {
    while (*v && !isdigit((unsigned char)*v)) v++;
    long long k = 0;
    for (int part = 0; part < 3; part++) {
        long n = strtol(v, (char **)&v, 10);
        k = k * 100000 + n;
        if (*v == '.') v++;
    }
    return k;
}

/* "key": "value" in a JSON text, starting at p */
static int json_string(const char *p, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return -1;
    k = strchr(k + strlen(pat), '"');
    if (!k) return -1;
    const char *e = strchr(k + 1, '"');
    if (!e || (size_t)(e - k - 1) >= n) return -1;
    memcpy(out, k + 1, (size_t)(e - k - 1));
    out[e - k - 1] = 0;
    return 0;
}

static int download(const char *url, const char *dest) {
    wchar_t wurl[1024], host[256], path[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return -1;
    char *body; size_t len;
    if (http_get(host, path, &body, &len, (size_t)512 << 20)) return -1;   /* redirects are followed */
    FILE *f = fopen(dest, "wb");
    int ok = f && fwrite(body, 1, len, f) == len;
    if (f) fclose(f);
    free(body);
    return ok ? 0 : -1;
}

/* Returns 1 if an update was started (the caller exits), 0 otherwise. */
int portable_check_update(const char *exe_dir) {
    char repo[128], v[32];
    ini_get("update_repo", repo, sizeof repo, GAME_UPDATE_REPO);   /* game.ini may name another repository */
    if (g_headless || !repo[0]) return 0;
    ini_get("check_updates", v, sizeof v, "1");
    if (v[0] == '0') return 0;
    wchar_t path[300];
    swprintf(path, 300, L"/repos/%hs/releases/latest", repo);
    char *body; size_t len;
    if (http_get(L"api.github.com", path, &body, &len, 4 << 20)) {
        LOG("update check: no answer from GitHub (offline?)");
        return 0;
    }
    char tag[64] = "", url[1024] = "";
    json_string(body, "tag_name", tag, sizeof tag);
    /* the portable zip among the release's assets */
    for (const char *p = body; (p = strstr(p, "\"browser_download_url\"")) != NULL; p++) {
        char u[1024];
        if (!json_string(p, "browser_download_url", u, sizeof u) && strstr(u, "-portable.zip")) {
            snprintf(url, sizeof url, "%s", u);
            break;
        }
    }
    free(body);
    if (!tag[0] || version_key(tag) <= version_key(GAME_VERSION)) {
        LOG("update check: %s is current (latest release %s)", GAME_VERSION, tag[0] ? tag : "?");
        return 0;
    }
    if (!url[0]) { LOG("update check: release %s has no *-portable.zip asset", tag); return 0; }
    char msg[512];
    ini_get("auto_update", v, sizeof v, "0");
    if (v[0] != '1') {
        snprintf(msg, sizeof msg, "A newer version of " GAME_TITLE " is available: %s (this is %s).\n\n"
                 "Download and install it now? Your saves and settings are kept.", tag, GAME_VERSION);
        if (MessageBoxA(NULL, msg, GAME_TITLE " update", MB_YESNO | MB_ICONINFORMATION) != IDYES) return 0;
    }
    char dir[600], zip[700];
    snprintf(dir, sizeof dir, "%s\\update", exe_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(zip, sizeof zip, "%s\\package.zip", dir);
    LOG("update: downloading %s", url);
    if (download(url, zip)) {
        MessageBoxA(NULL, "The update could not be downloaded. The current version starts instead.",
                    GAME_TITLE " update", MB_OK | MB_ICONWARNING);
        return 0;
    }
    /* update.ps1 waits for this process to exit, replaces the files, restarts */
    char exe[600], cmd[2400];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd,
             "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\\update.ps1\" "
             "-Zip \"%s\" -Dir \"%s\" -WaitPid %lu -Relaunch \"%s\"",
             exe_dir, zip, exe_dir, (unsigned long)GetCurrentProcessId(), exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, exe_dir, &si, &pi)) {
        MessageBoxA(NULL, "update.ps1 could not be started. The current version starts instead.",
                    GAME_TITLE " update", MB_OK | MB_ICONWARNING);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG("update: installing %s", tag);
    return 1;
}
#endif
