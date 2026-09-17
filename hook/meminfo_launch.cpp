// Launches a target program with meminfo_hook.dll already loaded.
//
// DetourCreateProcessWithDllEx starts the target suspended, rewrites its import
// table to pull the DLL in, and then resumes it. The hooks are therefore in
// place before the program's first instruction, which matters: GIMP allocates
// during startup, and injecting into an already-running process would leave
// those early blocks on the real heap, with a free() later handing an arena
// pointer to a CRT that never issued it.
//
//   meminfo_launch --target "C:\Program Files\GIMP 2\bin\gimp-2.10.exe" ^
//                  --mode remote --peer 192.168.1.42 --port 9200 ^
//                  --heap-size 4294967296 --budget 536870912 -- <target args>
//
// Anything after "--" is passed to the target unchanged.

#include <windows.h>
#include <detours.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void set_env(const char* name, const std::string& value) {
    if (!value.empty()) {
        SetEnvironmentVariableA(name, value.c_str());
    }
}

// The directory this launcher lives in, which is where the DLL sits too.
std::string module_directory() {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";

    std::string full(path, n);
    const size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : full.substr(0, slash);
}

void usage() {
    std::printf(
        "meminfo_launch -- start a program with its large allocations routed to a peer\n"
        "\n"
        "  --target <path>      program to launch (required)\n"
        "  --dll <path>         hook DLL (default: meminfo_hook.dll beside this exe)\n"
        "  --mode <log|remote>  log counts allocations only; remote redirects them\n"
        "                       (default: log)\n"
        "  --peer <ip>          peer address, bypassing discovery\n"
        "  --port <n>           peer memoryd port (default 9200)\n"
        "  --socket <name>      discoveryd control socket, when --peer is not given\n"
        "  --threshold <bytes>  allocations below this are left alone (default 1048576)\n"
        "  --heap-size <bytes>  size of the peer-backed region (default 4 GiB)\n"
        "  --budget <bytes>     resident bytes before eviction (default 512 MiB)\n"
        "  --page <bytes>       transfer granularity (default 65536)\n"
        "  --log <path>         where the DLL writes its log\n"
        "  --no-gslice          do not set G_SLICE=always-malloc\n"
        "  -- <args...>         everything after this goes to the target\n"
        "\n"
        "G_SLICE=always-malloc is set by default. GLib's slice allocator otherwise\n"
        "carves small objects out of large private blocks, and the sizes this hook\n"
        "sees stop reflecting what the program actually asked for.\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string target;
    std::string dll_path;
    std::string mode = "log";
    std::string peer;
    std::string port;
    std::string socket_name;
    std::string threshold;
    std::string heap_size;
    std::string budget;
    std::string page;
    std::string log_path;
    bool set_gslice = true;
    std::vector<std::string> target_args;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 < argc) out = argv[++i];
        };

        if (arg == "--target") value(target);
        else if (arg == "--dll") value(dll_path);
        else if (arg == "--mode") value(mode);
        else if (arg == "--peer") value(peer);
        else if (arg == "--port") value(port);
        else if (arg == "--socket") value(socket_name);
        else if (arg == "--threshold") value(threshold);
        else if (arg == "--heap-size") value(heap_size);
        else if (arg == "--budget") value(budget);
        else if (arg == "--page") value(page);
        else if (arg == "--log") value(log_path);
        else if (arg == "--no-gslice") set_gslice = false;
        else if (arg == "--help" || arg == "-h") { usage(); return 0; }
        else if (arg == "--") {
            for (int j = i + 1; j < argc; ++j) target_args.push_back(argv[j]);
            break;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (target.empty()) {
        std::fprintf(stderr, "--target is required\n\n");
        usage();
        return 2;
    }

    if (dll_path.empty()) {
        dll_path = module_directory() + "\\meminfo_hook.dll";
    }
    if (GetFileAttributesA(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "Hook DLL not found: %s\n", dll_path.c_str());
        return 1;
    }

    // The DLL reads its configuration from the environment, which the child
    // inherits. Nothing has to be passed on the target's own command line,
    // so the target sees exactly the arguments it would otherwise.
    set_env("MEMINFO_HOOK_MODE", mode);
    set_env("MEMINFO_PEER", peer);
    set_env("MEMINFO_PORT", port);
    set_env("MEMINFO_SOCKET", socket_name);
    set_env("MEMINFO_THRESHOLD", threshold);
    set_env("MEMINFO_HEAP_SIZE", heap_size);
    set_env("MEMINFO_BUDGET", budget);
    set_env("MEMINFO_PAGE", page);
    set_env("MEMINFO_LOG", log_path);
    if (set_gslice) {
        SetEnvironmentVariableA("G_SLICE", "always-malloc");
    }

    std::string command_line = "\"" + target + "\"";
    for (const auto& arg : target_args) {
        command_line += " \"" + arg + "\"";
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    std::printf("Launching %s\n  hook: %s\n  mode: %s\n",
                target.c_str(), dll_path.c_str(), mode.c_str());

    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    const BOOL ok = DetourCreateProcessWithDllEx(
        target.c_str(),
        mutable_command_line.data(),
        nullptr, nullptr, TRUE,
        CREATE_DEFAULT_ERROR_MODE,
        nullptr, nullptr,
        &si, &pi,
        dll_path.c_str(),
        nullptr);

    if (!ok) {
        const DWORD err = GetLastError();
        std::fprintf(stderr, "DetourCreateProcessWithDllEx failed: %lu\n", err);
        if (err == ERROR_INVALID_HANDLE || err == ERROR_BAD_EXE_FORMAT) {
            std::fprintf(stderr,
                         "This usually means the launcher and the target differ in bitness,\n"
                         "or the DLL does. Build meminfo_hook.dll for the same architecture\n"
                         "as the target (GIMP's Windows builds are 64-bit).\n");
        }
        return 1;
    }

    std::printf("Started pid %lu. Waiting for it to exit...\n", pi.dwProcessId);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::printf("Target exited with code %lu. See the DLL's log for the allocation summary.\n",
                exit_code);
    return static_cast<int>(exit_code);
}
