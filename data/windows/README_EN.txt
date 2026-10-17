Aurex Translator Plugins (Windows)

----------------------
Structure:
----------------------
plugins/
  libat-injector.dll                - Wrapper library (required for all hook plugins)
  bin/at-injector_x64.exe, _x86.exe - Injection utility (64/32-bit)

  Hook plugins (capture game text):
  libat-imhhw.dll                   - If My Heart Had Wings
  libat-renpy_x64.dll, _x86.dll     - Ren'Py engine games
  libat-kirikiri_x86.dll            - KiriKiri / KiriKiriZ engine games
  libat-unity_x64.dll, _x86.dll     - Unity games (Mono and IL2CPP)

  In-game overlay (shows translated text over D3D9-12, OpenGL and Vulkan games,
  captures the image for OCR):
  libat-overlay_x64.dll, _x86.dll

----------------------
Installation:
----------------------
1. Copy all files to the plugins folder (keep the bin\ subfolder):
   %LOCALAPPDATA%\AurexTranslator\plugins\

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
1. Vulkan games only: in Aurex Translator go to Output -> "In-game overlay" tab -> "Vulkan layer" -> Install.
   This must be done BEFORE the game is started (the program also offers it).
2. Output -> check "In-game text overlay"
   (optional: to take the game image for OCR from the game itself, also choose the "Overlay" capture mode;
   it can be combined with the text overlay)
3. Start the game
4. Click "Choose game..." -> "Add game..." and pick the running game

For pre-configured KiriKiri game setups, see the separate "kirikiri-configs" asset in this release.

Licenses: see the licenses\ folder (third-party notices included).
