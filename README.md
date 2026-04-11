# Aurex Translator Plugins

A collection of plugins for Aurex Translator — a cross-platform real-time text translator for Windows and Linux (X11/Wayland) that supports both OCR screen capture and plugin-based text hooking.

## 📁 Repository Structure

```
aurextranslator-plugins/
├── 📁 example/   # Basic plugin example and test target
├── 📁 injector/  # libat-injector: wrapper library for managing at-injector
│ └── 📁 bin/     # at-injector: CLI utility for loading/unloading libraries
├── 📁 games/     # Game-specific plugins
│ └── 📁 imhhw/   # If My Heart Had Wings
```


## 🧩 Architecture & How It Works

The system consists of three key components that exchange data via **Shared Memory (SHM)**:

### 1. Game Plugins (`games/*`)
- Dynamic libraries written for specific games
- Intercept text output function calls and pass the original text to SHM

### 2. Injection Infrastructure (`injector/`)
- `libat-injector` — wrapper library that manages `at-injector`: launches it with the required parameters and organizes data exchange via Shared Memory (SHM IPC)
- `at-injector` (in `bin/`) — CLI utility for loading (`load`) or unloading (`unload`) libraries into the address space of a running process. Available for x86 and x64.

### 3. Aurex Translator
- Main application that receives text from plugins, translates it using the selected service, and displays it in a separate window

## 📦 Building All Plugins

### Requirements
- Qt6
- libcap (on Linux)

### Instructions

```bash
git clone https://github.com/kirizaku/aurextranslator-plugins.git
cd aurextranslator-plugins
mkdir build && cd build
cmake .. -DBUILD_EXAMPLE_PLUGIN=ON   # ← enables building the example plugin
cmake --build .
```

> **NOTE:** `BUILD_EXAMPLE_PLUGIN` option controls whether the example plugin and test target are built. It is disabled by default, so you must explicitly enable it.

After a successful build, binaries and dynamic libraries will be placed in the plugins/ directory at the project root. The compiled example can be found in `example/output/`.

## ⚙️ Linux: Setting permissions for injection

On Linux, process injection requires `ptrace` privileges. To prevent the program from asking for admin rights every time, run these commands **once** after building or installing:

```bash
sudo setcap cap_sys_ptrace+ep /path/to/at-injector_x86
sudo setcap cap_sys_ptrace+ep /path/to/at-injector_x64
```

> **NOTE:** If you rebuild the plugins, the permissions will be reset — you need to run the commands again.

## 📥 Installation

Copy the built plugins to the Aurex Translator plugins directory:

| Platform | Path |
|-----------|------|
| Linux | `~/.config/AurexTranslator/plugins/` |
| Windows | `%LOCALAPPDATA%\AurexTranslator\plugins\` |

## 🔧 Development

The repository provides a template (`example/`) for creating new plugins.

### What's included in the example

| File | Purpose |
|------|------------|
| `libat-example.so` | Sample plugin implementing the basic interception interface |
| `test_target` | Test target program that simulates a game's behavior |

### How to run the demo

1. **Launch the target**
   ```bash
   ./example/output/test_target
   ```

2. **Configure Aurex Translator**
   - Open **Text Processing → HOOK**
   - Select the `test_target` process as the injection target

3. **Check the result**
   - The plugin will intercept `fwrite` function calls in `test_target`
   - The captured text will appear in the Aurex Translator window
  
## 📄 Licenses

**Important!** Different components of this repository are distributed under different licenses:

| Component | License | Path |
|-----------|----------|------|
| **Game plugins** (`games/*`) | MIT | `/games/` |
| **Example plugin** (`example/*`) | MIT | `/example/` |
| **libat-injector** | GPL-3.0 | `/injector/libat-injector/` |
| **at-injector** (CLI utility) | MIT | `/injector/bin/` |
