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

#ifndef UNICODE_API_H
#define UNICODE_API_H

#include <python3.10/Python.h>

// Function pointer types for Python C API
using fn_PyUnicode_AsUTF8    = const char* (*)(PyObject*);
using fn_PyUnicode_AsUTF8Str = PyObject*   (*)(PyObject*);
using fn_PyBytes_AsString    = char*       (*)(PyObject*);

enum class PyUnicodeVariant { Unknown, UTF8, UCS2, UCS4 };

struct PyUnicodeAPI {
    PyUnicodeVariant        variant     = PyUnicodeVariant::Unknown;

    // PyUnicode[UCS2|UCS4]_Format / PyUnicode_Format — the function we hook.
    void*                   fmt         = nullptr;

    // String conversion (variant-dependent):
    // Python 3 as_utf8(obj)
    // Python 2 as_string(as_utf8_str(obj))
    fn_PyUnicode_AsUTF8     as_utf8     = nullptr;
    fn_PyBytes_AsString     as_string   = nullptr;
    fn_PyUnicode_AsUTF8Str  as_utf8_str = nullptr;

    // Generic Python C API used to resolve Ren'Py substitutions
    void* py_dec_ref        = nullptr;  // Py_DecRef (public function, not macro)
    void* import_module     = nullptr;  // PyImport_ImportModule
    void* getattr_string    = nullptr;  // PyObject_GetAttrString
    void* call_obj_args     = nullptr;  // PyObject_CallFunctionObjArgs
    void* tuple_getitem     = nullptr;  // PyTuple_GetItem
    void* err_clear         = nullptr;  // PyErr_Clear
};

extern PyUnicodeAPI g_unicode_api;

// Detects which Unicode API variant is available in the given module
PyUnicodeAPI detect_unicode_api(void* python_module);

// Calls renpy.substitutions.substitute(format) to expand Ren'Py placeholders
const char* resolve_substitutions(PyObject* format);

#endif // UNICODE_API_H
