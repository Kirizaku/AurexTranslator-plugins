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

#include "il2cpp_runtime.h"

#include "managed_hooks.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ===============================================================
// Runtime API
// ===============================================================

// The part of the IL2CPP embedding API this plugin uses. Every entry is a raw
// address: call it through runtime_call<>() so the right convention is used.
// The functions are exported by GameAssembly.dll / GameAssembly.so (UnityPlayer
// finds them there by name); their declarations are in il2cpp-api-functions.h,
// shipped with the Unity editor (Editor/Data/il2cpp/libil2cpp/)
struct Il2CppApi {
    void*      module = nullptr;
    ModuleInfo range  = { nullptr, nullptr, 0 }; // where GameAssembly is mapped

    // Required
    void* domain_get             = nullptr;  // Il2CppDomain* il2cpp_domain_get(void)
    void* domain_get_assemblies  = nullptr;  // const Il2CppAssembly** il2cpp_domain_get_assemblies(const Il2CppDomain*, size_t* size)
    void* assembly_get_image     = nullptr;  // const Il2CppImage* il2cpp_assembly_get_image(const Il2CppAssembly*)
    void* class_from_name        = nullptr;  // Il2CppClass* il2cpp_class_from_name(const Il2CppImage*, const char* ns, const char* name)
    void* class_get_methods      = nullptr;  // const MethodInfo* il2cpp_class_get_methods(Il2CppClass*, void** iter)
    void* method_get_name        = nullptr;  // const char* il2cpp_method_get_name(const MethodInfo*)
    void* method_get_flags       = nullptr;  // uint32_t il2cpp_method_get_flags(const MethodInfo*, uint32_t* iflags)
    void* method_get_param_count = nullptr;  // uint32_t il2cpp_method_get_param_count(const MethodInfo*)
    void* method_get_param       = nullptr;  // const Il2CppType* il2cpp_method_get_param(const MethodInfo*, uint32_t index)
    void* method_get_return_type = nullptr;  // const Il2CppType* il2cpp_method_get_return_type(const MethodInfo*)
    void* type_get_type          = nullptr;  // int il2cpp_type_get_type(const Il2CppType*)
    void* thread_current         = nullptr;  // Il2CppThread* il2cpp_thread_current(void)
    void* string_new             = nullptr;  // Il2CppString* il2cpp_string_new(const char*)
    void* runtime_invoke         = nullptr;  // Il2CppObject* il2cpp_runtime_invoke(...): the bootstrap trigger

    void* runtime_invoke_convert_args = nullptr; // a second bootstrap trigger
    void* type_is_byref          = nullptr;  // bool il2cpp_type_is_byref(const Il2CppType*)
    void* class_from_type        = nullptr;  // Il2CppClass* il2cpp_class_from_type(const Il2CppType*)
    void* class_value_size       = nullptr;  // int32_t il2cpp_class_value_size(Il2CppClass*, uint32_t* align)
    void* class_get_field_from_name = nullptr; // FieldInfo* il2cpp_class_get_field_from_name(Il2CppClass*, const char*)
    void* field_get_offset       = nullptr;  // size_t il2cpp_field_get_offset(FieldInfo*)
    void* method_is_generic      = nullptr;  // bool il2cpp_method_is_generic(const MethodInfo*)
    void* method_is_inflated     = nullptr;  // bool il2cpp_method_is_inflated(const MethodInfo*)
    void* class_is_generic       = nullptr;  // bool il2cpp_class_is_generic(const Il2CppClass*)
    void* class_is_inflated      = nullptr;  // bool il2cpp_class_is_inflated(const Il2CppClass*)
    void* image_get_name         = nullptr;  // const char* il2cpp_image_get_name(const Il2CppImage*)
    void* image_get_class_count  = nullptr;  // size_t il2cpp_image_get_class_count(const Il2CppImage*)
    void* image_get_class        = nullptr;  // const Il2CppClass* il2cpp_image_get_class(const Il2CppImage*, size_t index)
    void* class_get_name         = nullptr;  // const char* il2cpp_class_get_name(Il2CppClass*)

    bool valid() const {
        return domain_get && domain_get_assemblies && assembly_get_image && class_from_name &&
               class_get_methods && method_get_name && method_get_flags && method_get_param_count &&
               method_get_param && method_get_return_type && type_get_type && thread_current &&
               string_new && runtime_invoke;
    }
};

static Il2CppApi g_il2cpp;

// GameAssembly.dll (Windows, Wine) and GameAssembly.so (Linux)
static bool is_il2cpp_module(const std::string& lower) {
    return lower.find("gameassembly") != std::string::npos;
}

static bool il2cpp_api_init(const char** why) {
    *why = nullptr;

    std::string file;
    void* mod = find_loaded_module(is_il2cpp_module, &file);
    if (!mod) {
        *why = "GameAssembly module";
        return false;
    }
    g_il2cpp.module = mod;
    g_il2cpp.range  = get_module(file.c_str());

    struct Entry { const char* name; void** slot; bool required; };
    const Entry entries[] = {
        { "il2cpp_domain_get",                 &g_il2cpp.domain_get,                true },
        { "il2cpp_domain_get_assemblies",      &g_il2cpp.domain_get_assemblies,     true },
        { "il2cpp_assembly_get_image",         &g_il2cpp.assembly_get_image,        true },
        { "il2cpp_class_from_name",            &g_il2cpp.class_from_name,           true },
        { "il2cpp_class_get_methods",          &g_il2cpp.class_get_methods,         true },
        { "il2cpp_method_get_name",            &g_il2cpp.method_get_name,           true },
        { "il2cpp_method_get_flags",           &g_il2cpp.method_get_flags,          true },
        { "il2cpp_method_get_param_count",     &g_il2cpp.method_get_param_count,    true },
        { "il2cpp_method_get_param",           &g_il2cpp.method_get_param,          true },
        { "il2cpp_method_get_return_type",     &g_il2cpp.method_get_return_type,    true },
        { "il2cpp_type_get_type",              &g_il2cpp.type_get_type,             true },
        { "il2cpp_thread_current",             &g_il2cpp.thread_current,            true },
        { "il2cpp_string_new",                 &g_il2cpp.string_new,                true },
        { "il2cpp_runtime_invoke",             &g_il2cpp.runtime_invoke,            true },
        { "il2cpp_runtime_invoke_convert_args",&g_il2cpp.runtime_invoke_convert_args, false },
        { "il2cpp_type_is_byref",              &g_il2cpp.type_is_byref,             false },
        { "il2cpp_class_from_type",            &g_il2cpp.class_from_type,           false },
        { "il2cpp_class_value_size",           &g_il2cpp.class_value_size,          false },
        { "il2cpp_class_get_field_from_name",  &g_il2cpp.class_get_field_from_name, false },
        { "il2cpp_field_get_offset",           &g_il2cpp.field_get_offset,          false },
        { "il2cpp_method_is_generic",          &g_il2cpp.method_is_generic,         false },
        { "il2cpp_method_is_inflated",         &g_il2cpp.method_is_inflated,        false },
        { "il2cpp_class_is_generic",           &g_il2cpp.class_is_generic,          false },
        { "il2cpp_class_is_inflated",          &g_il2cpp.class_is_inflated,         false },
        { "il2cpp_image_get_name",             &g_il2cpp.image_get_name,            false },
        { "il2cpp_image_get_class_count",      &g_il2cpp.image_get_class_count,     false },
        { "il2cpp_image_get_class",            &g_il2cpp.image_get_class,           false },
        { "il2cpp_class_get_name",             &g_il2cpp.class_get_name,            false },
    };
    for (const Entry& e : entries) {
        *e.slot = module_symbol(mod, e.name);
        if (!*e.slot && e.required) {
            *why = e.name;
            return false;
        }
    }
    return true;
}

// True when a GameAssembly module is loaded (an IL2CPP game)
bool il2cpp_module_present() {
    return find_loaded_module(is_il2cpp_module) != nullptr;
}

static void* il2cpp_export(const char* name) {
    return module_symbol(g_il2cpp.module, name);
}

// ===============================================================
// Backend
// ===============================================================

namespace {

bool call_bool(void* fn, void* a) { return fn && (runtime_call<uint32_t, void*>(fn, a) & 0xFF) != 0; }
void* domain_get() { return runtime_call<void*>(g_il2cpp.domain_get); }
uint32_t method_flags(void* m, uint32_t* iflags) { return runtime_call<uint32_t, void*, uint32_t*>(g_il2cpp.method_get_flags, m, iflags); }
int type_type(void* t) { return runtime_call<int, void*>(g_il2cpp.type_get_type, t); }

void* method_pointer(void* method)
{
    return *reinterpret_cast<void**>(method);
}

bool in_game_assembly(const void* p)
{
    const ModuleInfo& r = g_il2cpp.range;
    return !r.base || (p >= r.base && p < r.end);
}

// ---------------------------------------------------------------
// Callers: "Class.Method" of the code that contains an address
// ---------------------------------------------------------------

struct CallerEntry {
    uintptr_t addr;
    char      name[56];
};

std::vector<CallerEntry> g_caller_names;
std::vector<const void*> g_mapped_images;
std::atomic_flag g_callers_lock = ATOMIC_FLAG_INIT;

struct CallersLock {
    CallersLock()  { while (g_callers_lock.test_and_set(std::memory_order_acquire)) {} }
    ~CallersLock() { g_callers_lock.clear(std::memory_order_release); }
};

bool is_framework_image(const char* name)
{
    static const char* prefixes[] = { "mscorlib", "System", "Mono.", "UnityEngine", "Unity.", "netstandard",
                                      "Newtonsoft", "Microsoft", "I18N", "Accessibility", "nunit" };
    for (const char* p : prefixes) {
        if (std::strncmp(name, p, std::strlen(p)) == 0) {
            return true;
        }
    }
    return false;
}

bool ops_class_is_generic(void* klass)
{
    return call_bool(g_il2cpp.class_is_generic, klass) || call_bool(g_il2cpp.class_is_inflated, klass);
}

void map_image_methods(void* image, std::vector<CallerEntry>& out)
{
    if (!g_il2cpp.image_get_name || !g_il2cpp.image_get_class_count || !g_il2cpp.image_get_class || !g_il2cpp.class_get_name)
        return;

    const char* image_name = runtime_call<const char*, void*>(g_il2cpp.image_get_name, image);
    if (!image_name || is_framework_image(image_name))
        return;

    const size_t classes = runtime_call<size_t, void*>(g_il2cpp.image_get_class_count, image);
    for (size_t i = 0; i < classes && i < 200000; ++i) {
        void* klass = runtime_call<void*, void*, size_t>(g_il2cpp.image_get_class, image, i);
        if (!klass || ops_class_is_generic(klass)) {
            continue;
        }

        const char* cname = runtime_call<const char*, void*>(g_il2cpp.class_get_name, klass);
        if (!cname) {
            continue;
        }

        void* iter = nullptr;
        while (void* m = runtime_call<void*, void*, void**>(g_il2cpp.class_get_methods, klass, &iter)) {
            void* p = method_pointer(m);
            const char* mname = runtime_call<const char*, void*>(g_il2cpp.method_get_name, m);
            if (!p || !mname || !in_game_assembly(p)) {
                continue;
            }
            CallerEntry e{};
            e.addr = reinterpret_cast<uintptr_t>(p);
            std::snprintf(e.name, sizeof(e.name), "%s.%s", cname, mname);
            for (char* c = e.name; *c; ++c) {
                if (*c == ':') *c = '.';
            }
            out.push_back(e);
        }
    }
}

void ops_after_scan(const std::vector<void*>& images)
{
    std::vector<CallerEntry> fresh;
    std::vector<const void*> done;
    for (void* image : images) {
        {
            CallersLock lock;
            if (std::find(g_mapped_images.begin(), g_mapped_images.end(), image) != g_mapped_images.end())
                continue;
        }
        map_image_methods(image, fresh);
        done.push_back(image);
    }
    if (done.empty())
        return;

    CallersLock lock;
    g_mapped_images.insert(g_mapped_images.end(), done.begin(), done.end());
    g_caller_names.insert(g_caller_names.end(), fresh.begin(), fresh.end());
    std::sort(g_caller_names.begin(), g_caller_names.end(), [](const CallerEntry& a, const CallerEntry& b) { return a.addr < b.addr; });
}

bool caller_name(uintptr_t ip, char* out, size_t cap)
{
    CallersLock lock;
    auto it = std::upper_bound(g_caller_names.begin(), g_caller_names.end(), ip,
                               [](uintptr_t v, const CallerEntry& e) { return v < e.addr; });
    if (it == g_caller_names.begin()) {
        return false;
    }
    --it;
    if (ip - it->addr > 0x10000) { // too far to be the same function
        return false;
    }
    std::snprintf(out, cap, "%s", it->name);
    return true;
}

// ---------------------------------------------------------------
// RuntimeOps
// ---------------------------------------------------------------

bool ops_attached()
{
    return runtime_call<void*>(g_il2cpp.thread_current) != nullptr;
}

void ops_collect_images(std::vector<void*>& images)
{
    void* domain = domain_get();
    if (!domain) return;
    size_t count = 0;
    void** assemblies = runtime_call<void**, void*, size_t*>(g_il2cpp.domain_get_assemblies, domain, &count);
    if (!assemblies) return;
    for (size_t i = 0; i < count && i < 4096; ++i) {
        if (void* img = runtime_call<void*, void*>(g_il2cpp.assembly_get_image, assemblies[i])) {
            images.push_back(img);
        }
    }
}

void* ops_class_from_name(void* img, const char* ns, const char* name)
{
    return runtime_call<void*, void*, const char*, const char*>(g_il2cpp.class_from_name, img, ns, name);
}

void* ops_next_method(void* klass, void** iter)
{
    return runtime_call<void*, void*, void**>(g_il2cpp.class_get_methods, klass, iter);
}

const char* ops_method_name(void* m)
{
    return runtime_call<const char*, void*>(g_il2cpp.method_get_name, m);
}

// Code addresses shared by more than one method of the current class (IL2CPP folds
// identical method bodies into one): hooking such an address would redirect every
// method that shares it, so ops_code_address skips them
std::vector<uintptr_t> g_shared_addrs;

bool ops_begin_class(void* klass)
{
    if (ops_class_is_generic(klass))
        return false;

    std::vector<uintptr_t> addrs;
    void* iter = nullptr;
    while (void* m = ops_next_method(klass, &iter)) {
        if (void* p = method_pointer(m)) {
            addrs.push_back(reinterpret_cast<uintptr_t>(p));
        }
    }
    std::sort(addrs.begin(), addrs.end());

    g_shared_addrs.clear();
    for (size_t i = 1; i < addrs.size(); ++i) {
        if (addrs[i] == addrs[i - 1] && (g_shared_addrs.empty() || g_shared_addrs.back() != addrs[i])) {
            g_shared_addrs.push_back(addrs[i]);
        }
    }
    return true;
}

bool param_of_type(void* type, ParamInfo* out)
{
    if (call_bool(g_il2cpp.type_is_byref, type)) {
        out->kind = ParamKind::Ptr;
        out->size = 0;
        return true;
    }
    const int code = type_type(type);
    uint32_t size = 0;
    if (code == ET_VALUETYPE && g_il2cpp.class_from_type && g_il2cpp.class_value_size) {
        if (void* k = runtime_call<void*, void*>(g_il2cpp.class_from_type, type)) {
            uint32_t align = 0;
            const int32_t s = runtime_call<int32_t, void*, uint32_t*>(g_il2cpp.class_value_size, k, &align);
            if (s > 0) size = static_cast<uint32_t>(s);
        }
    }
    return param_from_element_type(code, size, out);
}

bool ops_analyze(void* method, MethodShape* shape)
{
    uint32_t iflags = 0;
    const uint32_t flags = method_flags(method, &iflags);
    if (flags & (METHOD_ATTR_ABSTRACT | METHOD_ATTR_PINVOKE_IMPL)) return false;
    if (iflags & METHOD_IMPL_RUNTIME) return false;
    if (call_bool(g_il2cpp.method_is_generic, method) || call_bool(g_il2cpp.method_is_inflated, method)) return false;
    shape->is_static = (flags & METHOD_ATTR_STATIC) != 0;

    if (void* ret = runtime_call<void*, void*>(g_il2cpp.method_get_return_type, method)) {
        if (type_type(ret) != ET_VOID) {
            ParamInfo r;
            if (!param_of_type(ret, &r) || r.kind == ParamKind::Struct) return false;
        }
    }

    const uint32_t count = runtime_call<uint32_t, void*>(g_il2cpp.method_get_param_count, method);
    if (count > 16) return false;
    ParamInfo params[16];
    for (uint32_t i = 0; i < count; ++i) {
        void* pt = runtime_call<void*, void*, uint32_t>(g_il2cpp.method_get_param, method, i);
        if (!pt || !param_of_type(pt, &params[i])) return false;
    }

    layout_string_args(current_call_abi(), shape->is_static, params, static_cast<int>(count), &shape->strings);
    return true;
}

void* ops_code_address(void* method)
{
    void* p = method_pointer(method);
    if (!p)
        return nullptr;
    if (!in_game_assembly(p))
        return nullptr;
    if (std::binary_search(g_shared_addrs.begin(), g_shared_addrs.end(), reinterpret_cast<uintptr_t>(p)))
        return nullptr;
    return p;
}

bool ops_field_offset(void* klass, const char* field, uint32_t* offset)
{
    if (!g_il2cpp.class_get_field_from_name || !g_il2cpp.field_get_offset) return false;
    void* f = runtime_call<void*, void*, const char*>(g_il2cpp.class_get_field_from_name, klass, field);
    if (!f) return false;
    *offset = static_cast<uint32_t>(runtime_call<size_t, void*>(g_il2cpp.field_get_offset, f));
    return true;
}

bool ops_learn_string_word()
{
    void* s = runtime_call<void*, const char*>(g_il2cpp.string_new, "");
    if (!s) return false;
    sink_set_string_word(*static_cast<uintptr_t*>(s));
    return true;
}

const RuntimeOps kIl2cppOps = {
    &ops_attached, &ops_collect_images, &ops_class_from_name,
    &ops_begin_class, &ops_next_method, &ops_method_name, &ops_analyze, &ops_code_address,
    &ops_field_offset, &ops_learn_string_word, &ops_after_scan,
};

void* resolve_export(const char* name)
{
    return il2cpp_export(name);
}

} // namespace

bool il2cpp_backend_init(const char** why)
{
    if (!il2cpp_api_init(why))
        return false;

    managed_init(&kIl2cppOps);
    sink_set_caller_namer(&caller_name);

    managed_install_native(kIl2cppNativeTargets, kIl2cppNativeTargetCount, &resolve_export);

    bool armed = managed_install_trigger(g_il2cpp.runtime_invoke) == HookResult::Ok;
    if (g_il2cpp.runtime_invoke_convert_args)
        armed |= managed_install_trigger(g_il2cpp.runtime_invoke_convert_args) == HookResult::Ok;
    if (!armed) {
        *why = "cannot hook il2cpp_runtime_invoke";
        return false;
    }
    return true;
}
