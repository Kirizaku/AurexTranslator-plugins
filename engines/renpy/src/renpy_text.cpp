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

#include "renpy_text.h"

// https://www.renpy.org/doc/html/text.html
std::string strip_renpy_markup(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    size_t i = 0;
    const size_t n = s.size();

    while (i < n) {
        // Escape Characters: \ (backslash)
        if (s[i] == '\\' && i + 1 < n) {
            switch (s[i+1]) {
            case '"':  out += '"';  i += 2; continue; // \" backslash-doublequote
            case '\'': out += '\''; i += 2; continue; // \' backslash-quote
            case ' ':  out += ' ';  i += 2; continue; // \  backslash-space
            case 'n':  out += ' ';  i += 2; continue; // \n backslash-n
            case '\\': out += '\\'; i += 2; continue; // \\ backslash-backslash
            case '%':  out += '%';  i += 2; continue; // \% backslash-percent
            default:   out += s[i++];       continue; // unknown
            }
        }

        // %% — same as \% (protected percent)
        if (s[i] == '%' && i + 1 < n && s[i+1] == '%') {
            out += '%'; i += 2; continue;
        }

        // Escape Characters: { (left brace)
        // {{ → literal {
        if (s[i] == '{' && i + 1 < n && s[i+1] == '{') {
            out += '{'; i += 2; continue;
        }

        // }} → literal }
        if (s[i] == '}' && i + 1 < n && s[i+1] == '}') {
            out += '}'; i += 2; continue;
        }

        // {tag}, {/tag}, {tag=value} — strip text tag
        if (s[i] == '{') {
            int depth = 1; ++i;
            while (i < n && depth > 0) {
                if      (s[i] == '{') ++depth;
                else if (s[i] == '}') --depth;
                ++i;
            }
            continue;
        }

        // Escape Characters: 【 (left lenticular bracket)
        // 【【 → literal 【
        if ((unsigned char)s[i] == 0xE3 &&
            i + 2 < n &&
            (unsigned char)s[i+1] == 0x80 &&
            (unsigned char)s[i+2] == 0x90)
        {
            if (i + 5 < n &&
                (unsigned char)s[i+3] == 0xE3 &&
                (unsigned char)s[i+4] == 0x80 &&
                (unsigned char)s[i+5] == 0x90)
            {
                out += s[i]; out += s[i+1]; out += s[i+2];
                i += 6; continue;
            }

            // 【ruby|base】
            i += 3;
            std::string ruby_text, base_text;
            bool found_pipe = false;

            while (i < n) {
                bool is_close = (unsigned char)s[i] == 0xE3 &&
                                i + 2 < n &&
                                (unsigned char)s[i+1] == 0x80 &&
                                (unsigned char)s[i+2] == 0x91;
                if (is_close) { i += 3; break; }

                if (s[i] == '|' && !found_pipe) {
                    found_pipe = true; ++i; continue;
                }

                if (!found_pipe) ruby_text += s[i++];
                else             base_text += s[i++];
            }

            out += found_pipe ? base_text : ruby_text;
            continue;
        }

        // Regular character
        out += s[i++];
    }

    return out;
}
