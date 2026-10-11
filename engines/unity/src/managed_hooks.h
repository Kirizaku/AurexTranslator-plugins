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

#ifndef MANAGED_HOOKS_H
#define MANAGED_HOOKS_H

#include <cstdint>
#include <vector>

#include "hook_engine.h"
#include "runtime_module.h"
#include "text_sink.h"

enum class ParamKind : uint8_t {
    String,
    Int,    // bool, char, 8..32-bit integers
    Int64,
    Float,
    Double,
    Ptr,    // objects, arrays, pointers, references
    Struct, // value type: `size` matters
};

struct ParamInfo {
    ParamKind kind;
    uint32_t  size; // Struct only
};

enum class CallAbi { Ms64, SysV64, X86 };

inline CallAbi current_call_abi()
{
#if defined(__x86_64__) || defined(_M_X64)
    return game_uses_ms_abi() ? CallAbi::Ms64 : CallAbi::SysV64;
#else
    return CallAbi::X86;
#endif
}

struct ArgLayout {
    uint8_t slots[8];
    int count;
};

inline void layout_string_args(CallAbi abi, bool is_static, const ParamInfo* p, int n, ArgLayout* out)
{
    out->count = 0;
    int  pos    = is_static ? 0 : 1;
    int  ints   = is_static ? 0 : 1;
    bool lost   = false;

    for (int i = 0; i < n; ++i) {
        const ParamKind k = p[i].kind;
        int at = -1;

        switch (abi) {
        case CallAbi::Ms64:
            at = pos++;
            break;

        case CallAbi::SysV64:
            if (k == ParamKind::Float || k == ParamKind::Double) break; // xmm register
            if (k == ParamKind::Struct) { lost = true; break; }
            if (!lost && ints < 6) at = ints++;
            break;

        case CallAbi::X86:
            at = pos;
            if (k == ParamKind::Int64 || k == ParamKind::Double) pos += 2;
            else if (k == ParamKind::Struct) pos += p[i].size ? static_cast<int>((p[i].size + 3) / 4) : 1;
            else pos += 1;
            break;
        }

        if (k == ParamKind::String && at >= 0 && out->count < 8)
            out->slots[out->count++] = static_cast<uint8_t>(at);
    }
}

// A managed method looked up by name in whichever loaded assembly holds its class.
// Both backends (Mono and IL2CPP) use the same table
enum class HookSet : uint8_t {
    Text,           // text components: always installed
    EngineStrings,  // runtime functions that take a managed string out to the engine
    CreatedStrings, // runtime functions that create a managed string
    SystemString,   // methods of System.String
};

struct ManagedTarget {
    uint8_t     group;
    const char* name_space; // "" for the global namespace
    const char* class_name;
    const char* method;     // "*" - every method of the class
    const char* field;      // GROUP_RENDER: the field of `this` that holds the text
    HookSet set = HookSet::Text;
};

extern const ManagedTarget kManagedTargets[];
extern const int           kManagedTargetCount;

// An exported string function of the Mono runtime
struct NativeTarget {
    const char* name;
    SinkKind kind;
    uint8_t slots[2]; // Utf8Z / Utf16Z / ManagedArgs: {text}; *Len: {text, length}
    int nslots;
    HookSet set = HookSet::Text;
};

extern const NativeTarget kMonoNativeTargets[];
extern const int          kMonoNativeTargetCount;

// The same for IL2CPP (exports of GameAssembly)
extern const NativeTarget kIl2cppNativeTargets[];
extern const int          kIl2cppNativeTargetCount;

// Entries of kManagedTargets (unity_targets.cpp)
constexpr int      kMaxManagedTargets = 128;

// How often (at most) the loaded assemblies are checked for new classes
constexpr unsigned kRescanIntervalMs  = 1000;

struct MethodShape {
    bool      is_static = false;
    ArgLayout strings{}; // call-argument index of every string parameter
};

struct RuntimeOps {
    // Is the calling thread one the runtime knows? Only such a thread may call into it
    bool        (*attached)();
    // Every assembly image loaded so far
    void        (*collect_images)(std::vector<void*>& out);
    void*       (*class_from_name)(void* image, const char* ns, const char* name);
    // Optional: called before the methods of a class are walked; false skips the class
    bool        (*begin_class)(void* klass);
    void*       (*next_method)(void* klass, void** iter);
    const char* (*method_name)(void* method);
    // Can the method be hooked, and where are its string arguments?
    bool        (*analyze)(void* method, MethodShape* shape);
    // The native code of the method (Mono compiles it first); nullptr if there is none to hook
    void*       (*code_address)(void* method);
    // The offset of an instance field, counted from the start of the object
    bool        (*field_offset)(void* klass, const char* field, uint32_t* offset);
    // Makes sink_set_string_word() know what a string object starts with
    bool        (*learn_string_word)();
    // Optional: called after a scan that found new assemblies
    void        (*after_scan)(const std::vector<void*>& images);
};

void managed_init(const RuntimeOps* ops);
void managed_install_native(const NativeTarget* targets, int count, void* (*resolve)(const char* name));
HookResult managed_install_trigger(void* function);
void managed_trigger();

// The element types of Mono and IL2CPP have the same values (ECMA-335)
enum ElementType : int {
    ET_VOID = 0x01, ET_BOOLEAN = 0x02, ET_CHAR = 0x03, ET_I1 = 0x04, ET_U1 = 0x05, ET_I2 = 0x06, ET_U2 = 0x07,
    ET_I4 = 0x08, ET_U4 = 0x09, ET_I8 = 0x0a, ET_U8 = 0x0b, ET_R4 = 0x0c, ET_R8 = 0x0d, ET_STRING = 0x0e,
    ET_PTR = 0x0f, ET_VALUETYPE = 0x11, ET_CLASS = 0x12, ET_I = 0x18, ET_U = 0x19, ET_FNPTR = 0x1b,
    ET_OBJECT = 0x1c, ET_SZARRAY = 0x1d,
};

inline bool param_from_element_type(int code, uint32_t value_size, ParamInfo* out)
{
    out->size = 0;
    switch (code) {
    case ET_BOOLEAN: case ET_CHAR: case ET_I1: case ET_U1: case ET_I2: case ET_U2: case ET_I4: case ET_U4:
        out->kind = ParamKind::Int;    return true;
    case ET_I8: case ET_U8:
        out->kind = ParamKind::Int64;  return true;
    case ET_R4:
        out->kind = ParamKind::Float;  return true;
    case ET_R8:
        out->kind = ParamKind::Double; return true;
    case ET_STRING:
        out->kind = ParamKind::String; return true;
    case ET_PTR: case ET_CLASS: case ET_OBJECT: case ET_SZARRAY: case ET_I: case ET_U: case ET_FNPTR:
        out->kind = ParamKind::Ptr;    return true;
    case ET_VALUETYPE:
        if (value_size == 0) return false;
        out->kind = ParamKind::Struct;
        out->size = value_size;
        return true;
    }
    return false;
}

// Method attributes (ECMA-335, the same in both runtimes)
constexpr uint32_t METHOD_ATTR_STATIC       = 0x0010;
constexpr uint32_t METHOD_ATTR_ABSTRACT     = 0x0400;
constexpr uint32_t METHOD_ATTR_PINVOKE_IMPL = 0x2000;
constexpr uint32_t METHOD_IMPL_RUNTIME      = 0x0008;

#endif // MANAGED_HOOKS_H
