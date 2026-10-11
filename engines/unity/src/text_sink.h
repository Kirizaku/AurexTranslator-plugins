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

#ifndef TEXT_SINK_H
#define TEXT_SINK_H

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "hook_engine.h"

// Hooks that report text: every hook has one entry in the sink table. Must stay below kBootstrapId
constexpr int kMaxSinks          = 4096;

// A managed string longer than this many UTF-16 units is not text
constexpr int kMaxTextChars      = 1 << 16;

// The same text from the same source again within this time is not sent again. Hooks on
// methods such as Equals see a few strings over and over (A, B, A, B, ...), which would
// flood the program; a text shown again after this long is a new event and is sent
constexpr unsigned kRepeatWindowMs    = 1500;

// All hooks together may be called this many times per second. Above it the hottest hooks are
// switched off: a text is never set that often, and the game would crawl
constexpr unsigned kMaxCallsPerSec    = 20000;

// A single hook called more often than this is switched off at once, after a few hundred calls
constexpr unsigned kMaxHookCallsPerSec = 10000;

// A switched-off hook is tried again after the first pause, then after the second one, and the
// third time it stays off
constexpr uint32_t kMuteFirstMs       = 30000;
constexpr uint32_t kMuteSecondMs      = 300000;

// How the text is found in the arguments of the hooked call
enum class SinkKind : uint8_t {
    None,
    ManagedArgs,  // each listed argument is a managed string (MonoString* / Il2CppString*)
    ManagedField, // `this` (argument 0) has a managed string at a fixed offset
    Utf8Z,        // argument is a NUL-terminated UTF-8 string
    Utf8Len,      // argument is a UTF-8 buffer, the length is in another argument
    Utf16Z,       // argument is a NUL-terminated UTF-16 string
    Utf16Len,     // argument is a UTF-16 buffer, the length (in characters) is in another argument
};

// The id passed to the hook engine for the hook that bootstraps the plugin
constexpr uint32_t kBootstrapId = 0xFFFFFF00u;

int sink_add(SinkKind kind, uint8_t group, const char* source,
              const uint8_t* slots, int nslots, int32_t field_offset = 0,
              bool split_by_caller = false, bool split_by_object = false);

void sink_disable(int id);
void sink_set_string_word(uintptr_t word);
void sink_set_game_abi(bool ms_abi);
void sink_set_trigger(void (*fn)());
void sink_set_caller_namer(bool (*fn)(uintptr_t return_address, char* out, size_t cap));
void sink_handler(const HookRegs* regs, uint32_t id);

#endif // TEXT_SINK_H
