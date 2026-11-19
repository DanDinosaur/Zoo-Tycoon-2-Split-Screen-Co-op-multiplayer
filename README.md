# Zoo Tycoon 2 Split-Screen Co-op

Couch co-op for **Zoo Tycoon 2** on one PC: two players, two controllers, one TV. When a second controller is connected, the game splits into a left half (player 1) and a right half (player 2). Each player gets their own camera and can use the zoo's build menus.

You can also **ride the animals** in the style of *Jurassic Park: Operation Genesis*. Walk, run, swim and dive with them, and fight other animals, guests, staff or the other player.

Made for a dad-and-son setup on a ROG Ally docked to a 75" TV at 1920x1080. It also works on a normal Windows PC.

> **Latest release: v1.0 (build ZT20e)** - see [Releases](../../releases). This is the first public release. It's a hobby mod, so expect a few rough edges (see [Known issues](#known-issues)).

---

## Which game versions work

The mod hooks fixed code addresses inside one specific `zt.exe`, so **only this version works**:

| | Version | Works? |
|---|---|---|
| ✅ | **zt.exe 33.5.0.2** (File version `33, 5, 0, 2`, product version `33.05.00.0002UO`) - *Zoo Tycoon 2: Ultimate Collection* / "Complete Edition" with all expansions | **Yes** - this is the only supported version |
| ❌ | Any other `zt.exe` (the original 2004 disc release, single expansion installs, other patches, other repacks with a changed exe) | **No** - the mod detects it, shows a message once and stays **off**. The game runs normally |
| ❌ | *Zoo Tycoon: Ultimate Animal Collection* (2017, Xbox / Steam / Microsoft Store) | **No** - that's a different game, not Zoo Tycoon 2 |

To check your copy, right-click `zt.exe` → *Properties* → *Details*. *File version* must be **33.5.0.2**. The supported exe is **8,422,032 bytes** with SHA-256 `12d9c4cd2c38ca5e7b19a5233784ae8bf703057d32129b39d8b0290c9697719c`.

Custom animal packs (`.z2f` downloads such as dinosaurs, WWD, JPOG skins) work. Custom animals can be ridden too.

### Mod packs installed in the test setup

v1.0 was tested on a game with all of the packs below in the game folder. Animals from these packs can be ridden, for example the JPOG T-Rex, Sarcosuchus, Deinosuchus, Mastodon, Goliath Grouper and Dracorex. You don't need any of them: the mod also works on a plain game.

| Pack (file in the game folder) | What it is |
|---|---|
| `zzzzzz_zt2_operation_genesis_part_b_v1.0.z2f` | Operation Genesis (JPOG) dinosaurs for ZT2, part B |
| `zzzzzzz_zt2og_patch1.z2f`, `zzzzzzzzz_zt2og_patch2_eggfix.z2f` | JPOG pack patches 1 and 2 (egg fix) |
| `zzzzzzzz_jpog_retextured_skins.z2f` | JPOG retextured dinosaur skins |
| `zzzzzzzzz_jpog_objects_v3.0.z2f` | JPOG buildings / objects v3.0 |
| `zWWD_1_NewBlood.z2f`, `zWWD_2_TimeOfTheTitans.z2f`, `zWWD_3_CruelSea.z2f`, `zWWD_5_SpiritsOfTheIceForest.z2f`, `zWWD_7_LandOfGiants.z2f`, `zWWD_8_GiantClaw.z2f`, `zWWD_enableFootsteps.z2f` | Walking With Dinosaurs packs (Sarcosuchus, Deinosuchus and more) + footsteps |
| `zWWB_Megatherium.z2f`, `zWWSM_Cymbospondylus.z2f` | Walking With Beasts / Sea Monsters animals |
| `DM_Dracorex.z2f`, `DM_Gallimimus.z2f`, `DM_Oviraptor.z2f`, `DM_EmperorTamarin.z2f` | Community animal packs |
| `DM_WLP_Dorado.z2f`, `DM_WLP_GoliathGrouper.z2f`, `DM_WLP_HumpheadWrasse.z2f` | Community marine animal packs |
| `zEuropeanExpeditions.z2f`, `zPalaeontologistsOnSpeed.z2f` | Community packs |
| `zUnderwaterTunnel_v1.z2f` | Underwater tunnel |
| `zWidescreen.z2f` | Widescreen fix |
| `zzzzzzzzzzzz_TankButtonsFix.z2f` | Tank buttons fix |
| `Addax`, `ElephantAsian_050505`, `LeopardAndDummy`, `TundraAndMuskOx`, `BeautificationPack_061305`, `ChallengePack1`, `GuestPack`, `HolidayPack`, `SceneryPack`, `PDLC1_2` (.z2f) and the `DLC` folder | Official Zoo Tycoon 2 download packs that come with the Complete Edition |

**Windows:** tested on Windows 11 (ROG Ally). It needs the DirectX 9 runtime that Zoo Tycoon 2 already uses.

## Which mod builds work

Only use the official release. Development builds are listed here because some were shared during testing and a few of them are broken:

| Build | Status |
|---|---|
| **ZT20e = v1.0** | ✅ **Release.** ZT20d plus a game-version check. |
| ZT20d | ✅ Good (the tested build v1.0 is based on) |
| ZT20c | ❌ Animals can't be ridden on some game starts |
| ZT20b | ⚠️ T-Rex / Sarcosuchus models jump around while ridden |
| ZT20 | ⚠️ Ridden walk animations stop after ~5 s (animal slides) |
| ZT19z | ❌ Controller stuck in the menus, map won't move |
| ZT19y and older | ⚠️ Main menu, pause menu and dialog controller problems; mammoth sinks into the ground |

---

## Install

1. Download `ZT2-Coop-v1.0.zip` from [Releases](../../releases).
2. Unzip **everything** into your Zoo Tycoon 2 folder (the folder with `zt.exe`, for example `C:\Program Files (x86)\Microsoft Games\Zoo Tycoon 2`). The zip contains:
   - `d3d9.dll` - the mod
   - `zt2mod.ini` - settings (open with Notepad)
   - `zzzzzzzzzzzzzz_zt2mod.z2f` - camera zoom range for split screen and quiet intro movies
   - `ZT2 Co-op - Setup.bat` / `ZT2 Co-op - Uninstall.bat`
3. Double-click **`ZT2 Co-op - Setup.bat`** once. It copies the game's own aim-circle textures out of your game files into a `zt2mod` folder (nothing is downloaded).
4. Start the game the normal way (`zt.exe` or your usual shortcut).

**Resolution / widescreen:** set the resolution in the game's own *Options*. For 16:9 screens, the community `zWidescreen.z2f` mod works together with this mod.

**Antivirus:** some antivirus programs flag any `d3d9.dll` placed in a game folder, because that's how many game mods hook in. The full source code is in this repository (`source/`), and you can build it yourself.

## Uninstall

Double-click **`ZT2 Co-op - Uninstall.bat`** in the game folder, or delete `d3d9.dll`, `zzzzzzzzzzzzzz_zt2mod.z2f`, `zt2mod.ini` and the `zt2mod` folder. Your zoos and saves aren't touched.

---

## How to play

- **One controller:** player 1 plays with the controller (or keyboard + mouse as normal).
- **Two controllers in a zoo:** split screen starts automatically. Player 1 = left half, player 2 = right half.
- **Player 2 on a second keyboard + mouse:** in a zoo, press **F9** on the second keyboard, then click with the second mouse. Press F9 on that keyboard again to switch player 2 back to a controller.
- Menus (main menu, load screen, pause menu, pop-up questions) work with the controller: stick or D-pad moves the highlight, **A** presses, **B** goes back.

### Controller - zoo building (both players)

| Button | Action |
|---|---|
| Left stick | Move the camera (the aim circle stays in the middle of your half) |
| Right stick | Left/right = turn, up/down = zoom |
| D-pad up / down | Tilt the camera |
| D-pad left/right, LB / RB | Rotate the object you're placing |
| **A** | Select / place / press the highlighted button |
| **B** | Back: close the menu, stop the tool, deselect, close an info window |
| **X** | Construction menu |
| **LT** | Animals menu |
| **Y** | Zoo menu (finances, status) |
| **RT** | Staff menu |
| **View / Back** | Delete tool. In an item list: **A-Z list** of the names to pick from |
| **Start** | Pause menu (pauses player 2 too) |
| Left stick click | Zookeeper first-person walk |
| Hold right stick 0.4 s | Switch between controller style (aim circle) and PC style (free pointer) |
| **RB + D-pad up** | **Ride** the animal at the aim (or the one you just selected) |

### Controller - riding an animal

| Button | Action |
|---|---|
| Left stick | Walk (turns towards where the camera looks). Stick down / sideways = step back / sideways |
| Left stick click | Run on / off |
| Right stick | Turn / tilt the riding camera |
| D-pad up / down | Camera closer / further |
| LB / RB | Turn the animal on the spot |
| **RT** | Attack (bite, kick, ram - whatever the species does) |
| **LT** | Special move. In water: hold to **dive** |
| **A** | Eat / drink / sniff. In water: hold to **swim up** |
| **Y** | Call (roar, trumpet...). Hold for its second call |
| **X** | Lie down → sleep → stand up |
| **B** | Let go of the animal |

Riding notes:
- An animal is beaten after **5 bites** (10 for big animals like mammoths, T-Rex and elephants). A beaten animal lies down and disappears after a few seconds.
- Guests and staff can be bitten too (3 bites).
- If one player beats the **other player's** animal, that player is sent back to building mode.
- A ridden animal breaks a fence after 5 attacks (never the zoo's outer wall). Walking into a fence is blocked.
- Sleeping animals wake up when bitten.

### Keyboard (player 1 riding)

`T` take / let go, `I J K L` walk, `Shift` run, `Y` bite, `H` special, `O` call, `Z` eat/drink, `B` lie/sleep.

---

## Settings (`zt2mod.ini`)

Open `zt2mod.ini` with Notepad. Useful keys:

| Section / key | What it does |
|---|---|
| `[split] mode` | `1` = split screen when two controllers are connected (default), `2` = always, `0` = off |
| `[split] sky` | Painted sky behind the riding cameras (1/0) |
| `[pad] deadzone`, `pointer_speed` | Stick feel |
| `[kbm] enabled` | Player 2 on a second keyboard + mouse (F9) |
| `[animal] walk_speed`, `run_speed`, `turn_rate` | Riding speeds |
| `[animal] bite_hits`, `bite_hits_big` | Bites needed to beat an animal |
| `[animal] fence_break`, `fence_hits` | Fence breaking while riding |
| `[animal] walk_reloop` | Seconds between walk-animation restarts while riding (default 4.6) |
| `[camera] max_zoom` | How far out the cameras zoom (player 1's limit is also set in the .z2f) |
| `[general] log` | `1` writes `zt2mod.log` in the game folder (attach it to bug reports) |

---

## Known issues

- Ridden **T-Rex** sometimes tries to lean down to eat or drink.
- Ridden **Sarcosuchus** model still jumps around a little now and then.
- The walk animation loop of a ridden animal restarts every few seconds and you can sometimes see it (`walk_reloop` in the ini).
- The very first **A** on the main menu right after the intro may need a second press.
- Guests can sometimes walk on the bottom of big water areas (seen with the underwater-tunnel add-on).
- One game UI is shared by both players: menus open over the whole screen, and the info window moves to the half of the player who selected something.

## Reporting bugs

Open an [issue](../../issues) with what you did, what happened, and the `zt2mod.log` file from your game folder. The log is overwritten every time the game starts, so copy it right after the problem.

---

## Building from source

The source is in [`source/`](source). It builds with MinGW-w64 (32-bit):

```sh
cd source
sh build.sh        # -> source/out/d3d9.dll
```

`d3d9.dll` is a proxy that forwards to the real system `d3d9.dll`. It hooks the game's rendering for the second viewport, reads XInput for both pads, and talks to the game's own Lua scripts (a Lua 5.0 runtime is linked in, `source/lua`). `miniz` (`source/miniz`) reads sounds from the game's `.z2f` archives.

## Credits

- Mod by **DanDinosaur**, built with Claude (Anthropic).
- Lua 5.0 - Copyright © 1994-2003 Tecgraf, PUC-Rio (MIT license). miniz - Rich Geldreich (MIT / Unlicense).
- Zoo Tycoon 2 © Microsoft / Blue Fang Games. This mod contains no game files. The aim-circle textures are copied from your own installation by the setup script.
