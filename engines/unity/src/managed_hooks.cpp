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

#include "managed_hooks.h"
#include "settings.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <time.h>
#endif

namespace {

const RuntimeOps*     g_ops = nullptr;
std::atomic<bool>     g_busy{false};
bool                  g_first_scan = true;
size_t                g_image_count = 0;
std::atomic<uint64_t> g_next_scan_ms{0};
bool                  g_target_done[kMaxManagedTargets];
bool                  g_string_word_known = false;

uint64_t now_ms()
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
#endif
}

// Built on first use: init() runs from the library constructor, before the dynamic initialisation of globals
std::vector<ManagedTarget>& targets()
{
    static std::vector<ManagedTarget> list;
    return list;
}

std::deque<std::string>& custom_names()
{
    static std::deque<std::string> names;
    return names;
}

bool hook_set_enabled(HookSet set)
{
    switch (set) {
    case HookSet::EngineStrings:  return settings().engine_strings;
    case HookSet::CreatedStrings: return settings().created_strings;
    case HookSet::SystemString:   return settings().system_string;
    case HookSet::Text:           return true;
    }
    return false;
}

void add_custom_target(const std::string& entry)
{
    std::string name_space, class_name, method;
    if (!split_hook_name(entry, &name_space, &class_name, &method))
        return;

    const bool system_string = name_space == "System" && class_name == "String";
    if (system_string && settings().system_string)
        return;

    custom_names().push_back(name_space);
    custom_names().push_back(class_name);
    custom_names().push_back(method);

    const size_t n = custom_names().size();
    targets().push_back({ system_string ? GROUP_STRING : GROUP_SETTER, custom_names()[n - 3].c_str(), custom_names()[n - 2].c_str(),
                          custom_names()[n - 1].c_str(), nullptr, HookSet::Text });
}

// "Group/Class.Method"
std::string source_name(const ManagedTarget& t, const char* method)
{
    std::string s = group_name(t.group);
    s += '/';
    if (t.group != GROUP_STRING) {
        s += t.class_name;
        s += '.';
    }
    s += method;
    return s;
}

// Installs the hook of an already registered sink; a hook that cannot be installed is switched off
void finish_hook(int id, void* addr)
{
    if (hook_engine_install(addr, static_cast<uint32_t>(id)) != HookResult::Ok)
        sink_disable(id);
}

// Setter / String groups: the string parameters (and `this` for System.String) are the text
void hook_string_method(void* method, const ManagedTarget& t, const MethodShape& shape, const std::string& source)
{
    uint8_t slots[8];
    int nslots = 0;
    if (t.group == GROUP_STRING) {
        if (!shape.is_static)               slots[nslots++] = 0;
        else if (shape.strings.count > 0)   slots[nslots++] = shape.strings.slots[0];
    } else {
        for (int i = 0; i < shape.strings.count && nslots < 8; ++i)
            slots[nslots++] = shape.strings.slots[i];
    }
    if (nslots == 0) return;

    void* addr = g_ops->code_address(method);
    if (!addr) return;

    const bool by_object = settings().split_by_object && !shape.is_static && t.group != GROUP_STRING;
    const int id = sink_add(SinkKind::ManagedArgs, t.group, source.c_str(), slots, nslots, 0, true, by_object);
    if (id < 0) return;
    finish_hook(id, addr);
}

void hook_render_method(void* klass, void* method, const ManagedTarget& t, const MethodShape& shape, const std::string& source)
{
    if (shape.is_static || !t.field) return;

    uint32_t offset = 0;
    if (!g_ops->field_offset(klass, t.field, &offset) || offset == 0 || offset > 0x10000)
        return;

    void* addr = g_ops->code_address(method);
    if (!addr) return;

    const int id = sink_add(SinkKind::ManagedField, t.group, source.c_str(), nullptr, 0, static_cast<int32_t>(offset),
                            false, settings().split_by_object);
    if (id < 0) return;
    finish_hook(id, addr);
}

void hook_class_methods(void* klass, const ManagedTarget& t)
{
    if (g_ops->begin_class && !g_ops->begin_class(klass))
        return;

    const bool any = std::strcmp(t.method, "*") == 0;

    void* iter = nullptr;
    while (void* m = g_ops->next_method(klass, &iter)) {
        const char* name = g_ops->method_name(m);
        if (!name || (!any && std::strcmp(name, t.method) != 0)) continue;
        if (any && name[0] == '.') continue;

        MethodShape shape;
        if (!g_ops->analyze(m, &shape)) continue;

        const std::string source = source_name(t, name);
        if (t.group == GROUP_RENDER)
            hook_render_method(klass, m, t, shape, source);
        else
            hook_string_method(m, t, shape, source);
    }
}

void scan()
{
    std::vector<void*> images;
    images.reserve(256);
    g_ops->collect_images(images);

    const bool first = g_first_scan;
    if (!first && images.size() == g_image_count)
        return;

    g_first_scan  = false;
    g_image_count = images.size();

    for (size_t i = 0; i < targets().size(); ++i) {
        const ManagedTarget& t = targets()[i];
        if (g_target_done[i]) continue;

        bool found = false;
        for (void* img : images) {
            if (void* klass = g_ops->class_from_name(img, t.name_space, t.class_name)) {
                found = true;
                hook_class_methods(klass, t);
            }
        }
        g_target_done[i] = found;
    }

    if (g_ops->after_scan)
        g_ops->after_scan(images);
}

} // namespace

void managed_init(const RuntimeOps* ops)
{
    g_ops = ops;

    for (int i = 0; i < kManagedTargetCount && targets().size() < kMaxManagedTargets; ++i) {
        if (hook_set_enabled(kManagedTargets[i].set))
            targets().push_back(kManagedTargets[i]);
    }
    for (const std::string& entry : settings().hooks) {
        if (targets().size() < kMaxManagedTargets)
            add_custom_target(entry);
    }

    sink_set_game_abi(game_uses_ms_abi());
    sink_set_trigger(&managed_trigger);
}

void managed_install_native(const NativeTarget* targets, int count, void* (*resolve)(const char*))
{
    for (int i = 0; i < count; ++i) {
        const NativeTarget& t = targets[i];
        if (!hook_set_enabled(t.set)) continue;

        void* addr = resolve(t.name);
        if (!addr) continue;

        const std::string source = std::string(group_name(GROUP_NATIVE)) + "/" + t.name;
        const int id = sink_add(t.kind, GROUP_NATIVE, source.c_str(), t.slots, t.nslots);
        if (id < 0) break;

        finish_hook(id, addr);
    }
}

HookResult managed_install_trigger(void* function)
{
    return hook_engine_install(function, kBootstrapId);
}

void managed_trigger()
{
    if (now_ms() < g_next_scan_ms.load(std::memory_order_relaxed))
        return;

    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    if (g_ops && g_ops->attached()) {
        if (!g_string_word_known && g_ops->learn_string_word())
            g_string_word_known = true;

        g_next_scan_ms.store(now_ms() + kRescanIntervalMs, std::memory_order_relaxed);
        scan();
    }
    g_busy.store(false, std::memory_order_release);
}
