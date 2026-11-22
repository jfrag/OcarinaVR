<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="images/logo-dark.png">
    <img src="images/logo-light.png" alt="OcarinaVR" width="520">
  </picture>
</p>

<p align="center"><b>First-person VR for the PC version of the Ocarina of Time remake. Your hands are Link's hands.</b></p>

<p align="center">
  <a href="#what-it-does">What it does</a> ·
  <a href="#what-you-need">What you need</a> ·
  <a href="#set-up-your-headset">Headset setup</a> ·
  <a href="#install">Install</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#settings-and-performance">Settings</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#troubleshooting">Troubleshooting</a> ·
  <a href="#legal-and-licences">Legal and licences</a>
</p>

OcarinaVR is a mod for [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (the continuation of
[Hookshot](https://github.com/NotRexed/Hookshot)), the native PC recompilation of
*The Legend of Zelda: Ocarina of Time* (Nintendo Switch 2). It puts you inside the game: you see through Link's eyes, at
his size, and his arms follow your own hands, fingers included. You draw the sword from your back, swing it for real,
lift a pot and throw it, pull the slingshot's pouch back with your other hand.

> **Early version.** It was built and tested on one PC (RTX 5090) with a Meta Quest 3 on Quest Link. Expect rough edges,
> and please report what you find.

<p align="center">
  <img src="images/screen-hands.jpg" width="49%" alt="Your own hands are Link's hands: the adult Link's gloved hands, in Hyrule Field">
  <img src="images/screen-sword.jpg" width="49%" alt="The Master Sword and the Hylian Shield in your own hands">
</p>

<p align="center">
  <img src="images/screen-third-person.jpg" width="70%" alt="The same game in the third person: a key gives the game's own camera back, behind Link">
  <br><em>Third person too: one key gives the game's own camera back, behind Link.</em>
</p>

## What it does

- **First person**, at the height of Link's eyes. The head drives the game's own camera, so the game draws what you
  look at, with the headset's field of view. A setting (key **P**) gives the game's camera back, behind Link.
- **You have Link's size.** The world is scaled so that his arms are yours: as a child you see the world as a child does.
- **Your hands are his.** His arms follow your controllers, or your bare hands, fingers included: with a controller the
  trigger bends his forefinger and the grip closes the others.
- **Gestures** that press the game's own buttons: draw the sword or the shield from your back, strike with a real swing,
  raise the shield, lift and throw rocks and pots, aim the slingshot and the bow with both hands.
- **What Link holds is in your hand**: the sword, the shield, the slingshot, the bow and its arrow, a lifted pot.
- **The game's interface** (hearts, buttons, map) floats on a panel in front of you. **Menus, cutscenes and loading
  screens** play on a big curved screen, in relief, with the game's own camera.
- **Rumble** goes to the headset's controllers.

Not done yet: the bombs, the boomerang and the hookshot are used with buttons as in the flat game; characters do not
answer a wave; a few effects drawn without depth may look flat in one eye.

## Known issues

This is an early version. These are known, and next on the list:

- **The bow is rough.** It shoots, and the game aims where your hands point, but the arrow is not laid straight between
  your two hands, it follows the hand that draws in odd ways, and the sight is small: it is hard to tell where the shot
  will go. A rework is under way (the bow turned onto the line of your two hands, as in archery games). The slingshot
  has the same sight.
- **Lighting and shadows.** Shadows can shift or fade when you move your head (seen in Hyrule Field), and some lights
  drawn without depth look flat or doubled in one eye.
- **The picture smears when you turn your head fast.** The game blurs its picture when its camera moves, and in the
  headset every head movement is a camera movement. Not switched off yet.
- **The shield hides much of the view**, above all as the child: his arms are short, so the shield stands close to your
  face. Hold that hand lower, or see the next line.
- **Link's arms are short** for a grown player. The menu line "Your arm length" (OcarinaVR section) changes how far your hands
  reach in the game's world: a smaller number reaches farther.
- **When you move your head sideways**, the scenery can seem to follow it a little: the game draws 50 to 60 pictures a
  second, the headset shows more, and only turns of the head are made up for in between.

## What you need

- **Your own copy of the game**, version 1.1.1 (with its update), as a `.nup` file or as `romfs` and `exefs` folders.
  **No game file is included here, and none will be.** Do not ask for one.
- **Windows 10 or 11**, 64-bit, with a CPU that has AVX2, 16 GB of RAM or more.
- **A graphics card with Vulkan 1.3 and mesh shaders** (as Longshot needs): NVIDIA GeForce RTX 20 series or newer, AMD
  Radeon RX 6000 series or newer, or Intel Arc. VR needs a fast one: tested on an RTX 5090.
- **A PC VR headset with OpenXR.** Tested: Meta Quest 3 with Quest Link (cable). Others (Air Link, Virtual Desktop,
  SteamVR headsets) should work but have not been tried.
- **About 60 GB free** while building, and an internet connection for the first install (the build tools).

## Set up your headset

OcarinaVR talks to the headset through **OpenXR**. Whatever your headset, two things must be true before you start the
game: its PC software is running with the headset connected, and that software is Windows' **active OpenXR runtime**.

**Meta Quest 2, 3, 3S, Pro** (tested: Quest 3, by cable)

1. Install the **Meta Quest Link** app on the PC and pair your headset with it.
2. In the app: **Settings → General → OpenXR Runtime → "Set Meta Quest Link as active"**.
3. Connect the headset (Link cable or Air Link) and start **Quest Link** from the headset's quick settings: you should be
   standing in Link's own home.
4. Start the game with `Play OcarinaVR.bat`.

**SteamVR headsets** (Valve Index, HTC Vive, Pico and others through SteamVR; a Quest through Steam Link or Virtual
Desktop). *Not tested.*

1. Start **SteamVR** with the headset tracked.
2. In SteamVR: **Settings → OpenXR → "Set SteamVR as OpenXR runtime"** (Virtual Desktop has its own choice of runtime in
   its Streamer's settings).
3. Start the game with `Play OcarinaVR.bat`.

**Controllers.** The controls are written for Touch-style controllers: two thumbsticks, A/B/X/Y, two triggers, two
grips. With other controllers SteamVR's controller-binding screen may be needed. Bare hands (hand tracking): written,
not tested.

**Play seated or standing**, with a little room for your arms. Look straight ahead when the game starts: that becomes
"forward". Hold the **Menu** button half a second to recentre at any time.

## Install

1. Download this repository: **Code → Download ZIP**, and unzip it in a folder of its own (for example
   `C:\Games\OcarinaVR`). Or `git clone` it.
2. Double-click **`Install OcarinaVR.bat`** and choose your game file in the window that opens.

   The installer works only inside its own folder (nothing is installed on the system). It downloads the Longshot Builder of
   the release this mod was written for (v3.1.1: it carries Longshot's source), the build tools Longshot names for it, the
   llvm-mingw compiler and Khronos's OpenXR loader, and checks every file's SHA-256. Then it adds this mod's lines to
   Longshot, builds the game **from your copy**
   (10 to 45 minutes), builds `retraced.dll`, and reads Link's arms and skeleton from your game's files.

   From PowerShell, with options:
   ```powershell
   powershell -ExecutionPolicy Bypass -File install.ps1 -Game "C:\path\your-game.nup" -Language en-US
   ```
   `-Language`: `en-US`, `en-GB`, `fr`, `fr-CA`, `de`, `it`, `es`, `es-419`, `pt-BR`, `nl`, `pl`, `ru`, `ja`, `ko`,
   `zh-Hans`, `zh-Hant`. `-Romfs` and `-Exefs` instead of `-Game` for extracted folders.
3. Start your headset's link with the PC (Quest Link), put the headset on, sit down and look straight ahead.
4. Double-click **`Play OcarinaVR.bat`**. Without a headset the game starts normally, flat on your screen.

**After an update of OcarinaVR**, run `Install OcarinaVR.bat` again: it rebuilds only what changed.

## Controls

<p align="center"><img src="images/controls.png" width="900" alt="OcarinaVR controls"></p>

| On the headset's controllers | What it does |
| --- | --- |
| Left thumbstick | Move, toward where you look |
| Right thumbstick | Turn, in 30-degree steps (smooth turning: in the menu) |
| Left trigger | Lock on, raise the shield |
| Right trigger | Use your item: hold to draw and aim, let go to shoot |
| Grips | Close Link's hands: take, lift, hold |
| A | Act: talk, open, lift; hold to run |
| B | Put the sword away, back |
| Y | Swing the sword |
| X | Jump |
| Menu button | Pause; hold half a second: recentre the view |
| Meta button | The headset's own menu |

The game's buttons that the headset's controllers do not have: press the **left thumbstick in** and keep it pressed, then
right thumbstick for the game's four arrows, right thumbstick click for the map, the grips for L and R. With the left
thumbstick pressed in, **Y** switches first / third person and **Menu** switches inside the game / big screen.

<details>
<summary>For those who know the game's own buttons</summary>

| Headset | Game |
| --- | --- |
| A, B, X, Y | A, B, X, Y |
| Left trigger, right trigger | ZL, ZR |
| Menu | + |
| Left thumbstick pressed in + grips | L, R |
| Left thumbstick pressed in + right thumbstick | The four arrows |
| Left thumbstick pressed in + right thumbstick click | − (the map) |

An ordinary gamepad works at the same time as the headset's controllers.
</details>

| Gesture | How |
| --- | --- |
| Draw | Empty right hand over your right shoulder, close it, pull it out: the sword is in your hand. Left hand: the shield |
| Strike | Swing for real, your hand closed on the hilt |
| Put away | Bring the sword back over your shoulder, or open your hand |
| Shield | Sword out: close your left hand |
| Take and throw | Reach down or forward and close your hand on a rock or a pot; open fast to throw, slowly to put it down |
| Aim and shoot | Slingshot or bow: hold the right trigger, hold it out with your left hand, pull back with your right, let go to shoot |

Keys, with the game's window in front: **P** first / third person, **V** in the game / big screen, **G** recentre,
**O** the same picture in both eyes (rests the eyes).

## Settings and performance

- **In the game**: Longshot's menu (**Insert** or **F1**, with the game's window in front; key **M** opens it too),
  **Graphics** tab, the **OcarinaVR** section at the top: first person, big screen, snap turning, Link's body visible, gestures, Link's hands on
  yours, hand tilt, your arm length. They are kept for the next start (`game\retraced_menu.ini`).
- **Left-handed**: the installer asks which hand holds the sword. To change it later, in `game\retraced.ini`, set `vr_left_handed = 1`: the sword goes to your left hand, as Link
  himself holds it, the shield to your right arm, and the left trigger becomes the item button. (By default the sword
  is in your right hand, for right-handed players.)
- **Comfort**: snap turning is on by default. If your eyes strain, key **O** shows the same picture to both eyes.
- **Picture size**: in VR the game draws at the size your headset's software recommends. If the game is slow, lower the
  **render resolution in that software** (Meta Quest Link: Devices → your headset → Graphics Preferences; SteamVR:
  Settings → Video → Render Resolution). The game itself runs at 60 frames a second at most.
- **The first start** prepares the graphics for a few minutes (a progress line in the game's window); the next starts
  take seconds. A new graphics driver makes it happen once more.
- **The game's own options** (language, controls, sound): Longshot's Insert menu, as without this mod. Longshot's
  Field of view line does nothing in VR (the headset sets it), and `Play OcarinaVR.bat` keeps its upscalers (DLSS,
  FSR, XeSS) and frame generation off: they do not work in the headset yet.

## Update and uninstall

- **Update**: replace this folder's files with the new version's (keep the `game` and `_build` folders), then run
  `Install OcarinaVR.bat` again: it rebuilds only what changed.
- **Uninstall**: delete this folder. Nothing was installed anywhere else on the PC.

## How it works

Longshot translates the game's program for the PC and draws its pictures with Vulkan. OcarinaVR adds about forty short
lines to Longshot (`hooks.json`, put in by `tools/apply-hooks.ps1`, each one found by a text anchor), one file beside
its renderer (`adapter/rtx_longshot.cpp`, with the menu's OcarinaVR section) and one DLL of its own, `retraced.dll`
(`src/`), which talks to the headset through OpenXR.

It is not a mod of Longshot's `mods` folder: those replace the game's files and patch its code with fixed bytes, and
Longshot loads no program of a mod. The VR mode needs code inside the renderer and the game's translation, so it is
built in, as before.

- **The camera.** Where the game builds its gameplay camera, the headset's pose and field of view replace the game's.
  The game itself believes it looks where you look: what it draws, its shadows and its level of detail follow.
- **The game's sorting.** The game leaves out what its camera does not see, and in the first person Link's hands are
  under and behind the camera. The game is given the same camera moved back for its sorting, and the true view is put
  back where it fills its views' data for the graphics card (`main+0x6bc520`), so what Link holds stays drawn.
- **Two eyes.** The game draws one picture a frame, from the middle of your eyes. Each eye's picture is made from it and
  its depth, in the exact field and orientation the headset gives for that eye.
- **The interface.** The game's interface is drawn into an image of ours, shown on a panel; menus and cutscenes go to a
  curved screen.
- **Link's arms.** `tools/vr-corps.py` reads Link's skeleton and which triangles are his arms from your copy of the game
  (the result stays on your PC). Each frame, the block of bone matrices the game sends for Link is rewritten: his arms
  are solved from your shoulders to your hands, finger by finger; the rest of his body is left out of your view (his
  shadow stays).
- **What he holds.** A thing drawn near his hands is followed: what moves with his hand is laid in yours, what stays
  on his back is hidden with his body.
- **Gestures** become the game's own buttons (Y, B, A, ZL, ZR); nothing in the game's program is changed for them.

## Troubleshooting

| What you see | What to do |
| --- | --- |
| The game starts flat, not in the headset | Start the headset's link first (Quest Link), then the game |
| "start-up: pipelines" for minutes at the first start | Longshot prepares the graphics once; the next starts take seconds |
| Your eyes strain, things look doubled | Take the headset off. Key **O**: the same picture for both eyes |
| The world is too big or too small | Menu, OcarinaVR section: "Your arm length" (shoulder to wrist, in cm) |
| Link's hands are tilted against yours | Menu, OcarinaVR section: "Hand tilt" |
| Something else | `game\retraced-out\retraced.log` (lines "xr:" and "vr:") and `game\logs\longshot_log.txt`: attach them to an issue |

## Legal and licences

- OcarinaVR is an unofficial fan project, **not affiliated with or endorsed by Nintendo or Meta**. *The Legend of Zelda* and
  *Ocarina of Time* are trademarks of Nintendo. Meta Quest is a trademark of Meta.
- **No game files** are in this repository. You need your own copy of the game. **Never share a built game folder**:
  it holds the game's translated program.
- OcarinaVR is free software under the **GNU General Public License, version 3 or later** ([LICENSE](LICENSE)). You may
  use it, change it and share it, also commercially, as long as what you share stays under the same licence, with its
  source.
- It builds on **Longshot** and **Hookshot** (GPL-3.0-or-later) and their authors' work, downloaded from Longshot's own
  releases at install. Other parts, their authors and their licences: [NOTICE.md](NOTICE.md).
- The drawings of the two controllers in the controls picture are **Meta's** (Meta Quest Hardware Art), used under
  Meta's [Art Attribution License](https://developers.meta.com/vr/licenses/art-1.0/) to show which control does what.
  They are not under the GPL.
- The VR mode of *Breath of the Wild*, [BetterVR](https://github.com/Crementif/BotW-BetterVR), inspired some of the
  ideas here (hands, drawing from the back). No code was taken from it.
