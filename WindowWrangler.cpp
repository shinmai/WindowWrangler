#include <windows.h>
#include <shellapi.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

constexpr wchar_t kAppName[] = L"WindowWrangler";
constexpr UINT kMenuIdAlwaysOnTop = 0xBEEFBABE;
constexpr UINT kMenuIdHideTitlebar = 0xBABEBABE;
constexpr UINT kMenuIdGhostMode   = 0xDEAFBABE;
constexpr wchar_t kMenuTextAlwaysOnTop[] = L"On top";
constexpr wchar_t kMenuTextHideTitlebar[] = L"Hide titlebar";
constexpr wchar_t kMenuTextGhostMode[] = L"Click-through";
constexpr BYTE kGhostAlpha = 200;
constexpr int kMenuOffsetFromBottom = 1;
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kTrayUid = 1;
constexpr UINT kTrayCmdExit = 40001;
static HHOOK g_mouseHook = nullptr;
static bool g_isAltDragging = false;
static HWND g_dragHwnd = nullptr;
static int g_dragOffsetX = 0;
static int g_dragOffsetY = 0;

struct GhostBackup {
    bool hadLayered = false;
    bool hadTransparent = false;
    bool hasAttrs = false;
    COLORREF colorKey = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
};

static std::mutex g_stateMutex;
static std::unordered_map<HWND, GhostBackup> g_ghostBackups;
static std::unordered_set<HWND> g_touchedWindows;

static void RestoreGhostMode(HWND hwnd, const GhostBackup& b) {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle = b.hadTransparent ? (exStyle | WS_EX_TRANSPARENT) : (exStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if(!b.hadLayered) exStyle &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    if(b.hasAttrs && (b.flags == LWA_ALPHA || b.flags == LWA_COLORKEY || b.flags == (LWA_ALPHA | LWA_COLORKEY)))
        SetLayeredWindowAttributes(hwnd, b.colorKey, b.alpha, b.flags);
    else if((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
}

static void UpdateSystemMenuForWindow(HWND hwnd) {
    if(!hwnd) return;
    HMENU sysMenu = GetSystemMenu(hwnd, FALSE);
    if(!sysMenu) return;
    int count = GetMenuItemCount(sysMenu);
    if(count < 0) {
        GetSystemMenu(hwnd, TRUE);
        sysMenu = GetSystemMenu(hwnd, FALSE);
        if(!sysMenu) return;
        count = GetMenuItemCount(sysMenu);
        if(count < 0) return;
    }
    auto ensureMenuItemById = [&](UINT id, const wchar_t* text) {
        MENUITEMINFOW probe{};
        probe.cbSize = sizeof(probe);
        probe.fMask = MIIM_ID;
        if(GetMenuItemInfoW(sysMenu, id, FALSE, &probe)) return;
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STATE | MIIM_FTYPE | MIIM_ID | MIIM_STRING;
        item.fType = MFT_STRING;
        item.fState = MFS_UNCHECKED;
        item.wID = id;
        item.dwTypeData = const_cast<LPWSTR>(text);
        InsertMenuItemW(sysMenu, static_cast<UINT>((count - kMenuOffsetFromBottom) > 0 ? (count - kMenuOffsetFromBottom) : 0), TRUE, &item);
    };
    auto setChecked = [&](UINT id, bool checked) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_STATE;
        mi.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
        SetMenuItemInfoW(sysMenu, id, FALSE, &mi);
    };
    ensureMenuItemById(kMenuIdGhostMode,   kMenuTextGhostMode);
    ensureMenuItemById(kMenuIdHideTitlebar, kMenuTextHideTitlebar);
    ensureMenuItemById(kMenuIdAlwaysOnTop, kMenuTextAlwaysOnTop);
    setChecked(kMenuIdAlwaysOnTop, (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    setChecked(kMenuIdHideTitlebar, (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == 0);
    setChecked(kMenuIdGhostMode, (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0);
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_touchedWindows.insert(hwnd);
}

static void CALLBACK WinEventFocusProc(HWINEVENTHOOK, DWORD eventType, HWND, LONG, LONG, DWORD, DWORD) {
    if(eventType == EVENT_OBJECT_FOCUS) UpdateSystemMenuForWindow(GetForegroundWindow());
}

static void CALLBACK WinEventInvokeProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG idChild, DWORD, DWORD) {
    const UINT cmd = static_cast<UINT>(idChild);
    if(cmd != kMenuIdAlwaysOnTop && cmd != kMenuIdHideTitlebar && cmd != kMenuIdGhostMode) return;
    HWND target = GetForegroundWindow();
    if(!target) return;
    __try {
        if(cmd == kMenuIdAlwaysOnTop) {
            SetWindowPos(target, ((GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        } else if(cmd == kMenuIdHideTitlebar) {
            LONG_PTR style = GetWindowLongPtrW(target, GWL_STYLE);
            style = ((style & WS_CAPTION) == 0) ?(style | WS_CAPTION) : (style & ~static_cast<LONG_PTR>(WS_CAPTION));
            SetWindowLongPtrW(target, GWL_STYLE, style);
            SetWindowPos(target, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        } else if(cmd == kMenuIdGhostMode) {
            const LONG_PTR exStyle = GetWindowLongPtrW(target, GWL_EXSTYLE);
            if((exStyle & WS_EX_TRANSPARENT) == 0) {
                GhostBackup backup{};
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if(g_ghostBackups.find(target) == g_ghostBackups.end()) {
                    backup.hadLayered = (exStyle & WS_EX_LAYERED) != 0;
                    backup.hadTransparent = (exStyle & WS_EX_TRANSPARENT) != 0;
                    COLORREF ck = 0;
                    BYTE a = 255;
                    DWORD f = 0;
                    if(GetLayeredWindowAttributes(target, &ck, &a, &f)) {
                        backup.hasAttrs = true;
                        backup.colorKey = ck;
                        backup.alpha = a;
                        backup.flags = f;
                    }
                    g_ghostBackups.emplace(target, backup);
                }
                const LONG_PTR newEx = exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT;
                SetWindowLongPtrW(target, GWL_EXSTYLE, newEx);
                SetLayeredWindowAttributes(target, 0, kGhostAlpha, LWA_ALPHA);
            } else {
                GhostBackup backup{};
                std::lock_guard<std::mutex> lock(g_stateMutex);
                auto it = g_ghostBackups.find(target);
                if(it != g_ghostBackups.end()) {
                    backup = it->second;
                    g_ghostBackups.erase(it);
                    RestoreGhostMode(target, backup);
                } else {
                    LONG_PTR cur = GetWindowLongPtrW(target, GWL_EXSTYLE);
                    cur &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
                    SetWindowLongPtrW(target, GWL_EXSTYLE, cur);
                }
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    UpdateSystemMenuForWindow(target);
}

static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if(nCode == HC_ACTION) {
        const auto* hs = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if(hs) {
            const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if(wParam == WM_LBUTTONDOWN) {
                if(altDown) {
                    HWND hwnd = WindowFromPoint(hs->pt);
                    if(hwnd) hwnd = GetAncestor(hwnd, GA_ROOT);
                    RECT r{};
                    if(hwnd && GetWindowRect(hwnd, &r)) {
                        g_dragHwnd = hwnd;
                        g_dragOffsetX = hs->pt.x - r.left;
                        g_dragOffsetY = hs->pt.y - r.top;
                        g_isAltDragging = true;
                        return 1;
                    }
                }
            } else if(wParam == WM_MOUSEMOVE) {
                if(g_isAltDragging && g_dragHwnd) {
                    if(!altDown) {
                        g_isAltDragging = false;
                        g_dragHwnd = nullptr;
                    } else {
                        const int newX = hs->pt.x - g_dragOffsetX;
                        const int newY = hs->pt.y - g_dragOffsetY;
                        SetWindowPos(g_dragHwnd, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                }
            } else if(wParam == WM_LBUTTONUP) {
                if(g_isAltDragging) {
                    g_isAltDragging = false;
                    g_dragHwnd = nullptr;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM, LPARAM lParam) {
    switch(msg) {
        case kTrayMsg:
            if(lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU || lParam == WM_LBUTTONDBLCLK) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, kTrayCmdExit, L"Exit");
                POINT pt{};
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                const UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if(cmd == kTrayCmdExit) PostQuitMessage(0);
                return 0;
            }
            break;

        case WM_DESTROY: {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = kTrayUid;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, 0, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    FreeConsole();
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"WinWra.Singleton");
    if(!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if(hMutex) CloseHandle(hMutex);
        return 0;
    }
    constexpr wchar_t kTrayClassName[] = L"WinWra.TrayWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kTrayClassName;
    RegisterClassW(&wc);
    HWND trayHwnd = CreateWindowExW(0, kTrayClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd;
    nid.uID = kTrayUid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, kAppName, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
    HWINEVENTHOOK focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, WinEventFocusProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK invokeHook = SetWinEventHook(EVENT_OBJECT_INVOKED, EVENT_OBJECT_INVOKED, nullptr, WinEventInvokeProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    UpdateSystemMenuForWindow(GetForegroundWindow());
    MSG msg{};
    while(GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    UnhookWinEvent(focusHook);
    UnhookWinEvent(invokeHook);
    std::vector<HWND> touched;
    std::vector<std::pair<HWND, GhostBackup>> ghosted;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    touched.assign(g_touchedWindows.begin(), g_touchedWindows.end());
    g_touchedWindows.clear();
    ghosted.reserve(g_ghostBackups.size());
    for(const auto& kv : g_ghostBackups) ghosted.push_back(kv);
    g_ghostBackups.clear();

    for(const auto& kv : ghosted) {
        if(IsWindow(kv.first)) {
            __try { RestoreGhostMode(kv.first, kv.second); }
            __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    for(HWND hwnd : touched)
        if(IsWindow(hwnd)) GetSystemMenu(hwnd, TRUE);
    DestroyWindow(trayHwnd);
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}