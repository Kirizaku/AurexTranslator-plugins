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

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

constexpr uintptr_t FIRST_ADDR  = 0x004b2ca5;
constexpr uintptr_t SECOND_ADDR = 0x0040792B;
constexpr uintptr_t THIRD_ADDR  = 0x004b2caa;

constexpr size_t FIRST_SIZE  = 5;
constexpr size_t SECOND_SIZE = 5;
constexpr size_t THIRD_SIZE  = 6;

#endif // CONFIG_H
