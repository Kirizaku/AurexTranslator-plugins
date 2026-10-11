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

const ManagedTarget kManagedTargets[] = {
    { GROUP_SETTER, "UnityEngine.UI", "Text",            "set_text", nullptr },
    { GROUP_SETTER, "UnityEngine",    "TextMesh",        "set_text", nullptr },
    { GROUP_SETTER, "UnityEngine",    "GUIText",         "set_text", nullptr },
    { GROUP_SETTER, "TMPro",          "TMP_Text",        "set_text", nullptr },
    { GROUP_SETTER, "TMPro",          "TMP_Text",        "SetText",  nullptr },
    { GROUP_SETTER, "TMPro",          "TextMeshProUGUI", "set_text", nullptr },
    { GROUP_SETTER, "TMPro",          "TextMeshProUGUI", "SetText",  nullptr },
    { GROUP_SETTER, "TMPro",          "TextMeshPro",     "set_text", nullptr },
    { GROUP_SETTER, "TMPro",          "TextMeshPro",     "SetText",  nullptr },
    { GROUP_SETTER, "",               "UILabel",         "set_text", nullptr },
    { GROUP_SETTER, "",               "tk2dTextMesh",    "set_text", nullptr },
    { GROUP_SETTER, "UnityEngine",    "GUIContent",      "*",        nullptr },
    { GROUP_SETTER, "UnityEngine",    "GUI",             "*",        nullptr },
    { GROUP_SETTER, "UnityEngine",    "GUILayout",       "*",        nullptr },
    { GROUP_RENDER, "UnityEngine.UI", "Text",     "OnPopulateMesh", "m_Text" },
    { GROUP_RENDER, "TMPro",          "TMP_Text", "ParseInputText", "m_text" },
    { GROUP_RENDER, "",               "UILabel",  "ProcessText",    "mText"  },
    { GROUP_STRING, "System", "String", "*", nullptr, HookSet::SystemString },
};
const int kManagedTargetCount = static_cast<int>(sizeof(kManagedTargets) / sizeof(kManagedTargets[0]));

// The string functions of the Mono API 
// https://www.mono-project.com/docs/advanced/embedding/
// https://github.com/mono/mono/blob/main/docs/sources/mono-api-string.html
const NativeTarget kMonoNativeTargets[] = {
    { "mono_string_to_utf8",           SinkKind::ManagedArgs, { 0 },    1, HookSet::EngineStrings },
    { "mono_string_to_utf8_checked",   SinkKind::ManagedArgs, { 0 },    1, HookSet::EngineStrings },
    { "mono_string_to_utf16",          SinkKind::ManagedArgs, { 0 },    1, HookSet::EngineStrings },
    { "mono_string_chars",             SinkKind::ManagedArgs, { 0 },    1, HookSet::EngineStrings },
    { "mono_string_intern",            SinkKind::ManagedArgs, { 0 },    1, HookSet::CreatedStrings },
    { "mono_string_is_interned",       SinkKind::ManagedArgs, { 0 },    1, HookSet::CreatedStrings },
    { "mono_marshal_string_to_utf16",  SinkKind::ManagedArgs, { 0 },    1, HookSet::EngineStrings },
    { "mono_string_new",               SinkKind::Utf8Z,       { 1 },    1, HookSet::CreatedStrings },
    { "mono_string_new_len",           SinkKind::Utf8Len,     { 1, 2 }, 2, HookSet::CreatedStrings },
    { "mono_string_new_wrapper",       SinkKind::Utf8Z,       { 0 },    1, HookSet::CreatedStrings },
    { "mono_string_new_utf16",         SinkKind::Utf16Len,    { 1, 2 }, 2, HookSet::CreatedStrings },
    { "mono_string_from_utf16",        SinkKind::Utf16Z,      { 0 },    1, HookSet::CreatedStrings },
    { "mono_utf8_from_external",       SinkKind::Utf8Z,       { 0 },    1, HookSet::CreatedStrings },
    { "mono_unicode_from_external",    SinkKind::Utf8Z,       { 0 },    1, HookSet::CreatedStrings },
    { "mono_unicode_to_external",      SinkKind::Utf16Z,      { 0 },    1, HookSet::EngineStrings },
};
const int kMonoNativeTargetCount = static_cast<int>(sizeof(kMonoNativeTargets) / sizeof(kMonoNativeTargets[0]));

// The string functions of the IL2CPP API. UnityPlayer finds them in GameAssembly by name and
// converts every managed string it needs through them. il2cpp_string_length is left out for
// the same reason as its Mono counterpart. They take no domain, unlike the Mono ones
const NativeTarget kIl2cppNativeTargets[] = {
    { "il2cpp_string_chars",           SinkKind::ManagedArgs, { 0 },    1, HookSet::EngineStrings },
    { "il2cpp_string_intern",          SinkKind::ManagedArgs, { 0 },    1, HookSet::CreatedStrings },
    { "il2cpp_string_is_interned",     SinkKind::ManagedArgs, { 0 },    1, HookSet::CreatedStrings },
    { "il2cpp_string_new",             SinkKind::Utf8Z,       { 0 },    1, HookSet::CreatedStrings },
    { "il2cpp_string_new_len",         SinkKind::Utf8Len,     { 0, 1 }, 2, HookSet::CreatedStrings },
    { "il2cpp_string_new_utf16",       SinkKind::Utf16Len,    { 0, 1 }, 2, HookSet::CreatedStrings },
    { "il2cpp_string_new_wrapper",     SinkKind::Utf8Z,       { 0 },    1, HookSet::CreatedStrings },
};

const int kIl2cppNativeTargetCount = static_cast<int>(sizeof(kIl2cppNativeTargets) / sizeof(kIl2cppNativeTargets[0]));
