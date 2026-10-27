Windows build of Supermodel with the OpenXR VR mode, set up for **The Lost World: Jurassic Park**.

**Download** `LostWorldVR-windows.zip` (Windows) or `LostWorldVR-linux-x86_64.tar.gz` (Linux, experimental) below and unpack it. Put your own `lostwsga.zip` ROM set in the `ROMs` folder. Connect the headset, with Virtual Desktop, Quest Link or SteamVR as the active OpenXR runtime, and run `Play The Lost World VR.bat` (Linux: `./play-lostworld-vr.sh`, with SteamVR, Monado, WiVRn or ALVR). Full instructions are in `README-VR.txt` inside the zip and in the README. No ROMs or other game files are included.

## v0.3.0

- **Sky without the kaleidoscope.** Around the original screen, the sky now comes from the game's own background layers: left and right it continues and turns exactly with the game's sky instead of showing mirrored copies that slid the other way; above and below it fades softly into the sky's colour. Thanks to the player who reported it.
- **Options menu for Valve Index and similar controllers.** Hold both sticks in for a second, or press M on the PC keyboard, to open or close the options menu. On Index, both menu buttons open SteamVR's own menu. A single stick click still re-centres the view, now when you let go.
- **Render resolution up to 2.0×, with the pixels per eye shown in the menu.** The old hidden 1664-pixel cap (`VRMaxEyeWidth`) is gone, so 1.0 now really means the size your headset or Virtual Desktop asks for. That's noticeably sharper and also heavier than before: on a weaker PC or a handheld, lower Render resolution in the menu if the game stutters.

## v0.2.0

- **The game's own guns in your hand.** Gun model › **Rifle** is the heroes' gun from the cutscenes and **Pistol** the handgun from the how-to-play screen, both read from your own ROM with their textures. Rifle is the default.
- **Arcade gun** is now its own choice: your own cabinet gun model (`Assets/VRGameGun.obj`) in blue or red. *Custom* (`Assets/VRGunModel.obj`) stays.
- **The gun is always in front** of the score and text, and keeps its textures in menus and cinema mode.
- **Gun lighting:** the guns are lit so every side you turn towards you reads, with soft highlights; double-sided polygons no longer flicker light/dark.
- **Flashlight scenes** (e.g. the night T-Rex) are lit correctly in the headset: the light lies on the scene in both eyes instead of floating in front of you.
- **Hide player 2's PRESS START** now works in The Lost World: player 2's INSERT COIN / Push 2P Start is hidden in play and in cutscenes.
- **Linux build (experimental):** a native x86_64 build, `LostWorldVR-linux-x86_64.tar.gz`, with start scripts and the libraries it needs. Use it instead of running the Windows build under Proton, which can't work (Proton's OpenXR layer has no OpenGL support). Tested with Monado's simulated headset only; reports welcome.
- The guns' textures are kept in `Saves/lostwsga.vrguntex` (made from your ROM on your PC) once the game has shown them.

## v0.1.0 — first release

- **Stereo 3D with head tracking**, the world drawn beyond the original 4:3 screen and the sky carried on past its edges.
- **Aim anywhere in view**, up to a screen-width past each edge of the original screen, with a red ring where your gun hand points (the game has no crosshair during play). The game's own cutscene sight is hidden, so there is only one.
- **Gun in your hand** from your own model: *Game* (`Assets/VRGameGun.obj`) in blue or red, chosen in the menu, or *Custom* (`Assets/VRGunModel.obj`). No model is included.
- **Recoil vibration**, right or left gun hand.
- **Steady 60 fps** whatever the headset's refresh rate.
- **Cinema mode** with a screen that keeps its size when the game zooms, TV-cabinet bezel, screen glow, 3D depth and the aiming ring on the screen.
- **No cutscene letterbox bars**; hit static and full-screen fades fill the whole view.
- **Options menu** in the headset, in the colours of the game's InGen screens.
- **Fast start**: after the first run, the game opens at the start of its attract demo.

Tested on a Meta Quest 3 with Virtual Desktop at 90 Hz.
