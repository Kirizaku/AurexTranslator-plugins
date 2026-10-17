Aurex Translator Plugins (Linux)

----------------------
Structure:
----------------------
plugins/
  libat-injector.so            - Wrapper library (required for all hook plugins)
  bin/at-injector_x64, _x86    - Injection utility (64/32-bit)

  Hook plugins (capture game text):
  libat-imhhw.so               - If My Heart Had Wings
  libat-renpy_x64.so, _x86.so  - Ren'Py engine games
  libat-kirikiri_x86.so        - KiriKiri / KiriKiriZ engine games
  libat-unity_x64.so, _x86.so  - Unity games (Mono and IL2CPP)

  In-game overlay (shows translated text, captures the image for OCR):
  libat-overlay-vulkan_x64.so, _x86.so    - Vulkan games
  libat-overlay-opengl_x64.so, _x86.so    - OpenGL games
  libat-overlay-shim_x64.so, _x86.so      - Launcher: enables the overlay in games started through it

----------------------
Installation:
----------------------
1. Copy all files to the plugins folder (keep the bin/ subfolder):
   ~/.config/AurexTranslator/plugins/
2. (Optional) Grant permissions to avoid the password prompt.
   Aurex Translator offers this itself in the HOOK settings; manually:
   sudo setcap cap_sys_ptrace+ep ~/.config/AurexTranslator/plugins/bin/at-injector_x64
   sudo setcap cap_sys_ptrace+ep ~/.config/AurexTranslator/plugins/bin/at-injector_x86

----------------------
Usage:
----------------------
Hook:
1. Launch the game
2. In Aurex Translator go to: Text Processing -> HOOK
3. Select the game or engine from the list
Text will be captured and translated automatically.
Each game/engine requires a separate plugin.

Overlay:
1. In Aurex Translator go to: Output -> "In-game overlay" tab -> "Vulkan/OpenGL launcher" -> Install
   (installs the Vulkan layer and the launcher script; the program also offers this itself)
2. Output -> check "In-game text overlay"
   (optional: to take the game image for OCR from the game itself, also choose the "Overlay" capture mode;
   it can be combined with the text overlay)
3. Start the game THROUGH the launcher script (the program shows the command)
4. Click "Choose game..." and pick the game

For pre-configured KiriKiri game setups, see the separate "kirikiri-configs" asset in this release.

Licenses: see the licenses/ folder (third-party notices included).
