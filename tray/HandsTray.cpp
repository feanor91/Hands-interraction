// HandsTray.cpp — icône de la barre des tâches pour la couche OpenXR « mains nues ».
//
// Rôle : un interrupteur (suivi activé / désactivé) + raccourcis (journaux, hands.ini)
// + installation / désinstallation de la couche (élévation UAC à la demande).
//
// L'interrupteur écrit `enabled = true|false` dans C:\ProgramData\HandsLayer\hands.ini
// (modifiable sans droits administrateur). La couche lit cette valeur à la création de
// l'instance OpenXR : le changement est donc effectif au PROCHAIN lancement du jeu.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>

#include "config/IniEdit.h"
#include "config/IniFile.h"

namespace {

constexpr wchar_t kWndClass[] = L"HandsLayerTrayWnd";
constexpr wchar_t kMutexName[] = L"Local\\HandsLayerTraySingleton";
constexpr wchar_t kInstallDir[] = L"C:\\ProgramData\\HandsLayer";
constexpr wchar_t kLayerJson[] = L"C:\\ProgramData\\HandsLayer\\XR_APILAYER_HANDS_bare.json";
constexpr wchar_t kGameExe[] = L"FlightSimulator2024.exe";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"HandsLayerTray";
constexpr wchar_t kTitle[] = L"Mains nues OpenXR";

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTrayId = 1;

enum Cmd : UINT {
    ID_TOGGLE = 100, ID_INSTALL, ID_UNINSTALL, ID_LOGS, ID_INI, ID_AUTOSTART, ID_QUIT,
    ID_CLICK_SIDE, ID_CLICK_PINCH, ID_CLICK_MIDDLE, ID_HIDE_MODEL, ID_HANDS_DRAW, ID_EMULATION,
    ID_PTR_HAND, ID_PTR_FINGER, ID_PTR_UP, ID_PTR_DOWN, ID_PTR_LEFT, ID_PTR_RIGHT, ID_PTR_RESET
};

HWND g_hwnd = nullptr;
HICON g_iconOn = nullptr, g_iconOff = nullptr, g_iconWarn = nullptr;
UINT g_taskbarCreated = 0;

struct State {
    bool installed = false;  // DLL présente et couche déclarée dans le registre
    bool enabled = true;     // valeur de `enabled` dans hands.ini
    bool gameRunning = false;
    // Réglages de [emulation] et [hands] (relus à chaud par la couche, sauf mention contraire)
    std::string clickMode = "side";  // side | pinch | middle
    bool hideModel = false;          // masquer les modèles de manettes du jeu (expérimental)
    bool emulationOn = true;         // pointeur au bout de l'index + clic
    bool handsDraw = true;           // squelette des mains en surimpression
    std::string pointerMode = "hand";  // hand | finger
    double pointerPitch = 0.0, pointerYaw = 0.0;  // degrés
};

std::wstring IniPath() { return std::wstring(kInstallDir) + L"\\hands.ini"; }

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadTextFile(const std::wstring& path, std::string* out) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool LayerRegistered() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, data = 1, size = sizeof(data);
    const LSTATUS r = RegQueryValueExW(key, kLayerJson, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kGameExe) == 0) { found = true; break; }
    CloseHandle(snap);
    return found;
}

State ReadState() {
    State s;
    s.installed = FileExists(std::wstring(kInstallDir) + L"\\XR_APILAYER_HANDS_bare.dll") && LayerRegistered();
    s.gameRunning = GameRunning();
    std::string text;
    if (ReadTextFile(IniPath(), &text)) {  // pas de fichier = valeurs par défaut de la couche
        const hands::config::IniFile ini = hands::config::IniFile::Parse(text);
        s.enabled = ini.GetBool("general", "enabled", true);
        s.clickMode = ini.GetString("emulation", "click_mode", "side");
        if (s.clickMode != "side" && s.clickMode != "pinch" && s.clickMode != "middle") s.clickMode = "side";
        s.hideModel = ini.GetBool("emulation", "hide_controller_model", false);
        s.emulationOn = ini.GetBool("emulation", "enabled", true);
        s.handsDraw = ini.GetBool("hands", "enabled", true);
        s.pointerMode = ini.GetString("emulation", "pointer_mode", "hand");
        if (s.pointerMode != "hand" && s.pointerMode != "finger") s.pointerMode = "hand";
        s.pointerPitch = ini.GetDouble("emulation", "pointer_pitch_deg", 0.0);
        s.pointerYaw = ini.GetDouble("emulation", "pointer_yaw_deg", 0.0);
    }
    return s;
}

// Écrit `section`/`key` = `value` dans hands.ini en ne touchant qu'à cette ligne.
bool WriteIniValue(const char* section, const char* key, const std::string& value, std::wstring* error) {
    std::string text;
    if (!ReadTextFile(IniPath(), &text)) {
        *error = L"hands.ini est introuvable : installez d'abord la couche.";
        return false;
    }
    const std::string out = hands::config::IniSetValue(text, section, key, value);
    // Écriture en place : le dossier n'est pas modifiable par un utilisateur standard, le fichier l'est.
    std::ofstream f(std::filesystem::path(IniPath()), std::ios::binary | std::ios::trunc);
    if (!f) {
        *error = L"Impossible d'écrire hands.ini (droits insuffisants ?).";
        return false;
    }
    f << out;
    f.flush();
    if (!f) {
        *error = L"Échec d'écriture de hands.ini.";
        return false;
    }
    return true;
}

// --- Icône : disque coloré dessiné en mémoire (aucune ressource externe) ---------------------
HICON MakeIcon(COLORREF fill) {
    const int sz = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    if (!color || !bits || !mask) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return nullptr;
    }
    const float c0 = (sz - 1) / 2.0f, r = sz / 2.0f - 1.5f;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            const float dx = x - c0, dy = y - c0, d2 = dx * dx + dy * dy;
            uint32_t argb = 0;
            if (d2 <= r * r) {
                const bool ring = d2 >= (r - 3.0f) * (r - 3.0f);
                const COLORREF c = ring ? RGB(255, 255, 255) : fill;
                argb = 0xFF000000u | (static_cast<uint32_t>(GetRValue(c)) << 16) |
                       (static_cast<uint32_t>(GetGValue(c)) << 8) | GetBValue(c);
            }
            px[y * sz + x] = argb;
        }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

HICON IconFor(const State& s) {
    if (!s.installed) return g_iconWarn;
    return s.enabled ? g_iconOn : g_iconOff;
}

std::wstring TooltipFor(const State& s) {
    std::wstring t = L"Mains nues OpenXR — ";
    if (!s.installed) t += L"couche non installée";
    else t += s.enabled ? L"suivi ACTIVÉ" : L"suivi DÉSACTIVÉ";
    if (s.gameRunning) t += L" (MSFS en cours)";
    return t;
}

void AddOrUpdateTray(bool add) {
    const State s = ReadState();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = IconFor(s);
    wcsncpy_s(nid.szTip, TooltipFor(s).c_str(), _TRUNCATE);
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
}

void Balloon(const std::wstring& text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, kTitle, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// --- Démarrage avec Windows -----------------------------------------------------------------
bool AutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const bool ok = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

void SetAutostart(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring v = L"\"" + std::wstring(exe, n) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// --- Installation (script PowerShell élevé) --------------------------------------------------
std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe, n);
    return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring FindScript(const wchar_t* name) {
    const std::wstring dir = ExeDir();
    // Paquet distribué : scripts\ à côté de l'exe ; arbre de compilation : build\tray\Release -> dépôt\scripts.
    for (const wchar_t* rel : {L"\\scripts\\", L"\\..\\scripts\\", L"\\..\\..\\scripts\\", L"\\..\\..\\..\\scripts\\"}) {
        const std::wstring p = dir + rel + name;
        if (FileExists(p)) return p;
    }
    return L"";
}

void RunElevatedScript(const wchar_t* name, const wchar_t* extraArgs) {
    if (GameRunning()) {
        MessageBoxW(g_hwnd,
                    L"Fermez MSFS avant d'installer ou de désinstaller la couche (la DLL est verrouillée tant que le jeu tourne).",
                    kTitle, MB_ICONINFORMATION);
        return;
    }
    const std::wstring script = FindScript(name);
    if (script.empty()) {
        MessageBoxW(g_hwnd,
                    (std::wstring(L"Script introuvable : ") + name +
                     L"\nIl doit se trouver dans un dossier « scripts » à côté de l'application ou du dépôt.").c_str(),
                    kTitle, MB_ICONERROR);
        return;
    }
    std::wstring quoted;
    for (wchar_t c : script) quoted += (c == L'\'') ? std::wstring(L"''") : std::wstring(1, c);
    const std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -Command \"& '" + quoted + L"' " + extraArgs +
                              L"; Write-Host ''; Read-Host 'Terminé. Appuyez sur Entrée pour fermer'\"";
    // "runas" = demande d'élévation (UAC).
    const HINSTANCE r = ShellExecuteW(g_hwnd, L"runas", L"powershell.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32)
        MessageBoxW(g_hwnd, L"L'élévation a été refusée ou a échoué : rien n'a été modifié.", kTitle, MB_ICONWARNING);
}

// --- Actions ---------------------------------------------------------------------------------
void ToggleTracking() {
    const State s = ReadState();
    if (!s.installed) {
        MessageBoxW(g_hwnd, L"La couche n'est pas installée. Utilisez « Installer / réparer la couche… ».", kTitle,
                    MB_ICONINFORMATION);
        return;
    }
    std::wstring err;
    if (!WriteIniValue("general", "enabled", !s.enabled ? "true" : "false", &err)) {
        MessageBoxW(g_hwnd, err.c_str(), kTitle, MB_ICONERROR);
        return;
    }
    AddOrUpdateTray(false);
    std::wstring msg = !s.enabled ? L"Suivi activé." : L"Suivi désactivé.";
    msg += s.gameRunning ? L" Effectif au prochain lancement de MSFS." : L" Effectif au prochain lancement du jeu.";
    Balloon(msg);
}

// Applique un réglage de [emulation] / [hands] (relu à chaud par la couche) et le confirme par une bulle.
void ApplySetting(const char* section, const char* key, const std::string& value, const wchar_t* confirmation) {
    std::wstring err;
    if (!WriteIniValue(section, key, value, &err)) {
        MessageBoxW(g_hwnd, err.c_str(), kTitle, MB_ICONERROR);
        return;
    }
    Balloon(confirmation);
}

// Ajoute (dpitch, dyaw) degrés à l'inclinaison du pointeur (bornée à ±45°) et l'enregistre.
void AdjustPointer(const State& s, double dpitch, double dyaw) {
    auto clamp = [](double v) { return v < -45.0 ? -45.0 : (v > 45.0 ? 45.0 : v); };
    const double pitch = clamp(s.pointerPitch + dpitch), yaw = clamp(s.pointerYaw + dyaw);
    std::wstring err;
    char b1[32], b2[32];
    std::snprintf(b1, sizeof(b1), "%.1f", pitch);
    std::snprintf(b2, sizeof(b2), "%.1f", yaw);
    if (!WriteIniValue("emulation", "pointer_pitch_deg", b1, &err) || !WriteIniValue("emulation", "pointer_yaw_deg", b2, &err)) {
        MessageBoxW(g_hwnd, err.c_str(), kTitle, MB_ICONERROR);
        return;
    }
    wchar_t msg[96];
    swprintf(msg, 96, L"Inclinaison du pointeur : %+.1f° haut/bas, %+.1f° gauche/droite.", pitch, yaw);
    Balloon(msg);
}

void OpenPath(const std::wstring& p) { ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void ShowMenu() {
    const State s = ReadState();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, s.installed ? L"Couche : installée" : L"Couche : NON installée");
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, s.gameRunning ? L"MSFS : en cours" : L"MSFS : arrêté");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (s.enabled ? MF_CHECKED : 0) | (s.installed ? 0 : MF_GRAYED), ID_TOGGLE,
                L"Couche activée (au prochain lancement du jeu)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // Réglages ci-dessous : relus à chaud par la couche, aucun redémarrage du jeu nécessaire.
    const UINT cfgGray = s.installed ? 0 : MF_GRAYED;
    const UINT emuGray = (s.installed && s.emulationOn) ? 0 : MF_GRAYED;  // sans émulation, ces choix n'ont aucun effet

    // --- Sous-menu « Pointeur » : tout ce qui concerne viser et cliquer -------------------------
    HMENU pointer = CreatePopupMenu();
    AppendMenuW(pointer, MF_STRING | (s.emulationOn ? MF_CHECKED : 0) | cfgGray, ID_EMULATION, L"Pointeur piloté par la main");
    AppendMenuW(pointer, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pointer, MF_STRING | MF_DISABLED, 0, L"Rayon (pour viser) :");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "hand" ? MF_CHECKED : 0), ID_PTR_HAND, L"   Axe de la main (stable, recommandé)");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "finger" ? MF_CHECKED : 0), ID_PTR_FINGER, L"   Orientation du bout de l'index");
    AppendMenuW(pointer, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pointer, MF_STRING | MF_DISABLED, 0, L"Clic (geste du pouce) :");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.clickMode == "side" ? MF_CHECKED : 0), ID_CLICK_SIDE, L"   Sur le côté de l'index (l'index reste fixe)");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.clickMode == "pinch" ? MF_CHECKED : 0), ID_CLICK_PINCH, L"   Sur le bout de l'index (pincement)");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.clickMode == "middle" ? MF_CHECKED : 0), ID_CLICK_MIDDLE, L"   Sur le bout du majeur");
    AppendMenuW(pointer, MF_SEPARATOR, 0, nullptr);
    HMENU tilt = CreatePopupMenu();
    wchar_t adj[96];
    swprintf(adj, 96, L"Actuel : %+.0f° haut/bas, %+.0f° gauche/droite", s.pointerPitch, s.pointerYaw);
    AppendMenuW(tilt, MF_STRING | MF_DISABLED, 0, adj);
    AppendMenuW(tilt, MF_STRING, ID_PTR_UP, L"Vers le haut (+2°)");
    AppendMenuW(tilt, MF_STRING, ID_PTR_DOWN, L"Vers le bas (−2°)");
    AppendMenuW(tilt, MF_STRING, ID_PTR_LEFT, L"Vers la gauche (+2°)");
    AppendMenuW(tilt, MF_STRING, ID_PTR_RIGHT, L"Vers la droite (−2°)");
    AppendMenuW(tilt, MF_STRING, ID_PTR_RESET, L"Réinitialiser");
    AppendMenuW(pointer, MF_POPUP | emuGray, reinterpret_cast<UINT_PTR>(tilt), L"Inclinaison du rayon");
    AppendMenuW(m, MF_POPUP | cfgGray, reinterpret_cast<UINT_PTR>(pointer), L"Pointeur");

    // --- Sous-menu « Affichage » ----------------------------------------------------------------
    HMENU display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING | (s.handsDraw ? MF_CHECKED : 0) | cfgGray, ID_HANDS_DRAW, L"Squelette des mains");
    AppendMenuW(display, MF_STRING | (s.hideModel ? MF_CHECKED : 0) | cfgGray, ID_HIDE_MODEL, L"Masquer les manettes du jeu (expérimental)");
    AppendMenuW(m, MF_POPUP | cfgGray, reinterpret_cast<UINT_PTR>(display), L"Affichage");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_INSTALL, L"Installer / réparer la couche…");
    AppendMenuW(m, MF_STRING | (s.installed ? 0 : MF_GRAYED), ID_UNINSTALL, L"Désinstaller la couche…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_LOGS, L"Ouvrir le dossier des journaux");
    AppendMenuW(m, MF_STRING, ID_INI, L"Ouvrir hands.ini");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Lancer avec Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quitter");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);  // nécessaire pour que le menu se ferme au clic extérieur
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case ID_TOGGLE: ToggleTracking(); break;
        case ID_CLICK_SIDE: ApplySetting("emulation", "click_mode", "side", L"Clic : pouce sur le côté de l'index."); break;
        case ID_CLICK_PINCH: ApplySetting("emulation", "click_mode", "pinch", L"Clic : pouce sur le bout de l'index."); break;
        case ID_CLICK_MIDDLE: ApplySetting("emulation", "click_mode", "middle", L"Clic : pouce sur le bout du majeur."); break;
        case ID_PTR_HAND: ApplySetting("emulation", "pointer_mode", "hand", L"Rayon : axe de la main."); break;
        case ID_PTR_FINGER: ApplySetting("emulation", "pointer_mode", "finger", L"Rayon : orientation du bout de l'index."); break;
        case ID_PTR_UP: AdjustPointer(s, 2.0, 0.0); break;
        case ID_PTR_DOWN: AdjustPointer(s, -2.0, 0.0); break;
        case ID_PTR_LEFT: AdjustPointer(s, 0.0, 2.0); break;
        case ID_PTR_RIGHT: AdjustPointer(s, 0.0, -2.0); break;
        case ID_PTR_RESET: AdjustPointer(s, -s.pointerPitch, -s.pointerYaw); break;
        case ID_EMULATION:
            ApplySetting("emulation", "enabled", s.emulationOn ? "false" : "true",
                         s.emulationOn ? L"Pointeur piloté par la main : désactivé." : L"Pointeur piloté par la main : activé.");
            break;
        case ID_HANDS_DRAW:
            ApplySetting("hands", "enabled", s.handsDraw ? "false" : "true",
                         s.handsDraw ? L"Squelette des mains masqué." : L"Squelette des mains affiché.");
            break;
        case ID_HIDE_MODEL:
            ApplySetting("emulation", "hide_controller_model", s.hideModel ? "false" : "true",
                         s.hideModel ? L"Manettes du jeu affichées." : L"Manettes du jeu masquées (expérimental).");
            break;
        case ID_INSTALL: RunElevatedScript(L"install.ps1", L""); break;
        case ID_UNINSTALL: RunElevatedScript(L"uninstall.ps1", L"-RemoveFiles"); break;
        case ID_LOGS: OpenPath(std::wstring(kInstallDir) + L"\\logs"); break;
        case ID_INI: OpenPath(IniPath()); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_QUIT: DestroyWindow(g_hwnd); break;
        default: break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {  // l'Explorateur a redémarré
        AddOrUpdateTray(true);
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowMenu();
            return 0;
        case WM_TIMER:
            AddOrUpdateTray(false);  // rafraîchit icône et infobulle (jeu lancé, config modifiée à la main...)
            return 0;
        case WM_DESTROY: {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            KillTimer(hwnd, kTimerId);
            PostQuitMessage(0);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // déjà lancée : une seule icône

    g_iconOn = MakeIcon(RGB(46, 160, 67));     // vert : actif
    g_iconOff = MakeIcon(RGB(128, 128, 128));  // gris : désactivé
    g_iconWarn = MakeIcon(RGB(214, 137, 16));  // orange : couche non installée
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, kWndClass, kTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    AddOrUpdateTray(true);
    SetTimer(g_hwnd, kTimerId, 3000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HICON i : {g_iconOn, g_iconOff, g_iconWarn})
        if (i) DestroyIcon(i);
    if (mutex) CloseHandle(mutex);
    return 0;
}
