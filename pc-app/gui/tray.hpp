// Tray icon: closing the window hides PhoneMic to the system tray instead of
// quitting, so the microphone keeps working. Linux (tray_linux.cpp) uses the
// StatusNotifierItem and dbusmenu D-Bus protocols (KDE Plasma, and GNOME with
// the AppIndicator extension) through libdbus loaded at run time, so there is
// no build dependency; Windows (tray_win.cpp) uses Shell_NotifyIcon.
//
// PhoneMic is also single-instance (a session-bus name on Linux, a named mutex
// on Windows): starting it again shows the running window instead.
#pragma once

#include <memory>
#include <vector>

namespace phonemic {

// One square RGBA image (row-major, size * size * 4 bytes).
struct TrayIcon {
    int size;
    std::vector<unsigned char> rgba;
};

class Tray {
public:
    enum class Start {
        Running,         // this is the only PhoneMic (the tray may or may not be shown)
        AlreadyRunning,  // another PhoneMic owns the session; it was asked to show its window
        Unavailable,     // no libdbus or session bus: no tray, close quits as before
    };

    Tray();
    ~Tray();
    Start start(std::vector<TrayIcon> icons);
    void stop();

    // An icon is in a system tray right now, so the window can hide into it.
    bool shown() const;
    // Requests from the tray (each true once): left click, "Show PhoneMic" or a
    // second launch, and "Quit".
    bool take_toggle();
    bool take_show();
    bool take_quit();

    struct Impl;  // defined in tray_linux.cpp (its D-Bus handlers use it)

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonemic
