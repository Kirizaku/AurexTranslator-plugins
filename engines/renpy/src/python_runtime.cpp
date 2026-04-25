/*
Licensed under the MIT License <http://opensource.org/licenses/MIT>.

Copyright (c) 2026 Daniil Nabiulin <https://github.com/kirizaku>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "python_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
    #include <tlhelp32.h>
#elif defined(__linux__)
    #include <dlfcn.h>
#endif

#if defined(__linux__)
bool g_is_wine = false;

bool detect_wine() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ntdll.dll")) { found = true; break; }
    }
    fclose(f);
    return found;
}
#endif

// internal

enum class PyRuntime { None, Standard, RenPython };

static PyRuntime detect_python_runtime(const char* path) {
    if (!path) return PyRuntime::None;

    std::string name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    for (char& c : name)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    if (name.find("python") == std::string::npos)
        return PyRuntime::None;

    if (name.find("renpython") != std::string::npos)
        return PyRuntime::RenPython;

    return PyRuntime::Standard;
}

// Finds the base address / module handle

void* find_python_module() {
    void* candidates[2] = { nullptr, nullptr };  // [Standard, RenPython]

#if defined(_WIN32)
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnap == INVALID_HANDLE_VALUE) return nullptr;

    MODULEENTRY32 module_entry;
    module_entry.dwSize = sizeof(module_entry);
    if (Module32First(hSnap, &module_entry)) {
        do {
            const char* name = module_entry.szModule;
            const auto py = detect_python_runtime(name);
            if (py == PyRuntime::None) continue;

            const int idx = (py == PyRuntime::Standard) ? 0 : 1;
            if (!candidates[idx])
                candidates[idx] = GetModuleHandleA(name);

            if (candidates[0]) break;
        } while (Module32Next(hSnap, &module_entry));
    }
    CloseHandle(hSnap);
#else
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) return nullptr;

    std::string line;
    while (std::getline(maps, line)) {
        size_t addr_end = line.find(' ');
        size_t dash = line.find('-');
        if (addr_end == std::string::npos || dash == std::string::npos || dash > addr_end)
            continue;

        uintptr_t start_addr = 0;
        try { start_addr = std::stoull(line.substr(0, dash), nullptr, 16); }
        catch (...) { continue; }

        size_t path_start = std::string::npos;
        int spaces = 0;
        for (size_t i = addr_end + 1; i < line.size(); ++i) {
            if (line[i] == ' ' && ++spaces == 5) { path_start = i + 1; break; }
        }
        if (path_start == std::string::npos) continue;

        std::string full_path = line.substr(path_start);
        if (const size_t first = full_path.find_first_not_of(" \t"); first != std::string::npos)
            full_path.erase(0, first);
        else
            continue;

        if (full_path.empty() || full_path[0] == '[') continue;

        const size_t last_slash = full_path.find_last_of("/\\");
        const std::string filename = (last_slash != std::string::npos)
                                         ? full_path.substr(last_slash + 1)
                                         : full_path;

        const auto py = detect_python_runtime(filename.c_str());
        if (py == PyRuntime::None) continue;

        const int idx = (py == PyRuntime::Standard) ? 0 : 1;
        if (!candidates[idx]) {
            candidates[idx] = g_is_wine
                                  ? reinterpret_cast<void*>(start_addr)
                                  : dlopen(full_path.c_str(), RTLD_NOW);
        }

        if (candidates[0]) break;
    }
#endif
    return candidates[0] ? candidates[0] : candidates[1];
}