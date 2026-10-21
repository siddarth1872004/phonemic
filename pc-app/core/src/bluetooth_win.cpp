// Windows: publish the PhoneMic RFCOMM service with Winsock's built-in
// Bluetooth support (AF_BTH) and register its SDP record with WSASetService.
// Windows' own Bluetooth stack does everything; no driver is involved.
#include "phonemic/bluetooth.hpp"

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
// clang-format on

#include <phonemic/wire.h>

#include <cstdlib>
#include <cstring>

namespace phonemic {

namespace {

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// PM_SERVICE_UUID ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") as a GUID.
GUID service_guid() {
    char hex[33] = {};
    size_t n = 0;
    for (const char* p = PM_SERVICE_UUID; *p && n < 32; ++p)
        if (*p != '-') hex[n++] = *p;
    auto field = [&](size_t off, size_t len) {
        char tmp[9] = {};
        std::memcpy(tmp, hex + off, len);
        return std::strtoul(tmp, nullptr, 16);
    };
    GUID g{};
    g.Data1 = static_cast<unsigned long>(field(0, 8));
    g.Data2 = static_cast<unsigned short>(field(8, 4));
    g.Data3 = static_cast<unsigned short>(field(12, 4));
    for (size_t i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(field(16 + 2 * i, 2));
    return g;
}

// The first Bluetooth radio's name; false if there is none (or it is disabled).
bool find_radio(std::string* name) {
    BLUETOOTH_FIND_RADIO_PARAMS p{sizeof p};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&p, &radio);
    if (!f) return false;
    BLUETOOTH_RADIO_INFO info{};
    info.dwSize = sizeof info;
    if (BluetoothGetRadioInfo(radio, &info) == ERROR_SUCCESS) *name = utf8(info.szName);
    CloseHandle(radio);
    BluetoothFindRadioClose(f);
    return true;
}

std::string device_name(BTH_ADDR addr) {
    BLUETOOTH_DEVICE_INFO d{};
    d.dwSize = sizeof d;
    d.Address.ullLong = addr;
    if (BluetoothGetDeviceInfo(nullptr, &d) != ERROR_SUCCESS) return {};
    return utf8(d.szName);
}

bool readable(SOCKET s, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(0, &set, nullptr, nullptr, &tv) > 0;
}

}  // namespace

void BluetoothServer::platform_loop() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_state(State::Unavailable, "Winsock failed to start");
        return;
    }
    GUID guid = service_guid();

    while (!stop_.load()) {
        std::string radio;
        if (!find_radio(&radio)) {
            set_state(State::PoweredOff, "turn Bluetooth on in Windows Settings");
            if (!sleep_unless_stopped(3000)) break;
            continue;
        }
        set_adapter_name(radio);

        SOCKET s = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == INVALID_SOCKET) {
            set_state(State::Unavailable, "this PC has no Bluetooth support");
            if (!sleep_unless_stopped(5000)) break;
            continue;
        }
        ULONG auth = TRUE;  // paired phones only (the link is then encrypted)
        setsockopt(s, SOL_RFCOMM, SO_BTH_AUTHENTICATE, reinterpret_cast<const char*>(&auth), sizeof auth);
        SOCKADDR_BTH local{};
        local.addressFamily = AF_BTH;
        local.port = BT_PORT_ANY;
        int len = sizeof local;
        if (::bind(s, reinterpret_cast<SOCKADDR*>(&local), sizeof local) != 0 || ::listen(s, 2) != 0 ||
            ::getsockname(s, reinterpret_cast<SOCKADDR*>(&local), &len) != 0) {
            closesocket(s);
            set_state(State::PoweredOff, "turn Bluetooth on in Windows Settings");
            if (!sleep_unless_stopped(3000)) break;
            continue;
        }

        CSADDR_INFO addr{};
        addr.LocalAddr.lpSockaddr = reinterpret_cast<SOCKADDR*>(&local);
        addr.LocalAddr.iSockaddrLength = sizeof local;
        addr.iSocketType = SOCK_STREAM;
        addr.iProtocol = BTHPROTO_RFCOMM;
        wchar_t service_name[] = L"" PM_SERVICE_NAME;
        WSAQUERYSETW q{};
        q.dwSize = sizeof q;
        q.lpszServiceInstanceName = service_name;
        q.lpServiceClassId = &guid;
        q.dwNameSpace = NS_BTH;
        q.dwNumberOfCsAddrs = 1;
        q.lpcsaBuffer = &addr;
        if (WSASetServiceW(&q, RNRSERVICE_REGISTER, 0) != 0) {
            closesocket(s);
            set_state(State::Failed, "can't publish the PhoneMic service (error " + std::to_string(WSAGetLastError()) + ")");
            if (!sleep_unless_stopped(5000)) break;
            continue;
        }
        set_state(State::Ready);

        uint64_t last_check = mono_ms();
        while (!stop_.load()) {
            if (mono_ms() - last_check >= 3000) {
                last_check = mono_ms();
                if (!find_radio(&radio)) break;  // switched off or unplugged: start over
            }
            if (!readable(s, 200)) continue;
            SOCKADDR_BTH peer{};
            int plen = sizeof peer;
            const SOCKET c = ::accept(s, reinterpret_cast<SOCKADDR*>(&peer), &plen);
            if (c == INVALID_SOCKET) continue;
            DWORD timeout_ms = 2000;  // a stalled phone can't wedge the STATUS writer
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
            accept(socket_stream(static_cast<intptr_t>(c)), device_name(peer.btAddr));
        }
        WSASetServiceW(&q, RNRSERVICE_DELETE, 0);
        closesocket(s);
    }
    WSACleanup();
}

}  // namespace phonemic
