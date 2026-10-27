# The Lost World PC VR — Supermodel with OpenXR

<p align="center"><img src="Docs/Images/title-screen.jpg" width="620" alt="The Lost World: Jurassic Park title screen running in Supermodel" /></p>

Play Sega's 1997 light-gun arcade game **The Lost World: Jurassic Park** in VR. This is a fork of the [Supermodel](https://github.com/trzy/Supermodel) Sega Model 3 emulator with an OpenXR mode. Your PC runs the emulator and streams it to the headset.

## Features

- **Stereo 3D with head tracking.** The game's 3D world is drawn around you, beyond the edges of the original 4:3 screen, and the sky carries on past them: left and right it's the game's own background, which turns with the game's sky; above and below it fades softly.
- **The controller is the gun.** You can aim and shoot anywhere in the headset's view, up to a screen-width past each edge of the original screen. The game has no crosshair during play, so a red ring shows where your gun hand points. The game's own cutscene sight is hidden, so there is only ever one.
- **A gun in your hand.** The heroes' rifle or the pistol from the game itself, read from your own ROM with their textures, or your own 3D model of the arcade gun in blue or red (see below). The gun is always drawn in front of the score and text.
- **Recoil.** The controller vibrates while you fire.
- **Steady 60 fps.** The game runs at its real speed whatever the headset's refresh rate, and the headset never waits for the emulator.
- **Flashlight scenes** are lit correctly: the game's spotlight lies on the scene in both eyes.
- **Cinema mode.** Play on a big screen in a dark room, flat or in 3D. The screen keeps its size when the game's camera zooms, can sit in a TV-cabinet frame whose inner walls softly reflect the picture, and an optional glow lets the picture's colours light up the room. The aiming ring is shown on the screen.
- **Comfort options.** A vignette darkens the edges of your view during fast camera turns.
- **Clean overlay.** The cutscene letterbox bars are removed, in the headset and on the cinema screen. The hit "static" and full-screen fades, like the CONTINUE screen's veil, fill your whole view. Score and text follow your head during play or stay fixed in the room.
- **In-headset options menu** in the colours of the game's InGen screens. Every setting can be changed from the headset and is saved straight away.
- **Fast start.** After the first run, the game opens at the start of its attract demo; the start-up screens are skipped.

Developed and tested on a Meta Quest 3 streamed with **Virtual Desktop** (VDXR runtime). Meta Quest Link and SteamVR are standard OpenXR runtimes and should work too.

> **No game files are included.** You need your own `lostwsga.zip` ROM set (The Lost World, Japan, Revision A), dumped from hardware you own. Please don't open issues or pull requests asking for ROMs.

## Quick start (Windows)

1. Download `LostWorldVR-windows.zip` from the [Releases](../../releases) page, or from the artifacts of the latest successful [Actions](../../actions) run, and unzip it.
2. Put `lostwsga.zip` in the `ROMs` folder.
3. Make your streaming app the active OpenXR runtime:
   - **Virtual Desktop:** in the Streamer app on the PC, set *OpenXR Runtime* to **VDXR**.
   - **Quest Link:** in the Meta Quest Link app, go to Settings › General › OpenXR Runtime.
   - **SteamVR:** in SteamVR's settings, go to OpenXR.
4. Connect the headset, then double-click **`Play The Lost World VR.bat`**. The game waits until the headset is in use, so you can start it from the PC first.

### Quick start (Linux, experimental)

Don't run the Windows build under Proton: Proton's OpenXR layer only supports Vulkan and Direct3D, and this emulator draws with OpenGL, so it stops at start-up with `XR_ERROR_GRAPHICS_DEVICE_INVALID`. Use the native build instead:

1. Download `LostWorldVR-linux-x86_64.tar.gz` from the [Releases](../../releases) page and unpack it.
2. Put `lostwsga.zip` in the `ROMs` folder.
3. Start your OpenXR runtime (SteamVR, Monado, WiVRn or ALVR) and make it the active one.
4. Run `./play-lostworld-vr.sh` (or `./play-lostworld-flat.sh` for a flat window).

It runs under X11 or XWayland. SDL2, SDL2_net, GLU and the Khronos OpenXR loader are included in `lib/` for systems that lack them. So far it has only been tested with Monado's simulated headset, so reports from real setups are very welcome.

### Controls

| Controller | Action |
|---|---|
| Trigger (gun hand) | Fire. The gun is in the hand chosen in the options: right or left. |
| Grip (gun hand) | Reload: the side button of the hand that holds the gun refills all five shells. |
| A or Y | Start |
| B or X | Coin |
| Left menu button | Open / close the options menu (the game pauses) |
| Both sticks held in for a second, or M on the PC keyboard | Open / close the options menu too (for Valve Index and other controllers whose menu button opens SteamVR's own menu) |
| Grip (side button) on the menu | Hold to pick the menu up and move it; let go to drop it |
| Either stick click | Re-centre the view (when you let go) |
| Left grip + left stick click | Operator (test) menu |
| Esc (PC keyboard) | Quit |

In the options menu, point at an option and pull the trigger.

### Options menu

| Tab | Option | Choices |
|---|---|---|
| **Comfort** | Vignette during fast camera turns | Off / Light / Medium / Strong |
| | Cinema mode | Off / On: the game on a big screen in a dark room, no camera motion around you |
| | Cinema screen size | Small / Medium / Large |
| | Cinema 3D depth | Flat / Shallow / Full |
| | Screen bezel | Off / On: the cinema screen sits in a TV-cabinet frame that reflects the picture |
| | Screen glow | Off / Subtle / Strong: the picture lights up the room around the cinema screen |
| **Gun** | Gun hand | Right / Left |
| | Gun model in your hand | Off / Rifle / Pistol (both from the game) / Arcade (`Assets/VRGameGun.obj`) / Custom (`Assets/VRGunModel.obj`) |
| | Gun colour | Blue / Red, for the Arcade model (shown when `VRGameGun.obj` is there) |
| | Recoil vibration | Off / Light / Strong |
| | Aim fine-tune | Nudge the aim left/right and up/down in 0.1° steps |
| **Screen** | Score / timer / text | Fixed in the room / Follow head / Auto (head during play) |
| | 2D graphics scaling | Soft / Sharp / Smooth edges |
| | Hide player 2's PRESS START | On / Off: player 2's INSERT COIN / Push 2P Start, in play and in cutscenes |
| | Hit static over the whole view | On / Off (also full-screen fades) |
| | Render resolution | 0.5× to 2.0× of what the headset (or Virtual Desktop) asks for, shown as pixels per eye (applies on the next start) |

The buttons at the bottom resume the game, re-centre the view or quit. Settings are saved to `Config/Supermodel.ini`. The menu's typeface is [Russo One](https://fonts.google.com/specimen/Russo+One) by Jovanny Lemonad (SIL Open Font License; the included Latin subset is renamed "LW Menu"). You can frame the menu with your own artwork, see `Assets/VR-MENU.txt`.

**Gun models.** *Rifle* (the heroes' gun from the cutscenes) and *Pistol* (the handgun from the how-to-play screen) are read from your own ROM while you play, with their textures; nothing from the game is included here. The textures are kept in `Saves/lostwsga.vrguntex` once the game has shown a gun, so it looks right from the start next time. *Arcade* and *Custom* take a Wavefront OBJ of your own (with an optional `.mtl`): `Assets/VRGameGun.obj` for *Arcade*, drawn in the blue or red chosen in the menu, and `Assets/VRGunModel.obj` for *Custom*, in its own material colours. Units are metres, the hand / grip at the origin, the barrel pointing along −Z and +Y up. The material's `Ks` (0–1) sets the strength of the sheen and `Ns` its sharpness (about 10 for matte plastic, 100 for glossy); `Kd` sets the colour of a Custom model.

For the smoothest play in Virtual Desktop, set Synchronous Spacewarp to **Disabled** and the frame rate to 72, 90 or 120 Hz. All VR settings are in `Config/Supermodel.ini`, documented in [`Docs/PCVR.txt`](Docs/PCVR.txt).

## Building

**Windows (what the CI does):** in MSYS2 with `mingw-w64-x86_64-gcc`, `SDL2` and `SDL2_net` installed, run:

```
make -f Makefiles/Makefile.Win32 release
```

Then add `openxr_loader.dll` from the [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK-Source/releases). See [`.github/workflows/pcvr-windows.yml`](.github/workflows/pcvr-windows.yml) for the full packaging. Every push builds a zip; a push to `main` publishes it as the release for the version in `VERSION`.

**Linux:** run `make -f Makefiles/Makefile.UNIX NET_BOARD=0` (the CI builds on Ubuntu 22.04 and packages the tarball, see the workflow). This builds the same VR code on GLX and is handy for testing with [Monado](https://monado.freedesktop.org/). The OpenXR headers are vendored in `Src/Pkgs/openxr`, and the loader is loaded at run time, so non-VR use doesn't need it.

Start it with `supermodel <rom.zip> -vr -window -res=992,768 -no-wide-screen`.

## How it works

The VR code is small and mostly self-contained:

| Where | What |
|---|---|
| `Src/OSD/VR/DesktopXR.*` | OpenXR session, swapchains, controller input, `xrWaitFrame` on its own thread, the 2D layers drawn into each eye (room-fixed or head-locked), backdrop, cinema mode, guns, pointer ring |
| `Src/OSD/VR/VRMenu.*` | The in-headset options menu (Dear ImGui) |
| `Src/Graphics/New3D/VRHooks.h` | Interface between the New3D renderer and the VR layer |
| `Src/Graphics/New3D/New3D.cpp`, `R3DShader.*` | The scene is built once per frame. It is drawn flat (for the PC window and the light-gun reads), then once per eye with the eye's view and field of view. Culling is off, so the world beyond the 4:3 frame is drawn. |
| `Src/Model3/Model3.*` | Non-blocking emulator frames (post / poll / collect), so the headset never waits for the PowerPC thread |
| `Src/OSD/SDL/Main.cpp` | The VR main loop, controller-to-light-gun mapping, quick boot, hold-until-headset, gun calibration |

Some findings about The Lost World that may help with other Model 3 games:

- **The game doesn't clamp the aim.** It maps the gun linearly through its GUN ADJUSTMENT values, which live in EEPROM and are copied to RAM at boot (`0x123C` for player 1: centre X, centre Y, right, left, bottom, top; player 2 follows at `0x1242`). The VR build sets them to the middle half of the gun's travel, so the game aims a screen-width past each edge. No NVRAM has to ship.
- **No crosshair during play.** The red "X" sight is only drawn in cutscenes (VROM model `0x8A7B0C`); during play the game parks its sight markers (`0x8D836A`) outside the screen, where a VR view would see them. The VR build leaves them out of the headset image.
- **The letterbox bars are models.** One black model (`0x8D8398`), drawn at the top and the bottom in a full-screen overlay viewport (tan ±0.352 × ±0.268). The 3D view itself stays full-screen in cutscenes.
- **The camera zooms.** The main view's field of view changes during play and between cutscenes (half-width tangent 0.384 to 0.564), so anything sized from it, like a cinema screen, has to be decoupled.
- **The guns are models too.** The heroes' rifle (`0x93CE1C`, a copy of the soldiers' `0x93FB35` with its own textures) and the how-to-play pistol (`0x8415A9`). Their polygons are double-sided (stored twice with flipped normals), so a renderer outside the game has to cull them by face normal as the game does.
- **No SEGA logo at start-up.** After the warning and copyright screens the attract demo starts (around frame 924), or "Push Start" screens when credits are left; that is where the quick-boot snapshot is taken.

More details and measurements are in [`Docs/PCVR.txt`](Docs/PCVR.txt).

## Credits and licence

- **Emulator:** [Supermodel](https://github.com/trzy/Supermodel) by Bart Trzynadlowski and the Supermodel Team. The original README is in [`Docs/Supermodel-README.md`](Docs/Supermodel-README.md).
- **OpenXR headers and loader:** The Khronos Group (Apache-2.0).
- **Menu typeface:** Russo One by Jovanny Lemonad (SIL Open Font License 1.1), see `Assets/VRMenuFont-OFL.txt`.

This project is licensed under the **GNU General Public License v3.0**, like Supermodel. See [`LICENSE`](LICENSE). Any changes you distribute must stay under the GPL, with their source available.

The Lost World: Jurassic Park, Model 3 and SEGA are trademarks of their respective owners. This project is not affiliated with or endorsed by SEGA, Universal Pictures or Amblin Entertainment.
