# NFSU2_PhotoMode B13 - MW-style floating buttons

Based on the working B9 native Direct3D9 device hook and the original NFSMW_PhotoMode's button layout.

The Photomode overlay has **only** three groups of white outlined control hints at the bottom of the picture:

- **ESC**  BACK
- **9** **0**  ZOOM
- **Arrow keys**  VIEW CAR

There is no black panel, green trim, top title, separator, or full-screen gradient. The position, white key outlines, dark transparent key fill, spacing and pixel-style lettering follow the supplied NFSMW_PhotoMode's overlay design.

## Unchanged from working B9

- The native **Photomode** pause-menu option, including its hardcoded icon hash
- Orbit camera and zoom controls
- World freezing
- Windows audio-session muting and restoration (B9 behavior; no B10 startup unmute logic)
- The native SPEED2.EXE D3D9 render hooks, including Present, EndScene, and swap-chain Present
- The existing `NFSU2_PhotoMode.ini` button-visibility setting

## Build and install

1. Open `NFSU2_PhotoMode.sln` in Visual Studio 2026.
2. Choose **Release | Win32** and build the solution.
3. Replace the prior `NFSU2_PhotoMode.asi` in the game's `scripts` folder with the new output from `bin\Release`.
4. Keep your current `NFSU2_PhotoMode.ini` alongside the ASI and set:

```ini
[Overlay]
ShowButtons=true
```

Set `ShowButtons=false` to hide all button hints; no other Photomode behavior is affected. This INI does not configure the pause-menu icon.

This is a source update based on the in-game-proven B9 renderer. It has been checked structurally and compared against B9, but it has not been compiled with Visual Studio or runtime-tested in the game here.


B13 change: overlay button outline/text color updated from white to lime-green #B3CA93.

B13 update: Logging is disabled. NFSU2_PhotoMode.log is no longer created or written. The existing INI toggle and all B12 overlay, camera, pause-menu, and audio behavior remain unchanged.
