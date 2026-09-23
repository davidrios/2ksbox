WineD3D: Direct3D through OpenGL, per game (wine9x @WINE9X@)

This is a fallback. Try the normal way first. The 2ksbox display driver
(DRIVER\ or DRIVER9X\, installed by SETUP.EXE) gives this machine its own
Direct3D, and a game that works on it needs nothing from this folder.

Use these files when:

  - the game does not start, or draws wrongly, on the display driver;
  - or the host has no Direct3D executor (no Vulkan 1.3 graphics card and
    no Wine), so the machine offers no Direct3D of its own.

WHAT TO COPY

Copy every file from ONE of these folders next to the game's EXE. Pick
the folder by the DirectX version the game uses:

  D3D8-9\   DirectX 8 and 9 games
  DDRAW\    DirectDraw, and DirectX 7 and older

Explorer is fine. Nothing needs renaming: the files already have the
names a game loads, and the same ones work on Windows 98 and XP. To have
SETUP do it:

  SETUP /GAME 4 C:\GAMES\<game>     the D3D8-9 files
  SETUP /GAME 5 C:\GAMES\<game>     the DDRAW files

Only the game's own folder changes, so every other game runs as before.
To undo it, delete the copied files from that folder:

  D3D8.DLL  D3D9.DLL  DDRAW.DLL  WINED3D.DLL  OPENGL32.DLL

Never put the files from D3DPT\ in the same folder as these. They are two
different Direct3D stacks under the same names.

TWO THINGS THAT MUST BE TRUE

OPENGL32.DLL is part of the set, not an extra. WineD3D draws through the
first opengl32.dll the loader finds. Without this one, that is Windows'
own software OpenGL, far too slow to play on.

That OPENGL32.DLL needs the device mapper, SETUP's "Glide and the device
mapper" component (SETUP /ALL installs it). Without it the DLL refuses to
load and the game dies at startup.

ON WINDOWS 98 AND ME, START THE GAME FIRST, OR LET SETUP DO IT

These files reach a game only if it is the first program of the session
to use DirectDraw. Windows 9x keeps one copy of a DLL per name for the
whole machine, and DDHELP.EXE keeps DirectDraw loaded once anything has
touched it. The second game you run gets Windows' own DDRAW.DLL and none
of this folder, and its 3D setup offers no 3D device at all. Restart the
machine and start the game before anything else that draws.

If that is a nuisance, SETUP's "WineD3D as this machine's DirectDraw"
component handles it for the whole machine, and only when needed. At
every login it asks the display driver whether this host has Direct3D of
its own. If not, it points DirectDraw at WineD3D; if so, back at
Windows' own. No folder has to be copied then. It writes
WINDOWS\D3DPRE.LOG saying which way it went.

Windows XP and 2000 do not work this way. There the folder is used
whatever ran before, and the component is not offered.
