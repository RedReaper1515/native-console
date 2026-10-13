// Cancelling a hub job ends everything the job started, not only the first
// process: a build is a script that starts a build tool that starts compilers.
//
// The command here is a shell that starts a second process, which prints its
// process id and then sleeps for two minutes. The test cancels once it has the
// id, and the second process has to be gone a moment later. Windows only: the
// hub's process runner there is what this holds (hub_platform_win.cpp).
//
//   test_hub_run_cancel <python executable>
#include "hub_platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// Whether the process is still running; false when it has ended or never was.
bool alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}

bool gone_within(DWORD pid, int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 50) {
        if (!alive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !alive(pid);
}

void end_process(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 1);
    CloseHandle(h);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_hub_run_cancel <python executable>\n");
        return 2;
    }
    using namespace hub;
    // cmd is the process the hub starts; python is the one it must not leave.
    const std::string child = platform::quote_arg(argv[1]) +
                              " -c \"import os, time; print('child', os.getpid(), flush=True); time.sleep(120)\"";
    const std::string command = "cmd.exe /S /C \"" + child + "\"";

    std::atomic<bool> cancel{false};
    std::atomic<unsigned long> child_pid{0};
    bool said_cancelled = false;
    const auto started = std::chrono::steady_clock::now();
    // A child that never reports is a broken test, not a reason to wait two minutes.
    std::thread giving_up([&] {
        for (int i = 0; i < 200 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel = true;
    });
    platform::RunResult result = platform::run_capture(command, "", [&](const std::string& line) {
        unsigned long pid = 0;
        if (std::sscanf(line.c_str(), "child %lu", &pid) == 1 && pid) {
            child_pid = pid;
            cancel = true;
        }
        if (line.find("cancelled") != std::string::npos) said_cancelled = true;
    }, &cancel);
    giving_up.join();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    int failures = 0;
    if (!result.started) {
        std::fprintf(stderr, "FAILED: the command did not start: %s\n", result.error.c_str());
        return 1;
    }
    const DWORD pid = child_pid;
    if (!pid) {
        std::fprintf(stderr, "FAILED: the child never reported its process id\n");
        return 1;
    }
    if (!said_cancelled) {
        std::fprintf(stderr, "FAILED: the log does not say the job was cancelled\n");
        ++failures;
    }
    if (took > 30.0) {
        std::fprintf(stderr, "FAILED: cancelling took %.1f s\n", took);
        ++failures;
    }
    if (!gone_within(pid, 5000)) {
        std::fprintf(stderr, "FAILED: process %lu, started by the cancelled command, is still running\n", pid);
        end_process(pid);  // not left behind by the test either
        ++failures;
    }

    // A command that ends by itself is unchanged: its output and its exit code.
    std::string said;
    std::atomic<bool> never{false};
    result = platform::run_capture("cmd.exe /S /C \"echo finished& exit /b 7\"", "",
                                   [&](const std::string& line) { said += line; }, &never);
    if (!result.started || result.exit_code != 7 || said.find("finished") == std::string::npos) {
        std::fprintf(stderr, "FAILED: an uncancelled command: started %d, exit %d, output '%s'\n", result.started ? 1 : 0,
                     result.exit_code, said.c_str());
        ++failures;
    }

    if (failures) return 1;
    std::printf("hub run cancel: the command's whole process tree ended (%.1f s)\n", took);
    return 0;
}

#else

int main() {
    std::printf("hub run cancel: Windows only; nothing checked\n");
    return 0;
}

#endif
