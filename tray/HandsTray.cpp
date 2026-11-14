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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ZoneEditor.h"
#include "config/IniEdit.h"
#include "config/IniFile.h"
#include "JoyPoll.h"

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
    ID_HIDE_HANDCTRL, ID_CLICK_ANY, ID_CLICK_SIDE, ID_CLICK_PINCH, ID_CLICK_MIDDLE, ID_HIDE_MODEL, ID_HANDS_DRAW, ID_EMULATION,
    ID_PTR_SHOULDER, ID_PTR_CONTACT, ID_PTR_PALM, ID_PTR_HAND, ID_PTR_FINGER, ID_PTR_UP, ID_PTR_DOWN, ID_PTR_LEFT, ID_PTR_RIGHT, ID_PTR_RESET,
    ID_UPDATE,
    ID_SENS_FAST, ID_SENS_NORMAL, ID_SENS_TOLERANT,
    ID_FINGER_INDEX, ID_FINGER_MIDDLE,
    ID_SHARED,
    ID_ZONES_ENABLE, ID_ZONES_TEACH_BTN, ID_ZONES_CLEAR_BTN, ID_ZONES_TEACH_OFF, ID_ZONES_CLEAR_OFF, ID_ZONES_EDIT, ID_ZONES_RECENTER_KEY, ID_MENU_KEY, ID_MENU_KEY_OFF, ID_MENU_BTN, ID_MENU_BTN_OFF
};

HWND g_hwnd = nullptr;
HICON g_iconOn = nullptr, g_iconOff = nullptr, g_iconWarn = nullptr;
HICON g_iconOnUp = nullptr, g_iconOffUp = nullptr;  // mêmes icônes avec une pastille bleue : mise à jour disponible
UINT g_taskbarCreated = 0;

// Action lancée si l'utilisateur clique sur la bulle de notification affichée en dernier.
enum class BalloonAction { None, Install, Update };
BalloonAction g_balloonAction = BalloonAction::None;
uint64_t g_notifiedUpdateStamp = 0;  // dernière version proposée (une seule bulle par version et par session du tray)

constexpr UINT kNinBalloonTimeout = WM_USER + 4;    // NIN_BALLOONTIMEOUT
constexpr UINT kNinBalloonUserClick = WM_USER + 5;  // NIN_BALLOONUSERCLICK

bool UpdateAvailable(uint64_t* stamp);  // défini plus bas (a besoin de ExeDir)

struct State {
    bool updateAvailable = false;  // une version plus récente de la DLL existe à côté du tray / dans le dépôt
    uint64_t updateStamp = 0;      // identifie cette version (date de la DLL source)
    bool installed = false;  // DLL présente et couche déclarée dans le registre
    bool enabled = true;     // valeur de `enabled` dans hands.ini
    bool gameRunning = false;
    // Réglages de [emulation] et [hands] (relus à chaud par la couche, sauf mention contraire)
    std::string clickMode = "any";  // any | side | pinch | middle
    bool hideModel = false;          // masquer les modèles de manettes du jeu (expérimental)
    bool emulationOn = true;         // pointeur au bout de l'index + clic
    bool handsDraw = true;           // squelette des mains en surimpression
    std::string pointerMode = "shoulder";  // shoulder | contact | palm | hand | finger
    double pointerPitch = 0.0, pointerYaw = 0.0;  // degrés
    double clickOn = 21.0, clickOff = 27.0;       // millimètres : seuils de clic / relâchement
    std::string pointerFinger = "index";          // index | middle : doigt qui porte le pointeur
    bool sharedPointer = false;                   // les deux mains actives : la main qui clique prend la main
    bool hideHandCtrl = true;                     // mains seules : manettes virtuelles issues des mains masquees
    bool zonesOn = false;                         // pochoir : zones apprises, contact -> touche StreamSim
    std::string zoneJoystick;                     // nom (ou morceau de nom) du joystick / HOTAS des boutons de zones
    std::string menuKey;                           // touche qui affiche / cache le menu en VR
    int menuButton = 0;                            // bouton de manette / HOTAS pour le meme usage
    std::string zoneRecenterKey = "LSHIFT";        // touche de recentrage de MSFS
    int zoneTeachButton = 0, zoneClearButton = 0;  // boutons de memorisation / d'effacement (0 = aucun)
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
    s.updateAvailable = s.installed && UpdateAvailable(&s.updateStamp);
    std::string text;
    if (ReadTextFile(IniPath(), &text)) {  // pas de fichier = valeurs par défaut de la couche
        const hands::config::IniFile ini = hands::config::IniFile::Parse(text);
        s.enabled = ini.GetBool("general", "enabled", true);
        s.clickMode = ini.GetString("emulation", "click_mode", "any");
        if (s.clickMode != "side" && s.clickMode != "pinch" && s.clickMode != "middle" && s.clickMode != "any") s.clickMode = "any";
        s.hideModel = ini.GetBool("emulation", "hide_controller_model", false);
        s.emulationOn = ini.GetBool("emulation", "enabled", true);
        s.handsDraw = ini.GetBool("hands", "enabled", true);
        s.pointerMode = ini.GetString("emulation", "pointer_mode", "shoulder");
        if (s.pointerMode != "shoulder" && s.pointerMode != "contact" && s.pointerMode != "palm" && s.pointerMode != "hand" && s.pointerMode != "finger") s.pointerMode = "shoulder";
        s.clickOn = ini.GetDouble("emulation", "click_on_mm", ini.GetDouble("emulation", "pinch_on_mm", 21.0));
        s.clickOff = ini.GetDouble("emulation", "click_off_mm", ini.GetDouble("emulation", "pinch_off_mm", 27.0));
        s.pointerFinger = ini.GetString("emulation", "pointer_finger", "index");
        s.sharedPointer = ini.GetBool("emulation", "shared_pointer", false);
        if (s.pointerFinger != "index" && s.pointerFinger != "middle") s.pointerFinger = "index";
        s.pointerPitch = ini.GetDouble("emulation", "pointer_pitch_deg", 0.0);
        s.pointerYaw = ini.GetDouble("emulation", "pointer_yaw_deg", 0.0);
        s.hideHandCtrl = ini.GetBool("hands", "hide_controllers", true);
        s.zonesOn = ini.GetBool("zones", "enabled", false);
        s.zoneJoystick = ini.GetString("zones", "joystick", "");
        s.zoneTeachButton = ini.GetInt("zones", "teach_button", 0);
        s.zoneClearButton = ini.GetInt("zones", "clear_button", 0);
        s.zoneRecenterKey = ini.GetString("zones", "recenter_key", "LSHIFT");
        s.menuKey = ini.GetString("menu", "toggle_key", "");
        s.menuButton = ini.GetInt("menu", "toggle_button", 0);
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

// --- Icone : pastille sombre, cube filaire (le pochoir) et point d'etat, dessinee en memoire (sur-echantillonnee, aucune ressource externe) ---
// La couleur du point dit l'etat (vert actif, gris desactive, orange couche absente) ; le cube est bleu, gris quand c'est desactive.
HICON MakeIcon(COLORREF fill, bool badge = false) {
    const int sz = 32, ss = 4;
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
    const bool gray = GetRValue(fill) == GetGValue(fill) && GetGValue(fill) == GetBValue(fill);
    const float cube[3] = {gray ? 140.0f : 108.0f, gray ? 150.0f : 192.0f, gray ? 165.0f : 255.0f};
    const float dot[3] = {static_cast<float>(GetRValue(fill)), static_cast<float>(GetGValue(fill)), static_cast<float>(GetBValue(fill))};
    const float hex[6][2] = {{16.0f, 6.5f}, {24.5f, 11.0f}, {24.5f, 21.0f}, {16.0f, 25.5f}, {7.5f, 21.0f}, {7.5f, 11.0f}};
    const float inner[3][2] = {{7.5f, 11.0f}, {16.0f, 15.5f}, {24.5f, 11.0f}};
    auto seg = [](float px, float py, float ax, float ay, float bx, float by) {
        const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
        float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
        return std::sqrt(dx * dx + dy * dy);
    };
    auto inRound = [](float x, float y) {  // rectangle arrondi 1.5..30.5, rayon 7
        const float cx = x < 8.5f ? 8.5f : (x > 23.5f ? 23.5f : x), cy = y < 8.5f ? 8.5f : (y > 23.5f ? 23.5f : y);
        return x >= 1.5f && x <= 30.5f && y >= 1.5f && y <= 30.5f && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 7.0f * 7.0f;
    };
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            float acc[4] = {0, 0, 0, 0};  // r, g, b premultiplies, alpha
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const float fx = x + (sx + 0.5f) / ss, fy = y + (sy + 0.5f) / ss;
                    float r = 0, g = 0, b = 0, a = 0;
                    auto over = [&](const float* c, float alpha) {
                        r = c[0] * alpha + r * (1.0f - alpha);
                        g = c[1] * alpha + g * (1.0f - alpha);
                        b = c[2] * alpha + b * (1.0f - alpha);
                        a = alpha + a * (1.0f - alpha);
                    };
                    if (inRound(fx, fy)) {
                        const float bg[3] = {23.0f, 32.0f, 46.0f};
                        over(bg, 1.0f);
                        float dInner = 99.0f, dHex = 99.0f;
                        for (int i = 0; i < 2; ++i) dInner = std::min(dInner, seg(fx, fy, inner[i][0], inner[i][1], inner[i + 1][0], inner[i + 1][1]));
                        dInner = std::min(dInner, seg(fx, fy, 16.0f, 15.5f, 16.0f, 25.5f));
                        for (int i = 0; i < 6; ++i) dHex = std::min(dHex, seg(fx, fy, hex[i][0], hex[i][1], hex[(i + 1) % 6][0], hex[(i + 1) % 6][1]));
                        if (dInner <= 0.9f) over(cube, 0.6f);
                        if (dHex <= 0.9f) over(cube, 1.0f);
                        if ((fx - 16.0f) * (fx - 16.0f) + (fy - 15.5f) * (fy - 15.5f) <= 3.2f * 3.2f) over(dot, 1.0f);
                    }
                    if (badge) {  // pastille bleue en bas a droite : « mise a jour disponible »
                        const float bd2 = (fx - 24.0f) * (fx - 24.0f) + (fy - 24.0f) * (fy - 24.0f);
                        if (bd2 <= 8.0f * 8.0f) {
                            const float c[3] = {bd2 >= 6.0f * 6.0f ? 255.0f : 30.0f, bd2 >= 6.0f * 6.0f ? 255.0f : 110.0f, bd2 >= 6.0f * 6.0f ? 255.0f : 230.0f};
                            r = c[0]; g = c[1]; b = c[2]; a = 1.0f;
                        }
                    }
                    acc[0] += r * a; acc[1] += g * a; acc[2] += b * a; acc[3] += a;
                }
            const float n = static_cast<float>(ss * ss);
            const float A = acc[3] / n;
            uint32_t argb = 0;
            if (acc[3] > 0.0f) {
                const uint32_t R = static_cast<uint32_t>(acc[0] / n + 0.5f), G = static_cast<uint32_t>(acc[1] / n + 0.5f), B = static_cast<uint32_t>(acc[2] / n + 0.5f);
                argb = (static_cast<uint32_t>(A * 255.0f + 0.5f) << 24) | (R << 16) | (G << 8) | B;  // premultiplie
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
    if (s.updateAvailable && g_iconOnUp && g_iconOffUp) return s.enabled ? g_iconOnUp : g_iconOffUp;
    return s.enabled ? g_iconOn : g_iconOff;
}

std::wstring TooltipFor(const State& s) {
    std::wstring t = L"Mains nues OpenXR — ";
    if (!s.installed) t += L"couche non installée";
    else t += s.enabled ? L"suivi ACTIVÉ" : L"suivi DÉSACTIVÉ";
    if (s.updateAvailable) t += L" — mise à jour disponible";
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

// Affiche une bulle de notification. Si `action` n'est pas None, un clic sur la bulle lance cette action
// (avec l'élévation UAC habituelle) : rien ne s'installe sans un clic explicite de l'utilisateur.
void Balloon(const std::wstring& text, BalloonAction action = BalloonAction::None) {
    g_balloonAction = action;
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

// --- Détection d'une version plus récente de la couche ----------------------------------------
constexpr wchar_t kDllName[] = L"XR_APILAYER_HANDS_bare.dll";
constexpr wchar_t kJsonName[] = L"XR_APILAYER_HANDS_bare.json";

// Dossier contenant la DLL « source » : à côté du tray (paquet distribué) ou dans l'arbre de compilation
// (build\tray\Release -> build\layer\Release), c'est-à-dire là où install.ps1 va la chercher.
std::wstring SourceDir() {
    const std::wstring dir = ExeDir();
    for (const wchar_t* rel : {L"", L"\\..\\layer\\Release", L"\\..\\..\\layer\\Release", L"\\..\\..\\layer\\RelWithDebInfo",
                               L"\\..\\..\\..\\build\\layer\\Release"}) {
        const std::wstring d = dir + rel;
        if (FileExists(d + L"\\" + kDllName)) return d;
    }
    return L"";
}

struct FileStamp {
    bool ok = false;
    uint64_t size = 0, mtime = 0;
    bool operator==(const FileStamp& o) const { return ok == o.ok && size == o.size && mtime == o.mtime; }
};

FileStamp StampOf(const std::wstring& path) {
    FileStamp s;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return s;
    s.ok = true;
    s.size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    s.mtime = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return s;
}

bool SameContent(const std::wstring& a, const std::wstring& b) {
    std::string x, y;
    if (!ReadTextFile(a, &x) || !ReadTextFile(b, &y)) return false;
    return x == y;
}

// Vrai si la DLL (ou le manifeste) source diffère de celle installée ET est plus récente. `stamp` identifie la
// version source. Le résultat est mis en cache tant que les tailles et dates des 4 fichiers ne changent pas
// (la comparaison de contenu n'est donc faite qu'après une recompilation ou une réinstallation).
bool UpdateAvailable(uint64_t* stamp) {
    static FileStamp cache[4];
    static bool cacheResult = false;
    static uint64_t cacheStamp = 0;
    const std::wstring src = SourceDir();
    if (stamp) *stamp = 0;
    if (src.empty()) return false;
    const std::wstring files[4] = {src + L"\\" + kDllName, std::wstring(kInstallDir) + L"\\" + kDllName, src + L"\\" + kJsonName,
                                   std::wstring(kInstallDir) + L"\\" + kJsonName};
    FileStamp now[4];
    for (int i = 0; i < 4; ++i) now[i] = StampOf(files[i]);
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && now[i] == cache[i];
    if (!same) {
        for (int i = 0; i < 4; ++i) cache[i] = now[i];
        const bool differs = !SameContent(files[0], files[1]) || (now[2].ok && now[3].ok && !SameContent(files[2], files[3]));
        cacheResult = now[0].ok && now[1].ok && differs && now[0].mtime > now[1].mtime;
        cacheStamp = now[0].mtime;
    }
    if (stamp) *stamp = cacheStamp;
    return cacheResult;
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

// --- Choix d'un bouton de joystick / HOTAS pour les zones : on appuie sur le bouton voulu ------------------------------------------
constexpr UINT_PTR kDetectTimerId = 2;
hands::win::JoyPoll g_joyPoll;
int g_detectWhat = 0;  // 0 = aucune detection, 1 = bouton de memorisation, 2 = bouton d'effacement
ULONGLONG g_detectUntil = 0;

void StartDetect(int what) {
    g_detectWhat = what;
    g_detectUntil = GetTickCount64() + 30000;
    g_joyPoll.PollAllPressed("");  // memorise l'etat actuel : seul un NOUVEL appui compte
    SetTimer(g_hwnd, kDetectTimerId, 50, nullptr);
    if (what == 3 || what == 4) {
        Balloon(what == 3 ? L"Appuyez maintenant sur la touche de RECENTRAGE de MSFS (la meme : 30 s)."
                          : L"Appuyez maintenant sur la touche qui AFFICHERA / CACHERA le menu en VR (30 s).");
        return;
    }
    if (what == 5) {
        Balloon(L"Appuyez maintenant sur le bouton de la manette / HOTAS qui AFFICHERA / CACHERA le menu en VR (30 s).");
        return;
    }
    Balloon(what == 1 ? L"Appuyez maintenant sur le bouton de la manette / HOTAS qui MÉMORISERA la zone (30 s)."
                      : L"Appuyez maintenant sur le bouton de la manette / HOTAS qui EFFACERA les zones (30 s).");
}

void PollDetect() {
    if (g_detectWhat == 0) return;
    if (GetTickCount64() > g_detectUntil) {
        g_detectWhat = 0;
        KillTimer(g_hwnd, kDetectTimerId);
        Balloon(L"Aucun bouton détecté : choix annulé (le joystick est-il branché ?).");
        return;
    }
    if (g_detectWhat == 3 || g_detectWhat == 4) {  // touche du clavier : on cherche la premiere touche enfoncee (hors souris), les touches precises avant les generiques
        int found = 0;
        static const int order[] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
        for (int vk : order)
            if (GetAsyncKeyState(vk) & 0x8000) { found = vk; break; }
        for (int vk = 8; vk < 255 && !found; ++vk)
            if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && (GetAsyncKeyState(vk) & 0x8000)) found = vk;
        if (!found) return;
        const int keyWhat = g_detectWhat;
        g_detectWhat = 0;
        KillTimer(g_hwnd, kDetectTimerId);
        static const struct { int vk; const char* name; } names[] = {{0xA0, "LSHIFT"}, {0xA1, "RSHIFT"}, {0xA2, "LCTRL"}, {0xA3, "RCTRL"}, {0xA4, "LALT"},
                                                                    {0xA5, "RALT"}, {0x20, "SPACE"}, {0x0D, "ENTER"}, {0x09, "TAB"}, {0x1B, "ESC"}};
        std::string name;
        for (const auto& n : names)
            if (n.vk == found) name = n.name;
        char b[16];
        if (name.empty()) {
            if (found >= 0x70 && found <= 0x87) { std::snprintf(b, sizeof(b), "F%d", found - 0x6F); name = b; }
            else if ((found >= 'A' && found <= 'Z') || (found >= '0' && found <= '9')) name = std::string(1, static_cast<char>(found));
            else { std::snprintf(b, sizeof(b), "0x%02X", found); name = b; }
        }
        std::wstring err;
        if (!WriteIniValue(keyWhat == 3 ? "zones" : "menu", keyWhat == 3 ? "recenter_key" : "toggle_key", name, &err)) {
            MessageBoxW(g_hwnd, err.c_str(), kTitle, MB_ICONERROR);
            return;
        }
        const std::wstring wn(name.begin(), name.end());
        Balloon(keyWhat == 3 ? L"Touche de recentrage : " + wn + L". Elle doit etre la meme que celle de MSFS." : L"Menu en VR : touche " + wn + L" pour l'afficher / le cacher.");
        return;
    }
    hands::win::JoyEvent ev;
    if (!g_joyPoll.PollPressed("", 0, &ev)) return;
    const int what = g_detectWhat;
    g_detectWhat = 0;
    KillTimer(g_hwnd, kDetectTimerId);
    std::wstring err;
    // Le nom du peripherique est commun aux deux boutons des zones : le dernier choix gagne. Le menu a le sien.
    const bool forMenu = what == 5;
    if (!WriteIniValue(forMenu ? "menu" : "zones", forMenu ? "toggle_joystick" : "joystick", ev.device, &err) ||
        !WriteIniValue(forMenu ? "menu" : "zones", forMenu ? "toggle_button" : (what == 1 ? "teach_button" : "clear_button"), std::to_string(ev.button), &err)) {
        MessageBoxW(g_hwnd, err.c_str(), kTitle, MB_ICONERROR);
        return;
    }
    int wl = MultiByteToWideChar(CP_UTF8, 0, ev.device.c_str(), -1, nullptr, 0);
    std::wstring name(wl > 0 ? wl - 1 : 0, L'\0');
    if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, ev.device.c_str(), -1, name.data(), wl);
    Balloon(std::wstring(forMenu ? L"Menu en VR (afficher / cacher) : bouton " : (what == 1 ? L"Mémorisation de la zone : bouton " : L"Effacement des zones : bouton ")) + std::to_wstring(ev.button) + L" de « " + name + L" ».");
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

// Préréglages de sensibilité du clic : seuil de clic / seuil de relâchement (millimètres).
void ApplySensitivity(double on, double off, const wchar_t* label) {
    std::wstring err;
    char b1[32], b2[32];
    std::snprintf(b1, sizeof(b1), "%.0f", on);
    std::snprintf(b2, sizeof(b2), "%.0f", off);
    if (!WriteIniValue("emulation", "click_on_mm", b1, &err) || !WriteIniValue("emulation", "click_off_mm", b2, &err)) {
        MessageBoxW(g_hwnd, err.c_str(), kTitle, MB_ICONERROR);
        return;
    }
    Balloon(std::wstring(L"Relâchement du clic : ") + label + L".");
}

void OpenPath(const std::wstring& p) { ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void ShowMenu() {
    const State s = ReadState();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"Hands Interaction " TEXT(HANDS_VERSION));
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, s.installed ? L"Couche : installée" : L"Couche : NON installée");
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, s.gameRunning ? L"MSFS : en cours" : L"MSFS : arrêté");
    if (s.updateAvailable)
        AppendMenuW(m, MF_STRING, ID_UPDATE, L"▶ Mettre à jour la couche… (nouvelle version disponible)");
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
    AppendMenuW(pointer, MF_STRING | emuGray | (s.sharedPointer ? MF_CHECKED : 0), ID_SHARED, L"Deux mains actives (la main qui clique prend la main)");
    AppendMenuW(pointer, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pointer, MF_STRING | MF_DISABLED, 0, L"Doigt du pointeur :");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerFinger == "index" ? MF_CHECKED : 0), ID_FINGER_INDEX, L"   Index");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerFinger == "middle" ? MF_CHECKED : 0), ID_FINGER_MIDDLE, L"   Majeur (bouge moins quand l'index clique)");
    AppendMenuW(pointer, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pointer, MF_STRING | MF_DISABLED, 0, L"Rayon (pour viser) :");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "shoulder" ? MF_CHECKED : 0), ID_PTR_SHOULDER, L"   Comme le casque : de l'épaule, par le point de contact");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "contact" ? MF_CHECKED : 0), ID_PTR_CONTACT, L"   Par le point de contact, depuis le poignet");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "palm" ? MF_CHECKED : 0), ID_PTR_PALM, L"   Depuis la paume, dans l'axe du doigt (comme le casque)");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "hand" ? MF_CHECKED : 0), ID_PTR_HAND, L"   Axe de la main (rigide à la paume)");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.pointerMode == "finger" ? MF_CHECKED : 0), ID_PTR_FINGER, L"   Orientation du bout du doigt");
    AppendMenuW(pointer, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pointer, MF_STRING | MF_DISABLED, 0, L"Clic (geste du pouce) :");
    AppendMenuW(pointer, MF_STRING | emuGray | (s.clickMode == "any" ? MF_CHECKED : 0), ID_CLICK_ANY, L"   Index ou majeur, au plus proche (recommandé)");
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

    // Relâchement du clic : préréglages (clic / relâchement en mm)
    HMENU sens = CreatePopupMenu();
    auto near2 = [](double a, double b) { return a > b - 0.5 && a < b + 0.5; };
    const bool fast = near2(s.clickOn, 20) && near2(s.clickOff, 24), normal = near2(s.clickOn, 21) && near2(s.clickOff, 27),
               tolerant = near2(s.clickOn, 22) && near2(s.clickOff, 35);
    wchar_t cur[96];
    swprintf(cur, 96, L"Actuel : clic à %.0f mm, relâchement à %.0f mm", s.clickOn, s.clickOff);
    AppendMenuW(sens, MF_STRING | MF_DISABLED, 0, cur);
    AppendMenuW(sens, MF_STRING | (fast ? MF_CHECKED : 0), ID_SENS_FAST, L"Rapide (20 / 24 mm) : le clic s'arrête dès que le pouce s'écarte");
    AppendMenuW(sens, MF_STRING | (normal ? MF_CHECKED : 0), ID_SENS_NORMAL, L"Normal (21 / 27 mm)");
    AppendMenuW(sens, MF_STRING | (tolerant ? MF_CHECKED : 0), ID_SENS_TOLERANT, L"Tolérant (22 / 35 mm) : le clic tient même si le pouce bouge");
    AppendMenuW(pointer, MF_POPUP | emuGray, reinterpret_cast<UINT_PTR>(sens), L"Relâchement du clic");

    AppendMenuW(m, MF_POPUP | cfgGray, reinterpret_cast<UINT_PTR>(pointer), L"Pointeur");

    // --- Sous-menu « Affichage » ----------------------------------------------------------------
    HMENU display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING | (s.handsDraw ? MF_CHECKED : 0) | cfgGray, ID_HANDS_DRAW, L"Squelette des mains");
    AppendMenuW(display, MF_STRING | (s.hideHandCtrl ? MF_CHECKED : 0) | cfgGray, ID_HIDE_HANDCTRL, L"Mains seules : masquer les manettes virtuelles des mains");
    AppendMenuW(display, MF_STRING | (s.hideModel ? MF_CHECKED : 0) | cfgGray, ID_HIDE_MODEL, L"Masquer les manettes du jeu (expérimental)");
    AppendMenuW(m, MF_POPUP | cfgGray, reinterpret_cast<UINT_PTR>(display), L"Affichage");

    // --- Sous-menu « Zones (pochoir) » ------------------------------------------------------------
    HMENU zones = CreatePopupMenu();
    AppendMenuW(zones, MF_STRING | (s.zonesOn ? MF_CHECKED : 0) | cfgGray, ID_ZONES_ENABLE, L"Zones actives (contact du doigt = commande du simulateur)");
    AppendMenuW(zones, MF_STRING, ID_ZONES_EDIT, L"Configurer les zones et leurs commandes…");
    AppendMenuW(zones, MF_SEPARATOR, 0, nullptr);
    {
        auto label = [&](const wchar_t* what, int button) {
            std::wstring l = what;
            if (button > 0) {
                l += L" : bouton " + std::to_wstring(button);
            } else {
                l += L" : (aucun)";
            }
            return l;
        };
        AppendMenuW(zones, MF_STRING | cfgGray, ID_ZONES_TEACH_BTN, (label(L"Choisir le bouton qui mémorise la zone", s.zoneTeachButton) + L"…").c_str());
        if (s.zoneTeachButton > 0) AppendMenuW(zones, MF_STRING | cfgGray, ID_ZONES_TEACH_OFF, L"   Ne plus utiliser ce bouton");
        AppendMenuW(zones, MF_STRING | cfgGray, ID_ZONES_CLEAR_BTN, (label(L"Choisir le bouton qui efface les zones", s.zoneClearButton) + L"…").c_str());
        if (s.zoneClearButton > 0) AppendMenuW(zones, MF_STRING | cfgGray, ID_ZONES_CLEAR_OFF, L"   Ne plus utiliser ce bouton");
    }
    {
        std::wstring k(s.zoneRecenterKey.begin(), s.zoneRecenterKey.end());
        AppendMenuW(zones, MF_STRING | cfgGray, ID_ZONES_RECENTER_KEY,
                    (L"Touche de recentrage de MSFS (les zones suivent le cockpit) : " + (k.empty() ? std::wstring(L"(aucune)") : k) + L"…").c_str());
    }
    AppendMenuW(zones, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(zones, MF_STRING | MF_DISABLED, 0, L"En vol : pointez la commande du bout de l'index, puis appuyez sur le bouton.");
    AppendMenuW(m, MF_POPUP | cfgGray, reinterpret_cast<UINT_PTR>(zones), L"Zones (pochoir)");
    {
        HMENU vr = CreatePopupMenu();
        AppendMenuW(vr, MF_STRING | MF_DISABLED, 0, L"Menu dans le casque : toujours possible en pinçant les deux mains.");
        std::wstring k(s.menuKey.begin(), s.menuKey.end());
        AppendMenuW(vr, MF_STRING | cfgGray, ID_MENU_KEY, (L"Choisir la touche qui affiche / cache le menu : " + (k.empty() ? std::wstring(L"(aucune)") : k) + L"…").c_str());
        if (!s.menuKey.empty()) AppendMenuW(vr, MF_STRING | cfgGray, ID_MENU_KEY_OFF, L"   Ne plus utiliser cette touche");
        AppendMenuW(vr, MF_STRING | cfgGray, ID_MENU_BTN,
                    (std::wstring(L"Choisir le bouton de manette / HOTAS qui affiche / cache le menu : ") + (s.menuButton > 0 ? L"bouton " + std::to_wstring(s.menuButton) : std::wstring(L"(aucun)")) + L"…").c_str());
        if (s.menuButton > 0) AppendMenuW(vr, MF_STRING | cfgGray, ID_MENU_BTN_OFF, L"   Ne plus utiliser ce bouton");
        AppendMenuW(m, MF_POPUP | cfgGray, reinterpret_cast<UINT_PTR>(vr), L"Menu en VR (touche / bouton)");
    }
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
        case ID_CLICK_ANY: ApplySetting("emulation", "click_mode", "any", L"Clic : pouce sur l'index ou le majeur."); break;
        case ID_CLICK_SIDE: ApplySetting("emulation", "click_mode", "side", L"Clic : pouce sur le côté de l'index."); break;
        case ID_CLICK_PINCH: ApplySetting("emulation", "click_mode", "pinch", L"Clic : pouce sur le bout de l'index."); break;
        case ID_CLICK_MIDDLE: ApplySetting("emulation", "click_mode", "middle", L"Clic : pouce sur le bout du majeur."); break;
        case ID_PTR_SHOULDER: ApplySetting("emulation", "pointer_mode", "shoulder", L"Rayon : comme le casque, de l'épaule par le point de contact."); break;
        case ID_PTR_CONTACT: ApplySetting("emulation", "pointer_mode", "contact", L"Rayon : par le point de contact du clic."); break;
        case ID_PTR_PALM: ApplySetting("emulation", "pointer_mode", "palm", L"Rayon : depuis la paume, dans l'axe du doigt."); break;
        case ID_PTR_HAND: ApplySetting("emulation", "pointer_mode", "hand", L"Rayon : axe de la main."); break;
        case ID_PTR_FINGER: ApplySetting("emulation", "pointer_mode", "finger", L"Rayon : orientation du bout du doigt."); break;
        case ID_SENS_FAST: ApplySensitivity(20, 24, L"rapide"); break;
        case ID_SENS_NORMAL: ApplySensitivity(21, 27, L"normal"); break;
        case ID_SENS_TOLERANT: ApplySensitivity(22, 35, L"tolérant"); break;
        case ID_SHARED:
            ApplySetting("emulation", "shared_pointer", s.sharedPointer ? "false" : "true",
                         s.sharedPointer ? L"Deux mains actives : désactivé." : L"Deux mains actives : la main dont le pouce s'approche prend la main.");
            break;
        case ID_FINGER_INDEX: ApplySetting("emulation", "pointer_finger", "index", L"Le pointeur est sur l'index."); break;
        case ID_FINGER_MIDDLE:
            // Avec le pointeur sur le majeur, le clic doit se faire avec l'index (il ne déplace plus le pointeur).
            ApplySetting("emulation", "pointer_finger", "middle", L"Le pointeur est sur le majeur. Clic : pouce sur le bout de l'index.");
            if (s.clickMode != "any") ApplySetting("emulation", "click_mode", "any", L"Le pointeur est sur le majeur. Clic : pouce sur l'index ou le majeur.");
            break;
        case ID_PTR_UP: AdjustPointer(s, 2.0, 0.0); break;
        case ID_PTR_DOWN: AdjustPointer(s, -2.0, 0.0); break;
        case ID_PTR_LEFT: AdjustPointer(s, 0.0, 2.0); break;
        case ID_PTR_RIGHT: AdjustPointer(s, 0.0, -2.0); break;
        case ID_PTR_RESET: AdjustPointer(s, -s.pointerPitch, -s.pointerYaw); break;
        case ID_EMULATION:
            ApplySetting("emulation", "enabled", s.emulationOn ? "false" : "true",
                         s.emulationOn ? L"Pointeur piloté par la main : désactivé." : L"Pointeur piloté par la main : activé.");
            break;
        case ID_ZONES_ENABLE:
            ApplySetting("zones", "enabled", s.zonesOn ? "false" : "true", s.zonesOn ? L"Zones désactivées." : L"Zones activées.");
            break;
        case ID_ZONES_TEACH_BTN: StartDetect(1); break;
        case ID_ZONES_CLEAR_BTN: StartDetect(2); break;
        case ID_ZONES_TEACH_OFF: ApplySetting("zones", "teach_button", "0", L"Plus de bouton de mémorisation (touche Ctrl+Alt+F9 seulement)."); break;
        case ID_ZONES_RECENTER_KEY: StartDetect(3); break;
        case ID_MENU_KEY: StartDetect(4); break;
        case ID_MENU_BTN: StartDetect(5); break;
        case ID_MENU_KEY_OFF: ApplySetting("menu", "toggle_key", "", L"Plus de touche pour le menu en VR (pincement des deux mains seulement)."); break;
        case ID_MENU_BTN_OFF: ApplySetting("menu", "toggle_button", "0", L"Plus de bouton pour le menu en VR (pincement des deux mains seulement)."); break;
        case ID_ZONES_EDIT: ZoneEditor_Show(g_hwnd, GetModuleHandleW(nullptr)); break;
        case ID_ZONES_CLEAR_OFF: ApplySetting("zones", "clear_button", "0", L"Plus de bouton d'effacement (touche Ctrl+Alt+F10 seulement)."); break;
        case ID_HANDS_DRAW:
            ApplySetting("hands", "enabled", s.handsDraw ? "false" : "true",
                         s.handsDraw ? L"Squelette des mains masqué." : L"Squelette des mains affiché.");
            break;
        case ID_HIDE_HANDCTRL:
            ApplySetting("hands", "hide_controllers", s.hideHandCtrl ? "false" : "true",
                         s.hideHandCtrl ? L"Manettes virtuelles des mains : visibles (Virtual Desktop les fabrique à partir des mains)."
                                        : L"Mains seules : les manettes virtuelles des mains sont masquées tant que vos mains sont suivies.");
            break;
        case ID_HIDE_MODEL:
            ApplySetting("emulation", "hide_controller_model", s.hideModel ? "false" : "true",
                         s.hideModel ? L"Manettes du jeu affichées." : L"Manettes du jeu masquées (expérimental).");
            break;
        case ID_UPDATE:
        case ID_INSTALL: RunElevatedScript(L"install.ps1", L""); break;
        case ID_UNINSTALL: RunElevatedScript(L"uninstall.ps1", L"-RemoveFiles"); break;
        case ID_LOGS: OpenPath(std::wstring(kInstallDir) + L"\\logs"); break;
        case ID_INI: OpenPath(IniPath()); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_QUIT: DestroyWindow(g_hwnd); break;
        default: break;
    }
}

// --- Relance automatique : le fichier du tray a été remplacé par une nouvelle version ---------------------------------
// Un programme en cours d'exécution garde son ancienne version en mémoire. Le tray surveille donc son propre fichier ; quand il change
// (et reste stable 2 contrôles de suite, pour ne pas lancer un fichier encore en cours d'écriture), il lance la nouvelle version avec
// `--replace` puis se ferme : la nouvelle attend que l'ancienne ait libéré l'instance unique.
std::wstring g_selfPath;

// Pont SimConnect (HandsBridge.exe, a cote de la tray) : lance s'il ne tourne pas, arrete a la fermeture de la tray.
void EnsureBridge() {
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\HandsBridge")) {
        CloseHandle(m);
        return;
    }
    const size_t slash = g_selfPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring exe = g_selfPath.substr(0, slash + 1) + L"HandsBridge.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_HIDE);
}

void StopBridge() {
    char reply[64];
    DWORD got = 0;
    char q[] = "quit";
    CallNamedPipeW(L"\\\\.\\pipe\\HandsBridge", q, 4, reply, sizeof(reply), &got, 500);
}

FileStamp g_selfStamp;
FileStamp g_selfPending;

bool SelfReplacedAndStable() {
    if (g_selfPath.empty()) return false;
    const FileStamp now = StampOf(g_selfPath);
    if (!now.ok || now == g_selfStamp) {
        g_selfPending = FileStamp{};
        return false;
    }
    if (g_selfPending == now) return true;  // même état que la fois précédente : le fichier est fini
    g_selfPending = now;
    return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {  // l'Explorateur a redémarré
        AddOrUpdateTray(true);
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
                ShowMenu();
            } else if (lp == kNinBalloonUserClick) {  // clic sur la bulle : action explicite de l'utilisateur
                const BalloonAction a = g_balloonAction;
                g_balloonAction = BalloonAction::None;
                if (a != BalloonAction::None) RunElevatedScript(L"install.ps1", L"");
            } else if (lp == kNinBalloonTimeout) {
                g_balloonAction = BalloonAction::None;
            }
            return 0;
        case WM_TIMER: {
            if (wp == kDetectTimerId) {
                PollDetect();
                return 0;
            }
            if (SelfReplacedAndStable()) {
                ShellExecuteW(nullptr, L"open", g_selfPath.c_str(), L"--replace", nullptr, SW_HIDE);
                DestroyWindow(hwnd);  // libère l'instance unique pour la nouvelle version
                return 0;
            }
            EnsureBridge();
            AddOrUpdateTray(false);  // rafraîchit icône et infobulle (jeu lancé, config modifiée à la main...)
            // Une nouvelle version vient d'apparaître (recompilation) : une seule bulle par version, jamais pendant le jeu.
            const State st = ReadState();
            if (st.updateAvailable && !st.gameRunning && st.updateStamp != g_notifiedUpdateStamp) {
                g_notifiedUpdateStamp = st.updateStamp;
                Balloon(L"Nouvelle version de la couche disponible. Cliquez ici pour la mettre à jour (MSFS doit être fermé).", BalloonAction::Update);
            }
            return 0;
        }
        case WM_DESTROY: {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            KillTimer(hwnd, kTimerId);
            StopBridge();
            PostQuitMessage(0);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Installateur : `HandsTray.exe --merge-ini <hands.ini livre> <hands.ini de l'utilisateur>` ajoute les cles nouvelles a la config
// existante (valeurs de l'utilisateur jamais modifiees) puis quitte. Le resultat est note dans logs\install.log a cote de la config.
int MergeIniCommand(const std::wstring& tmplPath, const std::wstring& userPath) {
    auto readAll = [](const std::wstring& path, std::string* out) {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        *out = ss.str();
        return true;
    };
    std::string tmpl, user;
    if (!readAll(tmplPath, &tmpl) || !readAll(userPath, &user)) return 2;
    std::vector<std::string> added;
    const std::string merged = hands::config::IniMergeMissing(user, tmpl, &added);
    if (merged != user) {
        const std::filesystem::path tmp = std::filesystem::path(userPath).wstring() + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return 3;
            f << merged;
            if (!f) return 3;
        }
        if (!MoveFileExW(tmp.c_str(), userPath.c_str(), MOVEFILE_REPLACE_EXISTING)) return 3;
    }
    std::ofstream log(std::filesystem::path(userPath).parent_path() / L"logs" / L"install.log", std::ios::app);
    if (log) {
        log << "hands.ini : " << added.size() << " cle(s) ajoutee(s)";
        for (const auto& k : added) log << " " << k;
        log << "\n";
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--merge-ini")) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int rc = 1;
        for (int i = 1; argv && i + 2 < argc; ++i)
            if (wcscmp(argv[i], L"--merge-ini") == 0) {
                rc = MergeIniCommand(argv[i + 1], argv[i + 2]);
                break;
            }
        if (argv) LocalFree(argv);
        return rc;
    }
    // `--replace` : lancée par l'ancienne version qui se ferme ; on attend (jusqu'à 10 s) qu'elle libère l'instance unique.
    const bool replace = cmdLine && wcsstr(cmdLine, L"--replace") != nullptr;
    HANDLE mutex = nullptr;
    for (int i = 0; i < (replace ? 100 : 1); ++i) {
        mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(mutex);
        mutex = nullptr;
        if (!replace) return 0;  // déjà lancée : une seule icône
        Sleep(100);
    }
    if (!mutex) return 0;
    {
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        g_selfPath.assign(self, n);
        g_selfStamp = StampOf(g_selfPath);
    }


    g_iconOn = MakeIcon(RGB(46, 160, 67));     // vert : actif
    g_iconOff = MakeIcon(RGB(128, 128, 128));  // gris : désactivé
    g_iconWarn = MakeIcon(RGB(214, 137, 16));  // orange : couche non installée
    g_iconOnUp = MakeIcon(RGB(46, 160, 67), true);      // pastille bleue : mise à jour disponible
    g_iconOffUp = MakeIcon(RGB(128, 128, 128), true);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, kWndClass, kTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    EnsureBridge();
    AddOrUpdateTray(true);
    SetTimer(g_hwnd, kTimerId, 3000, nullptr);
    if (replace) Balloon(L"Application relancée : nouvelle version chargée.");

    // Invites au démarrage. Rien ne s'installe sans clic de l'utilisateur sur la bulle (puis validation UAC).
    {
        const State st = ReadState();
        if (!st.installed) {
            if (!FindScript(L"install.ps1").empty())
                Balloon(L"La couche n'est pas installée. Cliquez ici pour l'installer maintenant (validation administrateur requise).", BalloonAction::Install);
        } else if (st.updateAvailable && !st.gameRunning) {
            g_notifiedUpdateStamp = st.updateStamp;
            Balloon(L"Nouvelle version de la couche disponible. Cliquez ici pour la mettre à jour (MSFS doit être fermé).", BalloonAction::Update);
        }
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HICON i : {g_iconOn, g_iconOff, g_iconWarn, g_iconOnUp, g_iconOffUp})
        if (i) DestroyIcon(i);
    if (mutex) CloseHandle(mutex);
    return 0;
}
