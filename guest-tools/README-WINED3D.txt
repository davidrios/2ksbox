WineD3D - Direct3D through OpenGL, per game (wine9x @WINE9X@)

This is a fallback. Try the normal way first: the 2ksbox display driver
(DRIVER\ or DRIVER9X\, installed by SETUP.EXE) gives this machine its own
Direct3D, and a game that works on it needs nothing from this folder.

Use these files when that is not enough:

  - the game does not start, or draws wrongly, on the display driver;
  - or the host has no Vulkan 1.3 graphics card, so the machine offers no
    Direct3D of its own at all.

WHAT TO COPY

Copy every file from ONE of these folders next to the game's EXE - the
folder is chosen by the DirectX version the game uses:

  D3D8-9\   DirectX 8 and 9 games
  DDRAW\    DirectDraw, and DirectX 7 and older

From Explorer is fine. Nothing needs renaming: the files already have the
names a game loads, and the same ones work on Windows 98 and XP. If you
would rather have SETUP do it:

  SETUP /GAME 4 C:\GAMES\<game>     the D3D8-9 files
  SETUP /GAME 5 C:\GAMES\<game>     the DDRAW files

Only the game's own folder changes, so every other game keeps running the
way it did. To undo it, delete the copied files from that folder:

  D3D8.DLL  D3D9.DLL  DDRAW.DLL  WINED3D.DLL  OPENGL32.DLL

Never put the files from D3DPT\ in the same folder as these - they are two
different Direct3D stacks under the same names.

TWO THINGS THAT HAVE TO BE TRUE

OPENGL32.DLL is part of the set, not an extra. WineD3D draws through the
first opengl32.dll the loader finds, and without this one that is Windows'
own software OpenGL, which is far too slow to play on.

That OPENGL32.DLL needs the device mapper installed, which is SETUP's
"Glide and the device mapper" component (SETUP /ALL installs it). Without
it the DLL refuses to load and the game dies at startup.

ON WINDOWS 98 AND ME, START THE GAME FIRST

These files reach a game only if it is the first program of the session to
use DirectDraw. Windows 9x keeps one copy of a DLL per name for the whole
machine, and DDHELP.EXE keeps DirectDraw loaded once anything has touched
it, so the second game you run gets Windows' own DDRAW.DLL and none of this
folder - a game's 3D setup then offers no 3D device at all. Restart the
machine and start the game before anything else that draws.

(Windows XP and 2000 do not work this way: there the folder is used
whatever ran before.)
