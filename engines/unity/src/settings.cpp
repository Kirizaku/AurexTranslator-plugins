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

#include "settings.h"

#include <fstream>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace {

constexpr size_t kMaxCustomHooks = 32;

Settings& storage()
{
    static Settings instance;
    return instance;
}

std::string own_directory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&settings_load), &module)
        || GetModuleFileNameA(module, path, MAX_PATH) == 0)
        return std::string();
#else
    Dl_info info;
    if (!dladdr(reinterpret_cast<void*>(&settings_load), &info) || !info.dli_fname)
        return std::string();

    const char* path = info.dli_fname;
#endif
    const std::string full = path;
    const size_t slash = full.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : full.substr(0, slash + 1);
}

std::string trim(const std::string& s)
{
    const char* blank = " \t\r\n";
    const size_t first = s.find_first_not_of(blank);
    if (first == std::string::npos)
        return std::string();
    
    return s.substr(first, s.find_last_not_of(blank) - first + 1);
}

bool is_on(const std::string& value)
{
    return value == "1" || value == "true";
}

} // namespace

void settings_load()
{
    Settings& result = storage();
    result = Settings();

    const std::string dir = own_directory();
    if (dir.empty())
        return;

    std::ifstream in(dir + "settings/libat-unity.ini");
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "engine_strings") result.engine_strings = is_on(value);
        else if (key == "created_strings") result.created_strings = is_on(value);
        else if (key == "system_string") result.system_string = is_on(value);
        else if (key == "split_by_object") result.split_by_object = is_on(value);
        else if (key == "hook" && !value.empty() && result.hooks.size() < kMaxCustomHooks)
            result.hooks.push_back(value);
    }
}

const Settings& settings()
{
    return storage();
}

bool split_hook_name(const std::string& entry, std::string* name_space, std::string* class_name, std::string* method)
{
    const size_t method_dot = entry.rfind('.');
    if (method_dot == std::string::npos || method_dot == 0 || method_dot + 1 == entry.size())
        return false;

    const std::string head = entry.substr(0, method_dot);
    const size_t class_dot = head.rfind('.');
    if (class_dot != std::string::npos && (class_dot == 0 || class_dot + 1 == head.size()))
        return false;

    *name_space = class_dot == std::string::npos ? std::string() : head.substr(0, class_dot);
    *class_name = class_dot == std::string::npos ? head : head.substr(class_dot + 1);
    *method = entry.substr(method_dot + 1);
    return true;
}
