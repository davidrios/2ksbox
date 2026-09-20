2ksbox guest tools - built from qemu-3dfx commit @REV@
(must match the host QEMU build's sign_commit stamp)

RUN SETUP.EXE. It knows which of these files your Windows wants, copies
them, registers what has to be registered, and writes SETUP.LOG. From a
command prompt or the Run box:  D:\SETUP.EXE
  SETUP /ALL             install everything this Windows can use
  SETUP /LIST            print the lists below and exit
  SETUP /GAME <n> <dir>  copy one file set next to a game's EXE

Every program on this disc writes its log - and any BMP it dumps - to
C:\2KSBOX, whichever folder you started it from: this disc is read-only,
and the folder the Run box happened to be in is nobody's guess. Set
BOXLOG to put them somewhere else:  set BOXLOG=E:\

What is on the disc, if you would rather do it by hand:

GLIDE\    the device mapper and the 3dfx Glide wrappers.
          Windows 98/Me: GLIDE.DLL GLIDE2X.DLL GLIDE3X.DLL FXMEMMAP.VXD
                         -> C:\WINDOWS\SYSTEM, GLIDE2X.OVL -> C:\WINDOWS
          DOS games:     GLIDE2X.OVL next to the game's EXE (or on the
                         PATH). A DOS/4GW Glide game loads it by name;
                         it needs no driver, from DOS or a 98 DOS box.
          2000/XP:       GLIDE*.DLL -> system32, FXPTL.SYS ->
                         system32\drivers, then run INSTDRV.EXE as
                         Administrator.
          The mapper is not optional for OPENGL32.DLL or for D3DPT\:
          they reach the device through it, and without it they refuse
          to load (0xc0000142 / "failed to initialize" on 2000/XP).
          A machine with a 3dfx card (the emulated Voodoo 2) gets its
          Glide from 3dfx's own driver, under these same names: SETUP
          then leaves GLIDE*.DLL, an FXMEMMAP.VXD already there and
          GLIDE2X.OVL alone, and SETUP /GAME 6 (GLIDE*.DLL) or /GAME 7
          (GLIDE2X.OVL) puts these next to one game that should use the
          pass-through instead of the card.

DRIVER\   the 2000/XP display driver for the paravirtual adapter. Boot
          the machine with -vga none -device d3dpt-vga, then run
          DRVINST.EXE -reboot. DRIVER\README.TXT lists its own test
          programs (DDTEST, D3D7TEST, SHTEST, ...). Windows 9x wants
          DRIVER9X\ instead; SETUP picks for you.

DRIVER9X\ the Windows 98/Me display driver for the same adapter, and the
          same rule about the machine: -vga none -device d3dpt-vga. There
          is nothing to run - copy D3DPT9X.INF, D3DPT9X.DRV and
          D3DPT9V.VXD into C:\WINDOWS\INF (SETUP does) and restart; Plug
          and Play matches the adapter and installs the driver with no
          clicks, then asks to restart once more. Both halves matter: the
          .DRV draws, and the .VXD is what claims the adapter's PCI
          resources, without which Windows takes them away again.

D3DPT\    Direct3D 8/9 through the paravirtual device, per game: copy
          D3D8.DLL / D3D9.DLL next to the game's EXE. DDRAW.DLL as well
          when a launcher checks video memory through DirectDraw (GTA
          Vice City: "cannot find enough available video memory") - it
          answers 256 MB and forwards the rest to Windows' own.
          DINPUT.DLL fixes "the keyboard does nothing in the game" when
          the game polls a non-exclusive DirectInput keyboard from a loop
          that never pumps messages (FIFA 2000's match): what Windows
          reports pressed is merged into the state. Log: d3dpt.log next
          to the EXE. Never mix these with WINED3D\ in one folder.

OPENGL\   OPENGL32.DLL, the OpenGL pass-through wrapper: next to an
          OpenGL game's EXE (Quake 2 and friends). TESTS\WGLGEARS.EXE in
          the same folder is the two-second check that it works.
          WRAPGL32.EXT goes with it, and is the one file here you edit:
          it caps the OpenGL extension list the game is shown. A modern
          host reports several thousand characters of extension names,
          and a game of the 1990s reads that into a fixed buffer -
          GLQuake's is 4096 bytes and it crashes on the real one. The
          shipped cap is what such a game can hold; raise the year for a
          later game, or delete the file to pass everything through.
          SETUP /GAME 3 copies both, and never overwrites a
          WRAPGL32.EXT you have already changed.

WINED3D\  Direct3D -> OpenGL in the guest (wine9x @WINE9X@): the fallback
          for what the two stacks above do not cover, and the only
          Direct3D a host without Vulkan 1.3 has (a Mac before macOS 26).
          Per game, copy every file in ONE of these folders next to the
          game's EXE - from Explorer is fine, nothing needs renaming, and
          the same files work on Windows 98 and XP:
            D3D8-9\  DirectX 8 and 9 games
            DDRAW\   DirectDraw and Direct3D 7 and older
          Both hold OPENGL32.DLL on purpose: WineD3D draws through it, and
          without it Windows' own software OpenGL is used instead.
          SETUP /GAME 4 and /GAME 5 copy the same two folders.
          WINED3D\README.TXT says the same at more length.

TESTS\    every test, benchmark and calibration program on the disc, one
          copy each; SETUP puts them in C:\2KSBOX. A test that has to run
          on a particular stack needs that stack's DLLs beside it - copy
          the EXE into a folder of its own and use SETUP's /GAME there.
            D3DGAME9 D3DGAME8   the reference scene (doc 14). -frames N
                                runs a fixed sequence, -dump N x.bmp
                                writes a frame. Run these on real
                                hardware first: those BMPs are what the
                                emulated paths are compared against.
            D3DFEAT9            shaders, queries, cube maps, state blocks
            D3D9TEST            adapter, caps, x87 control word, a
                                spinning triangle with fps
            DDVMTEST            what a video-memory check sees
            MODETEST            the display modes the driver offers -
                                run it when a fullscreen game dies at
                                startup
            WGLGEARS            OpenGL, next to OPENGL32.DLL
            WAITFILE            wait for a file to appear, then start
                                something - a login wait that leaves the
                                guest idle, unlike a batch file's CHOICE
            SSEBENCH            SSE and x87 throughput in ns per op
            CDTEST              CD audio through MCI
            CRTCAL              the CRT calibration patterns, exclusive
                                full-screen at the exact mode
            TEXTCAL.COM         the 720x400 text-mode patterns (DOS only)

CDSHELF\  the host's disc shelf, from inside the machine. CDSHELF.EXE on
          98/2000/XP, CDSHELF.COM in a DOS box; nothing to install (SETUP
          just puts the EXE where the Run box can find it). With no
          arguments the EXE opens a window and the COM lists the shelf
          and waits for a key. Either also takes a command:
            CDSHELF LIST   print the shelf and exit
            CDSHELF 3      put slot 3 in the drive
            CDSHELF E      empty the drive
          An insert always empties the drive first and waits for it -
          without that, Windows and MSCDEX keep showing the old disc.

VOODOO2\  V2START.EXE, for Windows 98/Me with the emulated Voodoo 2 and
          3dfx's own driver. That driver initialises the card from a
          start-up program for a few seconds after every login, and a
          Glide game started meanwhile hangs. SETUP moves the driver's
          "Voodoo2" entry out of HKLM\...\CurrentVersion\Run into
          HKLM\SOFTWARE\2ksbox\Voodoo2 and puts V2START.EXE (in
          C:\WINDOWS) in its place: it runs the same command and shows
          "Voodoo 2 driver is loading, please wait before running 3dfx
          games" until it has finished. What happened at the last login
          is in C:\WINDOWS\V2START.LOG.

Not included: GLIDE2X.OVL (DOS Glide games; needs Open Watcom to build).
