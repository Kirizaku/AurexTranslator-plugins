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

#include "unicode_api.h"
#include "python_runtime.h"
#include "memory_utils.h"

#include <string>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

PyUnicodeAPI g_unicode_api;

// ===============================================================
// symbol resolution
// ===============================================================

static void* resolve_sym(void* module, const char* name) {
    if (!module || !name) return nullptr;
#if defined(_WIN32)
    return GetProcAddress(static_cast<HMODULE>(module), name);
#else
    return g_is_wine ? find_export(module, name) : dlsym(module, name);
#endif
}

static void release_module(void* module) {
#if !defined(_WIN32)
    if (!g_is_wine && module) dlclose(module);
#else
    (void)module;
#endif
}

// ===============================================================
// API detection
// ===============================================================

PyUnicodeAPI detect_unicode_api(void* python_module) {
    PyUnicodeAPI api;

    // Probe format functions: Py3 first, then UCS4, UCS2
    struct { const char* sym; PyUnicodeVariant variant; } fmt_candidates[] = {
                           { "PyUnicode_Format",     PyUnicodeVariant::UTF8 },
                           { "PyUnicodeUCS4_Format", PyUnicodeVariant::UCS4 },
                           { "PyUnicodeUCS2_Format", PyUnicodeVariant::UCS2 },
                           };

    for (auto& c : fmt_candidates) {
        api.fmt = resolve_sym(python_module, c.sym);
        if (api.fmt) { api.variant = c.variant; break; }
    }

    if (api.fmt) {
        // Variant-specific string
        if (api.variant == PyUnicodeVariant::UTF8) {
            api.as_utf8 = reinterpret_cast<fn_PyUnicode_AsUTF8>(resolve_sym(python_module, "PyUnicode_AsUTF8"));
        } else {
            const std::string prefix = (api.variant == PyUnicodeVariant::UCS2)
            ? "PyUnicodeUCS2_" : "PyUnicodeUCS4_";
            api.as_string   = reinterpret_cast<fn_PyBytes_AsString>(resolve_sym(python_module, "PyString_AsString"));
            api.as_utf8_str = reinterpret_cast<fn_PyUnicode_AsUTF8Str>(resolve_sym(python_module, (prefix + "AsUTF8String").c_str()));
        }

        // Common functions for both Python 2 and Python 3
        api.py_dec_ref     = resolve_sym(python_module, "Py_DecRef");
        api.import_module  = resolve_sym(python_module, "PyImport_ImportModule");
        api.getattr_string = resolve_sym(python_module, "PyObject_GetAttrString");
        api.call_obj_args  = resolve_sym(python_module, "PyObject_CallFunctionObjArgs");
        api.tuple_getitem  = resolve_sym(python_module, "PyTuple_GetItem");
        api.err_clear      = resolve_sym(python_module, "PyErr_Clear");
    }

    release_module(python_module);
    return api;
}

// ===============================================================
// Wine ABI (Linux x64 only)
// ===============================================================

#if defined(__linux__) && defined(__x86_64__)

static inline void wine_call_void_arg(void* func, PyObject* arg) {
    asm volatile (
        "sub $32, %%rsp\n\t"
        "call *%0\n\t"
        "add $32, %%rsp\n\t"
        :
    : "r"(func), "c"(arg)
        : "rax", "rdx", "r8", "r9", "r10", "r11", "memory"
        );
}

template<typename Ret, typename A1>
static inline Ret wine_call(void* func, A1 a1) {
    Ret r;
    asm volatile (
        "sub $32, %%rsp\n\t"
        "call *%1\n\t"
        "add $32, %%rsp\n\t"
        : "=a"(r)
        : "r"(func), "c"(a1)
        : "rdx", "r8", "r9", "r10", "r11", "memory"
        );
    return r;
}

template<typename Ret, typename A1, typename A2>
static inline Ret wine_call(void* func, A1 a1, A2 a2) {
    Ret r;
    asm volatile (
        "sub $32, %%rsp\n\t"
        "call *%1\n\t"
        "add $32, %%rsp\n\t"
        : "=a"(r)
        : "r"(func), "c"(a1), "d"(a2)
        : "r8", "r9", "r10", "r11", "memory"
        );
    return r;
}

template<typename Ret, typename A1, typename A2, typename A3>
static inline Ret wine_call(void* func, A1 a1, A2 a2, A3 a3) {
    Ret r;
    asm volatile (
        "mov %4, %%r8\n\t"
        "sub $32, %%rsp\n\t"
        "call *%1\n\t"
        "add $32, %%rsp\n\t"
        : "=a"(r)
        : "r"(func), "c"(a1), "d"(a2), "r"(a3)
        : "r8", "r9", "r10", "r11", "memory"
        );
    return r;
}

// void-returning version (templates can't deduce void via "=a")
static inline void wine_call_void(void* func) {
    asm volatile (
        "sub $32, %%rsp\n\t"
        "call *%0\n\t"
        "add $32, %%rsp\n\t"
        :
        : "r"(func)
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"
        );
}
#endif

// ===============================================================
// Python C API
// ===============================================================

template<typename FnT, typename... Args>
static inline auto native_call(void* fn, Args... args) {
    return reinterpret_cast<FnT>(fn)(args...);
}

static inline PyObject* py_import(const char* name) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<PyObject*>(g_unicode_api.import_module, name);
#endif
    return native_call<PyObject*(*)(const char*)>(g_unicode_api.import_module, name);
}

static inline PyObject* py_getattr(PyObject* obj, const char* name) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<PyObject*>(g_unicode_api.getattr_string, obj, name);
#endif
    return native_call<PyObject*(*)(PyObject*, const char*)>(g_unicode_api.getattr_string, obj, name);
}

static inline PyObject* py_call_1arg(PyObject* func, PyObject* arg) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<PyObject*>(g_unicode_api.call_obj_args, func, arg, (PyObject*)nullptr);
#endif
    return native_call<PyObject*(*)(PyObject*, ...)>(g_unicode_api.call_obj_args, func, arg, nullptr);
}

static inline PyObject* py_tuple_get(PyObject* tup, Py_ssize_t i) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<PyObject*>(g_unicode_api.tuple_getitem, tup, i);
#endif
    return native_call<PyObject*(*)(PyObject*, Py_ssize_t)>(g_unicode_api.tuple_getitem, tup, i);
}

static inline void py_err_clear() {
    if (!g_unicode_api.err_clear) return;
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine) {
        wine_call_void(g_unicode_api.err_clear);
        return;
    }
#endif
    native_call<void(*)()>(g_unicode_api.err_clear);
}

static inline void py_decref(PyObject* obj) {
    if (!obj || !g_unicode_api.py_dec_ref) return;
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine) {
        wine_call_void_arg(g_unicode_api.py_dec_ref, obj);
        return;
    }
#endif
    native_call<void(*)(PyObject*)>(g_unicode_api.py_dec_ref, obj);
}

// ===============================================================
// String conversion
// ===============================================================

static inline PyObject* call_as_utf8_str(PyObject* obj) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<PyObject*>((void*)g_unicode_api.as_utf8_str, obj);
#endif
    return g_unicode_api.as_utf8_str(obj);
}

static inline const char* call_as_utf8(PyObject* obj) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<const char*>((void*)g_unicode_api.as_utf8, obj);
#endif
    return g_unicode_api.as_utf8(obj);
}

static inline const char* call_as_string(PyObject* obj) {
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return wine_call<const char*>((void*)g_unicode_api.as_string, obj);
#endif
    return g_unicode_api.as_string(obj);
}

static const char* unicode_to_utf8(PyObject* obj) {
    if (g_unicode_api.as_utf8)
        return call_as_utf8(obj);
    if (g_unicode_api.as_utf8_str && g_unicode_api.as_string) {
        PyObject* bytes = call_as_utf8_str(obj);
        return bytes ? call_as_string(bytes) : nullptr;
    }
    return nullptr;
}

// ===============================================================
// Ren'Py substitution: [player_name] -> "User123", etc.
// ===============================================================

const char* resolve_substitutions(PyObject* format) {
    PyObject* mod = py_import("renpy.substitutions");
    if (!mod) return unicode_to_utf8(format);

    PyObject* func = py_getattr(mod, "substitute");
    py_decref(mod);
    if (!func) return unicode_to_utf8(format);

    PyObject* result = py_call_1arg(func, format);
    py_decref(func);
    if (!result) {
        py_err_clear();
        return unicode_to_utf8(format);
    }

    PyObject* resolved = py_tuple_get(result, 0);
    if (!resolved) {
        py_err_clear();
        resolved = result;
    }

    const char* text = unicode_to_utf8(resolved);
    return text ? text : unicode_to_utf8(format);
}