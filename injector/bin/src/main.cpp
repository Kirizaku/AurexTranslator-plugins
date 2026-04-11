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

#include <iostream>
#include <string>
#include <optional>
#include <string_view>
#include <charconv>
#include <system_error>

#include "memory/memory.h"
#include "license.h"

enum class Command { Load, Unload, Version, License };

struct Options {
    Command command;
    std::string pid;
    std::optional<std::string> library_path;
    std::optional<std::string> handle_str;
};

#if defined(_WIN32)
constexpr std::string_view UNLOAD_OPTION = "--module-base";
#else
constexpr std::string_view UNLOAD_OPTION = "--handle";
#endif

static void print_help(std::string_view prog_name) {

    std::cerr << "Usage:\n"
              << "  " << prog_name << " load   --pid <name> --library <path>\n"
              << "  " << prog_name << " unload " << UNLOAD_OPTION << " <hex|dec> --pid <name>\n\n"
              << "Options:\n"
              << "  --pid <name>      Target process name (required)\n"
              << "  --library <path>  Path to .dll/.so (load only)\n"
              << "  " << UNLOAD_OPTION << " <value>  Module identifier in decimal or 0xHEX (unload only)\n"
              << "  --version         Show program version\n"
              << "  --license         Show license information\n"
              << "  --help, -h        Show this help\n";
}

static void print_version() {
    std::cout << "at-injector version 1.0.0" << "\n";
}

static void print_license() {
    std::cout << MIT_LICENSE << "\n";
}

static void print_unknown_command(std::string_view prog_name, std::string_view cmd) {
    std::cerr << "Error: unknown command '" << cmd << "'\n"
              << "See '" << prog_name << " --help' for a list of options\n";
}

static void print_unknown_option(std::string_view prog_name, std::string_view opt) {
    std::cerr << "Error: unknown option '" << opt << "'\n"
              << "See '" << prog_name << " --help' for a list of options\n";
}

class ArgParser {
    int argc_;
    char** argv_;

public:
    ArgParser(int argc, char** argv) : argc_(argc), argv_(argv) {}

    std::optional<Options> parse() {
        if (argc_ < 2) {
            std::cerr << "Error: no command specified\n"
                      << "See '" << argv_[0] << " --help' for a list of options\n";
            return std::nullopt;
        }

        Options opt;
        std::string_view cmd = argv_[1];

        if (cmd == "--help" || cmd == "-h") {
            print_help(argv_[0]);
            std::exit(0);
        }

        if (cmd == "--version") {
            opt.command = Command::Version;
            return opt;
        }

        if (cmd == "--license") {
            opt.command = Command::License;
            return opt;
        }

        if (cmd == "load") {
            opt.command = Command::Load;
        } else if (cmd == "unload") {
            opt.command = Command::Unload;
        } else {
            print_unknown_command(argv_[0], cmd);
            return std::nullopt;
        }

        for (int i = 2; i < argc_; ++i) {
            std::string_view arg = argv_[i];

            if (arg == "--help" || arg == "-h") {
                print_help(argv_[0]);
                std::exit(0);
            }
            else if (arg == "--pid") {
                if (!next_arg(i, opt.pid)) return std::nullopt;
            }
            else if (arg == "--library") {
                if (opt.command != Command::Load) {
                    std::cerr << "Warning: --library option is only valid for load command, ignoring\n";
                    if (!consume_next_arg(i)) return std::nullopt;
                } else {
                    if (!next_arg(i, opt.library_path.emplace())) return std::nullopt;
                }
            }
            else if (arg == UNLOAD_OPTION) {
                if (opt.command != Command::Unload) {
                    std::cerr << "Warning: " << UNLOAD_OPTION << " option is only valid for unload command, ignoring\n";
                    if (!consume_next_arg(i)) return std::nullopt;
                } else {
                    if (!next_arg(i, opt.handle_str.emplace())) return std::nullopt;
                }
            }
            else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
                print_unknown_option(argv_[0], arg);
                return std::nullopt;
            }
            else if (arg[0] == '-') {
                std::cerr << "Error: unknown short option '" << arg << "'\n"
                          << "See '" << argv_[0] << " --help' for a list of options\n";
                return std::nullopt;
            }
            else {
                std::cerr << "Error: unexpected argument '" << arg << "'\n"
                          << "See '" << argv_[0] << " --help' for a list of options\n";
                return std::nullopt;
            }
        }

        if (opt.pid.empty()) {
            std::cerr << "Error: --pid option is required\n"
                      << "See '" << argv_[0] << " --help' for a list of options\n";
            return std::nullopt;
        }

        if (opt.command == Command::Load && !opt.library_path) {
            std::cerr << "Error: --library option is required for load command\n"
                      << "See '" << argv_[0] << " --help' for a list of options\n";
            return std::nullopt;
        }

        if (opt.command == Command::Unload && !opt.handle_str) {
            std::cerr << "Error: " << UNLOAD_OPTION << " option is required for unload command\n"
                      << "See '" << argv_[0] << " --help' for a list of options\n";
            return std::nullopt;
        }

        return opt;
    }

private:
    bool next_arg(int& i, std::string& out) {
        if (i + 1 >= argc_) {
            std::cerr << "Error: " << argv_[i] << " option requires a value\n"
                      << "See '" << argv_[0] << " --help' for a list of options\n";
            return false;
        }
        out = argv_[++i];
        return true;
    }

    bool consume_next_arg(int& i) {
        if (i + 1 >= argc_) {
            std::cerr << "Error: " << argv_[i] << " option requires a value\n"
                      << "See '" << argv_[0] << " --help' for a list of options\n";
            return false;
        }
        ++i;
        return true;
    }
};

[[nodiscard]] bool parse_handle(std::string_view s, mem::module_handle_t& out) {
    if (s.empty()) return false;

    int base = 10;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
        if (s.empty()) return false;
    }

    uintptr_t value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);

    if (ec == std::errc{} && ptr == s.data() + s.size()) {
#if defined(_WIN32)
        out.base = static_cast<uintptr_t>(value);
#else
        out.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(value));
#endif
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_pid(const std::string& pid_str, mem::mem_pid_t& out_pid) {
    try {
        size_t pos;
        int pid = std::stoi(pid_str, &pos);
        if (pos != pid_str.length()) {
            std::cerr << "Error: invalid PID format\n";
            return false;
        }
        out_pid = static_cast<mem::mem_pid_t>(pid);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error: invalid PID value: " << e.what() << "\n";
        return false;
    }
}

[[nodiscard]] bool execute_load(mem::mem_pid_t pid, std::string_view lib_path) {
    auto result = mem::load_module(pid, std::string(lib_path));

    if (!result.base) {
        std::cerr << "Error: failed to load module '" << lib_path << "'\n";
        return false;
    }

#if defined(_WIN32)
    constexpr const char* name   = "module base";
    auto value = result.base;
#else
    constexpr const char* name   = "handle";
    auto value = result.handle;
#endif

    std::cout << "Module loaded successfully, " << name << " = " << (void*)value << '\n';
    return true;
}

[[nodiscard]] bool execute_unload(mem::mem_pid_t pid, mem::module_handle_t module) {
    bool result = mem::unload_module(pid, module);

    if (!result) {
#if defined(_WIN32)
        constexpr const char* name = "base";
        auto value = module.base;
#else
        constexpr const char* name = "handle";
        auto value = module.handle;
#endif
        std::cerr << "Error: failed to unload module (" << name << ": 0x" << std::hex << value << std::dec << ")\n";
        return false;
    }

    std::cout << "Module unloaded successfully\n";
    return true;
}

int main(int argc, char** argv) {
    auto options = ArgParser(argc, argv).parse();
    if (!options) {
        return EXIT_FAILURE;
    }

    switch (options->command) {
    case Command::Version:
        print_version();
        return EXIT_SUCCESS;
    case Command::License:
        print_license();
        return EXIT_SUCCESS;
    default:
        break;
    }

    mem::mem_pid_t pid;
    if (!parse_pid(options->pid, pid)) {
        return EXIT_FAILURE;
    }

    bool success = false;

    if (options->command == Command::Load) {
        uintptr_t existing_module = mem::get_module(pid, *options->library_path);
        if (existing_module != 0) {
            std::cerr << "Error: module is already loaded\n";
            return EXIT_FAILURE;
        }
        success = execute_load(pid, *options->library_path);
    } else if (options->command == Command::Unload) {
        mem::module_handle_t module;
        if (!parse_handle(*options->handle_str, module)) {
            std::cerr << "Error: invalid format. Use decimal or 0xHEX\n";
            return EXIT_FAILURE;
        }
        success = execute_unload(pid, module);
    }

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
