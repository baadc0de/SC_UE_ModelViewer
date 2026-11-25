SC Viewer - any Star Citizen ship, in any hangar, in any paint
==============================================================

START:  double-click SCViewer.exe (in this folder).

  - Star Citizen must be installed. The viewer finds it in the usual place
    (C:\Program Files\Roberts Space Industries\StarCitizen\LIVE) or asks you
    for the file Data.p4k in your game folder.
  - The first start reads the game's ship and hangar lists: give it a minute
    or two. Later starts are quick.
  - F1 opens the menu: pick a hangar, a ship and a paint. SHOW SHIP keeps
    the paint list beside the ship, to change the paint's colours.

If Windows says "Windows protected your PC": More info, then Run anyway.
If Windows says VCRUNTIME140.dll is missing: run
    Engine\Extras\Redist\en-us\vc_redist.x64.exe
once, then start the viewer again.

Keep this folder somewhere you can write to (not C:\Program Files): the viewer
keeps a cache of what it read in the "cache" folder here (up to about 40 GB;
safe to delete while the viewer is closed).

Needs: Windows 10/11 64-bit, a DirectX 12 graphics card with Shader Model 6.6
(GeForce GTX 1060 / Radeon RX 5000 / Intel Arc or newer), 16 GB RAM.
Without hardware ray tracing the viewer shows "Basic lighting" and still works.

User guide (with pictures):
    https://github.com/baadc0de/UnrealCitizen/blob/main/docs/viewer/USER_GUIDE.md
Problems and ideas:
    https://github.com/baadc0de/UnrealCitizen/issues
    (attach %LOCALAPPDATA%\SCViewer\Saved\Logs\SCViewer.log)

SC Viewer is a fan project, not made or endorsed by Cloud Imperium Games. It
only reads the game's files. Licence: LICENSE.txt (MIT). Star Citizen and its
ships are (c) Cloud Imperium Rights LLC and Cloud Imperium Rights Ltd. Built
with Unreal Engine (c) Epic Games, Inc.
