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

#include "mono_runtime.h"

#include "managed_hooks.h"

#include <cstdio>
#include <string>
#include <vector>

// ===============================================================
// Runtime API
// ===============================================================

// https://github.com/mono/mono/blob/master/mono/metadata/object.h
// https://github.com/mono/mono/blob/master/mono/metadata/object-internals.h
// https://github.com/mono/mono/blob/master/mono/metadata/appdomain.h
// https://github.com/mono/mono/blob/master/mono/metadata/class.h
// https://github.com/mono/mono/blob/master/mono/metadata/metadata.h

struct MonoApi {
    void* module = nullptr;

    // Required
    void* domain_get               = nullptr;  // MonoDomain* mono_domain_get(void)
    void* assembly_foreach         = nullptr;  // void mono_assembly_foreach(GFunc func, gpointer user_data)
    void* assembly_get_image       = nullptr;  // MonoImage* mono_assembly_get_image(MonoAssembly*)
    void* class_from_name          = nullptr;  // MonoClass* mono_class_from_name(MonoImage*, const char* ns, const char* name)
    void* class_get_methods        = nullptr;  // MonoMethod* mono_class_get_methods(MonoClass*, gpointer* iter)
    void* method_get_name          = nullptr;  // const char* mono_method_get_name(MonoMethod*)
    void* method_get_flags         = nullptr;  // uint32_t mono_method_get_flags(MonoMethod*, uint32_t* iflags)
    void* method_signature         = nullptr;  // MonoMethodSignature* mono_method_signature(MonoMethod*)
    void* signature_get_params     = nullptr;  // MonoType* mono_signature_get_params(MonoMethodSignature*, gpointer* iter)
    void* signature_get_return_type = nullptr; // MonoType* mono_signature_get_return_type(MonoMethodSignature*)
    void* type_get_type            = nullptr;  // int mono_type_get_type(MonoType*)
    void* compile_method           = nullptr;  // gpointer mono_compile_method(MonoMethod*)
    void* runtime_invoke           = nullptr;  // MonoObject* mono_runtime_invoke(...): the bootstrap trigger
    void* string_new               = nullptr;  // MonoString* mono_string_new(MonoDomain*, const char*)

    void* type_is_byref            = nullptr;  // mono_bool mono_type_is_byref(MonoType*)
    void* type_size                = nullptr;  // int mono_type_size(MonoType*, int* alignment)
    void* class_get_field_from_name = nullptr; // MonoClassField* mono_class_get_field_from_name(MonoClass*, const char*)
    void* field_get_offset         = nullptr;  // uint32_t mono_field_get_offset(MonoClassField*)
    void* pmip                     = nullptr;  // char* mono_pmip(void* ip): "Namespace.Class:Method (args) + 0x1c" of the JIT-compiled code at ip
    void* mono_free                = nullptr;  // void mono_free(void*): releases what mono_pmip returned

    bool valid() const {
        return domain_get && assembly_foreach && assembly_get_image && class_from_name &&
               class_get_methods && method_get_name && method_get_flags && method_signature &&
               signature_get_params && signature_get_return_type && type_get_type &&
               compile_method && runtime_invoke && string_new;
    }
};

static MonoApi g_mono;

static bool is_mono_module(const std::string& lower) {
    static const char* names[] = {
        "mono-2.0-bdwgc",
        "monobdwgc-2.0",
        "mono-2.0-sgen",
        "monosgen-2.0",
        "mono-2.0",
        "mono.dll",
        "libmono.so",
    };
    for (const char* n : names)
        if (lower.find(n) != std::string::npos) return true;
    return false;
}

static bool mono_api_init(const char** why) {
    *why = nullptr;

    void* mod = find_loaded_module(is_mono_module);
    if (!mod) {
        *why = "mono runtime module";
        return false;
    }
    g_mono.module = mod;

    struct Entry { const char* name; void** slot; bool required; };
    const Entry entries[] = {
        { "mono_domain_get",                  &g_mono.domain_get,                true },
        { "mono_assembly_foreach",            &g_mono.assembly_foreach,          true },
        { "mono_assembly_get_image",          &g_mono.assembly_get_image,        true },
        { "mono_class_from_name",             &g_mono.class_from_name,           true },
        { "mono_class_get_methods",           &g_mono.class_get_methods,         true },
        { "mono_method_get_name",             &g_mono.method_get_name,           true },
        { "mono_method_get_flags",            &g_mono.method_get_flags,          true },
        { "mono_method_signature",            &g_mono.method_signature,          true },
        { "mono_signature_get_params",        &g_mono.signature_get_params,      true },
        { "mono_signature_get_return_type",   &g_mono.signature_get_return_type, true },
        { "mono_type_get_type",               &g_mono.type_get_type,             true },
        { "mono_compile_method",              &g_mono.compile_method,            true },
        { "mono_runtime_invoke",              &g_mono.runtime_invoke,            true },
        { "mono_string_new",                  &g_mono.string_new,                true },
        { "mono_type_is_byref",               &g_mono.type_is_byref,             false },
        { "mono_type_size",                   &g_mono.type_size,                 false },
        { "mono_class_get_field_from_name",   &g_mono.class_get_field_from_name, false },
        { "mono_field_get_offset",            &g_mono.field_get_offset,          false },
        { "mono_pmip",                        &g_mono.pmip,                      false },
        { "mono_free",                        &g_mono.mono_free,                 false },
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

static void* mono_export(const char* name) {
    return module_symbol(g_mono.module, name);
}

// ===============================================================
// Backend
// ===============================================================

namespace {

// ---------------------------------------------------------------
// Wrappers over the runtime API
// ---------------------------------------------------------------

void* domain_get()                                   { return runtime_call<void*>(g_mono.domain_get); }
void* assembly_image(void* assembly)                 { return runtime_call<void*>(g_mono.assembly_get_image, assembly); }
uint32_t method_flags(void* m, uint32_t* iflags)     { return runtime_call<uint32_t>(g_mono.method_get_flags, m, iflags); }
void* method_signature(void* m)                      { return runtime_call<void*>(g_mono.method_signature, m); }
void* signature_params(void* sig, void** iter)       { return runtime_call<void*>(g_mono.signature_get_params, sig, iter); }
void* signature_return(void* sig)                    { return runtime_call<void*>(g_mono.signature_get_return_type, sig); }
int   type_type(void* t)                             { return runtime_call<int>(g_mono.type_get_type, t); }

std::vector<void*>* g_collect = nullptr;

void collect_assembly(void* assembly)
{
    if (!g_collect) return;
    if (void* img = assembly_image(assembly))
        g_collect->push_back(img);
}

extern "C" void mono_collect_cb_native(void* assembly, void*) { collect_assembly(assembly); }
#if defined(__linux__) && defined(__x86_64__)
extern "C" __attribute__((ms_abi)) void mono_collect_cb_ms(void* assembly, void*) { collect_assembly(assembly); }
#endif

// ---------------------------------------------------------------
// RuntimeOps
// ---------------------------------------------------------------

bool ops_attached()
{
    return domain_get() != nullptr;
}

void ops_collect_images(std::vector<void*>& images)
{
    g_collect = &images;
    void* cb = reinterpret_cast<void*>(&mono_collect_cb_native);
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine) cb = reinterpret_cast<void*>(&mono_collect_cb_ms);
#endif
    runtime_call<void, void*, void*>(g_mono.assembly_foreach, cb, nullptr);
    g_collect = nullptr;
}

void* ops_class_from_name(void* img, const char* ns, const char* name)
{
    return runtime_call<void*>(g_mono.class_from_name, img, ns, name);
}

void* ops_next_method(void* klass, void** iter)
{
    return runtime_call<void*>(g_mono.class_get_methods, klass, iter);
}

const char* ops_method_name(void* m)
{
    return runtime_call<const char*>(g_mono.method_get_name, m);
}

bool param_of_type(void* type, ParamInfo* out)
{
    if (g_mono.type_is_byref && runtime_call<int>(g_mono.type_is_byref, type)) {
        out->kind = ParamKind::Ptr;
        out->size = 0;
        return true;
    }
    const int code = type_type(type);
    uint32_t size = 0;
    if (code == ET_VALUETYPE && g_mono.type_size) {
        int align = 0;
        const int s = runtime_call<int, void*, int*>(g_mono.type_size, type, &align);
        if (s > 0) size = static_cast<uint32_t>(s);
    }
    return param_from_element_type(code, size, out);
}

bool ops_analyze(void* method, MethodShape* shape)
{
    uint32_t iflags = 0;
    const uint32_t flags = method_flags(method, &iflags);
    if (flags & (METHOD_ATTR_ABSTRACT | METHOD_ATTR_PINVOKE_IMPL)) return false;
    if (iflags & METHOD_IMPL_RUNTIME) return false;
    shape->is_static = (flags & METHOD_ATTR_STATIC) != 0;

    void* sig = method_signature(method);
    if (!sig) return false;

    if (void* ret = signature_return(sig)) {
        if (type_type(ret) != ET_VOID) {
            ParamInfo r;
            if (!param_of_type(ret, &r) || r.kind == ParamKind::Struct) return false;
        }
    }

    ParamInfo params[16];
    int n = 0;
    void* iter = nullptr;
    while (void* pt = signature_params(sig, &iter)) {
        if (n >= 16 || !param_of_type(pt, &params[n])) return false;
        ++n;
    }

    layout_string_args(current_call_abi(), shape->is_static, params, n, &shape->strings);
    return true;
}

void* ops_code_address(void* method)
{
    return runtime_call<void*>(g_mono.compile_method, method);
}

bool ops_field_offset(void* klass, const char* field, uint32_t* offset)
{
    if (!g_mono.class_get_field_from_name || !g_mono.field_get_offset) return false;
    void* f = runtime_call<void*>(g_mono.class_get_field_from_name, klass, field);
    if (!f) return false;
    *offset = runtime_call<uint32_t>(g_mono.field_get_offset, f);
    return true;
}

bool ops_learn_string_word()
{
    void* domain = domain_get();
    if (!domain) return false;
    void* s = runtime_call<void*>(g_mono.string_new, domain, "");
    if (!s) return false;
    sink_set_string_word(*static_cast<uintptr_t*>(s));
    return true;
}

const RuntimeOps kMonoOps = {
    &ops_attached, &ops_collect_images, &ops_class_from_name,
    nullptr, // begin_class
    &ops_next_method, &ops_method_name, &ops_analyze, &ops_code_address,
    &ops_field_offset, &ops_learn_string_word,
    nullptr, // after_scan
};

bool caller_name(uintptr_t ip, char* out, size_t cap)
{
    if (!g_mono.pmip || !domain_get())
        return false;

    char* raw = runtime_call<char*>(g_mono.pmip, reinterpret_cast<void*>(ip));
    if (!raw)
        return false;

    std::string name(raw);
    if (g_mono.mono_free)
        runtime_call<void, void*>(g_mono.mono_free, raw);

    if (const size_t plus = name.rfind(" + 0x"); plus != std::string::npos)
        name.resize(plus);

    if (!name.empty() && name[0] == '[') {
        if (const size_t close = name.find("] "); close != std::string::npos) {
            name.erase(0, close + 2);
        }
    }

    if (const size_t paren = name.find(" ("); paren != std::string::npos)
        name.resize(paren);

    while (!name.empty() && static_cast<unsigned char>(name[0]) < 0x21)
        name.erase(0, 1);

    if (name.empty())
        return false;

    // "Ns.Sub.Class:Method" -> "Class.Method"
    if (const size_t colon = name.find(':'); colon != std::string::npos) {
        const size_t dot = name.rfind('.', colon);
        const std::string cls = dot == std::string::npos ? name.substr(0, colon) : name.substr(dot + 1, colon - dot - 1);
        name = cls + "." + name.substr(colon + 1);
    }
    for (char& c : name)
        if (c == ':') c = '.';

    std::snprintf(out, cap, "%s", name.c_str());
    return true;
}

void* resolve_export(const char* name)
{
    return mono_export(name);
}

} // namespace

bool mono_backend_init(const char** why)
{
    if (!mono_api_init(why))
        return false;

    managed_init(&kMonoOps);
    sink_set_caller_namer(&caller_name);

    managed_install_native(kMonoNativeTargets, kMonoNativeTargetCount, &resolve_export);

    if (managed_install_trigger(g_mono.runtime_invoke) != HookResult::Ok) {
        *why = "cannot hook mono_runtime_invoke";
        return false;
    }
    return true;
}
