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

#ifndef CONFIG_H
#define CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

constexpr const char* PIPE_NAME = "AurexTranslator_libat-unity";
constexpr size_t kMaxSourceLen = 64;

enum Group : uint8_t {
    GROUP_NONE   = 0,
    GROUP_SETTER = 1, // methods that set the text of a UI component (Text, TMP_Text, GUI.Label, ...)
    GROUP_RENDER = 2, // methods that build the mesh of a component: the text is read from its field
    GROUP_NATIVE = 3, // string functions of the runtime API (mono_string_new, il2cpp_string_new, ...)
    GROUP_STRING = 4, // System.String methods: any text is processed by them sooner or later
};

inline const char* group_name(uint8_t group)
{
    switch (group) {
    case GROUP_SETTER: return "Setter";
    case GROUP_RENDER: return "Render";
    case GROUP_NATIVE: return "Native";
    case GROUP_STRING: return "String";
    }
    return "?";
}

void plugin_send_text(const char* source, uint8_t variant, const std::string& utf8);

#endif // CONFIG_H
