// ZoneEditor.cpp — voir ZoneEditor.h.
//
// Chaque modification relit zones.ini, change la zone concernee et reecrit le fichier : les positions apprises entre-temps par la couche
// (dans le jeu) ne sont jamais ecrasees. La couche recharge le fichier a chaud.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ZoneEditor.h"
#include "sim/ZoneFile.h"

namespace {

using hands::sim::ZoneAction;
using hands::sim::ZoneRecord;

constexpr wchar_t kClass[] = L"HandsZoneEditor";
constexpr wchar_t kDataDir[] = L"C:\\ProgramData\\HandsLayer\\data";
constexpr wchar_t kLogsDir[] = L"C:\\ProgramData\\HandsLayer\\logs";

struct LibItem {
    std::wstring label;
    std::string type, target, mode;
};

const struct {
    const wchar_t* label;
    const char* type;
    const char* target;
    const char* mode;
} kLibrary[] = {
#include "sim/CommandLibrary.inc"
};

enum Ctl {
    IDC_LIST = 100, IDC_NEW, IDC_DELETE, IDC_CLEARPOS, IDC_NAME, IDC_FILTER, IDC_CMD, IDC_TYPE, IDC_TARGET, IDC_MODE, IDC_VALUE, IDC_UNIT, IDC_APPLY,
    IDC_TEST, IDC_REFRESH, IDC_CLOSE, IDC_STATUS
};

HWND g_wnd = nullptr;
HWND g_list, g_name, g_filter, g_cmd, g_type, g_target, g_mode, g_value, g_unit, g_status;
std::vector<ZoneRecord> g_view;  // zones affichees (derniere lecture du fichier)
std::vector<LibItem> g_items;    // commandes proposees (bibliotheque + Input Events lus dans le simulateur)
std::vector<LibItem> g_shown;    // sous-ensemble filtre, dans l'ordre du combo
std::vector<std::string> g_liveInputs;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(static_cast<size_t>(n));
    return w;
}
void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

// zones.ini : data\ s'il existe (installation recente), sinon logs\ (meme regle que la couche).
std::wstring ZonesPath() {
    const DWORD a = GetFileAttributesW(kDataDir);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return std::wstring(kDataDir) + L"\\zones.ini";
    return std::wstring(kLogsDir) + L"\\zones.ini";
}

std::vector<ZoneRecord> LoadFile() {
    std::ifstream f(ZonesPath());
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return hands::sim::ParseZones(ss.str());
}
bool SaveFile(const std::vector<ZoneRecord>& z) {
    std::ofstream f(ZonesPath(), std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << hands::sim::SerializeZones(z);
    return static_cast<bool>(f);
}

// Requete au pont (HandsBridge.exe) ; vide si injoignable.
std::string PipeCall(const std::string& req, DWORD timeoutMs, bool* reached = nullptr) {
    std::vector<char> buf(256 * 1024);
    DWORD got = 0;
    const BOOL ok = CallNamedPipeW(L"\\\\.\\pipe\\HandsBridge", const_cast<char*>(req.data()), static_cast<DWORD>(req.size()), buf.data(),
                                   static_cast<DWORD>(buf.size()), &got, timeoutMs);
    if (reached) *reached = ok != FALSE;
    if (!ok) return {};
    std::string r(buf.data(), got);
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    return r;
}

std::wstring Describe(const ZoneRecord& z) {
    std::wstring s = Wide(z.id) + L"   —   ";
    s += z.action.type.empty() ? L"(sans action)" : Wide(z.action.type + " " + z.action.target);
    s += z.hasPos ? L"   [position apprise]" : L"   [a apprendre]";
    return s;
}

void FillList(int select) {
    g_view = LoadFile();
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const ZoneRecord& z : g_view) SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Describe(z).c_str()));
    if (select >= 0 && select < static_cast<int>(g_view.size())) SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(select), 0);
}

void SelectCombo(HWND c, const char* value) {
    const LRESULT i = SendMessageW(c, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(Wide(value).c_str()));
    SendMessageW(c, CB_SETCURSEL, i == CB_ERR ? 0 : static_cast<WPARAM>(i), 0);
}

std::string ComboText(HWND c) { return Utf8(GetText(c)); }

void ShowRecord(const ZoneRecord* z) {
    ZoneAction a;
    std::string id;
    if (z) {
        a = z->action;
        id = z->id;
    }
    SetWindowTextW(g_name, Wide(id).c_str());
    SelectCombo(g_type, a.type.empty() ? "event" : a.type.c_str());
    SetWindowTextW(g_target, Wide(a.target).c_str());
    SelectCombo(g_mode, a.mode.empty() ? "press" : a.mode.c_str());
    char b[48];
    std::snprintf(b, sizeof(b), "%g", a.value);
    SetWindowTextW(g_value, Wide(b).c_str());
    SetWindowTextW(g_unit, Wide(a.unit).c_str());
}

ZoneAction FormAction() {
    ZoneAction a;
    a.type = ComboText(g_type);
    a.target = Utf8(GetText(g_target));
    a.mode = ComboText(g_mode);
    a.value = std::atof(Utf8(GetText(g_value)).c_str());
    a.unit = Utf8(GetText(g_unit));
    if (a.unit.empty()) a.unit = "number";
    return a;
}

void RebuildItems() {
    g_items.clear();
    for (const auto& l : kLibrary) g_items.push_back({l.label, l.type, l.target, l.mode});
    for (const std::string& n : g_liveInputs) g_items.push_back({L"Input Event : " + Wide(n), "input", n, "toggle"});
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

void FillCombo() {
    const std::wstring f = Lower(GetText(g_filter));
    g_shown.clear();
    SendMessageW(g_cmd, CB_RESETCONTENT, 0, 0);
    for (const LibItem& it : g_items) {
        if (!f.empty() && Lower(it.label).find(f) == std::wstring::npos && Lower(Wide(it.target)).find(f) == std::wstring::npos) continue;
        g_shown.push_back(it);
        SendMessageW(g_cmd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(it.label.c_str()));
    }
    wchar_t t[64];
    std::swprintf(t, 64, L"%zu commande(s) proposee(s)", g_shown.size());
    SetStatus(t);
}

int CurSel() { return static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0)); }

void Apply() {
    std::string id = hands::sim::SanitizeZoneId(Utf8(GetText(g_name)));
    if (id.empty()) {
        SetStatus(L"Donnez un nom a la zone (lettres, chiffres, _).");
        return;
    }
    std::vector<ZoneRecord> disk = LoadFile();
    const int sel = CurSel();
    std::string oldId = (sel >= 0 && sel < static_cast<int>(g_view.size())) ? g_view[static_cast<size_t>(sel)].id : std::string();
    ZoneRecord* rec = nullptr;
    for (auto& z : disk)
        if (z.id == oldId) rec = &z;
    for (auto& z : disk)
        if (z.id == id && &z != rec) {
            SetStatus(L"Une autre zone porte deja ce nom.");
            return;
        }
    if (!rec) {
        disk.push_back(ZoneRecord{});
        rec = &disk.back();
    }
    rec->id = id;
    rec->action = FormAction();
    std::string err;
    if (hands::sim::BuildBridgeRequest(rec->action, &err).empty()) {
        SetStatus(L"Action invalide : " + Wide(err));
        return;
    }
    if (!SaveFile(disk)) {
        SetStatus(L"Ecriture de zones.ini impossible (droits ? reinstallez la couche).");
        return;
    }
    int idx = 0;
    for (size_t i = 0; i < disk.size(); ++i)
        if (disk[i].id == id) idx = static_cast<int>(i);
    FillList(idx);
    SetStatus(L"Enregistre. Zone sans position : appuyez sur le bouton de memorisation en vol pour l'apprendre.");
}

void Test() {
    std::string err;
    const std::string req = hands::sim::BuildBridgeRequest(FormAction(), &err);
    if (req.empty()) {
        SetStatus(L"Action invalide : " + Wide(err));
        return;
    }
    bool reached = false;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::string r = PipeCall(req, 8000, &reached);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!reached) SetStatus(L"Pont injoignable : HandsBridge.exe ne tourne pas (il est lance par la tray).");
    else if (r.rfind("ok", 0) == 0) SetStatus(L"Commande envoyee au simulateur.");
    else SetStatus(L"Refusee : " + Wide(r));
}

void RefreshInputs() {
    bool reached = false;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::string r = PipeCall("inputs", 12000, &reached);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!reached) {
        SetStatus(L"Pont injoignable : HandsBridge.exe ne tourne pas.");
        return;
    }
    if (r.rfind("ok", 0) != 0) {
        SetStatus(L"Lecture impossible : " + Wide(r));
        return;
    }
    g_liveInputs.clear();
    size_t p = r.find('\t');
    while (p != std::string::npos) {
        const size_t q = r.find('\t', p + 1);
        const std::string n = r.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
        if (!n.empty()) g_liveInputs.push_back(n);
        p = q;
    }
    std::sort(g_liveInputs.begin(), g_liveInputs.end());
    RebuildItems();
    FillCombo();
    wchar_t t[96];
    std::swprintf(t, 96, L"%zu Input Events lus dans l'avion charge.", g_liveInputs.size());
    SetStatus(t);
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void Build() {
    const int rx = 285;
    g_list = Make(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 10, 28, 265, 300, IDC_LIST);
    Make(L"STATIC", L"Zones (dans l'ordre d'apprentissage)", 0, 10, 8, 265, 18, 0);
    Make(L"BUTTON", L"Nouvelle", BS_PUSHBUTTON, 10, 336, 80, 26, IDC_NEW);
    Make(L"BUTTON", L"Supprimer", BS_PUSHBUTTON, 95, 336, 80, 26, IDC_DELETE);
    Make(L"BUTTON", L"Oublier position", BS_PUSHBUTTON, 180, 336, 95, 26, IDC_CLEARPOS);

    Make(L"STATIC", L"Nom", 0, rx, 11, 90, 18, 0);
    g_name = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, rx + 95, 8, 250, 22, IDC_NAME);
    Make(L"STATIC", L"Chercher une commande", 0, rx, 45, 150, 18, 0);
    g_filter = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, rx + 150, 42, 195, 22, IDC_FILTER);
    Make(L"STATIC", L"Commande connue", 0, rx, 77, 120, 18, 0);
    g_cmd = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, rx + 95, 74, 250, 300, IDC_CMD);
    Make(L"BUTTON", L"Actions", BS_GROUPBOX, rx, 110, 345, 170, 0);
    Make(L"STATIC", L"Type", 0, rx + 10, 135, 80, 18, 0);
    g_type = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST, rx + 95, 132, 120, 120, IDC_TYPE);
    Make(L"STATIC", L"Cible", 0, rx + 10, 167, 80, 18, 0);
    g_target = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, rx + 95, 164, 240, 22, IDC_TARGET);
    Make(L"STATIC", L"Mode", 0, rx + 10, 199, 80, 18, 0);
    g_mode = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST, rx + 95, 196, 120, 120, IDC_MODE);
    Make(L"STATIC", L"Valeur", 0, rx + 10, 231, 80, 18, 0);
    g_value = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, rx + 95, 228, 70, 22, IDC_VALUE);
    Make(L"STATIC", L"Unite", 0, rx + 175, 231, 50, 18, 0);
    g_unit = Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, rx + 225, 228, 110, 22, IDC_UNIT);
    Make(L"STATIC", L"event : evenement SimConnect. var : variable L: ou SimVar. input : Input Event de l'avion. code : code avionique (H:).", 0, rx, 288,
         345, 40, 0);

    Make(L"BUTTON", L"Appliquer", BS_DEFPUSHBUTTON, rx, 336, 90, 26, IDC_APPLY);
    Make(L"BUTTON", L"Tester", BS_PUSHBUTTON, rx + 95, 336, 80, 26, IDC_TEST);
    Make(L"BUTTON", L"Lire les Input Events", BS_PUSHBUTTON, rx + 180, 336, 165, 26, IDC_REFRESH);
    g_status = Make(L"STATIC", L"", SS_LEFT, 10, 374, 620, 22, IDC_STATUS);
    Make(L"BUTTON", L"Fermer", BS_PUSHBUTTON, 545, 370, 85, 26, IDC_CLOSE);

    for (const wchar_t* t : {L"event", L"var", L"input", L"code"}) SendMessageW(g_type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    for (const wchar_t* t : {L"press", L"set", L"toggle"}) SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(
        g_wnd, [](HWND c, LPARAM f) -> BOOL { SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(f), TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font));
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case IDC_LIST:
                    if (code == LBN_SELCHANGE) {
                        const int s = CurSel();
                        if (s >= 0 && s < static_cast<int>(g_view.size())) ShowRecord(&g_view[static_cast<size_t>(s)]);
                    }
                    break;
                case IDC_NEW:
                    SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                    ShowRecord(nullptr);
                    SetFocus(g_name);
                    SetStatus(L"Nouvelle zone : nom, commande, puis Appliquer.");
                    break;
                case IDC_DELETE: {
                    const int s = CurSel();
                    if (s < 0 || s >= static_cast<int>(g_view.size())) break;
                    if (MessageBoxW(hwnd, L"Supprimer cette zone ?", L"Zones", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                    std::vector<ZoneRecord> disk = LoadFile();
                    const std::string gone = g_view[static_cast<size_t>(s)].id;
                    disk.erase(std::remove_if(disk.begin(), disk.end(), [&](const ZoneRecord& z) { return z.id == gone; }), disk.end());
                    SaveFile(disk);
                    FillList(-1);
                    ShowRecord(nullptr);
                    SetStatus(L"Zone supprimee.");
                    break;
                }
                case IDC_CLEARPOS: {
                    const int s = CurSel();
                    if (s < 0 || s >= static_cast<int>(g_view.size())) break;
                    std::vector<ZoneRecord> disk = LoadFile();
                    for (auto& z : disk)
                        if (z.id == g_view[static_cast<size_t>(s)].id) z.hasPos = false;
                    SaveFile(disk);
                    FillList(s);
                    SetStatus(L"Position oubliee : la zone sera reapprise au prochain appui sur le bouton de memorisation.");
                    break;
                }
                case IDC_FILTER:
                    if (code == EN_CHANGE) FillCombo();
                    break;
                case IDC_CMD:
                    if (code == CBN_SELCHANGE) {
                        const int i = static_cast<int>(SendMessageW(g_cmd, CB_GETCURSEL, 0, 0));
                        if (i >= 0 && i < static_cast<int>(g_shown.size())) {
                            const LibItem& it = g_shown[static_cast<size_t>(i)];
                            SelectCombo(g_type, it.type.c_str());
                            SetWindowTextW(g_target, Wide(it.target).c_str());
                            SelectCombo(g_mode, it.mode.c_str());
                            SetWindowTextW(g_value, L"1");
                            if (GetText(g_name).empty()) {
                                std::string nm = hands::sim::SanitizeZoneId(Utf8(it.label));
                                if (it.label.rfind(L"Input Event : ", 0) == 0) nm = hands::sim::SanitizeZoneId(it.target);
                                SetWindowTextW(g_name, Wide(nm).c_str());
                            }
                        }
                    }
                    break;
                case IDC_APPLY: Apply(); break;
                case IDC_TEST: Test(); break;
                case IDC_REFRESH: RefreshInputs(); break;
                case IDC_CLOSE: DestroyWindow(hwnd); break;
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: g_wnd = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ZoneEditor_Show(HWND, HINSTANCE inst) {
    if (g_wnd) {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    RECT r{0, 0, 645, 410};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExW(0, kClass, L"Zones du pochoir", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst,
                            nullptr);
    if (!g_wnd) return;
    Build();
    RebuildItems();
    FillCombo();
    FillList(0);
    if (!g_view.empty()) ShowRecord(&g_view[0]);
    else ShowRecord(nullptr);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}
