// phonemic-driver (Windows): creates / removes the PhoneMic virtual microphone
// device and installs its driver. install-driver.ps1 runs it after signing the
// package; it needs administrator rights.
//
//   phonemic-driver install <path\to\phonemic.inf>
//   phonemic-driver remove
//   phonemic-driver status
//
// The driver is root-enumerated (there is no hardware to plug in), so the device
// node is created here, the way devcon/devgen do: SetupDiCreateDeviceInfo +
// DIF_REGISTERDEVICE with the hardware ID from phonemic.inf, then
// UpdateDriverForPlugAndPlayDevices to install the INF onto it.
//
// Exit codes: 0 ok, 3010 ok but Windows must restart, 1 error, 2 usage.
#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "driver_feed.hpp"

namespace {

constexpr wchar_t kHardwareId[] = L"Root\\PhonemicCapture";

void print_error(const char* what) {
    const DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::fwprintf(stderr, L"%hs failed (0x%08lx): %ls\n", what, e, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

bool has_our_hardware_id(HDEVINFO set, SP_DEVINFO_DATA* d) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, d, SPDRP_HARDWAREID, &type, nullptr, 0, &size);
    if (size == 0 || type != REG_MULTI_SZ) return false;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, d, SPDRP_HARDWAREID, &type, reinterpret_cast<BYTE*>(buf.data()),
                                           size, nullptr))
        return false;
    for (const wchar_t* p = buf.data(); *p; p += std::wcslen(p) + 1)
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    return false;
}

// Calls fn(set, devinfo) for every device (present or not) with our hardware ID.
template <typename Fn>
int for_each_our_device(Fn fn) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return -1;
    int n = 0;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        if (has_our_hardware_id(set, &d)) {
            ++n;
            fn(set, &d);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int install(const wchar_t* inf_arg) {
    wchar_t inf[MAX_PATH];
    if (!GetFullPathNameW(inf_arg, MAX_PATH, inf, nullptr) || GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"INF not found: %ls\n", inf_arg);
        return 1;
    }
    GUID cls{};
    wchar_t cls_name[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf, &cls, cls_name, MAX_CLASS_NAME_LEN, nullptr)) {
        print_error("SetupDiGetINFClass");
        return 1;
    }

    if (for_each_our_device([](HDEVINFO, SP_DEVINFO_DATA*) {}) <= 0) {
        HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
        if (set == INVALID_HANDLE_VALUE) {
            print_error("SetupDiCreateDeviceInfoList");
            return 1;
        }
        SP_DEVINFO_DATA d{};
        d.cbSize = sizeof d;
        // REG_MULTI_SZ: the ID plus two terminating NULs.
        wchar_t ids[std::size(kHardwareId) + 1] = {};
        std::wmemcpy(ids, kHardwareId, std::size(kHardwareId));
        const bool ok =
            SetupDiCreateDeviceInfoW(set, cls_name, &cls, nullptr, nullptr, DICD_GENERATE_ID, &d) &&
            SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids),
                                              static_cast<DWORD>(sizeof ids)) &&
            SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d);
        if (!ok) print_error("Creating the PhoneMic device");
        SetupDiDestroyDeviceInfoList(set);
        if (!ok) return 1;
        std::printf("Created the PhoneMic device (%ls).\n", kHardwareId);
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf, INSTALLFLAG_FORCE, &reboot)) {
        print_error("Installing the driver");
        return 1;
    }
    std::printf("Driver installed%s.\n", reboot ? " (Windows must restart to finish)" : "");
    return reboot ? 3010 : 0;
}

int remove() {
    bool reboot_needed = false, failed = false;
    const int n = for_each_our_device([&](HDEVINFO set, SP_DEVINFO_DATA* d) {
        if (SetupDiCallClassInstaller(DIF_REMOVE, set, d)) {
            SP_DEVINSTALL_PARAMS_W p{};
            p.cbSize = sizeof p;
            if (SetupDiGetDeviceInstallParamsW(set, d, &p) && (p.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
                reboot_needed = true;
        } else {
            print_error("Removing the PhoneMic device");
            failed = true;
        }
    });
    if (n < 0) {
        print_error("Listing devices");
        return 1;
    }
    std::printf("%d PhoneMic device(s) removed.\n", failed ? 0 : n);
    return failed ? 1 : (reboot_needed ? 3010 : 0);
}

int status() {
    int present = 0;
    const int n = for_each_our_device([&](HDEVINFO, SP_DEVINFO_DATA* d) {
        ULONG st = 0, problem = 0;
        if (CM_Get_DevNode_Status(&st, &problem, d->DevInst, 0) == CR_SUCCESS) {
            ++present;
            if (problem) std::printf("PhoneMic device present, problem code %lu (see Device Manager).\n", problem);
        }
    });
    const bool running = phonemic::DriverFeed::available();
    std::printf("device: %s\ndriver: %s\n", n > 0 ? (present ? "installed" : "installed, not present") : "not installed",
                running ? "running (Global\\PhonemicRing is open)" : "not running");
    return running ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wcscmp(argv[1], L"install") == 0 && argc == 3) return install(argv[2]);
    if (argc == 2 && std::wcscmp(argv[1], L"remove") == 0) return remove();
    if (argc == 2 && std::wcscmp(argv[1], L"status") == 0) return status();
    std::fwprintf(stderr, L"usage: phonemic-driver install <phonemic.inf> | remove | status\n");
    return 2;
}
