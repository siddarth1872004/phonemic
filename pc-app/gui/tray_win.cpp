// Windows tray icon: Shell_NotifyIcon on a hidden window of our own. Its
// messages arrive through GLFW's event loop (same thread). See tray.hpp.
#include "tray.hpp"

#include <windows.h>
#include <shellapi.h>

namespace phonemic {

namespace {

const wchar_t* const kClass = L"PhoneMicTray";
const wchar_t* const kMutex = L"Local\\io.github.siddarth1872004.phonemic";
constexpr UINT kTrayMsg = WM_APP + 1, kShowMsg = WM_APP + 2;
constexpr UINT kMenuShow = 1, kMenuQuit = 2;

}  // namespace

struct Tray::Impl {
    HANDLE mutex = nullptr;
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid{};
    UINT taskbar_created = 0;
    // Only touched on the GUI thread: the window procedure runs inside glfwWaitEvents.
    bool shown = false, toggle = false, show = false, quit = false;
};

namespace {

using Impl = Tray::Impl;

bool add_icon(Impl& t) { return Shell_NotifyIconW(NIM_ADD, &t.nid) != FALSE; }

void show_menu(Impl& t) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kMenuShow, L"Show PhoneMic");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuQuit, L"Quit PhoneMic");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(t.hwnd);  // so the menu closes when the user clicks elsewhere
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, t.hwnd, nullptr);
    PostMessageW(t.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == kMenuShow) t.show = true;
    if (cmd == kMenuQuit) t.quit = true;
}

LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* t = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (t && msg == kTrayMsg) {
        if (lp == WM_LBUTTONUP) t->toggle = true;
        if (lp == WM_RBUTTONUP) show_menu(*t);
        return 0;
    }
    if (t && msg == kShowMsg) {  // a second launch
        t->show = true;
        return 0;
    }
    if (t && msg == t->taskbar_created) {  // Explorer restarted: the icon is gone
        t->shown = add_icon(*t);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

Tray::Tray() = default;
Tray::~Tray() { stop(); }

Tray::Start Tray::start(std::vector<TrayIcon>) {  // the icon comes from the .exe's resources
    stop();
    impl_ = std::make_unique<Impl>();
    Impl& t = *impl_;
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    t.mutex = CreateMutexW(nullptr, FALSE, kMutex);
    if (t.mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);  // let it bring its window to the front
            PostMessageW(other, kShowMsg, 0, 0);
        }
        stop();
        return Start::AlreadyRunning;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only): it can own the popup menu
    // and receives Explorer's TaskbarCreated broadcast.
    t.hwnd = CreateWindowExW(0, kClass, L"PhoneMic", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!t.hwnd) {
        stop();
        return Start::Unavailable;
    }
    SetWindowLongPtrW(t.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&t));
    t.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    t.nid.cbSize = sizeof t.nid;
    t.nid.hWnd = t.hwnd;
    t.nid.uID = 1;
    t.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    t.nid.uCallbackMessage = kTrayMsg;
    t.nid.hIcon = static_cast<HICON>(LoadImageW(inst, L"GLFW_ICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), 0));
    if (!t.nid.hIcon) t.nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(t.nid.szTip, L"PhoneMic", ARRAYSIZE(t.nid.szTip));
    t.shown = add_icon(t);
    return Start::Running;
}

void Tray::stop() {
    if (!impl_) return;
    Impl& t = *impl_;
    if (t.shown) Shell_NotifyIconW(NIM_DELETE, &t.nid);
    if (t.hwnd) DestroyWindow(t.hwnd);
    if (t.mutex) CloseHandle(t.mutex);
    impl_.reset();
}

bool Tray::shown() const { return impl_ && impl_->shown; }

bool Tray::take_toggle() {
    if (!impl_ || !impl_->toggle) return false;
    impl_->toggle = false;
    return true;
}

bool Tray::take_show() {
    if (!impl_ || !impl_->show) return false;
    impl_->show = false;
    return true;
}

bool Tray::take_quit() {
    if (!impl_ || !impl_->quit) return false;
    impl_->quit = false;
    return true;
}

}  // namespace phonemic
