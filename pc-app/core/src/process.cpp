#include "phonemic/process.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace phonemic {

#ifdef _WIN32

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// Quote one argument per the MSVC runtime's command-line parsing rules.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            q.append(backslashes * 2 + 1, L'\\');
            q.push_back(c);
            backslashes = 0;
        } else {
            q.append(backslashes, L'\\');
            q.push_back(c);
            backslashes = 0;
        }
    }
    q.append(backslashes * 2, L'\\');
    q.push_back(L'"');
    return q;
}
}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv, int timeout_ms) {
    ProcessResult r;
    if (argv.empty()) return r;
    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += quote(widen(a));
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // The child inherits the output pipe and nothing else. Sockets are
    // inheritable by default, and `adb` leaves a server running that would keep
    // our listening ports open after PhoneMic exits.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return r;
    }
    HANDLE inherit[] = {wr};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.StartupInfo.hStdInput = nullptr;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return r;
    }
    r.started = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD n = 0;
            if (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) r.output.append(buf, n);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            // Drain what's left.
            DWORD n = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                   ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0)
                r.output.append(buf, n);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            r.exit_code = static_cast<int>(code);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            r.exit_code = -1;
            break;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return r;
}

#else

ProcessResult run_process(const std::vector<std::string>& argv, int timeout_ms) {
    ProcessResult r;
    if (argv.empty()) return r;
    int fds[2];
    if (::pipe(fds) != 0) return r;
    // Report exec failure through a second, close-on-exec pipe.
    int errpipe[2];
    if (::pipe(errpipe) != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }
    ::fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    struct rlimit rl {};
    const int max_fd = ::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
                           ? static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536))
                           : 4096;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        ::close(errpipe[0]);
        ::close(errpipe[1]);
        return r;
    }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        ::close(errpipe[0]);
        // Nothing but stdio crosses exec: `adb` leaves a server running that
        // would otherwise keep our sockets (and listening ports) open.
#ifdef SYS_close_range
        if (::syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) != 0)
#endif
            for (int fd = 3; fd < max_fd; ++fd) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::execvp(args[0], args.data());
        const char c = 1;
        (void)!::write(errpipe[1], &c, 1);
        ::_exit(127);
    }
    ::close(fds[1]);
    ::close(errpipe[1]);
    char c;
    r.started = ::read(errpipe[0], &c, 1) != 1;  // EOF (closed on exec) = started
    ::close(errpipe[0]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    bool killed = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            ::kill(pid, SIGKILL);
            killed = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, static_cast<int>(left));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n <= 0) break;  // EOF
        r.output.append(buf, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!killed && WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    return r;
}

#endif

}  // namespace phonemic
