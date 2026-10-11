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

#include "text_sink.h"
#include "config.h"
#include "hook/memory_utils.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <time.h>
#endif

// ===============================================================
// Text helpers
// ===============================================================

static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static std::string utf16_to_utf8(const char16_t* chars, int32_t length) {
    std::string out;
    if (!chars || length <= 0) return out;
    out.reserve(static_cast<size_t>(length) * 3);

    for (int32_t i = 0; i < length; ++i) {
        uint32_t u = chars[i];

        if (u >= 0xD800 && u <= 0xDBFF) {
            // High surrogate: combine with a following low surrogate
            if (i + 1 < length && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
                ++i;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        append_utf8(out, u);
    }
    return out;
}

static bool is_valid_utf8(const char* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        size_t extra = 0;
        uint32_t min = 0;
        if (c < 0x80)              { ++i; continue; }
        else if ((c >> 5) == 0x6)  { extra = 1; min = 0x80; }
        else if ((c >> 4) == 0xE)  { extra = 2; min = 0x800; }
        else if ((c >> 3) == 0x1E) { extra = 3; min = 0x10000; }
        else return false;

        if (i + extra >= n) return false;
        uint32_t cp = c & (0x3F >> extra);
        for (size_t k = 1; k <= extra; ++k) {
            const uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

static uint64_t fnv1a64(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

constexpr int kRecent = 8;
struct Recent {
    std::atomic<uint64_t> hash[kRecent];
    std::atomic<uint32_t> at[kRecent];
    std::atomic<uint8_t> next;
};

struct SinkSpec {
    uint8_t kind;
    uint8_t group;
    uint8_t nslots;
    uint8_t slots[8];
    int32_t field_offset;
    bool split_by_caller;
    bool split_by_object;
    char source[112];
    Recent recent;
    std::atomic<uint32_t> calls;
    std::atomic<uint32_t> fuse_start;
    std::atomic<uint32_t> muted_until;
    uint8_t strikes;
};

namespace {

// No runtime constructor: a hooked function can fire from another thread before this
// file's dynamic initialisation has run, so these must be ready at load time
SinkSpec g_specs[kMaxSinks];
std::atomic<int> g_spec_count{0};
std::atomic<uint32_t> g_total_calls{0};
std::atomic<uint32_t> g_window_start{0};
std::atomic<uintptr_t>  g_string_word{0};
bool g_ms_abi  = false;
void (*g_trigger)() = nullptr;
bool (*g_caller_namer)(uintptr_t, char*, size_t) = nullptr;

struct Out {
    const char* source;
    uint8_t group;
    Recent* recent;
};

constexpr int kCallerSlots  = 16384;
constexpr int kCallerProbes = 32;
constexpr size_t kCallerSource = 256;

struct CallerSlot {
    std::atomic<uint64_t> key;
    std::atomic<uint8_t> ready;
    Recent recent;
    char source[kCallerSource];
};
CallerSlot g_callers[kCallerSlots];

inline uint64_t caller_key(uint32_t spec, uintptr_t ret, uintptr_t object)
{
    uint64_t k = static_cast<uint64_t>(ret) * 0x9E3779B97F4A7C15ULL ^ (static_cast<uint64_t>(spec) + 1) * 0xC2B2AE3D27D4EB4FULL ^ 
                 static_cast<uint64_t>(object) * 0xD6E8FEB86659FD93ULL;
    k ^= k >> 29;
    return k ? k : 1;
}

constexpr int kChunkCache = 256;
std::atomic<uintptr_t> g_chunk_cache[kChunkCache]; // (address >> 16) + 1, 0 = empty

inline unsigned chunk_slot(uintptr_t chunk)
{
    return static_cast<unsigned>((chunk ^ (chunk >> 8)) & (kChunkCache - 1));
}

inline bool read_object(uintptr_t p, void* dst, size_t n)
{
    const uintptr_t chunk = (p >> 16) + 1;
    const bool one_chunk  = ((p + n - 1) >> 16) == (p >> 16);

    if (one_chunk && g_chunk_cache[chunk_slot(chunk)].load(std::memory_order_relaxed) == chunk) {
        std::memcpy(dst, reinterpret_cast<const void*>(p), n);
        return true;
    }
    return safe_read(reinterpret_cast<const void*>(p), dst, n);
}

inline void remember_heap_chunk(uintptr_t p)
{
    const uintptr_t chunk = (p >> 16) + 1;
    g_chunk_cache[chunk_slot(chunk)].store(chunk, std::memory_order_relaxed);
}

inline uintptr_t get_arg(const HookRegs* r, unsigned i)
{
#if defined(__x86_64__) || defined(_M_X64)
    return hook_arg(r, i, g_ms_abi);
#else
    return hook_stack_args(r)[i];
#endif
}

uint32_t tick_ms()
{
#if defined(_WIN32)
    return static_cast<uint32_t>(GetTickCount());
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return static_cast<uint32_t>(ts.tv_sec) * 1000u + static_cast<uint32_t>(ts.tv_nsec / 1000000);
#endif
}

constexpr uint32_t kBudgetWindowMs = 1000;
constexpr uint32_t kBudgetCheckEach = 1024;
constexpr uint32_t kFuseCalls = 256;
constexpr uint8_t kMaxStrikes = 3;

inline uint32_t nonzero(uint32_t v)
{
    return v ? v : 1;
}

void mute(SinkSpec& spec, uint32_t now)
{
    if (++spec.strikes >= kMaxStrikes) {
        spec.kind = static_cast<uint8_t>(SinkKind::None);
        return;
    }
    const uint32_t pause = spec.strikes == 1 ? kMuteFirstMs : kMuteSecondMs;
    spec.muted_until.store(nonzero(now + pause), std::memory_order_relaxed);
}

void check_fuse(SinkSpec& spec)
{
    const uint32_t now = tick_ms();
    const uint32_t start = spec.fuse_start.exchange(nonzero(now), std::memory_order_relaxed);
    if (start == 0)
        return;

    const uint64_t elapsed = now - start;
    if (static_cast<uint64_t>(kFuseCalls) * 1000 > static_cast<uint64_t>(kMaxHookCallsPerSec) * elapsed)
        mute(spec, now);
}

void enforce_budget()
{
    const uint32_t now = tick_ms();
    uint32_t start = g_window_start.load(std::memory_order_relaxed);
    if (start == 0) {
        g_window_start.compare_exchange_strong(start, nonzero(now));
        return;
    }

    const uint32_t elapsed = now - start;
    if (elapsed < kBudgetWindowMs || !g_window_start.compare_exchange_strong(start, nonzero(now)))
        return;

    static uint32_t calls[kMaxSinks];
    static uint32_t seen[kMaxSinks];
    const int n = g_spec_count.load(std::memory_order_acquire);
    uint64_t total = 0;
    for (int id = 0; id < n; ++id) {
        SinkSpec& spec = g_specs[id];
        const uint32_t until = spec.muted_until.load(std::memory_order_relaxed);
        if (until != 0 && static_cast<int32_t>(now - until) >= 0) {
            spec.fuse_start.store(0, std::memory_order_relaxed);
            spec.muted_until.store(0, std::memory_order_relaxed);
        }

        const uint32_t made = spec.calls.load(std::memory_order_relaxed);
        calls[id] = made - seen[id];
        seen[id] = made;
        total += calls[id];
    }

    while (total * 1000 > static_cast<uint64_t>(kMaxCallsPerSec) * elapsed) {
        int hot = -1;
        for (int id = 0; id < n; ++id) {
            if (calls[id] != 0 && (hot < 0 || calls[id] > calls[hot]))
                hot = id;
        }
        if (hot < 0)
            break;

        mute(g_specs[hot], now);
        total -= calls[hot];
        calls[hot] = 0;
    }
}

bool is_repeat(const Out& s, const void* data, size_t bytes)
{
    uint64_t h = fnv1a64(data, bytes);
    if (h == 0) h = 1; // 0 marks an empty entry
    const uint32_t now = tick_ms();

    Recent& r = *s.recent;
    for (int i = 0; i < kRecent; ++i) {
        if (r.hash[i].load(std::memory_order_relaxed) == h &&
            static_cast<uint32_t>(now - r.at[i].load(std::memory_order_relaxed)) < kRepeatWindowMs) {
            r.at[i].store(now, std::memory_order_relaxed);
            return true;
        }
    }
    const unsigned slot = r.next.fetch_add(1, std::memory_order_relaxed) % kRecent;
    r.hash[slot].store(h, std::memory_order_relaxed);
    r.at[slot].store(now, std::memory_order_relaxed);
    return false;
}

template<typename C>
bool is_junk(const Out& s, const C* p, size_t n)
{
    if (s.group != GROUP_STRING)
        return false;
    if (n < 3)
        return true;
    for (size_t i = 0; i < n; ++i) {
        if (static_cast<std::make_unsigned_t<C>>(p[i]) > 0x40)
            return false;
    }
    return true;
}

void emit_text(const Out& s, const std::string& text)
{
    if (is_junk(s, text.data(), text.size()))
        return;

    if (!text.empty() && !is_repeat(s, text.data(), text.size()))
        plugin_send_text(s.source, s.group, text);
}

void emit_utf16(const Out& s, const char16_t* chars, int32_t len)
{
    if (len > 0 && is_junk(s, chars, static_cast<size_t>(len)))
        return;

    if (len > 0 && !is_repeat(s, chars, static_cast<size_t>(len) * 2))
        plugin_send_text(s.source, s.group, utf16_to_utf8(chars, len));
}

// A managed string object: { vtable/klass, sync/monitor, int32 length, char16_t chars[] }
void emit_managed(const Out& s, uintptr_t obj)
{
    const uintptr_t word = g_string_word.load(std::memory_order_relaxed);
    if (!word || !obj || (obj & (sizeof(void*) - 1)))
        return;

    uint8_t hdr[2 * sizeof(void*) + 4];
    if (!read_object(obj, hdr, sizeof(hdr)))
        return;

    uintptr_t first;
    std::memcpy(&first, hdr, sizeof(first));
    if (first != word)
        return;
    remember_heap_chunk(obj);

    int32_t len;
    std::memcpy(&len, hdr + 2 * sizeof(void*), sizeof(len));
    if (len <= 0 || len > kMaxTextChars)
        return;

    char16_t stack_buf[512];
    std::u16string heap_buf;
    char16_t* buf = stack_buf;
    
    if (len > 512) {
        heap_buf.resize(static_cast<size_t>(len));
        buf = &heap_buf[0];
    }

    if (!read_object(obj + sizeof(hdr), buf, static_cast<size_t>(len) * 2))
        return;

    emit_utf16(s, buf, len);
}

// Bytes left in the page that contains p
size_t bytes_to_page_end(const void* p)
{
    return 0x1000 - (reinterpret_cast<size_t>(p) & 0xFFF);
}

template<typename CharT>
bool read_z(uintptr_t p, size_t max_chars, std::basic_string<CharT>& out)
{
    while (out.size() < max_chars) {
        CharT chunk[128];
        size_t bytes = bytes_to_page_end(reinterpret_cast<const void*>(p));
        if (bytes > sizeof(chunk)) bytes = sizeof(chunk);
        bytes -= bytes % sizeof(CharT);
        if (bytes == 0) return false;
        if (!safe_read(reinterpret_cast<const void*>(p), chunk, bytes)) return !out.empty();

        const size_t n = bytes / sizeof(CharT);
        for (size_t i = 0; i < n; ++i) {
            if (chunk[i] == 0) return true;
            out.push_back(chunk[i]);
            if (out.size() >= max_chars) return true;
        }
        p += bytes;
    }
    return true;
}

void emit_utf8_z(const Out& s, uintptr_t p)
{
    if (!p) return;
    std::string text;
    if (read_z<char>(p, kMaxTextChars, text) && is_valid_utf8(text.data(), text.size()))
        emit_text(s, text);
}

void emit_utf8_len(const Out& s, uintptr_t p, uintptr_t len)
{
    if (!p || len == 0 || len > static_cast<uintptr_t>(kMaxTextChars)) return;
    std::string text(len, '\0');
    if (safe_read(reinterpret_cast<const void*>(p), &text[0], len) && is_valid_utf8(text.data(), text.size()))
        emit_text(s, text);
}

void emit_utf16_z(const Out& s, uintptr_t p)
{
    if (!p || (p & 1)) return;
    std::u16string text;
    if (read_z<char16_t>(p, kMaxTextChars, text))
        emit_utf16(s, text.data(), static_cast<int32_t>(text.size()));
}

void emit_utf16_len(const Out& s, uintptr_t p, uintptr_t len)
{
    if (!p || (p & 1) || len == 0 || len > static_cast<uintptr_t>(kMaxTextChars)) return;
    std::u16string text(len, u'\0');
    if (safe_read(reinterpret_cast<const void*>(p), &text[0], len * 2))
        emit_utf16(s, text.data(), static_cast<int32_t>(len));
}

// "<source> <- <caller>", at most kMaxSourceLen characters (the program cuts longer names,
// which would make different callers one source). When it does not fit the caller is
// shortened and a few characters of its hash keep it apart from the others
void compose_caller_source(const char* source, const char* who, uintptr_t object, char* out, size_t cap)
{
    char object_tag[16] = "";
    if (object)
        std::snprintf(object_tag, sizeof(object_tag), " #%04x", static_cast<unsigned>(fnv1a64(&object, sizeof(object)) & 0xFFFF));

    std::string s = source;
    if (who[0])
        s += std::string(" <- ") + who;
    if (s.size() + std::strlen(object_tag) > kMaxSourceLen) {
        char tag[8];
        std::snprintf(tag, sizeof(tag), "~%03x", static_cast<unsigned>(fnv1a64(who, std::strlen(who)) & 0xFFF));
        s.resize(kMaxSourceLen - std::strlen(object_tag) - std::strlen(tag));
        s += tag;
    }
    s += object_tag;
    std::snprintf(out, cap, "%s", s.c_str());
}

// The slot of a (hook, caller) pair, made on first use. nullptr when the table is full
// or another thread is still naming the slot: the text is then reported under the plain source
CallerSlot* caller_slot(uint32_t spec, uintptr_t ret, uintptr_t object, const SinkSpec& s)
{
    const uint64_t key = caller_key(spec, ret, object);
    for (int p = 0; p < kCallerProbes; ++p) {
        CallerSlot& slot = g_callers[(key + static_cast<uint64_t>(p)) & (kCallerSlots - 1)];
        uint64_t cur = slot.key.load(std::memory_order_acquire);
        if (cur == 0) {
            uint64_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                char who[128] = "";
                if (ret && (!g_caller_namer || !g_caller_namer(ret, who, sizeof(who))))
                    std::snprintf(who, sizeof(who), "@%llx", static_cast<unsigned long long>(ret));
                compose_caller_source(s.source, who, object, slot.source, sizeof(slot.source));
                slot.ready.store(1, std::memory_order_release);
                return &slot;
            }
            cur = expected;
        }
        if (cur == key)
            return slot.ready.load(std::memory_order_acquire) ? &slot : nullptr;
    }
    return nullptr;
}

} // namespace

int sink_add(SinkKind kind, uint8_t group, const char* source,
             const uint8_t* slots, int nslots, int32_t field_offset, bool split_by_caller, bool split_by_object)
{
    const int id = g_spec_count.load();
    if (id >= kMaxSinks || nslots < 0 || nslots > 8)
        return -1;

    SinkSpec& s = g_specs[id];
    s.group = group;
    s.nslots = static_cast<uint8_t>(nslots);
    s.field_offset = field_offset;
    s.split_by_caller = split_by_caller;
    s.split_by_object = split_by_object;
    std::memset(s.slots, 0, sizeof(s.slots));
    if (slots) std::memcpy(s.slots, slots, static_cast<size_t>(nslots));
    std::strncpy(s.source, source, sizeof(s.source) - 1);
    s.source[sizeof(s.source) - 1] = 0;
    s.kind = static_cast<uint8_t>(kind);

    g_spec_count.store(id + 1, std::memory_order_release);
    return id;
}

void sink_disable(int id)
{
    if (id >= 0 && id < g_spec_count.load())
        g_specs[id].kind = static_cast<uint8_t>(SinkKind::None);
}

void sink_set_string_word(uintptr_t word) { g_string_word.store(word); }
void sink_set_game_abi(bool ms_abi) { g_ms_abi = ms_abi; }
void sink_set_trigger(void (*fn)()) { g_trigger = fn; }
void sink_set_caller_namer(bool (*fn)(uintptr_t, char*, size_t)) { g_caller_namer = fn; }

void sink_handler(const HookRegs* regs, uint32_t id)
{
    if (id >= kBootstrapId) {
        if (g_trigger) g_trigger();
        return;
    }

    if (id >= static_cast<uint32_t>(g_spec_count.load(std::memory_order_acquire)))
        return;

    SinkSpec& spec = g_specs[id];
    if (static_cast<SinkKind>(spec.kind) == SinkKind::None)
        return;

    if (g_total_calls.fetch_add(1, std::memory_order_relaxed) % kBudgetCheckEach == 0)
        enforce_budget();
    if (spec.muted_until.load(std::memory_order_relaxed) != 0)
        return;
    if ((spec.calls.fetch_add(1, std::memory_order_relaxed) + 1) % kFuseCalls == 0) {
        check_fuse(spec);
        if (spec.muted_until.load(std::memory_order_relaxed) != 0)
            return;
    }

    Out s{ spec.source, spec.group, &spec.recent };
    if (spec.split_by_caller || spec.split_by_object) {
        const uintptr_t ret = spec.split_by_caller ? static_cast<uintptr_t>(regs->ret) : 0;
        const uintptr_t object = spec.split_by_object ? get_arg(regs, 0) : 0;
        if (CallerSlot* c = caller_slot(id, ret, object, spec)) {
            s = Out{ c->source, spec.group, &c->recent };
        }
    }

    switch (static_cast<SinkKind>(spec.kind)) {
    case SinkKind::ManagedArgs:
        for (unsigned i = 0; i < spec.nslots; ++i) {
            emit_managed(s, get_arg(regs, spec.slots[i]));
        }
        break;

    case SinkKind::ManagedField: {
        const uintptr_t self = get_arg(regs, 0);
        uintptr_t str = 0;
        if (self && !(self & (sizeof(void*) - 1)) &&
            read_object(self + static_cast<uintptr_t>(spec.field_offset), &str, sizeof(str)))
            emit_managed(s, str);
        break;
    }
    case SinkKind::Utf8Z:    emit_utf8_z(s, get_arg(regs, spec.slots[0])); break;
    case SinkKind::Utf8Len:  emit_utf8_len(s, get_arg(regs, spec.slots[0]), get_arg(regs, spec.slots[1])); break;
    case SinkKind::Utf16Z:   emit_utf16_z(s, get_arg(regs, spec.slots[0])); break;
    case SinkKind::Utf16Len: emit_utf16_len(s, get_arg(regs, spec.slots[0]), get_arg(regs, spec.slots[1])); break;
    case SinkKind::None:     break;
    }
}
