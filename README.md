# Black Mesa VR

> [!IMPORTANT]
> **New here? Start with the newest experimental release.** Most of the current development happens on the
> `experimental-beta` branch, for the **Black Mesa public beta** (Steam → Black Mesa → Properties → Betas →
> `public-beta`). Those releases are marked *Pre-release* on GitHub, but they get the current fixes and
> features first, and they are what we test against.
> Switch Black Mesa to the public beta, then get the top *Experimental* entry on the
> **[Releases page](https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr/releases)**. If you're on Linux, make sure to use the Proton version of the game.

<a href="https://www.youtube.com/watch?v=S9kmK95yqgM">
  <img src="assets/release-2026-09-06.jpg" alt="Black Mesa VR — Weapon Menu" width="90%">
</a>

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/P5P56X4HH)

A VR mod for the Steam version of **Black Mesa**, bringing 6DOF motion controls, true stereo rendering, OpenXR, and VR-first weapon interaction to the game.

The foundation of the VR implementation is based heavily on the approach pioneered by **L4D2VR**, adapted and extended for Black Mesa.

L4D2VR source:
https://github.com/keyou91/l4d2vr

MacOS version here:
https://github.com/armax9/black-mesa-vr-macos

## Current Status

**The project is working start-to-finish and is playable as a full VR mod.**

* True stereo rendering
* 6DOF motion controls
* HL2VR-style weapon selection
* Accurate crowbar swinging (HL2VR)
* OpenXR implementation
* Health and armor on the hand 
* Weapon-mounted ammo counter
* Per-hand item holding and use
* In-world transparent pause menu
* Bare hands until HEV (and throughout Blue Shift)
* Crossbow zoom (headset aim while scoped)
* VR main menu with a motion-controller cursor
* Blue Shift HUD theme (weapon wheel and wrist HUD)
* Multicore rendering

## Known Issues

The VR experience is fully playable, but several features are still a work in progress:

* **Quest 3:** Meta Link uses the Link / Oculus OpenXR runtime. Virtual Desktop must use Streamer Options → **OpenXR Runtime = VDXR**. SteamVR in that dropdown inverts the world
* If shots misalign with the weapon, press the right thumbstick once to recenter. Roomscale support is coming.
* Two-handed weapons are planned for a later update.
* Manual reloading is not currently implemented and is planned for a future update.

## Linux quick start: OpenXR runtime

On Linux the OpenXR loader needs an *active runtime*, and running SteamVR is not enough: SteamVR also has to be
registered as the system's OpenXR runtime. If it is not, `bmvr_xfer_helper --xr-probe` fails with
`failed to determine active runtime file path` / `no OpenXR runtime installed or active?` (`-51`, `UNAVAILABLE`).

SteamVR's *Set SteamVR as OpenXR runtime* button (Settings → Developer, advanced settings) is missing on some Linux
installs. The manual way works regardless:

```bash
mkdir -p ~/.config/openxr/1
ln -sf ~/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json \
       ~/.config/openxr/1/active_runtime.json
```

Adjust the path if SteamVR lives in another Steam library (Flatpak Steam: under `~/.var/app/com.valvesoftware.Steam/`).
Or override per launch, without touching the system setting:

```bash
XR_RUNTIME_JSON=$HOME/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json %command%
```

Then, with SteamVR running and a headset connected, check it:

```bash
"$HOME/.local/share/Steam/steamapps/common/Black Mesa/bmvr_xfer_helper" --xr-probe
```

It should end with `xr-probe: PASS`. With the headset on you can then run the frame-loop self-test, which needs no game:
`"$HOME/.local/share/Steam/steamapps/common/Black Mesa/bmvr_xfer_helper" --selftest-xr` (left eye should be BLUE with an "L", right eye RED
with an "R"; Ctrl-C ends it). Experimental: it passed on SteamVR with an RTX 3080 (one run); the game itself is not wired to it yet. (Observed: SteamVR/OpenXR 2.19.4, NVIDIA RTX 3080, session reaches READY. The probe
submits no frames.) If it still says `UNAVAILABLE`, see `docs/LINUX_PORT.md` (Stage 4a).

## Black Mesa: Blue Shift

Black Mesa VR also supports **Black Mesa: Blue Shift**.

After installing Blue Shift, launch the game with:

```text
-game bshift
```

The same VR setup can then be used with the Blue Shift campaign. Calhoun never
wears the HEV suit, so this build keeps the bare-hand models for the whole
Blue Shift session. The weapon wheel and wrist HUD use Calhoun blue instead of
HEV amber.


## Huge thanks wormslayer for HL2VR & to my testers and contributors for their patience and help:

marnovdm for quality of life features and polish + beta support (https://github.com/marnovdm)
TheRealBubble
SilentVortiguant - [Check out his youtube](https://www.youtube.com/@SilentVortigauntVR).

yakupagagaming

Credits for the custom models:
https://steamcommunity.com/sharedfiles/filedetails/?id=2969302231 (HEV Gloves)
