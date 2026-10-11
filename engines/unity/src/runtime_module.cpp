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

#include "runtime_module.h"

#include <fstream>

#if defined(_WIN32)
    #include <windows.h>
    #include <tlhelp32.h>
#else
    #include <dlfcn.h>
#endif

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void* find_loaded_module(bool (*match)(const std::string&), std::string* found_name) {
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    void* result = nullptr;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (match(to_lower(me.szModule))) {
                result = GetModuleHandleA(me.szModule);
                if (found_name) *found_name = me.szModule;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return result;
#else
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) return nullptr;

    std::string line;
    while (std::getline(maps, line)) {
        const size_t dash = line.find('-');
        if (dash == std::string::npos) continue;

        // The path is everything after the 5th space-separated field
        size_t path_start = std::string::npos;
        int spaces = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == ' ' && ++spaces == 5) { path_start = i + 1; break; }
        }
        if (path_start == std::string::npos) continue;

        std::string full = line.substr(path_start);
        if (const size_t first = full.find_first_not_of(" \t"); first != std::string::npos)
            full.erase(0, first);
        else
            continue;
        if (full.empty() || full[0] == '[') continue;

        const size_t slash = full.find_last_of("/\\");
        const std::string filename = (slash != std::string::npos) ? full.substr(slash + 1) : full;
        if (!match(to_lower(filename))) continue;

        if (found_name) *found_name = filename;
        if (g_is_wine) {
            // Under Wine the module is a PE image: its base is the start of the first mapping
            uintptr_t base = 0;
            try { base = static_cast<uintptr_t>(std::stoull(line.substr(0, dash), nullptr, 16)); }
            catch (...) { continue; }
            return reinterpret_cast<void*>(base);
        }
        return dlopen(full.c_str(), RTLD_NOW | RTLD_NOLOAD);
    }
    return nullptr;
#endif
}

void* module_symbol(void* module, const char* name) {
    if (!module || !name) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return g_is_wine ? find_export(module, name) : dlsym(module, name);
#endif
}
