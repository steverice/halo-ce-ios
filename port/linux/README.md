# Linux

`ninja linux` compiles the game with clang for 32-bit x86 Linux. The result
is a native executable, `build/linux/halo`. The game shows its graphics with
OpenGL 4.5. It plays sound through SDL3. It accepts keyboard, mouse and
gamepad input.

The game is 32-bit code because its data (tags, cache files, saved games)
contains 32-bit pointers, as on the Xbox.

## Requirements

You do not need the Xbox SDK. The declarations that the game uses are in
`port/include/xdk`.

To build:

- Python and ninja.
- clang. The option `--linux-cc` of `configure.py` selects a different
  compiler.
- The 32-bit glibc development files: `lib32-glibc` on Arch Linux,
  `gcc-multilib` and `libc6-dev-i386` on Debian and Ubuntu.
- The 32-bit SDL3: `lib32-sdl3` on Arch Linux, `libsdl3-dev:i386` on Debian
  and Ubuntu.

To start the game:

- The 32-bit OpenGL libraries (`lib32-mesa`).
- The 32-bit PipeWire or PulseAudio client libraries (`lib32-pipewire` or
  `lib32-libpulse`).

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja linux`.

## Start the game

Enter `build/linux/halo`.

The game data is the folder that contains `maps/`, from an Xbox disc image
of any version of the game. The game looks for this folder in this
sequence:

1. `paths.data` in `config.toml`.
2. The current folder.
3. The folder of the executable.
4. `assets/` in the current folder, and `assets/` in the repository that
   contains the executable.

If the game finds no data, it asks for an Xbox disc image (`.xiso` or
`.iso`). This occurs at the first start:

- Select "No" to stop the game.
- Select "Yes" to open a file picker. Select the disc image. The game copies
  `maps/` next to the executable and shows the progress.

The game writes the copy to `maps.partial`. When the copy is complete, the
game changes the name to `maps`. If the copy stops before it is complete,
the game asks for the disc image again at the next start.

### NVIDIA on Wayland

The game is a 32-bit program. NVIDIA's driver draws OpenGL into a Wayland
window through its `egl-wayland` library. If only a 64-bit version is
installed, a 32-bit OpenGL window on Wayland can fall back to Mesa's
`llvmpipe`, which draws on the processor. The log shows
`OpenGL ... on llvmpipe`, and the game is slow or never finishes loading.

So on a Wayland desktop with NVIDIA's driver loaded, the game opens its
window through XWayland (X11), where the 32-bit driver draws, unless a
32-bit `egl-wayland` is installed. The log says so ("video: NVIDIA's
driver ..."). `SDL_VIDEO_DRIVER=wayland` or `SDL_VIDEO_DRIVER=x11` chooses
the video driver yourself.

## Files and folders

| Xbox drive | Folder |
| --- | --- |
| `d:\` | The data root: the folder that contains `maps/`. |
| `z:\` | `z/` in the save root. This folder contains the cache (approximately 800 MB of map data) and the saved games. |
| `u:\` | `u/` in the save root. This folder contains the user data. |

The save root is `paths.saves` in `config.toml`. If that setting is empty,
the save root is `$XDG_DATA_HOME/halo-linux` (usually
`~/.local/share/halo-linux`).

The game makes the folders when it needs them. Names of files and folders
are not case-sensitive, as on the Xbox.

These files are in the data root:

| File | Contents |
| --- | --- |
| `debug.txt` | The log of the game. At start-up, the game shows the data root in the terminal. A crash writes its report (the faulting address and the calls that led to it) here as well; the `reference address` line at the top of each session places those addresses in the build. |
| `init.txt` | Console commands that the game does at start-up. For example, `map_name levels\a10\a10` starts the first campaign level. |
| `tags/` | Sound tag files that replace the sounds of the maps (`audio.loose_sounds`). |

The settings are in `config.toml` next to the executable. Refer to
"Settings". Internet play's MQTT brokers are in `brokers.txt` next to it
(`network.brokers_file`).

If the game stops because of a fatal signal, it writes the address and a
backtrace to the standard error. To find the function at the address, enter
`addr2line -e build/linux/halo <address>`.

## Controls

The keyboard and the mouse are a control scheme of their own for the player
of controller 1: each action has up to two keys or mouse buttons, which
Settings > Controls Setup (or `[controls]` in `config.toml`) changes. The
game adds the input of the first gamepad to controller 1. The other
gamepads operate controllers 2 to 4. With two or more players on this
computer (co-op, or split screen in a network game) and only one gamepad,
that gamepad is controller 2 (player 2) and the keyboard and mouse stay
controller 1. The gamepad changes controller only when none of its buttons
is held. The profile's button layout (Settings > Gamepads) is the
gamepads' only. Besides the Xbox's five, it has the Master Chief
Collection's Bumper Jumper: jump on the left bumper, melee on the right,
switching grenades on A, action and reload on B and the flashlight on X.

| Action | Keys and buttons (default) |
| --- | --- |
| move | W, A, S, D |
| aim | mouse (direct aim) |
| fire | left mouse button |
| throw a grenade | right mouse button, G |
| jump (and skip a cutscene) | space |
| crouch | left ctrl, C |
| melee | F, mouse button 4 |
| reload | R |
| action (pick up, hold to swap weapons, enter or leave a vehicle; never reloads) | E |
| change the weapon | mouse wheel, 1 |
| change the grenade | X |
| flashlight | Q |
| zoom | Z, middle mouse button |
| show the scores (hold) | tab |
| pause menu | escape |

Always: \` opens the developer console, F12 releases or captures the mouse,
F11 changes between fullscreen and window.

One movement of the mouse wheel changes the weapon one time. A second
movement after a short pause changes it again.

In the menus, the mouse moves a pointer:

- The item below the pointer gets the focus.
- A left click selects the item. On a setting with values, a click on the
  left or right half changes the value. On a button in the key of a screen
  (for example "B = Back"), a click pushes that button.
- A right click goes back.
- The mouse wheel moves through the items.

The keyboard also operates the menus, with keys of its own: the arrow keys
(and W, A, S, D) move, space or enter selects, escape or backspace goes
back (escape resumes the game from the pause menu), delete deletes. When the game continues, the mouse aims again. A mouse button that you hold from the menu does not fire until
you push it again.

## Menus

The menus are the PC version's (Halo Custom Edition's): its main menu and
every screen it leads to (campaign, profiles, multiplayer with its server
browser, direct IP and server setup, the gametype editor, and the profile's
settings: controls, gamepads, mouse, audio, video, network and colour),
laid out as it has them. Their pictures are the high-res redraws; the few
that are not yet (3D renders, screenshots) are drawn from the Xbox's own
menus where it has the same, else a placeholder, listed in
`port/assets/menus/NON_HANDDRAWN.md`.

The campaign is wired: Campaign, on player 1's profile (the one last used),
continues its saved game, starts a level it has reached at a difficulty (New
Game), or loads or deletes any profile's saved game (Load Game). Many of the
other functions behind the screens are not wired to this game yet
(`port/assets/menus/UNWIRED.md`): the lists they fill (profiles, maps,
servers, key bindings) are empty, and the settings they change do not
change. The screens open, close and move between each other as the PC
version's. `display.menus = "xbox"` gives the Xbox's menus, which start every
kind of game.

Multiplayer > CO-OP CAMPAIGN is the Xbox's cooperative play, which the PC
version does not have: two players on this computer play the campaign in
split screen. Player 1 is the player who chose it, on the current profile.
Player 2 then chooses a profile with their own controller (a gamepad), and
New Game's levels are those either profile has reached. Either player's
controller chooses the level and the difficulty. A co-op game does
not continue a saved game of one player.

Network games have split screen too: up to 4 players on each computer. In
the game lobby, another controller presses START to join, and the new
player's profile is chosen on the ADD PLAYER screen that opens (with any
controller). With one gamepad, choose the lobby's ADD PLAYER button first:
until then that gamepad shares controller 1 with the keyboard. Two players on one profile get different names from the host. A
player's B in the lobby leaves the game alone, and the last player of the
computer leaves it for all of them. In the game, each player's pause menu
opens on their part of the screen, and its LEAVE GAME is theirs: their part
of the screen stays until the game ends. A game under way shows its own
screen before JOIN GAME: players join there the same way, START or ADD
PLAYER then START, and JOIN GAME brings them all into the game.

In a multiplayer game, the pause menu (escape) has SETTINGS, which opens
the profile's settings while the game goes on, and for the host END GAME.
END GAME ends the game as its time limit does: the players stay, and after
the carnage report the host picks the next map and gametype (PICK GAME).
LEAVE GAME of the host still ends the game for everyone. The buttons go into
the map's own pause menu, before LEAVE GAME, so the buttons of a custom map
stay. The Xbox's pause box is drawn taller to hold them (a redraw,
`port/assets/menus/port_svg/pause`); a custom map's box keeps its size, and
what is below its list moves down. The few pictures of the settings that
come from the main menu's map are not drawn there.

The menus are XML files in `port/assets/menus` (`tools/ce_menus.py` writes
them from the PC version's tags), which the game contains. To change them,
put files in a `menus` folder next to `config.toml`: a file with the same
path replaces one of the game's, and another `.xml` file is added.
`port/assets/menus/README.md` describes the files. If a file has a problem,
the game uses the Xbox's menus and the log names the file, the line and the
problem.

## Settings

The settings are in `config.toml` next to the executable
(`build/linux/config.toml`). At the first start, the game writes the file
with the default values and a comment for each setting. To get the default
values again, delete the file.

The game reads the file at start-up, except `display.upscaled_textures`,
which it reads again at each level load. If a key is not correct, or a value
has the wrong type, the game writes the line to the log and uses the default
value. The Settings menu (Video, Mouse, Audio, Network and Controls Setup)
changes the useful settings, writes them into the file (only their lines
change) and applies them at once, but `audio.enabled`, and `display.menus`
from the next main menu.

Each setting has an environment variable. The environment variable changes
the setting for one start of the game. It has priority over the file.

<!-- BEGIN GENERATED SETTINGS config -->

### display

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `display.fullscreen` | boolean | `true` | desktop | `HALO_FULLSCREEN` | Where display.mode is empty: start borderless over the whole display; false starts in a window. |
| `display.mode` | string | `""` | desktop | `HALO_DISPLAY_MODE` | "fullscreen" takes the display (at display.resolution's mode), "borderless" is a window over the whole desktop, "windowed" a window (display.window_size). |
| `display.resolution` | string | `"native"` | desktop | `HALO_RESOLUTION` | What fullscreen and borderless draw at: "native", the display's own, or "<width>x<height>" ("1920x1080"), 640x480 or more. |
| `display.resolution_scaling` | string | `"native"` | desktop | `HALO_RESOLUTION_SCALING` | "native" draws at the window's resolution (fullscreen, the display's or display.resolution); "original" draws the Xbox's 640x480 and scales it up. |
| `display.window_size` | string | `""` | desktop | `HALO_WINDOW_SIZE` | The window's size, "<width>x<height>" ("1920x1080"), 640x480 or more (it can be resized). |
| `display.window_scale` | integer | `2` | desktop | `HALO_WINDOW_SCALE` | Where display.window_size is empty: the window's size as a multiple of 640x480. |
| `display.screen_width` | integer | `0` | apple | `HALO_SCREEN_WIDTH` | Columns of the 480-line picture: 0 for the display's shape, 640 for the Xbox's 4:3. |
| `display.render_height` | integer | `0` | apple | `HALO_RENDER_HEIGHT` | Internal rendering height in physical pixels: 0 (default) uses native display resolution. |
| `display.anisotropic_filtering` | integer | `16` | apple, desktop | `HALO_ANISOTROPIC_FILTERING` | Anisotropic filtering of the game's textures, 1 (off, as on the Xbox) to 16: sharper floors and terrain at a distance, for a few percent of GPU time. |
| `display.decal_batching` | boolean | `true` | apple, desktop | `HALO_DECAL_BATCHING` | Draw the decals (bullet holes, blood, scorch marks) that share a texture in one draw each rather than one draw a decal: the same picture for less CPU time. |
| `display.shadow_batching` | boolean | `true` | apple, desktop | `HALO_SHADOW_BATCHING` | Draw all of a view's character and vehicle shadows before laying any on the world, and in stereo lay one eye's shadows in the other too: the picture's pass is no longer interrupted for each shadow. |
| `display.dynamic_light_limit` | integer | `0` | apple, desktop | `HALO_DYNAMIC_LIGHT_LIMIT` | Device-profile cap on dynamic lights illuminating level surfaces, 0 to 128. |
| `display.specular_light_limit` | integer | `0` | apple, desktop | `HALO_SPECULAR_LIGHT_LIMIT` | Device-profile cap on specular lights among the level's selected dynamic lights, 0 to 128. |
| `display.shadow_map_size` | integer | `512` | apple, desktop | `HALO_SHADOW_MAP_SIZE` | The size of the textures characters' and vehicles' shadows are drawn into, 128 (as on the Xbox) to 2048: larger is smoother. |
| `display.effect_resolution` | string | `"half"` | apple, desktop | `HALO_EFFECT_RESOLUTION` | The resolution of the 320x240 target the reflections in mirror floors and the active camouflage are drawn into: "xbox" keeps the Xbox's 320x240, "half" is half the screen's resolution each way, and "full" the screen's resolution (sharper; up to a whole second render of the scene's pixels where a mirror shows). |
| `display.model_lod` | string | `"max"` | apple, desktop | `HALO_MODEL_LOD` | Which geometry detail level models draw: "max" (default) always the highest they have, so they never pop between levels or drop out with distance; "auto" picks by on-screen size, as the Xbox did. |
| `display.lod_scale` | number | `1.0` | apple | `HALO_LOD_SCALE` | HEAD mode and the side-by-side view: a multiplier on the pixel size the game picks model detail and particle distance by; 1.0 is the Xbox's (0.5 to 4). |
| `display.compressed_textures` | boolean | `true` | apple | `HALO_COMPRESSED_TEXTURES` | With the Metal renderer, keep the game's DXT textures compressed on the GPU where it supports them (BC1-3): a quarter to an eighth of the memory, and faster loads. |
| `display.render_scale` | number | `1.0` | apple | `HALO_RENDER_SCALE` | Render at this fraction of the display's pixels each way (0.25 to 1.0; 0.67 draws under half the pixels), scaled back up by display.upscaler. display.render_height, when set, caps the result. |
| `display.native_hud` | boolean | `true` | apple | `HALO_NATIVE_HUD` | When the scene renders below the display's size (display.render_height, display.render_scale, or a build that sets a render height), draw the HUD, menus, text and letterbox bars at the display's own resolution over the scaled-up scene. |
| `display.dynamic_resolution` | boolean | `false` | apple | `HALO_DYNAMIC_RESOLUTION` | Metal renderer, in gameplay: lower the 3D view's resolution while the GPU cannot keep up and raise it again when it can, between frames, from display.render_height (the ceiling) down to display.dynamic_resolution_floor. |
| `display.dynamic_resolution_floor` | integer | `540` | apple | `HALO_DYNAMIC_RESOLUTION_FLOOR` | The fewest lines display.dynamic_resolution draws the 3D view at; never fewer than half the ceiling's. |
| `display.anti_aliasing` | string | `"off"` | apple, desktop | `HALO_ANTI_ALIASING` | Smoothing of jagged edges, which the Xbox did not have: "off", or "fxaa" or "smaa", a pass over the 3D view once it is drawn, at the size it is drawn at (the HUD and menus are drawn after it and stay sharp). |
| `display.upscaler` | string | `"bilinear"` | apple | `HALO_UPSCALER` | How a picture rendered below the display's size is scaled up: "bilinear" (default), or "metalfx" (Metal renderer, on a device: MetalFX's spatial scaler, sharper edges and text for a fraction of a millisecond). |
| `display.immersive` | boolean | `false` | apple | `HALO_IMMERSIVE` | Apple Vision Pro, visionOS 26 and later: show the game on a screen standing in the room (an immersive space) instead of in a window. |
| `display.stereo` | string | `"off"` | apple | `HALO_STEREO` | Apple Vision Pro: "off" (flat), "head" (in stereo, the head turns the view), "screen" (in stereo on the theater screen), or "side_by_side" (a debug view of both eyes in the window). |
| `display.frame_repeat` | integer | `0` | apple | `HALO_FRAME_REPEAT` | Apple Vision Pro, display.stereo = "head" or "screen": how many extra display refreshes each frame stays up, 0 to 3: 0 draws a frame every refresh (90 a second), 1 every other (45 a second) for when 90 costs too much. |
| `display.foveation` | boolean | `true` | apple | `HALO_FOVEATION` | Apple Vision Pro, display.stereo = "head": render through the Compositor's foveation, sharper at the middle of each view than at its edges, at display.render_quality. |
| `display.render_quality` | number | `0.6` | apple | `HALO_RENDER_QUALITY` | With display.foveation: the Compositor's render quality, 0 to 1 (the headset's lowest to its highest): how many pixels the middle of each view gets. |
| `display.first_person_body` | boolean | `true` | apple | `HALO_FIRST_PERSON_BODY` | With display.stereo = "head": draw the Master Chief's body below the view (no head or neck, and no third-person arms while you hold a weapon), so looking down shows your chest and feet. |
| `display.first_person_body_offset` | number | `0.08` | apple | `HALO_FIRST_PERSON_BODY_OFFSET` | With display.first_person_body: how far behind your eyes the body stands, in world units (0 to 0.2; 0.08 is about 24 cm), so the feet show past the chest when you look down. |
| `display.first_person_body_seats` | boolean | `true` | apple | `HALO_FIRST_PERSON_BODY_SEATS` | With display.first_person_body: also draw the legs in a vehicle's first-person seat (the Warthog's passenger seat, the Scorpion's rider seats). |
| `display.first_person_body_seat_offset` | number | `0.0` | apple | `HALO_FIRST_PERSON_BODY_SEAT_OFFSET` | With display.first_person_body_seats: how far the seated legs move back along the seat's facing, in world units (0 to 0.2). |
| `display.weapon_offset_down` | number | `0.0` | apple | `HALO_WEAPON_OFFSET_DOWN` | With display.stereo = "head": how far the first-person weapon and arms sit below where the game puts them, in world units (0 to 0.2; 0.01 is about 3 cm), so the headset's taller view doesn't show where the arms end. |
| `display.weapon_offset_back` | number | `0.0` | apple | `HALO_WEAPON_OFFSET_BACK` | With display.stereo = "head": how far the first-person weapon and arms sit closer to your eyes than where the game puts them, in world units (0 to 0.2). |
| `display.eye_height_offset` | number | `0.0` | apple | `HALO_EYE_HEIGHT_OFFSET` | With display.stereo = "head": how far your eyes sit above the game's camera, in world units (-0.1 to 0.1; 0.03 is about 9 cm), for trying a taller eye height. |
| `display.seat_look_fade` | boolean | `true` | apple | `HALO_SEAT_LOOK_FADE` | With display.stereo = "head": in a first-person seat whose aim has a limit (a Warthog's passenger seat, the Pelican's bench, a10's cryo pod), looking past the limit fades the view and the HUD toward black as the head turns on, while the world still moves with the head. |
| `display.seat_look_fade_start` | number | `5.0` | apple | `HALO_SEAT_LOOK_FADE_START` | With display.seat_look_fade: how many degrees past the seat's limit the fade begins (0 to 90). |
| `display.seat_look_fade_black` | number | `45.0` | apple | `HALO_SEAT_LOOK_FADE_BLACK` | With display.seat_look_fade: how many degrees past the seat's limit the view is black (1 to 180, past display.seat_look_fade_start). |
| `display.pod_look_fade_start` | number | `15.0` | apple | `HALO_POD_LOOK_FADE_START` | As display.seat_look_fade_start, for a10's cryo pod alone: how many degrees past the pod's limit the fade begins (0 to 90). |
| `display.pod_look_fade_end` | number | `70.0` | apple | `HALO_POD_LOOK_FADE_END` | In a10's cryo pod: how many degrees past the pod's limit the fade is at its darkest (1 to 180, past display.pod_look_fade_start). |
| `display.pod_look_fade_dim` | number | `0.85` | apple | `HALO_POD_LOOK_FADE_DIM` | In a10's cryo pod: the most the view darkens past the limit, 0.0 to 1.0 (1.0 is black). |
| `display.seat_look_fade_blur` | boolean | `true` | apple | `HALO_SEAT_LOOK_FADE_BLUR` | With display.seat_look_fade: blur the view as it fades, the blur full half way to black; false only dims it. |
| `display.film_depth_share` | number | `0.25` | apple | `HALO_FILM_DEPTH_SHARE` | With display.stereo: how deep a cutscene (a 3D film on the screen) looks: how far behind the screen the far distance sits, as a share of your eye separation (0.05 to 0.9; more is deeper). |
| `display.film_convergence` | number | `1.75` | apple | `HALO_FILM_CONVERGENCE` | With display.stereo: in a cutscene, what lies this many meters ahead of the camera sits on the screen's surface; nearer comes out in front of it (0.3 to 10; less pushes faces back behind the screen). |
| `display.screen_depth_share` | number | `0.3` | apple | `HALO_SCREEN_DEPTH_SHARE` | With display.stereo = "screen": how deep the game looks on the screen (a 3D TV): how far behind the screen the far distance sits, as a share of your eye separation (0.05 to 0.9; more is deeper, less keeps the reticle from doubling on far targets). |
| `display.screen_convergence` | number | `1.0` | apple | `HALO_SCREEN_CONVERGENCE` | With display.stereo = "screen": what lies this many meters ahead of the player sits on the screen's surface; nearer comes out in front of it (0.3 to 10; 0.6 to 0.7 brings only walls you nearly touch in front). |
| `display.screen_framing` | string | `"band"` | apple | `HALO_SCREEN_FRAMING` | With display.stereo = "screen": "band" (default) shows the game's horizontal view across the 16:9 screen, so things look their natural size, and the screen's bottom edge cuts the weapon like a window frame; "wide" shows the flat game's wider view, everything a quarter smaller and the whole weapon in view. |
| `display.stereo_vehicle_screen` | boolean | `false` | apple | `HALO_STEREO_VEHICLE_SCREEN` | With display.stereo = "head": put third-person cameras (a vehicle's chase camera) on the theater screen, as cutscenes are. |
| `display.hud_corner_across` | number | `28.0` | apple | `HALO_HUD_CORNER_ACROSS` | With display.stereo = "head": how far out to the sides the HUD sits, in degrees from the view's center: the outer corners of the weapon's counters (top left), the shields and health (top right), the motion tracker (bottom left) and the prompts and messages (left). |
| `display.hud_corner_up` | number | `20.0` | apple | `HALO_HUD_CORNER_UP` | With display.stereo = "head": how far up the HUD's top corners sit (the weapon's counters, the shields and health), in degrees from the view's center, 0 to 40. |
| `display.hud_tracker_down` | number | `22.0` | apple | `HALO_HUD_TRACKER_DOWN` | With display.stereo = "head": how far down the motion tracker's bottom edge sits, in degrees below the view's center, 0 to 40. |
| `display.hud_messages_up` | number | `12.0` | apple | `HALO_HUD_MESSAGES_UP` | With display.stereo = "head": how far up the top of the help text, prompts and messages sits, in degrees from the view's center; they run down from there. 0 to 40. |
| `display.hud_scale` | number | `1.0` | apple | `HALO_HUD_SCALE` | With display.stereo = "head": the size of the HUD's pieces (not the crosshair) against their natural 0.072 degrees a line, 0.5 to 2. |
| `display.hud_distance` | number | `2.0` | apple | `HALO_HUD_DISTANCE` | With display.stereo = "head": how far ahead the HUD, the crosshair and the menus rest, in meters, 1 to 4. |
| `display.hud_depth` | boolean | `true` | apple | `HALO_HUD_DEPTH` | With display.stereo = "head": bring each HUD piece (the crosshair, the corners) nearer while something under it is nearer than its resting distance, so it always looks in front of what it covers. |
| `display.hud_depth_share` | number | `0.85` | apple | `HALO_HUD_DEPTH_SHARE` | With display.hud_depth: how near a piece comes, as a share of the distance to the nearest thing under it, 0.5 to 1. |
| `display.hud_depth_floor` | number | `0.3` | apple | `HALO_HUD_DEPTH_FLOOR` | With display.hud_depth: the nearest a piece comes, in meters, below display.hud_distance. |
| `display.hud_depth_pull_in` | number | `0.1` | apple | `HALO_HUD_DEPTH_PULL_IN` | With display.hud_depth: the seconds a piece takes to come from its resting distance to the nearest, 0 to 5. |
| `display.hud_depth_relax` | number | `1.0` | apple | `HALO_HUD_DEPTH_RELAX` | With display.hud_depth: the seconds a piece takes to go back from the nearest to its resting distance, 0 to 5. |
| `display.hud_depth_relax_delay` | number | `0.5` | apple | `HALO_HUD_DEPTH_RELAX_DELAY` | With display.hud_depth: the seconds a piece waits, once nothing near is under it, before it goes back, 0 to 5. |
| `display.hud_resolution` | number | `1.0` | apple | `HALO_HUD_RESOLUTION` | HEAD mode and the side-by-side view: the pixels the HUD, the crosshair and the menus are drawn with, as a fraction of the view's along each axis, 0.5 to 1. |
| `display.texture_idle_seconds` | number | `60.0` | apple, desktop | `HALO_TEXTURE_IDLE_SECONDS` | Seconds a texture goes undrawn before the cache drops it, 10 to 600. |
| `display.clutter_distance` | number | `0.0` | apple, desktop | `HALO_CLUTTER_DISTANCE` | Beyond this many world units from the camera, dead bodies, weapons and equipment on the ground and garbage objects are not drawn; 0 (default) draws them all, 0 to 200. |
### input

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `input.turn` | string | `"snap"` | apple | `HALO_TURN` | With display.stereo = "head": how the right stick turns the view: "snap" (default; a flick turns input.snap_angle at once), "smooth" (steadily, at input.smooth_turn_speed), or "off" (it doesn't; turn your body). |
| `input.snap_angle` | number | `30.0` | apple | `HALO_SNAP_ANGLE` | With input.turn = "snap": degrees per snap (5 to 180). |
| `input.smooth_turn_speed` | number | `120.0` | apple | `HALO_SMOOTH_TURN_SPEED` | With input.turn = "smooth": degrees per second with the stick all the way over (10 to 720; 120 is the game's own speed before it speeds up). |
| `input.comfort_vignette` | boolean | `false` | apple | `HALO_COMFORT_VIGNETTE` | With input.turn = "smooth": darken the edges of the view while the stick turns it, for comfort. |
| `input.aim` | string | `"controller"` | apple | `HALO_AIM` | With display.stereo = "head": what aims, "head" (the view's center) or "controller" (default; the aiming hand's Sense controller). |
| `input.aim_loss_grace` | number | `0.5` | apple | `HALO_AIM_LOSS_GRACE` | With input.aim = "controller": seconds of lost hand tracking during which the last hand aim is held, in local and network play, before the aim eases to the head over 0.2 seconds (0 to 5). |
| `input.aim_hand` | string | `"right"` | apple | `HALO_AIM_HAND` | With input.aim = "controller": the aiming hand, "right" (default) or "left" (the buttons mirror). |
| `input.aim_pitch_offset` | number | `0.0` | apple | `HALO_AIM_PITCH_OFFSET` | With input.aim = "controller": degrees the barrel ray and gun tilt down from the aim axis (-10 to 45), for aiming from the hip with the forearm low. |
| `input.aim_assist` | number | `1.0` | apple | `HALO_AIM_ASSIST` | With input.aim = "controller": how much of the game's bullet autoaim a hand-aimed shot keeps, 0.0 to 2.0. |
| `input.aim_steady` | number | `0.0` | apple | `HALO_AIM_STEADY` | With input.aim = "controller": how much a hand's tremor is smoothed while its aim is on a target (inside the autoaim cone, most where the reticle is red) and the weapon is not zoomed, 0.0 to 2.0: 0.0 none (default), 1.0 moderate, 2.0 strong. |
| `input.aim_steady_zoom_2x` | number | `1.0` | apple | `HALO_AIM_STEADY_ZOOM_2X` | As input.aim_steady, for a weapon zoomed up to 2x (default 1.0). |
| `input.aim_steady_zoom_8x` | number | `2.0` | apple | `HALO_AIM_STEADY_ZOOM_8X` | As input.aim_steady, for a weapon zoomed 8x or more (default 2.0); between 2x and 8x the strength is in proportion. |
| `input.controller_rumble` | number | `1.0` | apple | `HALO_CONTROLLER_RUMBLE` | How strongly the game's rumble plays on a gamepad (0 to 2: 1 is the game's own strength, 0 is off). |
| `input.blast_rumble` | boolean | `true` | apple, desktop | `HALO_BLAST_RUMBLE` | A blast that shakes the view but has no rumble of its own in the game's data (a grenade's shock wave beyond 7 m) rumbles the gamepad's heavy motor, at half the blast's strength at your distance for as long as the shake lasts. |
| `input.laser` | string | `"off"` | apple | `HALO_LASER` | HEAD mode with controller aim: a line from the gun's muzzle to the reticle. "off" (default), "close" (only while the reticle's surface is within input.laser_distance, where a hand held below the eyes makes the reticle slide down a surface as you walk up to it) or "on" (always). |
| `input.reticle_ease` | number | `0.05` | apple | `HALO_RETICLE_EASE` | HEAD mode with controller aim: the time, in seconds, over which the reticle moves along the gun's line when that line goes from a far surface onto a near one or back (0.0 to 0.3; default 0.05; 0.0 steps at once, as before). |
| `input.weapon_turn_limit` | number | `25.0` | apple | `HALO_WEAPON_TURN_LIMIT` | input.first_person_arms "limited": the most the turned model turns across the view from its center, in degrees (0 to 90; 0 leaves the turn alone). |
| `input.weapon_roll_limit` | number | `15.0` | apple | `HALO_WEAPON_ROLL_LIMIT` | input.first_person_arms "limited": the most the turned model rolls, in degrees (0 to 90; 0 leaves the roll alone). |
| `input.weapon_smoothing` | number | `0.08` | apple | `HALO_WEAPON_SMOOTHING` | HEAD mode with controller aim: the time, in seconds, over which the first-person gun and arms follow the hand (0.0 to 0.5; default 0.08; 0.0 follows it directly, as before). |
| `input.first_person_arms` | string | `"solved"` | apple | `HALO_FIRST_PERSON_ARMS` | HEAD mode with controller aim: how the first-person gun and arms follow the hand. "turned": the whole model is turned onto the hand's line at the eyes (as before; a hand pointed to the side brings the arms in from the side of the view). "limited": the same, with the turn held within 25 degrees and the roll within 15, the model sinking out of view beyond. "solved" (default): the shoulders stay with the view, the gun turns at its grip onto the hand's line within input.arms_turn_limit, input.arms_up_limit, input.arms_down_limit and input.arms_roll_limit, the hands stay on it and the arms bend to follow. |
| `input.arms_follow` | number | `0.5` | apple | `HALO_ARMS_FOLLOW` | HEAD mode with controller aim, input.first_person_arms "solved": how much of the hand's movement away from its usual place (its average over the last ten seconds) moves the gun, 0.0 (the gun stays where the animation puts it) to 1.0 (it moves with the hand, as far as the arm reaches); default 0.5. |
| `input.arms_turn_limit` | number | `35` | apple | `HALO_ARMS_TURN_LIMIT` | HEAD mode with controller aim, input.first_person_arms "solved": how far across the view, in degrees, the drawn gun turns with the hand (0 to 90; default 35). |
| `input.arms_up_limit` | number | `40` | apple | `HALO_ARMS_UP_LIMIT` | HEAD mode with controller aim, input.first_person_arms "solved": how far above the view's center, in degrees, the drawn gun turns (0 to 89; default 40), eased as input.arms_turn_limit is. |
| `input.arms_down_limit` | number | `50` | apple | `HALO_ARMS_DOWN_LIMIT` | HEAD mode with controller aim, input.first_person_arms "solved": how far below the view's center, in degrees, the drawn gun turns (0 to 89; default 50), eased as input.arms_turn_limit is. |
| `input.arms_roll_limit` | number | `60` | apple | `HALO_ARMS_ROLL_LIMIT` | HEAD mode with controller aim, input.first_person_arms "solved": how far the drawn gun rolls about its barrel with the hand, in degrees either way (0 to 180; default 60), eased as input.arms_turn_limit is; the forearm takes half of the wrist's twist. |
| `input.head_magnetism` | boolean | `false` | apple | `HALO_HEAD_MAGNETISM` | HEAD mode: false (default), the game's aim magnetism never turns or slows the view (with a stick it drags the look after a target while you move, which in the headset turns the world by itself). |
| `input.zoom_takeover` | boolean | `false` | apple | `HALO_ZOOM_TAKEOVER` | HEAD mode with controller aim, zoomed: false (default), the reticle stays where the gun points, and when the gun points outside the zoomed picture there is no reticle; the shots follow the gun either way. |
| `input.aim_origin_raise` | number | `0.0` | apple | `HALO_AIM_ORIGIN_RAISE` | HEAD mode with controller aim: where the aim ray starts, from 0.0 (the hand, default) to 1.0 (the eyes), along the line between them; the ray keeps the hand's direction. |
| `input.laser_distance` | number | `3.0` | apple | `HALO_LASER_DISTANCE` | With input.laser = "close": how near the reticle's surface is, in meters from the eyes (1 to 20), for the line to show. |
### debug

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `debug.test_controller` | string | `""` | apple | `HALO_TEST_CONTROLLER` | A synthetic right Sense controller in place of the real ones, to check controller aim on the Mac and in the simulator: "fixed:<yaw>,<pitch>" holds it still in the room at those degrees from the room's forward (yaw right, pitch up positive; "fixed:30,0"), "sweep" turns it 30 degrees each way over 4 s, and "loss:<seconds>" holds it at fixed:0,0 but untracked for that long every 10 s, then orientation-only as long (up to 5). |
| `debug.controller_record` | string | `""` | apple | `HALO_CONTROLLER_RECORD` | Write every pose sample the host hands the game (both hands, the buttons, the head's yaw) to NAME.poses in the data folder (a headset's Documents folder), flushed once a second. |
| `debug.controller_replay` | string | `""` | apple | `HALO_CONTROLLER_REPLAY` | Answer the game's pose samples from NAME.poses (debug.controller_record) instead of the controllers or debug.test_controller, each kind in its own order, holding the last at the end; works on any host. |
| `debug.controller_aim_axes` | string | `""` | apple | `HALO_CONTROLLER_AIM_AXES` | The Sense controller's aim axes as measured, "<forward>,<up>" (each one of +x, -x, +y, -y, +z and -z, on different lines; "-z,+y"), in place of the built-in ones; empty for the built-in ones. |
| `debug.controller_correction` | string | `"none"` | apple | `HALO_CONTROLLER_CORRECTION` | The Sense controllers' pose correction from ARKit: "none" (default) or "rendered" (aligned with passthrough), to compare the two for lag and swim. |
| `debug.controller_axes` | boolean | `false` | apple | `HALO_CONTROLLER_AXES` | HEAD mode with Sense controllers (visionOS 27): log the right controller's aim and grip axes in the head's frame twice a second ("spatial: right aim x (...) y (...) z (...) grip ..."), to measure which of the aim's axes points along the barrel. |
| `debug.aim_trace` | boolean | `false` | apple | `HALO_AIM_TRACE` | Controller aim: log a line every drawn frame while a hand aims, with the sightline's and the first-person gun's yaw and pitch, the reticle's distance, the action's blend and gate and the magnification. |
| `debug.aim_loss_grace` | number | `-1.0` | apple | `HALO_AIM_LOSS_GRACE_DEBUG` | Tuning input.aim_loss_grace live: from 0.0 up, the seconds of lost hand tracking that hold the last aim, in place of the device profile's. |
| `debug.texture_idle_seconds` | number | `-1.0` | apple, desktop | `HALO_TEXTURE_IDLE_SECONDS_DEBUG` | Tuning display.texture_idle_seconds live: from 10 up, the seconds a texture goes undrawn before the cache drops it, in place of the device profile's. |
| `debug.clutter_distance` | number | `-1.0` | apple, desktop | `HALO_CLUTTER_DISTANCE_DEBUG` | Tuning display.clutter_distance live: from 0 up, the distance in world units in place of the device profile's. |
| `debug.clutter_lowest_lod` | boolean | `false` | apple, desktop | `HALO_CLUTTER_LOWEST_LOD` | Draw the clutter display.clutter_distance names (dead bodies, loose weapons and equipment, garbage) at its lowest model detail level, wherever it is. |
| `debug.offscreen_window_wall_seconds` | number | `60.0` | apple, desktop | `HALO_OFFSCREEN_WINDOW_WALL_SECONDS` | How many seconds of wall time an offscreen benchmark window may run before it is ended where it stands (the report's capped and measured_ticks), from 30 to 1800. |
| `debug.collision_type_filter` | boolean | `true` | apple, desktop | `HALO_COLLISION_TYPE_FILTER` | Collision rays and sphere queries skip an object whose type the query does not test before looking at it, instead of after: the same answers with less work in a crowded cluster. |
| `debug.sphere_query_type_filter` | boolean | `true` | apple, desktop | `HALO_SPHERE_QUERY_TYPE_FILTER` | The nearby-object queries (objects_in_sphere) skip an object whose type the caller did not ask for before collecting it, instead of after: the same answers with less work. |
| `debug.static_light_relink` | boolean | `false` | apple, desktop | `HALO_STATIC_LIGHT_RELINK` | An attached light that has not moved keeps its place in the map's light lists instead of leaving and joining them again every tick. |
| `debug.immediate_merge` | boolean | `true` | apple, desktop | `HALO_IMMEDIATE_MERGE` | Consecutive immediate-mode draws (the HUD's and the text's quads) in the same state are made as one draw, in the same order: the same pictures with fewer draws. |
| `debug.collision_candidates` | boolean | `true` | apple, desktop | `HALO_COLLISION_CANDIDATES` | Each cluster keeps a second list of its vehicles, scenery and other objects that are not bipeds, items or projectiles, and a collision ray that tests none of those three walks only that list: the same answers in the same order without stepping past every body and weapon in a crowded cluster. |
| `debug.aim_steady_level` | number | `-1.0` | apple | `HALO_AIM_STEADY_LEVEL` | Testing input.aim_steady without an enemy: from 0.0 up, how firmly on a target the hand's aim counts as (1.0 a red reticle), in place of the game's own targeting. |
### display

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `display.theater_width` | number | `60.0` | apple | `HALO_THEATER_WIDTH` | With display.immersive: how much of the view the screen spans, in degrees across (the headset's view is about 100; 60 is the front of a cinema). |
| `display.theater_distance` | number | `4.0` | apple | `HALO_THEATER_DISTANCE` | With display.immersive: how far away the screen stands, in meters. |
| `display.theater_environment` | string | `"passthrough"` | apple | `HALO_THEATER_ENVIRONMENT` | With display.immersive: around the screen, "passthrough" (the room) or "dark" (black). |
| `display.renderer` | string | `"metal"` | apple | `HALO_RENDERER` | What draws the game: "metal" (default) Metal, or "gl" OpenGL ES, for debugging only: it lacks the Metal renderer's performance work and measures no GPU time. |
| `display.frame_deadline` | boolean | `true` | apple | `HALO_FRAME_DEADLINE` | Tell the system each frame's deadline (the frame rate's period), so the CPU keeps the clock that finishes frames in time while frames under a frame rate cap wait for the display; without it an A9X's clock fell to its lowest and frames ran late. |
| `display.frame_pacing` | string | `"off"` | apple | `HALO_FRAME_PACING` | How the Metal renderer times frames: "off" (default) shows each frame as soon as it's drawn; "tick" shows every frame for the same whole number of display refreshes, at a rate that is a multiple of the game's 30 ticks a second (90 or 30 at 90 Hz, 60 or 30 at 60 Hz), dropping to the next rate when frames take too long; "refresh" allows any whole number of refreshes (45 at 90 Hz). |
| `display.stereo_flare_counting` | boolean | `false` | apple | `HALO_STEREO_FLARE_COUNTING` | On a Vision Pro in stereo, dim each eye's lens flares by how much of the light that eye can see, as the flat screen does, each eye for itself. |
| `display.late_look` | boolean | `false` | apple, desktop | `HALO_LATE_LOOK` | Read the controller's look stick again just before a frame's camera is built and turn the first-person view by what it has done since the game read it, a few milliseconds on the frames that run a game tick. |
| `display.late_start` | string | `""` | apple | `HALO_LATE_START` | Start each frame's work as late as it can be and still be done for the next refresh of the display, so the controller is read later and what you do reaches the screen sooner (the Metal renderer's frame pacer). |
| `display.frame_source` | string | `""` | apple | `HALO_FRAME_SOURCE` | Where the Metal renderer's frames get their place on the display: "clock" asks for one when a frame is ready and applies display.frames_in_flight and display.frame_spacing itself; "system" takes one from the system for each refresh (iOS 17, tvOS 17, macOS 14 and later, and only with display.frame_rate = "display"; elsewhere "clock" is used). |
| `display.frames_in_flight` | string | `""` | apple | `HALO_FRAMES_IN_FLIGHT` | How many frames the Metal renderer may have on their way to the display at once, "1" to "3": a frame isn't started on its way until fewer than that many are waiting to be shown. "1" gives the least delay between the game and the screen, and costs frames when a frame's CPU and GPU work together take longer than a refresh; "system" leaves the queue to the system (up to 3). |
| `display.frame_spacing` | boolean | `true` | apple | `HALO_FRAME_SPACING` | Keep each frame on screen for its share of the display's time before the next may replace it (the Metal renderer), so frames show evenly spaced: one refresh, or with a display.frame_rate cap under the display's rate, the cap's. |
| `display.frame_rate` | string | `"display"` | apple | `HALO_FRAME_RATE` | The most frames a second: "display" (default) draws one frame for each refresh of the display; a number ("60", "240") holds that rate; "unlimited" draws as fast as it can. |
| `display.async_shaders` | string | `"auto"` | apple | `HALO_ASYNC_SHADERS` | How the Metal renderer compiles shaders: "auto" (default) on worker threads, skipping a draw for the frames until its pipeline is ready, so the game never waits on the compiler (on Apple Vision Pro a long wait ends the app); a run that must draw the same frames every time (fixed timestep, an input replay or benchmark, debug.exit_after, screenshots) compiles each at its first draw instead. "on" or "off" forces either. |
| `display.movies` | string | `"auto"` | apple | `HALO_MOVIES` | The game's intro and credits movies (bink/intro.bik and bink/credits.bik beside maps/, imported from the disc image with the maps): "auto" (default) plays them, except in a run that must draw the same frames every time (fixed timestep, an input replay or benchmark, debug.exit_after, screenshots); "on" plays them there too; "off" skips them. |
| `display.vsync` | boolean | `true` | apple, desktop | `HALO_NO_VSYNC` | Wait for the display between frames; false draws as fast as possible. |
| `display.max_fps` | integer | `0` | desktop | `HALO_MAX_FPS` | With vsync off, the most frames a second: 0 for twice the display's refresh rate, -1 for no limit (which can hang some Intel graphics). |
| `display.interpolation` | boolean | `true` | apple, desktop | `HALO_INTERPOLATION` | Draw a frame for every display refresh, blending between the game's 30 ticks a second; false keeps the original 30 frames a second. |
| `display.direct_camera` | boolean | `true` | apple, desktop | `HALO_DIRECT_CAMERA` | In first person, point the view where the player aims now instead of where the last tick left it: the view turns the frame the stick or the mouse moves, not up to two ticks (66 ms) later. |
| `display.high_res_hud` | boolean | `true` | apple, desktop | `HALO_HIGH_RES_HUD` | Draw the HUD (meters, counters, panels, motion sensor, reticles, waypoints, scopes) from the high-res assets (8x the maps' bitmaps); false draws the maps' own bitmaps. |
| `display.high_res_text` | boolean | `true` | apple, desktop | `HALO_HIGH_RES_TEXT` | Draw the menus' and HUD's text with the fonts in port/assets/fonts (Overpass) at the resolution the game draws at, and the menus' titles from port/assets/titles; false draws the maps' bitmap fonts and titles. |
| `display.upscaled_textures` | boolean | `true` | apple, desktop | `HALO_UPSCALED_TEXTURES` | Draw upscaled textures (made on this device from your copy of the game) where they exist; false draws the maps' own. |
| `display.upscaled_textures_memory` | integer | `0` | apple, desktop | `HALO_UPSCALED_TEXTURES_MEMORY` | The most memory the upscaled textures may take, in MB; 0 (default) picks it from the device's memory. |
| `display.menus` | string | `"pc"` | apple, desktop | `HALO_MENUS` | The menus: "pc" for the PC version's main menu (port/assets/menus, and a menus folder here for your own), "xbox" for the Xbox's. |
| `display.player_names` | string | `"none"` | apple, desktop | `HALO_PLAYER_NAMES` | In multiplayer, whose names are drawn above their heads: "none" (the Xbox's: the name of the player under the reticle shows, and teammates have the game's marker), "all", "allies" or "enemies". |
| `display.player_name_scale` | number | `1.0` | apple, desktop | `HALO_PLAYER_NAME_SCALE` | How large the players' names are drawn: 1.0 three quarters of the size of the HUD's text, 0.25 to 4. |
| `display.scoreboard_team_layout` | string | `"teams"` | apple, desktop | `HALO_SCOREBOARD_TEAM_LAYOUT` | How the scoreboard lists a team game's players: "teams" in a column for each team (red on the left, blue on the right), "score" all in order of score. |
| `display.scoreboard_background` | boolean | `true` | apple, desktop | `HALO_SCOREBOARD_BACKGROUND` | Draw a panel behind the multiplayer scoreboard, for clearer text. |
| `display.scoreboard_background_color` | string | `"16, 16, 16, 150"` | apple, desktop | `HALO_SCOREBOARD_BACKGROUND_COLOR` | The scoreboard panel's colour: "red, green, blue, alpha", each 0 to 255 (alpha 0 is see-through, 255 solid). |
### streamer

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `streamer.enabled` | boolean | `false` | apple, desktop | `HALO_STREAMER_MODE` | Hide offensive words in names and text from other players, maps, and mods as stars. |
| `streamer.slurs` | string | `"mild"` | apple, desktop | `HALO_STREAMER_SLURS` | With streamer.enabled, hide slurs at the selected severity: "off", "severe", "strong", or "mild" (all). |
| `streamer.swearing` | string | `"strong"` | apple, desktop | `HALO_STREAMER_SWEARING` | With streamer.enabled, hide swearing at the selected severity: "off", "severe", "strong", or "mild" (all). |
| `streamer.insults` | string | `"off"` | apple, desktop | `HALO_STREAMER_INSULTS` | With streamer.enabled, hide insults at the selected severity: "off", "severe", "strong", or "mild" (all). |
### audio

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `audio.enabled` | boolean | `true` | apple, desktop | `HALO_NO_AUDIO` | Play sound. |
| `audio.volume` | number | `1.0` | apple, desktop | `HALO_VOLUME` | The volume of everything, 0.0 to 1.0. |
| `audio.music_volume` | number | `1.0` | apple, desktop | `HALO_MUSIC_VOLUME` | The music's volume, 0.0 to 1.0 (of audio.volume). |
| `audio.effects_volume` | number | `1.0` | apple, desktop | `HALO_EFFECTS_VOLUME` | The volume of every other sound (effects and speech), 0.0 to 1.0 (of audio.volume). |
| `audio.loose_sounds` | boolean | `false` | apple, desktop | `HALO_LOOSE_SOUNDS` | For those making sounds: play each of a map's sounds that has a Halo PC sound tag file of its name under the data root's tags folder (tags/sound/.../name.sound) from that file. |
### input

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `input.stick_dead_zone` | number | `0.27` | apple, desktop | `HALO_STICK_DEAD_ZONE` | How far a controller stick moves before it counts, from 0 to 1. |
| `input.mouse_sensitivity` | number | `1.0` | desktop | `HALO_MOUSE_SENSITIVITY` | How far the view turns for the mouse's movement. |
| `input.invert_mouse` | boolean | `false` | desktop | `HALO_MOUSE_INVERT` | Moving the mouse forward looks down. |
### network

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `network.block_mouse_aim_assist` | boolean | `false` | apple, desktop | `HALO_NET_BLOCK_MOUSE_AIM_ASSIST` | Host: disable optional mouse magnetism for mouse/keyboard players. |
### input

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `input.mouse_aim_assist` | boolean | `false` | desktop | `HALO_MOUSE_AIM_ASSIST` | Magnetism while aiming with the mouse, as with a controller: the view slowed and dragged along by a target. |
| `input.mouse_vertical_sensitivity` | number | `0.0` | desktop | `HALO_MOUSE_VERTICAL_SENSITIVITY` | How far the view turns up and down for the mouse's movement; 0 for the same as input.mouse_sensitivity. |
### controls

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `controls.move_forward` | string | `"W"` | apple, desktop | `HALO_KEY_MOVE_FORWARD` | The keyboard and mouse's controls, which Settings > Controls Setup changes: up to two keys or buttons each, separated by a comma. |
| `controls.move_backward` | string | `"S"` | apple, desktop | `HALO_KEY_MOVE_BACKWARD` | Moving backward. |
| `controls.strafe_left` | string | `"A"` | apple, desktop | `HALO_KEY_STRAFE_LEFT` | Moving left. |
| `controls.strafe_right` | string | `"D"` | apple, desktop | `HALO_KEY_STRAFE_RIGHT` | Moving right. |
| `controls.jump` | string | `"Space"` | apple, desktop | `HALO_KEY_JUMP` | Jumping (and skipping cutscenes). |
| `controls.crouch` | string | `"Left Ctrl, C"` | apple, desktop | `HALO_KEY_CROUCH` | Crouching. |
| `controls.fire` | string | `"Mouse Left"` | apple, desktop | `HALO_KEY_FIRE` | Firing. |
| `controls.throw_grenade` | string | `"Mouse Right, G"` | apple, desktop | `HALO_KEY_THROW_GRENADE` | Throwing a grenade. |
| `controls.melee` | string | `"F, Mouse 4"` | apple, desktop | `HALO_KEY_MELEE` | Melee attack. |
| `controls.reload` | string | `"R"` | apple, desktop | `HALO_KEY_RELOAD` | Reloading. |
| `controls.zoom` | string | `"Z, Mouse Middle"` | apple, desktop | `HALO_KEY_ZOOM` | Zooming the scope. |
| `controls.switch_weapon` | string | `"Wheel, 1"` | apple, desktop | `HALO_KEY_SWITCH_WEAPON` | Switching weapons. |
| `controls.switch_grenade` | string | `"X"` | apple, desktop | `HALO_KEY_SWITCH_GRENADE` | Switching grenades. |
| `controls.action` | string | `"E"` | apple, desktop | `HALO_KEY_ACTION` | The action: picking up (held: swapping weapons), entering and leaving vehicles, pressing switches; never reloading (the controller's X does when there is nothing to act on). |
| `controls.flashlight` | string | `"Q"` | apple, desktop | `HALO_KEY_FLASHLIGHT` | The flashlight. |
| `controls.scoreboard` | string | `"Tab"` | apple, desktop | `HALO_KEY_SCOREBOARD` | Showing the scores (the controller's Back). |
| `controls.pause` | string | `"Escape"` | apple, desktop | `HALO_KEY_PAUSE` | The pause menu (the controller's Start). |
### game

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `game.console_log` | string | `"important"` | apple, desktop | `HALO_CONSOLE_LOG` | What the game's console shows on screen of what it logs: "important" (bans, players dropped for cheating, what refuses a command, and the asserts that stop the game), "all" (every line, the game's own chatter too), or "none" (the asserts that stop the game only). |
| `game.language` | string | `""` | apple, desktop | `HALO_LANGUAGE` | The language the game asks the Xbox for: "ja", "de", "fr", "es" or "it"; empty for English. |
| `game.loading_screen` | string | `"auto"` | apple, desktop | `HALO_LOADING_SCREEN` | The loading screen over a campaign level's load: the last frame blurring away, the ring's picture lit from left to right as the load goes, and its tone, until the level is read and its shaders are compiled. "auto" (default) shows it except in a run that must draw the same frames every time (fixed timestep, an input replay or benchmark, debug.exit_after, screenshots) and except in stereo; "on" or "off" forces either. |
| `game.loading_screen_seconds` | number | `1.5` | apple, desktop | `HALO_LOADING_SCREEN_SECONDS` | The least time the loading screen stays up, in seconds (0 to 30), so a level that is ready at once doesn't flash it. |
| `game.enhanced_animations` | boolean | `true` | apple, desktop | `HALO_ENHANCED_ANIMATIONS` | The player bipeds' grenade throws keep their legs moving (crouched, in the air and in a vehicle's seat too), riders' hands leave the grips to throw and reload, and a player turns with the aim while throwing; false: the original animations, which freeze the legs and stand a rider up. |
| `game.custom_edition` | boolean | `true` | apple, desktop | `HALO_CUSTOM_EDITION` | Load and run Halo Custom Edition maps (not those that need OpenSauce): put them and Custom Edition's bitmaps.map, sounds.map and loc.map in the custom_maps folder beside the maps folder; the map lists show them as CUSTOM SINGLEPLAYER and CUSTOM MULTIPLAYER. |
### display

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `display.first_person_legs` | boolean | `false` | apple, desktop | `HALO_FIRST_PERSON_LEGS` | Your own legs in first person, seen looking down: your body as others see it, drawn from the waist down, moving as it moves. |
| `display.first_person_legs_seat_shift` | number | `1.0` | apple, desktop | `HALO_FIRST_PERSON_LEGS_SEAT_SHIFT` | With display.first_person_legs, in a vehicle seat seen through your own eyes (a Warthog's passenger seat): how far your seated body is drawn moved ahead so the waist sits under the view, 0.0 (where the game seats it, well behind the eye) to 1.0 (toward the eye, default), never more than about 0.37 m so the knees stay short of the dash. |
### paths

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `paths.data` | string | `""` | desktop | `HALO_DATA_ROOT` | The folder holding the game data's maps folder; empty looks in the working directory and its assets folder. |
| `paths.saves` | string | `""` | desktop | `HALO_SAVE_ROOT` | Where saved games and profiles go; empty for the usual place (~/.local/share/halo-linux, or %APPDATA%\halo on Windows). |
| `paths.custom_edition` | string | `""` | desktop | `HALO_CUSTOM_EDITION_ROOT` | A Halo Custom Edition install whose maps folder is looked in after the custom_maps folder for Custom Edition maps and their bitmaps.map, sounds.map and loc.map (game.custom_edition); empty for none. |
### network

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `network.address` | string | `""` | apple, desktop | `HALO_NET_ADDRESS` | This machine's IPv4 address for system link, for a machine on several networks; empty chooses one. |
| `network.broadcast` | string | `""` | apple, desktop | `HALO_NET_BROADCAST` | Comma-separated IPv4 addresses system link sends its announcements to instead of the local network's broadcast address (for VPNs); empty for the local network. |
| `network.lan_broadcast` | string | `"auto"` | apple | `HALO_NET_LAN_BROADCAST` | System link by broadcast, which finds games of every system: "auto" where this device may (the Mac; an iPhone, iPad or Vision Pro only with Apple's multicast entitlement), "on" or "off". |
| `network.online` | boolean | `false`; desktop `true` | apple, desktop | `HALO_NET_ONLINE` | Internet play: hosting makes an invite link (logged, and put on the clipboard) that lets whoever has it join over the internet; opening a link (or copying one before switching to the game) joins. |
| `network.online_ask` | boolean | `false` | apple | `HALO_NET_ONLINE_ASK` | While network.online is false: ask whether to turn Internet Play on the first time Create Game > Internet, Join Game > Server Browser or Direct Link is opened, saying which servers it reaches and what they see. |
| `network.join_from_clipboard` | boolean | `false`; desktop `true` | apple, desktop | `HALO_NET_JOIN_FROM_CLIPBOARD` | Join the game of an invite link found on the clipboard when the game comes to the front. |
| `network.tunnel_port` | integer | `0` | apple, desktop | `HALO_NET_TUNNEL_PORT` | The UDP port internet play uses; 0 picks one. |
| `network.allow_upnp` | boolean | `true` | apple, desktop | `HALO_NET_ALLOW_UPNP` | Let internet play ask the router (UPnP; on Apple devices NAT-PMP or PCP) to forward its port, for networks whose NAT stops connections: when a player joins this machine's game, and when joining a game takes too long. |
| `network.background_grace` | integer | `60` | apple, desktop | `HALO_NET_BACKGROUND_GRACE` | Seconds the other players of an internet game wait for this machine when its app goes to the background (an Apple device suspends it, its game held) before they drop it. |
| `network.public_lobby` | boolean | `true` | apple, desktop | `HALO_NET_PUBLIC_LOBBY` | The server browser: public games are listed through the signalling brokers, and Join Game > Server Browser shows them. |
| `network.delta_browse` | boolean | `true` | apple, desktop | `HALO_NET_DELTA_BROWSE` | The server browser also lists the games of Delta's public list (network.delta_url), run by others, with the network version of each; games of a version this build can't join are shown and not joined. |
| `network.delta_announce` | boolean | `false` | apple, desktop | `HALO_NET_DELTA_ANNOUNCE` | List the public game this machine hosts for the internet (Create Game > Internet, PUBLIC, no password) in Delta's public list (network.delta_url), so that players using it find it. |
| `network.delta_url` | string | `"https://halo.milenko.org"` | apple, desktop | `HALO_NET_DELTA_URL` | The address of Delta's public game list, whose /v1/games the server browser reads (network.delta_browse) and whose /v1/announce a hosted game is listed at (network.delta_announce). |
| `network.host_public` | boolean | `false`; desktop `true` | apple, desktop | `HALO_NET_HOST_PUBLIC` | Whether a new game of Create Game > Internet starts as PUBLIC (listed in everyone's server browser: anyone can see and join it) or, false, PRIVATE (only players with its invite link can join). |
| `network.player_capacity` | integer | `0` | apple, desktop | `HALO_NET_PLAYER_CAPACITY` | The most players this device holds at 30 frames a second at the lowest settings, 2 to 128; 0 (default) names no limit. |
| `network.map_share` | boolean | `true` | apple, desktop | `HALO_NET_MAP_SHARE` | Hosting: send a player who joins without the game's Custom Edition map the map (from custom_maps), and the resource maps it needs that the player lacks (bitmaps.map, sounds.map, loc.map). |
| `network.map_share_upload` | integer | `1024` | apple, desktop | `HALO_NET_MAP_SHARE_UPLOAD` | Hosting: the most kilobytes a second the host sends of maps (16 to 65536), so that the game's own traffic keeps the rest of the upload. |
| `network.map_seed` | boolean | `false`; desktop `true` | apple, desktop | `HALO_NET_MAP_SEED` | Playing in another's game: help send its map to players who join without it, as a torrent's peers do (the pieces this machine has, at most network.map_seed_upload). |
| `network.map_seed_upload` | integer | `256` | apple, desktop | `HALO_NET_MAP_SEED_UPLOAD` | Playing in another's game: the most kilobytes a second this machine sends of maps to other players (16 to 65536). |
| `network.custom_maps` | boolean | `false` | apple, desktop | `HALO_NET_CUSTOM_MAPS` | Experimental: the server browser also shows games on Custom Edition maps, those of a map in custom_maps or, with network.map_download on, any. |
| `network.map_download` | boolean | `false`; desktop `true` | apple, desktop | `HALO_NET_MAP_DOWNLOAD` | Joining a game on a Custom Edition map this machine does not have: download it (and the resource maps it needs, up to about 2.3 GB) from the game's players into custom_maps, if its host shares it (network.map_share); false leaves the game, saying which map is missing and that downloads are off. |
| `network.coop_public` | boolean | `false` | apple, desktop | `HALO_NET_COOP_PUBLIC` | Whether an online co-op game (Create Game > Internet, a SINGLEPLAYER map) starts as PUBLIC or, false, PRIVATE: Server Setup's LISTING in co-op, which writes its choice here. |
| `network.coop_friendly_fire` | string | `"on"` | apple, desktop | `HALO_NET_COOP_FRIENDLY_FIRE` | Whether the players of an online co-op game hurt each other: "off", "on", "shields_only" or "explosives_only" (Server Setup's FRIENDLY FIRE in co-op, which writes its choice here). |
| `network.coop_own_friendly_fire` | boolean | `true` | apple, desktop | `HALO_NET_COOP_OWN_FRIENDLY_FIRE` | Whether this machine's players hurt, and are hurt by, the other players of an online co-op game: two players hurt each other only if both have it true. |
| `network.coop_ally_friendly_fire` | boolean | `true` | apple, desktop | `HALO_NET_COOP_ALLY_FRIENDLY_FIRE` | Whether players can hurt friendly non-player units and vehicles in online co-op. |
| `network.coop_player_collisions` | boolean | `true` | apple, desktop | `HALO_NET_COOP_PLAYER_COLLISIONS` | Whether the players of an online co-op game bump into each other; false, they walk through each other (the AI's characters they still bump into). |
| `network.coop_enemies_mode` | string | `"per_player"` | apple, desktop | `HALO_NET_COOP_ENEMIES_MODE` | Online co-op's extra enemies: "none", "per_player" (each squad of enemies grows by coop_enemies for each player past the first) or "multiplier" (each is coop_enemies_multiplier times as large, for any number of players). |
| `network.coop_enemies` | integer | `50` | apple, desktop | `HALO_NET_COOP_ENEMIES` | Online co-op's extra enemies per player, a percentage: for each player past the first, each squad of enemies a level places gets this much of itself more (100: as many again; 25 to 200). |
| `network.coop_enemies_multiplier` | integer | `2` | apple, desktop | `HALO_NET_COOP_ENEMIES_MULTIPLIER` | Online co-op's static multiplier of its enemies: each squad of enemies a level places is this many times as large (2 to 32). |
| `network.brokers_file` | string | `"brokers.txt"` | apple, desktop | `HALO_NET_BROKERS_FILE` | The file of the public MQTT brokers through which the machines of an invite find each other (its messages are encrypted), beside this file unless a full path: one host:port on each line, up to 4. |
| `network.stun_servers` | string | `"stun.l.google.com:19302,stun.cloudflare.com:3478"` | apple, desktop | `HALO_NET_STUN` | Public STUN servers that tell this machine its internet address; comma-separated host:port. |
### discord

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `discord.application_id` | string | `"1553978809840050229"` | desktop | `HALO_DISCORD_APPLICATION` | The Discord application internet play invites go through while the Discord desktop client runs; empty for none. |
### update

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `update.auto` | boolean | `true` | desktop | `HALO_UPDATE_AUTO` | Look for a new version when the game starts, and offer to update to it; false never looks (the game's "Do not ask again" writes false here). |
### debug

| Setting | Type and accepted values | Default (Apple) | Platforms | Environment | Description |
| --- | --- | --- | --- | --- | --- |
| `debug.external_mode` | string | `""` | apple | `HALO_EXTERNAL_MODE` | Force an external display mode for testing: preferred, WIDTHxHEIGHT, or a comma-separated list. |
| `debug.output_height` | integer | `0` | apple | `HALO_OUTPUT_HEIGHT` | Cap the built-in display's drawable at this many lines for measuring (0, the default: the device's own size, or an Apple TV profile's display.output_height); the system scales it to the panel. iPhone, iPad and Apple TV with Metal, read at launch. |
| `debug.render_scale_dpad` | boolean | `false` | apple | `HALO_RENDER_SCALE_DPAD` | While Back is held, the D-pad's left and right step the render scale (display.render_scale) down and up by 0.05 as the game runs, shown on screen for two seconds; the game doesn't see the D-pad meanwhile. |
| `debug.knob_cycle` | boolean | `false` | apple | `HALO_KNOB_CYCLE` | While Back is held, the D-pad's up and down choose a quality setting (render height, shadow map size, anisotropic filtering, model detail, anti-aliasing, effect resolution, mirror resolution) and left and right step its value as the game runs, from the next frame, shown on screen for two seconds and logged as "knob: <setting>=<value>". |
| `debug.sense_tuning_chord` | boolean | `false` | apple | `HALO_SENSE_TUNING_CHORD` | While a hand aims and Back (Create) is held, controller buttons change aim and arms tuning as the game runs (laser mode, aim assist, steadying, pitch offset, origin raise, reticle easing, arms, gun smoothing), and the game sees neither stick meanwhile. |
| `debug.halt_screen` | boolean | `false` | apple | `HALO_HALT_SCREEN` | On a fatal game error (a failed assertion or a halt), stop on the game's halt screen, its recent messages on a blue background, instead of crashing. |
| `debug.crash_test` | string | `""` | apple | `HALO_CRASH_TEST` | Fail on purpose 150 frames after the game starts, to check crash reports: "halt" (a failed assertion), "fault" (a memory fault in game code), "trap" (a trap instruction) or "abort" (the game runtime's fatal error); "map" fails the first level load instead, which shows a message; empty, never. |
| `debug.upscaled_textures_toggle` | boolean | `false` | apple | `HALO_UPSCALED_TEXTURES_TOGGLE` | While Back is held, the D-pad's up switches all of the level's textures between upscaled (and debug.texture_override_directory's) and original, shown on screen for two seconds; each level starts upscaled, and display.upscaled_textures is not changed. |
| `debug.upscaled_textures_toggle_every` | integer | `0` | apple | `HALO_UPSCALED_TEXTURES_TOGGLE_EVERY` | With upscaled_textures_toggle: press its toggle every this many frames, for a test run with no controller; 0 never. |
| `debug.input_record` | string | `""` | apple, desktop | `HALO_INPUT_RECORD` | Record controller 1 while a map plays, to <name>.input in the data folder (port/linux/src/input_replay.c); empty records nothing. |
| `debug.input_replay` | string | `""` | apple, desktop | `HALO_INPUT_REPLAY` | Replay a recording made with debug.input_record in place of controller 1, at the same moments of the game time. |
| `debug.input_replay_route` | integer | `0` | apple, desktop | `HALO_INPUT_REPLAY_ROUTE` | With debug.input_replay or debug.input_record: log player 1's position every this many ticks, with the segment, the tick and whether the tick took a recorded action (tools/replay_check.py reads it); 0 never. |
| `debug.enemy_multiplier` | number | `1.0` | apple, desktop | `HALO_ENEMY_MULTIPLIER` | A single-player campaign's enemies, each squad this many times as large (1.0 off; 1.5 gives every second enemy a companion; up to 8). |
| `debug.scenario_help` | boolean | `true` | apple, desktop | `HALO_SCENARIO_HELP` | Show the in-game help dialogs a level's scripts raise (taking a Scorpion's or a Banshee's seat), which stop the game until a button is pressed. |
| `debug.profile_controls` | boolean | `false` | apple, desktop | `HALO_PROFILE_CONTROLS` | A level started without the menu (the console's map_name) takes the controls of the last profile used: look sensitivity, inverted look, button and stick layout. |
| `debug.benchmark` | boolean | `false` | apple, desktop | `HALO_BENCHMARK` | With debug.input_replay: time every frame of the replay, write benchmark-<name>-<time>.txt and quit when the replay ends. |
| `debug.benchmark_lip_sync` | boolean | `false` | apple, desktop | `HALO_BENCHMARK_LIP_SYNC` | Use simulated, tick-driven mouths while recording or replaying benchmark takes. |
| `debug.benchmark_suite` | string | `""` | apple | `HALO_BENCHMARK_SUITE` | "quick" or "full" runs the device benchmark and quits: the suite's scenes, several times over at different settings, with a report in the data folder's benchmark folder (port/linux/src/benchmark_suite.c); empty for none. |
| `debug.benchmark_offscreen` | boolean | `false` | apple | `HALO_BENCHMARK_OFFSCREEN` | With debug.benchmark_suite set at launch: draw each frame of the suite into a texture that is never shown, with no wait for the display, one 1/30 s tick a frame (debug.fixed_timestep), as fast as the device goes. |
| `debug.benchmark_offscreen_mode` | string | `"serialized"` | apple | `HALO_BENCHMARK_OFFSCREEN_MODE` | With debug.benchmark_offscreen, how the offscreen runs' frames go: "serialized" (default) waits for each lens flare's visibility answer, so the CPU and the GPU take turns and the CPU time counts the GPU's; "pipelined" takes every answer from three frames before, the same every run, so frames overlap as they do on screen: frames a second near 1/max(CPU, GPU), for comparing settings' costs, not for frame rate tiers. |
| `debug.benchmark_virtual_display` | boolean | `false` | apple | `HALO_BENCHMARK_VIRTUAL_DISPLAY` | With debug.benchmark_offscreen: every run of the suite is drawn offscreen in real time against a virtual display: refreshes at the display's rate (or display.frame_rate's cap), three virtual drawables, each held on the virtual screen until the next frame replaces it, and the same frame pacer and frame timing as a shown run. |
| `debug.network_test` | string | `""` | apple, desktop | `HALO_NETWORK_TEST` | Automated system link sessions for testing (port/linux/game/network_test.c): "host:<map>" hosts a game on that map, "join" joins the first game found; empty for none. |
| `debug.multiplayer_benchmark_probe` | boolean | `false` | apple, desktop | `HALO_MULTIPLAYER_BENCHMARK_PROBE` | In a four-local-player automated Chill Out Slayer match, draw only player 1's view and drive the controllers with distinct game-tick scripts. |
| `debug.network_test_start` | number | `15.0` | apple, desktop | `HALO_NETWORK_TEST_START` | Seconds after hosting that an automated test game starts. |
| `debug.network_test_players` | integer | `1` | apple, desktop | `HALO_NETWORK_TEST_PLAYERS` | The players an automated test host has on its own computer (split screen): controller 1 and the next ones, which debug.test_input plays when no gamepad is connected to them. |
| `debug.network_test_kill` | number | `0.0` | apple, desktop | `HALO_NETWORK_TEST_KILL` | Every this many seconds an automated test host kills its last player; 0 never. |
| `debug.network_test_score` | integer | `0` | apple, desktop | `HALO_NETWORK_TEST_SCORE` | The score an automated test host's game type plays to (a short game, to test the next); 0 the game type's own. |
| `debug.network_test_shoot` | number | `0.0` | apple, desktop | `HALO_NETWORK_TEST_SHOOT` | Every this many seconds each automated test player hits the next with their weapon, within its reach (the host brings far players near the first a second before); 0 never. |
| `debug.network_test_vehicle` | number | `0.0` | apple, desktop | `HALO_NETWORK_TEST_VEHICLE` | This many seconds into an automated test game the host seats its last player as a vehicle's driver (and out 15 seconds on); 0 never. |
| `debug.network_test_pickup` | number | `0.0` | apple, desktop | `HALO_NETWORK_TEST_PICKUP` | This many seconds into an automated test game the host stands its last player on a weapon, which a joining player then picks up; 0 never. |
| `debug.network_test_pickup_weapon` | string | `""` | apple, desktop | `HALO_NETWORK_TEST_PICKUP_WEAPON` | The weapon network_test_pickup stands the player on: the first whose tag name has this in it ("sniper", say); empty any. |
| `debug.network_test_public` | boolean | `false` | apple, desktop | `HALO_NETWORK_TEST_PUBLIC` | The network test's host lists its game in the server browser; browse joins it. |
| `debug.telnet_console` | boolean | `false` | apple, desktop | `HALO_TELNET_CONSOLE` | Listen on 127.0.0.1 (port telnet_console_port) for a script console that runs what it is sent as the game's console does, with no password; false none. |
| `debug.telnet_console_port` | integer | `2323` | apple, desktop | `HALO_TELNET_CONSOLE_PORT` | The port of the script console (telnet_console); the Xbox's was 23, which only the administrator can listen on. |
| `debug.network_latency` | number | `0.0` | apple, desktop | `HALO_NETWORK_LATENCY` | Milliseconds everything received is held back (a round trip between two machines of twice it), to test the netcode as over the internet; 0 none. |
| `debug.network_loss` | number | `0.0` | apple, desktop | `HALO_NETWORK_LOSS` | Percent of datagrams received that are dropped, for the same; 0 none. |
| `debug.network_corrupt` | number | `0.0` | apple, desktop | `HALO_NETWORK_CORRUPT` | Percent of the datagrams received that are damaged at random, to test that nothing a machine sends can crash the game; 0 none. |
| `debug.network_corrupt_stream` | number | `0.0` | apple, desktop | `HALO_NETWORK_CORRUPT_STREAM` | Percent of the reads of streams that are damaged at random, for the same (a damaged stream is closed, so a little goes a long way); 0 none. |
| `debug.network_corrupt_after` | number | `0.0` | apple, desktop | `HALO_NETWORK_CORRUPT_AFTER` | Seconds after the start before anything is damaged, so that a game can be set up and started first (a host's messages to its own client are damaged too). |
| `debug.test_input` | string | `""` | apple, desktop | `HALO_TEST_INPUT` | "bot:<seed>" plays controller 1 with a scripted pattern (automated network tests); "look:<seed>" stands still, only turning and looking up and down; "walklook:<seed>" looks down, then walks; "zoom:<start>,<swaps>,<clicks>[,<lights>[,<unclicks>]]" stands still and from poll <start> swaps weapons <swaps> times, zooms <clicks> times, presses the flashlight <lights> times (night vision), then clicks the zoom <unclicks> times more (out of it); "aachord:<start>,<period>" presses the live anti-aliasing chord (Back and R1) from poll <start> and every <period> polls after; empty for none. |
| `debug.side_by_side_screen` | boolean | `false` | apple | `HALO_SIDE_BY_SIDE_SCREEN` | With display.stereo = "side_by_side": show the eyes SCREEN mode's gameplay would have (a 3D TV on the default screen, 64 mm eyes) in place of head-tracked ones. |
| `debug.side_by_side_tangents` | string | `""` | apple | `HALO_SIDE_BY_SIDE_TANGENTS` | With display.stereo = "side_by_side": the left eye's frustum as "left,right,up,down" tangents (the right eye's mirrored left to right), for asymmetric eyes like the headset's; empty for 0.8 on every side. |
| `debug.screen_lean` | number | `0.0` | apple | `HALO_SCREEN_LEAN` | With debug.side_by_side_screen: a fixed lean of the head, in meters to the right (up to 0.25), as if you leaned in front of the screen. |
| `debug.cutscene_immersive` | boolean | `false` | apple | `HALO_CUTSCENE_IMMERSIVE` | A spike: in HEAD mode (and the side-by-side view), a cutscene's third-person film frame renders immersive instead of on the 3D-film screen: the eyes are the cutscene camera turned by the head, the director's 16:9 frame is anchored in the room where the head pointed as the cutscene began, and outside it the picture is blurred, darkened and desaturated. |
| `debug.cutscene_immersive_min_fov` | number | `40.0` | apple | `HALO_CUTSCENE_IMMERSIVE_MIN_FOV` | With debug.cutscene_immersive: the narrowest shot that renders immersive, as the camera's horizontal field of view in degrees (70 for the game's default lens; the director's frame spans that across its width). |
| `debug.cutscene_outside_dim` | number | `0.6` | apple | `HALO_CUTSCENE_OUTSIDE_DIM` | With debug.cutscene_immersive: how much the blurred picture outside the director's frame is darkened and desaturated, 0 (not at all) to 1 (black). |
| `debug.side_by_side_head_yaw` | string | `""` | apple | `HALO_SIDE_BY_SIDE_HEAD_YAW` | With display.stereo = "side_by_side" and debug.cutscene_immersive: a head that turns side to side in the immersive cutscene, as "amplitude,period" (degrees, seconds), so the Mac shows the room-anchored frame; empty for none. |
| `debug.head_yaw_log` | boolean | `false` | apple | `HALO_HEAD_YAW_LOG` | HEAD mode: log a line each frame while the head turns ("stereo: head yaw"): the head's yaw from the frame's predicted pose, the yaw the eye cameras used, and how far the world moved against the head since the last line, to see whether the world stays put while the head turns. |
| `debug.head_sweep` | string | `""` | apple | `HALO_HEAD_SWEEP` | HEAD mode: turn the head by itself, left and right in a sine, as "amplitude_degrees,period_seconds" ("30,4"), in place of the yaw ARKit gives, to check the view's tracking in the simulator; empty for none. |
| `debug.foveation_eye_passes` | boolean | `true` | apple | `HALO_FOVEATION_EYE_PASSES` | With display.foveation: false renders the game's eyes unfoveated at the foveated view's full size, and only the last step that fills each view goes through the Compositor's rate map (composite-only foveation, the reference the full path is compared against). |
| `debug.anti_aliasing_folded` | boolean | `false` | apple, desktop | `HALO_ANTI_ALIASING_FOLDED` | With display.anti_aliasing = "fxaa" and the HUD at the display's size: run FXAA inside the stretch to the display's size (Metal), at each of the display's pixels, rather than as a pass over the scene before it. |
| `debug.rate_map_test` | boolean | `false` | apple | `HALO_RATE_MAP_TEST` | Metal renderer on the Mac: give the screen-sized targets a synthetic, lopsided rate map (full rate in the middle third, half outside it, denser at the top than the bottom), resolved at present, to check foveated rendering off the headset. |
| `debug.update_answer` | string | `""` | desktop | `HALO_UPDATE_ANSWER` | The answer to the new version question, for automated tests: "yes", "no" or "never" (do not ask again, confirmed); empty asks. |
| `debug.link_consent_answer` | string | `""` | apple | `HALO_LINK_CONSENT_ANSWER` | The answer to "Join this game over the internet?" and to "Turn on Internet Play?" for automated tests: "join" (Join, Turn On) or "cancel"; empty asks. |
| `debug.exit_after` | number | `0.0` | apple, desktop | `HALO_EXIT_AFTER` | Quit this many seconds after the window opens (of game time under debug.fixed_timestep); 0 never. |
| `debug.init_after` | number | `0.0` | apple, desktop | `HALO_INIT_AFTER` | Run init.txt's commands this many seconds after start-up instead of at once, so a test can wait at the main menu and then start a level ('map_name a10') in the same launch; 0 at once. |
| `debug.benchmark_window_marker` | boolean | `false` | apple, desktop | `HALO_BENCHMARK_WINDOW_MARKER` | Show the measured benchmark window and a five-second countdown onscreen. |
| `debug.benchmark_review_take` | boolean | `false` | apple, desktop | `HALO_BENCHMARK_REVIEW_TAKE` | In an onscreen benchmark replay, play the rest of the recorded input after the measured window closes. |
| `debug.fixed_timestep` | boolean | `false` | apple, desktop | `HALO_FIXED_TIMESTEP` | Advance the game's clock 1/30 s per frame, however long the frame took, so two runs show the same frames; debug.exit_after then counts seconds of game time, 30 frames each (600 = 18,000 frames). |
| `debug.hidden_window` | boolean | `false` | desktop | `HALO_HIDDEN_WINDOW` | Keep the window hidden (and never fullscreen). |
| `debug.null_renderer` | boolean | `false` | apple, desktop | `HALO_NULL_RENDERER` | Run without a window, drawing nothing. |
| `debug.fixed_timestep_paced` | boolean | `false` | apple, desktop | `HALO_FIXED_TIMESTEP_PACED` | With fixed_timestep, hold each frame for at least 1/30 s, so the game plays at its own speed instead of as fast as frames are presented. |
| `debug.frame_trace` | boolean | `false` | apple, desktop | `HALO_FRAME_TRACE` | Log every frame's time, the blend between ticks and the camera's yaw, to see how smoothly the view turns. |
| `debug.present_times` | boolean | `false` | apple | `HALO_PRESENT_TIMES` | Log when each frame reached the display and how long after its work began (the frame pacer, with the Metal renderer; not a Vision Pro or a simulator), to see how evenly and how late frames are shown. |
| `debug.frame_cpu_probe` | boolean | `false` | apple | `HALO_FRAME_CPU_PROBE` | Benchmark reports also carry, for every frame, the drawing thread's own CPU time, the whole app's, and how long a fixed loop took (which falls as the core's clock rises), to tell a slow clock or another thread's work from waiting. |
| `debug.visibility_any_sample` | boolean | `false` | apple | `HALO_VISIBILITY_ANY_SAMPLE` | The Metal renderer's visibility tests (lens flares) only say whether anything was visible, as before they counted how much: a light that is mostly hidden then flares at full strength. |
| `debug.frame_load_cpu` | number | `0.0` | apple | `HALO_FRAME_LOAD_CPU` | Milliseconds of made-up CPU work added to every frame (the frame pacer), to see what display.frames_in_flight does when frames don't fit in a refresh. 0 (default): none. |
| `debug.frame_load_every` | number | `0.0` | apple | `HALO_FRAME_LOAD_EVERY` | With debug.frame_load_cpu: the made-up work comes and goes, on in every other span of this many seconds, as a fight would. 0 (default): always. |
| `debug.low_power_frame_rate` | string | `"true"` | apple | `HALO_LOW_POWER_FRAME_RATE` | In Low Power Mode, gameplay and cutscenes aim for half the frame rate they aim for otherwise (120 to 60, 60 to 30, never under 30) and menus keep the display's pace: "true" (default); "false" keeps the rate in Low Power Mode; "always" acts as if Low Power Mode were on, for testing. |
| `debug.frame_load_gpu` | integer | `0` | apple | `HALO_FRAME_LOAD_GPU` | Made-up GPU work added to every frame: that many extra full-size draws (the Metal renderer), with debug.frame_load_cpu. 0 (default): none. |
| `debug.frame_counter` | boolean | `false` | apple | `HALO_FRAME_COUNTER` | Show the frame number and the game time in a corner of the screen, to point at a moment; with debug.fixed_timestep the same frame is the same moment in every run. |
| `debug.terminal_on_screen` | boolean | `false` | apple | `HALO_TERMINAL_ON_SCREEN` | Draw the console's output (what print and commands print) over the game, as the desktop builds do; false keeps it in the log only. |
| `debug.test_extra_scene` | boolean | `false` | apple | `HALO_TEST_EXTRA_SCENE` | Ask for a second window scene a few seconds in, to check that the game ignores its start, keeps its event pump and closes the scene (the host's host_main.m). |
| `debug.test_reclaim_sockets` | boolean | `false` | apple | `HALO_TEST_RECLAIM_SOCKETS` | On coming back from the background, make every socket of the game's again and break its TCP connections, as an Apple device does to a suspended app (the host's host_socket_revival.c); for the simulator, whose apps keep their sockets. |
| `debug.test_text_answer` | string | `""` | apple | `HALO_TEST_TEXT_ANSWER` | On an Apple TV, answer the dialog a menu's text field asks with (a join code, a password) with this text, 3 seconds after it comes up, as if it were typed and Done pressed (the host's host_tv.m); empty for none. |
| `debug.test_theater_reopen` | boolean | `false` | apple | `HALO_TEST_THEATER_REOPEN` | With display.immersive: close the immersive space a few seconds in, then press the window's button that opens it again (the host's host_theater.m). |
| `debug.gl_debug` | boolean | `false` | apple, desktop | `HALO_GL_DEBUG` | Report OpenGL errors in the log. |
| `debug.render_validation` | string | `"log"` | apple | `HALO_RENDER_VALIDATION` | In a build made with render validation (tools/ios_build.py --render-validation; no other build has any of it): every call into the GPU backend is checked before it is made, and every place the renderer substitutes, ignores or skips something is named and counted in the log. "off", "log" (each distinct cause once, with the draw it first happened in, and a summary with counts) or "strict" (the first failure or fallback ends the run with exit status 3). |
| `debug.render_capture` | string | `""` | apple | `HALO_RENDER_CAPTURE` | In a build made with render validation: a folder (in the data folder unless a full path) where the frames named by render_capture_frames are written as the calls the game made into the GPU backend, one file a window, for tools/render_replay.py to replay without the game. |
| `debug.render_capture_frames` | string | `""` | apple | `HALO_RENDER_CAPTURE_FRAMES` | The windows render_capture writes: "600+30,1200+30" is 30 frames from frame 600 and 30 from frame 1200. |
| `debug.render_validation_allow` | string | `""` | apple | `HALO_RENDER_VALIDATION_ALLOW` | With debug.render_validation = "strict": reasons the run goes on past, separated by commas ("fill-mode-ignored,sampler-unbound"). |
| `debug.metal_state_cache` | boolean | `true` | apple | `HALO_METAL_STATE_CACHE` | With the Metal renderer, skip the encoder calls that set what the encoder already holds; false makes every call, to compare. |
| `debug.shader_list_warm` | boolean | `true` | apple | `HALO_SHADER_LIST_WARM` | Compile the shaders and pipelines in a map's shader list while it loads; false compiles each when a frame first draws with it, to compare. |
| `debug.shader_list_record` | string | `"shader-lists-missed"` | apple | `HALO_SHADER_LIST_RECORD` | With the Metal renderer, a folder (in the data folder unless a full path) where the shaders and pipelines made while drawing, those a map's shader list missed, are appended to MAP.txt, each line once; "" for none. tools/shader_lists.py merges them into port/shader-lists. |
| `debug.metal_pipeline_archive` | boolean | `false` | apple | `HALO_METAL_PIPELINE_ARCHIVE` | With the Metal renderer, keep the compiled pipelines in an archive in the app's caches between launches. |
| `debug.metal_specialize` | boolean | `true` | apple | `HALO_METAL_SPECIALIZE` | With the Metal renderer, specialize each pipeline's vertex shader for its attribute formats; false uses the unspecialized shaders, which read the formats per draw (the fallback when specializing fails), to compare. |
| `debug.metal_vertex_bgra` | boolean | `true` | apple | `HALO_METAL_VERTEX_BGRA` | With the Metal renderer, the vertex shaders read vertex colors in the game's own byte order (BGRA); false swaps the bytes of every colored vertex on the CPU as it is uploaded, as the OpenGL ES renderer has to, to compare. |
| `debug.metal_base_vertex` | boolean | `true` | apple | `HALO_METAL_BASE_VERTEX` | With the Metal renderer, indexed draws take a base vertex, so their indices are drawn as the game holds them; false copies and rebases every draw's indices on the CPU, as the OpenGL ES renderer has to, to compare. |
| `debug.menu_open` | string | `""` | apple, desktop | `HALO_MENU_OPEN` | Start on this screen of the menus (port/assets/menus) instead of the main menu, a player profile being edited; empty for the main menu. |
| `debug.gpu_flush_draws` | integer | `-1` | desktop | `HALO_GPU_FLUSH_DRAWS` | Flush the GPU's pipeline every this many draws: -1 for every 3 on Intel graphics with Mesa's driver (which can hang without), 0 never. |
| `debug.dynamic_resolution_shadow` | boolean | `false` | apple | `HALO_DYNAMIC_RESOLUTION_SHADOW` | With the Metal renderer, log and record the dynamic resolution planner's choices without changing the rendered resolution. |
| `debug.dynamic_resolution_lines` | integer | `0` | apple | `HALO_DYNAMIC_RESOLUTION_LINES` | Turn dynamic resolution on and hold the 3D view at this many lines inside its ceiling's targets (from its floor to its ceiling), for comparisons; 0 for the planner's choice. |
| `debug.dynamic_resolution_validate` | boolean | `false` | apple | `HALO_DYNAMIC_RESOLUTION_VALIDATE` | Turn dynamic resolution on, also in benchmark runs, where a case's lines are the ceiling: the per-model validation run. |
| `debug.dynamic_resolution_sweep` | boolean | `false` | apple | `HALO_DYNAMIC_RESOLUTION_SWEEP` | Turn dynamic resolution on and step the 3D view from its ceiling to its floor and back, a level every 30 frames, in place of the planner. |
| `debug.gpu_stats` | boolean | `false` | apple, desktop | `HALO_GPU_STATS` | Log the renderer's draw counts once a second. |
| `debug.gpu_trace_frame` | integer | `-1` | apple, desktop | `HALO_GPU_TRACE` | Log every draw of this frame; -1 none. |
| `debug.gpu_trace_constants` | boolean | `false` | apple, desktop | `HALO_GPU_TRACE_CONSTANTS` | With gpu_trace_frame, also the vertex shader constants. |
| `debug.gpu_skip_vertex_shaders` | string | `""` | apple, desktop | `HALO_GPU_SKIP_VS` | Comma-separated ids of vertex shaders not to draw with. |
| `debug.gpu_dump_shaders` | string | `""` | apple, desktop | `HALO_GPU_DUMP_SHADERS` | A folder to write the generated shaders (GLSL or MSL) to; empty none. |
| `debug.gpu_shader_replay` | string | `""` | apple, desktop | `HALO_GPU_SHADER_REPLAY` | A folder of shader inputs (.vsh, .key) that gpu_dump_shaders recorded: translate each at startup into its replay folder; empty none. |
| `debug.gpu_shader_replay_dialect` | string | `""` | apple, desktop | `HALO_GPU_SHADER_REPLAY_DIALECT` | The dialect gpu_shader_replay translates into: empty the context's, "450" desktop GLSL, "msl" Metal (which the Metal renderer also compiles). |
| `debug.gpu_debug_expression` | string | `""` | apple, desktop | `HALO_GPU_DEBUG_EXPR` | A GLSL expression every pixel shader shows instead of its result. |
| `debug.gpu_debug_texture0` | boolean | `false` | apple, desktop | `HALO_GPU_DEBUG_T0` | Pixel shaders show their first texture. |
| `debug.gpu_debug_flat` | boolean | `false` | apple, desktop | `HALO_GPU_DEBUG_FLAT` | Pixel shaders show their vertex colour. |
| `debug.companion_capture` | boolean | `false` | apple, desktop | `HALO_COMPANION_CAPTURE` | Draw the companion HUD without an external display and save it with screenshots. |
| `debug.screenshot_directory` | string | `""` | apple, desktop | `HALO_SCREENSHOT_DIR` | A folder to save frames to (with screenshot_every); empty none. |
| `debug.shadow_blur` | string | `"footprint"` | apple, desktop | `HALO_SHADOW_BLUR` | How object shadows larger than 128 texels (display.shadow_map_size) are blurred: "footprint" over the area the game's blur covers at 128, "xbox" the game's taps at their fixed offsets (at 512 they fall 4 texels apart and show as separate copies). 128-texel shadows always get the game's blur. |
| `debug.screenshot_every` | integer | `0` | apple, desktop | `HALO_SCREENSHOT_EVERY` | Save every this many frames to screenshot_directory; 0 none. |
| `debug.texture_astc` | string | `"auto"` | apple | `HALO_TEXTURE_ASTC` | DXT-to-ASTC texture preparation: "auto" (default) converts on GPUs without BC once the release enables it, "force" emulates a GPU without BC for validation, "off" disables it. |
| `debug.texture_astc_sync` | boolean | `false` | apple | `HALO_TEXTURE_ASTC_SYNC` | Wait for ASTC map preparation before loading, for screenshot validation only. |
| `debug.texture_upscale_generate` | string | `"auto"` | apple | `HALO_TEXTURE_UPSCALE_GENERATE` | Make upscaled textures on this device: "auto" (default) where the device's profile and the display.upscaled_textures switch both say so and Core ML has a Neural Engine (at import, in menus, loading and pauses, never in gameplay), "force" whatever the profile says, "off" never. |
| `debug.texture_upscale_export` | boolean | `false` | apple | `HALO_TEXTURE_UPSCALE_EXPORT` | Export the generator's policy table and uncompressed textures for parity checks. |
| `debug.texture_dump_directory` | string | `""` | apple, desktop | `HALO_TEXTURE_DUMP` | A folder to write every texture to as it is uploaded; empty none. |
| `debug.texture_override_directory` | string | `""` | apple, desktop | `HALO_TEXTURE_OVERRIDE` | A folder of replacement textures, <hash>.rgba (step-tools/texture-upscale.py), each drawn in place of the texture whose first mip level hashes to <hash>; empty none. |
| `debug.texture_compare_directory` | string | `""` | apple, desktop | `HALO_TEXTURE_COMPARE` | A second folder of replacement textures to compare texture_override_directory's with. |
| `debug.texture_override_name` | string | `""` | apple, desktop | `HALO_TEXTURE_OVERRIDE_NAME` | What the comparison's readout calls texture_override_directory's set; empty "SET A". |
| `debug.texture_compare_name` | string | `""` | apple, desktop | `HALO_TEXTURE_COMPARE_NAME` | What the comparison's readout calls texture_compare_directory's set; empty "SET B". |
| `debug.texture_log` | boolean | `false` | apple, desktop | `HALO_TEXTURE_LOG` | Log texture uploads. |
| `debug.texture_no_cache` | boolean | `false` | apple, desktop | `HALO_TEXTURE_NO_CACHE` | Upload textures again every time they are used. |
| `debug.texture_replacement_stats` | boolean | `false` | apple, desktop | `HALO_TEXTURE_REPLACEMENT_STATS` | Log the upscaled textures' memory ("texture replacements, now: ...") every 300 frames, as well as at each level load. |
| `debug.device_profile` | string | `""` | apple | `HALO_DEVICE_PROFILE` | Which device profile (profiles/ in the app) sets this device's graphics settings: empty chooses by the device; "none" uses the built-in defaults; a model identifier ("AppleTV5,3") or a Mac's chip name chooses as that device, for trying its profile elsewhere. |
| `debug.frame_log` | string | `"auto"` | apple | `HALO_FRAME_LOG` | With the Metal renderer: append to frame-log.txt, in this folder, a line every 5 seconds and at each change of scene class with the frames drawn, frames a second, the 95th and 99th percentile frame times, late frames, the render size, the scene class, the map, memory and thermal state, so a session played by hand can be read afterwards (tools/frame_log.py). "auto" (default) logs on an Apple TV only; "on" and "off" force it. |
| `debug.test_win_level` | number | `0.0` | apple | `HALO_TEST_WIN_LEVEL` | Win the level after this many seconds of its game time, once, as when the player reaches its end: a run nobody plays then goes through the change to the next level. 0 (default) does nothing. |
| `debug.settings_dump` | boolean | `false` | apple | `HALO_SETTINGS_DUMP` | Log every setting a device profile may set at start, with the layer each value came from; false logs only those something set. |
| `debug.profile_record` | boolean | `false` | apple, desktop (profiling builds) | `HALO_PROFILE_RECORD` | Record a profile with no command (configure.py --profile and tools/ios_build.py --profile builds; docs/profiling.md): from when profile_record_when says until profile_stop, a map change or the end, into numbered part files in the data folder's profiles folder (tools/net_report.py reads it). |
| `debug.profile_record_when` | string | `"start"` | apple, desktop (profiling builds) | `HALO_PROFILE_RECORD_WHEN` | "start": from the first frame until the first map change; "game": each game that is not the main menu, a recording each, until its map goes. |
| `debug.profile_memory` | integer | `256` | apple, desktop (profiling builds) | `HALO_PROFILE_MEMORY` | Megabytes a recording keeps in memory, 4 to 1024: two halves, each written out as a part when it fills. |

<!-- END GENERATED SETTINGS config -->

With Mesa drivers, the game sends its GL calls through the GL thread of
Mesa. To stop this, set the environment variable `mesa_glthread=false`.

## Streamer mode

Turn on STREAMER MODE in Settings > Network Setup to replace offensive words
with stars in text the game displays, including player names, the kill feed,
scoreboards, lobbies, map and mod text, and message boxes. It changes only
what this machine displays. It does not change names or messages sent to
other players. Text you are typing remains visible while you edit it.

The default filter includes slurs at every severity and swearing at strong
or severe levels. Insults are off by default. The `streamer.slurs`,
`streamer.swearing`, and `streamer.insults` settings in `config.toml` can
select `"off"`, `"severe"`, `"strong"`, or `"mild"` for each group. For
personal terms, put `text_filter.txt` in the data root with one `add <word>`
or `allow <word>` entry per line. The game reads it at startup and when the settings change.
The built-in list and its sources are described in
[`port/assets/text_filter/README.md`](../assets/text_filter/README.md).

## Updates

The builds from GitHub Actions (refer to the main [README](../../README.md#download))
can update themselves. At start-up, the game asks GitHub for the latest
release. The game does not wait for the answer. If the latest release is not
newer, the game does nothing.

The direction of the view is the exception. The game reads the mouse and
the sticks every frame, so in first person, on foot, each frame points the
view where the player aims at that moment (`display.direct_camera`): the
view turns in the frame the stick or the mouse moves. In a vehicle's seat
and in cinematics the view blends as everything else does.

`interpolation = false` in `config.toml` restores the original behaviour: one frame per
tick, throttled to 30 per second.
If the latest release is newer, the game asks: "Do you want to update?"

- Select "Yes" to update. The game downloads the release for this platform,
  replaces its files and starts the new version. The old files get the
  extension `.old`. The new version deletes them.
- Select "No" to continue. The game asks again at the next start.
- Select "Do not ask again", then "Yes", to stop the questions. The game
  writes `auto = false` in the `[update]` section of `config.toml`. To get
  the questions again, set `auto = true`.

Under gamescope (the Game Mode of the Steam Deck), the game does not ask,
because a system dialog stops the game there. The game writes the new
version in the log. To update, start the game from the desktop, or set
`update_answer = "yes"` in the `[debug]` section of `config.toml`.

The game downloads through HTTPS. It examines the certificate of the server
against the certificate authorities of the system: on Linux, the bundle of
the distribution (`src/posix_update.c`, with Mbed TLS); on Windows, the
certificate store of Windows (WinHTTP). The folder of the executable must
let the game write to it.

Builds that you make yourself have no build number. They do not look for
updates.

## Frame rate

The game calculates its world at 30 Hz, as on the Xbox. On the Xbox, the
game showed one frame for each calculation (tick). This port shows one frame
for each refresh of the display, for example at 60, 120 or 240 Hz.

Each frame shows the world between the last two ticks
(`game/render_interpolation.c`):

- After each tick, the game keeps the camera, the position of each part of
  each object, and the first-person weapon.
- Each frame mixes the last two ticks. The mix agrees with the time since
  the last tick.
- Rotations use quaternions. Positions and scales are linear.
- A teleport, a respawn or a cut of the camera does not mix. It jumps.

Thus the frames are one tick (33 ms) after the calculation. The calculation
does not change.

The direction of the view is an exception. The game reads the mouse and the
sticks in each frame. In first person, on foot, each frame points the view
where the player aims at that time (`display.direct_camera`). Thus the view
turns in the frame that the mouse moves. In a vehicle and in cinematics, the
view mixes as the other things do. On Android, the view mixes as before.

To get 30 frames each second, set `display.interpolation = false`.

To see the frame rate:

1. Push \` to open the developer console.
2. Enter `display_framerate true`.

In the game of another host, the console runs only the commands that change
nothing of the game (such as `display_framerate`), and the game puts back
cheats, the game speed and the settings of the drawing that show more of
the world (such as `rasterizer_wireframe`). Refer to `NETCODE.md`.

The frame rate shows at the bottom right of the screen. It is the mean over
half a second.

## System link

The Xbox game lets 16 players on 4 machines play a system link game. This
port lets up to 128 players on up to 128 machines play. Each machine can
have up to 4 players (split screen).

- `include/halo_port_limits.h` sets the limits.
- `include/halo_port_capacity.h` sets the memory for the limits. The game
  state is 16 MB at `0x81A00000` (3.3 MB on the Xbox). The pools of objects,
  effects, particles, contrails, lights and sounds are also larger.

Obey these rules:

- All the machines in a game must use a build with the same limits.
- The port uses protocol version 2. It does not see the Xbox game or older
  builds of the port. They do not see the port.

These are the differences from the Xbox:

- The host waits up to 60 seconds (15 seconds on the Xbox) for the other
  machines to load the map.
- If a machine does not read the messages of the host for two seconds, the
  host removes it from the game.
- The saved games contain all the game state. Thus a saved game is 16 MB.
  Saved games from older builds of the port do not operate.
- In campaign and in games of up to 16 players, the game removes garbage
  (bodies, dropped weapons) as on the Xbox. In larger games, it keeps more
  garbage, in proportion to the players.
- The lobby shows the local machine and the first three remote machines.
  The other machines are also in the game.
- In free-for-all games, each player is a team.

Linux, Windows and Android machines can play in the same game. Each machine
simulates the players from the same inputs, and the host does not correct
all of the game. Thus each machine must calculate the same floating-point
results, and all the ports:

- Compile without fused multiply-add (`-ffp-contract=off`).
- Use the math functions of musl (`port/include/halo_math.h`,
  `port/third_party/musl-math`), not the math functions of the system.

### Play on one computer

More than one copy of the game can play on one computer. Each copy must
have a different loopback address. A copy with an address gets no
broadcasts. Thus each copy must send its broadcasts to the other copies.

For a host and two clients, enter these commands in three terminals:

```sh
HALO_NET_ADDRESS=127.0.0.200 HALO_NET_BROADCAST=127.0.0.201,127.0.0.202 build/linux/halo
HALO_NET_ADDRESS=127.0.0.201 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
HALO_NET_ADDRESS=127.0.0.202 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
```

Do not give 127.0.0.1 to a copy. Each copy gets to its own address through
127.0.0.1. Linux and Windows send all of 127.0.0.0/8 to the loopback
interface.

### Test with many machines

`tools/system_link_bots.py` adds simple machines to a game. Each machine has
one player. The machines obey the system link protocol, but they do not
calculate the game or move their players.

1. Start a game on the host.
2. Enter `python tools/system_link_bots.py --host 127.0.0.200 --machines 127 --start`.

Each machine uses its own loopback address, from 127.0.0.2. The option
`--start` starts the game when all the machines are in the lobby. If the
host has no `network.address`, do not give `--host`.

## Internet play

Machines with an invite link can play system link on the internet. This
project has no server.

When a copy of the game starts to host a system link game, it makes an
invite link: `halo://join/<64 hexadecimal digits>`. The game writes the link
to the standard error and puts it on the clipboard. The links of older
versions of the game (44 digits) do not operate. The game writes a message
when it gets one.

To join a game, do one of these steps:

- Open the link. The game is the handler of `halo://` links. If the game
  already operates, the new copy gives the link to it and stops. A key in a
  file that only the user can read (`halo-ce-universal.key` in
  `$XDG_RUNTIME_DIR`, else `~/.halo-ce-universal.key`; on Windows in
  `%LOCALAPPDATA%`) encrypts the link, so the programs of other users cannot
  read it.
- Copy the link (or the 64 digits) and go to the game.
- Enter `halo <link>`.
- Accept a Discord invite. Refer to "Discord".

When the machines connect, the game of the host shows in Multiplayer,
System Link. Join the game as on a local network. System link on a local
network does not need an invite.

While the game joins an invite, Join Game > Direct Link shows each step,
and what went wrong as soon as the game knows it. The log has the same
steps, with the seconds since the invite came:

- "reaching the matchmaking servers": the game connects to the brokers.
  If it cannot reach any broker in 12 seconds (or all of them fail), the
  page says so.
- "looking for the host's game": the game asks the host through the
  brokers. A host answers in a second or two. If no answer comes in 20
  seconds, the game of the host was not found: it ended, or the host quit.
  The brokers cannot tell which. A public game's listing (refer to "Server
  browser") tells more: it ended, the invite is old (the host made a new
  one), or the host has another network version. The game tells these at
  once.
- "found the host: connecting directly": the two machines send packets to
  each other (refer to "Connection"). If no packet gets through in 30
  seconds, the networks cannot connect directly. If the system refused
  the packets to the host's address on the local network, the page asks
  for the Local Network permission (Apple's systems).

The game asks again until 90 seconds pass, so a step can still succeed
after the page shows a problem. `python3 tools/p2p_broker_probe.py
[invite link]` checks the brokers and STUN servers from a computer's
network, and finds the public listing of the host of an invite.

### Server browser

Server Setup in Create Game > Internet has a LISTING row:

- PUBLIC (the default): the game also shows in Join Game > Server Browser
  on every machine. Anyone can see and join the game.
- PRIVATE: only players with the invite link can join.

Each new game starts as PUBLIC (`network.host_public = false` makes new
games start as PRIVATE). An online co-op game starts as PRIVATE, and keeps
the last choice of its LISTING (`network.coop_public`). A LAN game is never listed. `network.public_lobby = false` turns the server browser off.

A PUBLIC game can also have a PASSWORD (a row of Server Setup, below
LISTING). The Server Browser shows a lock at the left of a game with a
password. A player who selects that game must type the password, and JOIN
GAME joins only with the correct password. The invite link of the game joins
it without the password. The host keeps the password only while the game
runs.

In the Server Browser, select a game to join it. The game joins the invite
of the game, as for a link. When it reaches the host, it opens the lobby.
If it cannot reach the host in 30 seconds, it marks the game FAILED. A game
that is full or starting shows CLOSED. REFRESH asks the hosts for their
listings again.

The Server Browser shows the games on the game's own maps (its multiplayer
maps, and the campaign levels of a co-op game). A game on any other map is
a game on a Custom Edition map, and is shown only with CUSTOM MAPS
(experimental) on in Network Setup (`network.custom_maps`, off
by default), and then if `custom_maps` has a map of that name or map
downloads are on (`network.map_download`, DOWNLOAD MAPS). A game on the map
of a map pack (Delta's
list says that the family of the map is `pack`, such as CE+'s
`wizard@cex`) is never shown. On joining, a client takes only a Custom
Edition map of the version of the host, as for any join. A host's map with
a name of the game's own maps but other contents cannot be detected: the
host does not send a version for those maps, and the client plays its own
copy. If the host sends a version for one, the client logs it.

#### Delta's game list

The Server Browser also shows the games of Delta's public game list
(`https://halo.milenko.org`, run by others; `network.delta_url`), after the
games of the brokers. Each game shows its players and, in the column where
the others show LIVE, CLOSED or FAILED, `V<number>` for a game of a network
version this build cannot join (this build plays versions 29 to 32, 34 to 36,
38 and 39; a game of another version is shown and Join refuses it). The line
below the rows names that version. A game with a password or a game of the campaign is
left out. A game that a broker also lists is shown once.

- Browsing (`network.delta_browse`, on): while the Server Browser is open,
  and `network.online` and `network.public_lobby` are on, the game asks the
  list for its games (`GET /v1/games`) every 15 seconds and when REFRESH is
  pressed. It sends nothing but the request (the program name VISR, and no
  language, setting, name or game). The list's server sees this machine's
  internet address, as the brokers do.
- Announcing (`network.delta_announce`, off): the PUBLIC game this machine
  hosts for the internet, without a password, is also listed in the list
  (`POST /v1/announce`), every minute and when the game changes (the list
  drops a game it has not heard of in 75 seconds), and withdrawn
  (`POST /v1/withdraw`) when the game ends, goes private, gets a password
  or a new invite, and when this game quits (nothing is sent while
  `network.online` is off; the list drops the game in 75 seconds). It sends
  the invite (anyone who has it can join the game), the name, map, game type
  number (not its name), player counts, whether it is open and has teams,
  this build's network version, and `delta=0` (the game's own protocol
  alone), and the list's server sees this machine's internet address. It
  sends no player's name, platform, build or address of the machine. The list tries
  to reach a new game before it lists it, and refuses it for two minutes if
  it cannot: this build waits and tries again.
- `network.delta_url = ""` turns both off. The requests are made on a
  thread of their own (`src/p2p_delta.c`) with the system's TLS (Mbed TLS
  and the system's certificate authorities on Linux, WinHTTP on Windows,
  `NSURLSession` on Apple devices) and never follow a redirect. The answer
  is read as untrusted text (`src/delta_list.c`).

Delta Peer (the version and capability exchange on UDP port 5160), the
signed table of network versions, the statistics and link calls and the
control interface are not used: a game joined from the list is joined by
its invite, as any other, and plays the game's own protocol.

How it operates (`src/p2p_lobby.c`):

- The host of a public game publishes a listing: the invite, and the name,
  map, gametype and player counts of the game. The listing goes to the
  same MQTT brokers as the invites (`network.brokers_file`), retained,
  to a topic of the host (`hceu/3/lobby/s/<hash of its key>`).
- The key of the host signs the listing (Ed25519). The key is the key of the
  invite, so no other machine can list the invite of the host, change its
  listing, or list a game with false details.
- The host publishes the listing again every 30 seconds, when the game
  changes, and when a browser asks. If the topic of the host is empty or
  holds another listing, the host publishes again in 5 seconds or less. Thus
  a broker that deletes the listing does not remove the game.
- When the host stops (it stops hosting, the game becomes private, or the
  game quits), it publishes a closed listing, and then empties its topic.
  If the host loses its connection, the broker empties its topic (the will
  of the connection). A browser removes a game that it does not hear for 90
  seconds.
- When a public game becomes private, the host makes a new invite. Thus a
  player who saw the listing cannot join with the old invite.
- The listing of a game with a password does not hold the invite in clear
  text. The secret part of the invite (its token) is encrypted with a key
  from the password (Argon2id, salted with the key of the host, then
  XChaCha20-Poly1305). The browser makes the key from the password that the
  player types, and opens the invite only if the password is correct. When
  the host sets or changes the password, it makes a new invite. A player who
  has the listing can try passwords on their own machine without the host,
  so use a long password.

A public game does not publish the address of the host. But any machine
with the invite can ask the host to connect, and the host then sends its
addresses. Thus anyone can learn the address of the host of a public game,
as for any public server.

The brokers are in `brokers.txt` next to the executable (from
`port/assets/network/brokers.txt`; on Android, the app writes it next to
`config.toml` at each start), one `host:port` on each line. The game uses
all of them at once (up to 4), so one that works is enough. An update
replaces `brokers.txt`: to use brokers of your own, put them in another
file and name it in `network.brokers_file`. All the players must use the
same broker to see each other's games. The game uses
MQTT 5 if the broker has it, else MQTT 3.1.1. A broker that does not keep
retained messages, or does not let clients subscribe with wildcards, carries
only invites, not listings.

### Map sharing

When you join a game on a Custom Edition map that you do not have, the game
downloads the map from the players of the game, if the host shares its maps
(SHARE MAPS in Server Setup, `network.map_share`, on by
default). This operates on a local network and
on the internet. It also downloads the resource maps that the map needs
(`bitmaps.map`, `sounds.map`, `loc.map`) if you do not have them. A line at
the top of the screen shows the progress. In the lobby, the game starts when
every player has the map. If the game already operates, you join it when
the download is complete. If no player sends any of the map for a minute,
you leave the game, and the main menu tells you why. If the downloaded map
cannot be played, the game deletes the files that it downloaded, and you
leave the game.

All the players help (HELP SEND MAPS in Settings > Network Setup,
`network.map_seed`), as the peers of a torrent do:
the downloaded map comes in pieces from the host and from each player who
has it or has a part of it. The game checks each piece against its hash, so
a player cannot send a different map. A player who sends a bad piece is not
asked again. To keep the upload free for the game, the host sends at most
`network.map_share_upload` (1024 KB/s, in `config.toml`) and each other
player at most `network.map_seed_upload` (256 KB/s: MAP UPLOAD LIMIT in
Settings > Network Setup). Set these lower on
a slow connection. DOWNLOAD MAPS there (`network.map_download`) turns
downloading off: you then leave a game whose map you do not have.

Maps keep their names. A game is on a map by its name (`hugeass`), and says
which version of it by the start of the map's hash, so a game always loads
the version of the host, whatever your copy is called. A downloaded map
takes its own name (`hugeass.map`); if you have another version of that
name, the download is named with the start of its hash
(`hugeass_0e2b3ee4.map`), and the menus show it as "Hugeass (2)". A map that
an earlier build renamed this way gets its name back, unless you have a map
of that name. A download that stops leaves a `.part` file in `custom_maps`
(only of the last map whose download stopped, and only if it holds part of
the map); the next download of that map continues from it. The host shares
maps in the folder of a Halo Custom Edition install (`paths.custom_edition`)
too; downloads go to `custom_maps`.

Over the internet, the players of a game connect only to the host. To send
each other a map, a player answers the other players of the game (who have
its invite) as the host does, and a player who downloads connects to them in
the same way that it connected to the host. Map sharing uses port 5153 for
its connections. Refer to `NETCODE.md`, "Map sharing".

### Security

Only machines with the invite can find the game:

- Each copy of the game makes an Ed25519 key pair when it starts, and from
  it an X25519 key pair (Monocypher, `port/third_party/monocypher`). Its
  identifier is from the hash of its X25519 public key. The Ed25519 key
  signs the listing of a public game.
- The link contains a 16-byte hash of the public key of the host and a
  random 16-byte token. The identifier of the host is from the first 6
  bytes of the hash.
- The machines exchange their public keys and addresses through public MQTT
  brokers (`network.brokers_file`). The topics are HMACs of the token.
  A key from the token encrypts and authenticates the messages
  (`src/p2p_signal.c`, `src/p2p_crypto.c`). The host authenticates its answer
  with a key that only it and the player can calculate. Its public key must
  agree with the hash in the link. The hash is long, so no other machine can
  find a key with the same hash.
- Then the player shows in the same way that it has the private key of its
  public key. Only then does the host make a session for the player. Thus
  other machines with the invite cannot make sessions in the name of a
  player (such a session would keep the player out).
- Each two machines get the keys of their packets from their key pairs and
  a random number from each. The keys do not go through the brokers. Thus
  other machines with the invite cannot read or change the packets.
- Each packet is encrypted and authenticated, with a different key in each
  direction. A machine ignores a packet that it already received.
- A machine can send only to the ports of the game on the other machine.
- The host makes one session from each request of a player. If a person
  sends a copy of an old request again, the host ignores it. A player that
  must ask again sends a new request.
- The host tries to reach at most 8 new players at the same time. The
  other players ask again.
- The host answers a request that is not proven at most one time each
  second through each broker. It answers at most 20 of these requests each
  second, after a first 32. Each answer goes only through the broker that
  brought the request. Thus a flood of requests does not use much of the
  bandwidth of the host.
- The host does the key work of at most 20 requests each second from keys
  that it does not know, after a first 32. It keeps the key work of the
  last 256 keys. Thus the proof of a player does not need more key work. A
  flood of requests can make players join more slowly. A player asks again
  for 90 seconds.
- The host refuses the predicted movement of a player whose game runs
  faster than time (a speed hack). If the messages on the player's
  connection were also ahead for ten seconds, the host drops and bans the
  player: each player sees who in red on the console, and the host adds a
  line to `cheaters.txt` and `bans.txt` (beside `debug.txt`) with the
  address and hardware id of the player, and the Discord name and id that
  the game of the player told it, marked `(self-reported)` (a player can
  change these). The host refuses a machine whose address or hardware id is
  in `bans.txt`. If only the player's datagrams were ahead, the host does
  not drop the player, because another machine can send datagrams with the
  player's address: it adds an `unverified` line to `cheaters.txt`.
- The host can ban a player with `ban <player name>` in the developer
  console (Tab completes the name). Remove a line from `bans.txt` to unban.
  Refer to `NETCODE.md`. `kick <player name>` drops the player the same
  way, but keeps nothing: no line in `bans.txt`, and the player can join
  again at once. In co-op, `bringto` brings every player to the host.
  So that every player can be named, the host trims the spaces around a
  name and removes characters that draw as nothing. A
  letter with a mark is typed as the plain letter (`ban jose` for "José").
  A name with nothing left to type becomes "Player", and a name that another
  player already has gets a number ("Player 2"). The game refuses a profile
  name that is blank, and a multiplayer game refuses a profile whose name was
  made blank before this check.
- An invite operates while the copy of the game that made it operates.

### Connection

Each machine gets its public address from public STUN servers. Then the two
machines send packets to each other until the packets get through (UDP hole
punching). There is no relay.

Some networks give a different port for each destination (for example some
mobile and company networks). Two machines behind such networks cannot
connect. To connect, forward `network.tunnel_port` on the router of one of
the machines.

The game can ask the router to forward the port (UPnP,
`src/posix_upnp.c`, with `port/third_party/miniupnpc`):

- The host asks its router when a player uses its invite.
- A player that joins asks its router when it does not reach the host in
  5 seconds.
- The forwarded port is one more address that the machine gives to the
  other machine.
- The forward has a duration of one hour. The game makes it longer while
  it operates. When the game stops hosting (and is not joining a game),
  or stops normally, it removes the forward. It
  does not remove the forward after a crash, or if a request to the router
  is still under way 3 seconds after the game starts to stop. Some routers only make forwards without a duration.
- When the game finds the router, it removes the forwards to this machine
  that have the description "Halo internet play" and that no copy of the
  game uses now (forwards that a copy of the game did not remove).
- UPnP does not help behind a second NAT, for example the NAT of a mobile
  network provider. Then the router has a private address, and the game
  does not ask.

On Apple devices the game asks with NAT-PMP, then PCP, instead
(`port/ios/host/host_natpmp.c`): a UPnP router is found by multicast, which
an Apple app can send only with an entitlement that Apple grants on request.
NAT-PMP and PCP are unicast UDP to the router's port 5351, at the gateway of
the default route. The forward has a duration of one hour, or the shorter
one that the router gives, and the game asks again after half of it. It
asks again too when it comes back from the background.

To stop all UPnP, NAT-PMP and PCP requests, set `network.allow_upnp` to
`false`.

In the game, each machine has an address in 100.64.0.0/10:

- `src/xnet.c` gives the datagrams that the bound UDP sockets of the game
  send to such an address to `src/p2p.c`. Other datagrams (of sockets that
  are not bound yet, or that are connected to the address) and the TCP
  connections of the game go through local sockets on 127.0.0.1 (or
  `network.address`). The traffic from the other machines comes to the
  game from local sockets too.
- `src/p2p.c` sends that traffic through one UDP socket. UDP datagrams go
  as they are. TCP connections go as KCP streams (`port/third_party/kcp`).
- The broadcasts of the game go to all the machines. Thus the game of the
  host shows on the other machines.

### Discord

If the Discord desktop client operates, the game of the host shows in
Discord (through the application of `discord.application_id`). The activity
has a private party with the invite as its join secret. The host can send
the invite with the invite button of Discord. When a person accepts it, that
person joins the game. If the game does not operate, Discord starts it.
The game sends the activity only to a Discord client of the same user.

## Map checks

The game reads a map's tags straight into memory and uses them as its own
structures: every pointer, count, index and enum in them is the map's, and
the game writes values into tags as it runs. So before anything reads a
map's tags, the port checks every tag against a schema of its group
(`game/tag_schema_*.c`, read by `game/tag_validate.c`), and each structure
BSP as it loads:

- Every block and every piece of data must lie in the tags (or the BSP) and
  overlap no other. Otherwise the game refuses the map.
- A block with more elements than the game has room for is cut to the
  maximum. A tag reference that is not a tag of the right group becomes
  none. So do an index past its block and an enum past its values (or they
  become 0, where the game cannot take none). A string gets its terminator.
  Values that the game sets as it runs are reset.
- Checks that the schema cannot express run last: the BSPs' and the models'
  graphs, vertex and index buffers, and indices into other tags.

Each correction goes to `debug.txt`. The game's own maps need none.
`build/linux/map_validate [--strict] map.map...` runs the same checks on map
files without the game, and `tools/test_linux_port.py` runs it on the maps
in `assets/maps`. `map_validate --fuzz <runs> map.map` changes a few words
of the tags at random in each run. The checks must not crash or hang, and a
map that they let through must need no more corrections.

A map's scripts can call only the script functions that a map needs (the
allowlist in `hs/hs.c`). They cannot call the functions for files, the
saved state of the game, the console, debugging or cheats. A script that
calls one does not run. The developer console can call every function.

Halo Custom Edition maps get the same checks (those that need OpenSauce are
refused). Their own loader (`game/cache_file_formats.c`) reads them into
their tag cache at 0x40440000 and converts what Custom Edition lays out
differently, then the validator checks their tags and each of their BSPs as
it checks this build's maps, before the game converts their models, BSP
geometry and scripts. Put them with `bitmaps.map`, `sounds.map` and
`loc.map` in `custom_maps`, beside `maps`, or set `paths.custom_edition` to
a Custom Edition install; the map lists show them as CUSTOM SINGLEPLAYER and
CUSTOM MULTIPLAYER, played as campaign levels (alone, or as network co-op)
or as multiplayer maps by their scenario type, and `game.custom_edition =
false` refuses them. `map_validate` checks them too, with the resource maps
beside each map or in `--maps <folder>`. See
`docs/custom_edition_caches.md`.

Defensive checks stay in the game code too. An index into a tag block, the
tags or a tag's data that is out of range gets zeros (`tag_empty_data` in
`tag_files/tag_groups.c`), not other memory.

A map's name must be its file's: the cache file slots are found by the name
in the map's header, so a map file whose header names another map (a
renamed one) is refused, not copied again for ever.

The loading screen's picture is `loading.tga` in the maps folder, read only
if it is an uncompressed 24-bit picture of 320 by 240. A PAL disc has the
file; an NTSC disc holds the picture in its `default.xbe`, and the iPhone,
iPad, Apple TV and Vision Pro importers write the file from it
(`port/runtime/loading_picture.h`). For maps imported before that, or copied
by hand, `python3 tools/loading_picture.py DISC_IMAGE MAPS_FOLDER` writes it.
Without the file the loading screen is the blur and the tone alone. The
screen is held over a campaign level's whole load, shader compiles included
(`port/linux/game/loading_screen.c`).

A checkpoint (`savegame.bin`, in the profile's folder) and a core are
images of the game state's memory: with the data arrays' pointers to their
elements, the objects' memory pool's blocks and the references to them, and
the caches' procedures. Before one is taken, each of those is checked
against what the game made at startup (`game_state_image_accept` in
`saved games/game_state.c`): an image that does not match (a damaged or
crafted file) is refused, and the level starts over.

## What operates

| Area | Status |
| --- | --- |
| Game code | All 466 C files of the game. The changes are in "Game source changes". |
| Graphics | Direct3D 8 on a GPU backend behind `src/gpu.h` (`src/gpu_gl.c`: OpenGL 4.5 core through SDL3, or OpenGL ES 3 on iOS), with the device in `src/d3d8_device.c`: NV2A vertex shader microcode and register combiner pixel shaders are translated to GLSL, Xbox textures (swizzled, linear, DXT, palettized, cube and volume) are decoded and cached with page-protection write tracking, vertex and index buffers are drawn from a copy of the Xbox's contiguous memory in GPU buffers kept current the same way, GPU state is set only when it changes, render targets are framebuffer objects, and the picture is presented letterboxed in a resizable window. |
| High-res HUD | The HUD is drawn from high-res assets: redraws at 8x the size of the maps' bitmaps (4x for the largest), in `port/assets/hud`. They cover the meters, counters, panels and their outlines, the motion sensor, reticles, waypoints and scopes, but no bitmap with English text. `tools/hud_assets.py` makes them from the SVG redraws, and the build embeds them in the executable. When the game uploads one of those bitmaps, `src/hud_hires.c` gives the high-res texture in its place, if the bitmap's pixels are those of the English maps: another language's maps keep their own. The game sizes and places the HUD from its tags as before. `display.high_res_hud = false` turns this off. |
| High-res text | The menus' and HUD's text is drawn with Overpass (`port/assets/fonts`, SIL Open Font License) in place of the maps' bitmap fonts, which are Interstate. `src/text_hires.c` rasterizes each glyph with stb_truetype (`port/third_party/stb`) at the resolution the game draws at, into an atlas that a placeholder bitmap of the game stands for. The game lays the text out from its font tags as before. The menus' titles (the screens' headers and the main menu's items) are pictures of text in the maps, so they are drawn as the high-res HUD is: `tools/title_assets.py` sets each one again in OpenCE, Roger White's public-domain Newtown respaced to match the maps' commercial title typeface (`tools/title_font.py`), at 4x the bitmap's size over its own plate or glow, each letter placed where the map's letter is, in `port/assets/titles`. The postgame carnage report's title is set over a hand-made SVG redraw of its panel (`port/assets/titles/svg`) instead. `display.high_res_text = false` turns it off. |
| Sound | Xbox DirectSound on SDL3 audio (`src/dsound_sdl.c`): PCM and Xbox ADPCM, mixed at 48 kHz, with volume, pitch, mix bins, distance, stereo pan, occlusion and obstruction. There is no Doppler effect, no cones and no reverb. |
| Input | XInput on SDL3 (`src/xinput_sdl.c`): keyboard, mouse, gamepads with rumble, and the debug keyboard for the console. |
| Files | The Win32 file functions and the MSVC file functions on POSIX, with the translation of Xbox paths. |
| Threads | Threads, events, mutexes, critical sections, interlocked operations and alertable waits. |
| Memory | The port reserves the Xbox memory at `0x80000000`. Thus the game gets the fixed addresses that it expects. |
| Saved games | The Xbox `UDATA` layout, with SHA-1 signatures. |
| Networking | Winsock on BSD sockets. System link on a local network and on the internet. |
| Bink video | Not available. The game skips the movies. |

## How the port operates

### The compiler

`tools/linux_build.py` compiles the game with clang and these options, which
give the ABI of the MSVC compiler:

- `--target=i686-linux-gnu`: 32-bit x86.
- `-fms-extensions`: the MSVC extensions.
- `-fshort-wchar`: 16-bit `wchar_t`.
- `-malign-double`: 8-byte alignment of 64-bit members.
- `-fcommon`: tentative definitions, as in C89.

glibc gives only ISO C (`__STRICT_ANSI__`). Thus POSIX names, for example
`random`, do not conflict with the names of the game.

These files supply the MSVC functions that clang does not have:

| File | Contents |
| --- | --- |
| `include/halo_linux_prefix.h` | The first header of each file: the architecture macros of the SDK, MSVC `__inline`, SEH keywords, `__declspec(selectany)`. |
| `include/` | Headers that add MSVC names to the C runtime headers. |
| `port/include/xdk` | The Xbox SDK declarations. The compiler reads this folder after all the other folders. |
| `tools/linux_msvc_semantics.py` | Makes a header that declares each struct tag at file scope, as MSVC does. It also makes the header inline functions weak, as the COMDAT functions of MSVC. `game/msvc_comdat.c` gives one external copy of each. |
| `include/halo_linux_winsock_names.h` | Gives new names to the Winsock functions of the SDK. Thus they do not link to the glibc functions with the same names. |
| `include/halo_linux_source_fixups.h` | Repairs one declaration conflict (`rasterizer_debug_drawing_begin`). |

`tools/linux_link_check.py` stops the link if a weak reference has no
definition. Without this check, the linker gives the reference the address
0.

### The platform layer (`src/`)

- The files `posix_*.c` use glibc. The compiler uses the ABI of the host
  for these files, because some glibc structures have a different layout
  with `-malign-double`.
- The other files include the SDK declarations through `platform.h`. Thus
  the compiler examines each definition against the SDK prototype.
- `src/halo_linker_common.c` gives weak storage for some globals of the
  January link, and for `fast_ftol_C` and `main_crash`.
- `main/d3d_intimacy.cpp` reads a private structure of the Xbox Direct3D.
  The Linux build does not use this file. `src/d3d8_device.c` gives
  `d3d_find_flipcount`.
- The build returns small structures and unions in registers
  (`-freg-struct-return`), as on Win32.

### Game source changes

Five files of the game have changes for clang. These changes do not change
the MSVC objects: a comparison of all 612 C objects showed no difference in
code or data.

| File | Change |
| --- | --- |
| `cseries/cseries.c` | The naked function `stristr` uses `[ebp+8]` and `[ebp+12]` for its parameters. |
| `bitmaps/bitmap_drawing.c` | `*((word *)p)++` is now `*(*(word **)&p)++`. |
| `rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.c` | `&(T *)x` is now `(T **)&x`. |
| `hs/hs.c` | Local prototypes that did not agree with `ai_script.h` are removed. |
| `units/vehicles.c` | The local prototype of `unit_update_animation` uses the type of `units.h`. |

`math/real_math.h` had a copy of `plane2d_from_points` that did not agree
with the function in `effects/decals.c`. clang used the copy, and parts of
levels were not visible. The copy now agrees with the function.

Other changes:

| File | Change |
| --- | --- |
| `scenario/scenario.c` | The BSP connection tables have names, not MSVC offsets. |
| `rasterizer/xbox/rasterizer_xbox_environment_fog.c` | A local pointer gets its value from the file-scope array with the same name. |
| `game/player_control.c` | The mouse aims the player on controller 1 directly. |
| `sound/game_sound.c` | The game calculates the obstruction of each sound one time for each tick, not for each frame. |
| `sound/sound_manager.c` | A track of a looping sound that is gone stops, even in the millisecond it started (`refresh_sound`). The Xbox never drew three frames in one millisecond; the port can (the null renderer, no frame limit), and then read the deleted looping sound and crashed. |
| `cseries/errors.c` | `debug.txt` stays open between lines. |
| `networking/`, `game/`, `interface/`, `bungie_net/network/` and the pools of objects, effects and sounds | The system link limits and the memory for them. |
| `game/`, `objects/`, `units/`, `networking/` | The distributed netcode. Refer to `NETCODE.md`. |
| `cache/cache_files.c` | When a map's tags load and unload, the port finds the bitmaps that the high-res HUD replaces (`game/hud_hires_tags.c`), and adds the tags of the menus to the menus' map (`game/menu_tags.c`). |
| `interface/ui_widget.c`, `interface/ui_widget_event_handler_functions.c`, `interface/ui_widget_game_data_input_functions.c` | The main menu is the PC version's from `port/assets/menus` (`display.menus`); the menus' widgets can call the port's functions (`game/menu_functions.c`) and send the PC version's custom activation event; the widgets' memory is 256 KB, not 16 KB; the main menu and Multiplayer clear co-op's controllers, so that a gamepad going back to controller 1 is not a controller unplugged; the lobby's split screen players leave alone, and one who quits in game is not joined to the next game (`interface/player_ui.c`). |
| `interface/player_ui.c` | Fast setup opens the pregame screen through the port's menus (the lobby), as `interface/ui_widget.c` does. |
| `input/input_abstraction.c`, `game/player_control.c`, `game/players.c`, `game/player_queues_new.c`, `units/units.h` | The keyboard and mouse's actions (`src/xinput_sdl.c`, `include/halo_keyboard.h`) join controller 1's game controls; their reload key reloads on its own, and their action key only acts (a control flag of the port's, sent with the player's action, stops the reload the controller's X falls back to). |
| `saved games/player_profile.h`, `interface/player_ui.c` | A sixth button layout, the Master Chief Collection's Bumper Jumper (`profile.button_preset` 5). A layout the build does not know (a later build's) gets the standard buttons, not none, which moved start and back and stopped the game. |
| `sound/sound_manager.c`, `interface/hud.c`, `game/game_engine.c` | The music's and the other sounds' volumes; the HUD's and the scoreboard's settings are read again when Settings changes them. |
| `rasterizer/rasterizer.c` | The 128 KB of vertices of the debug display of a model's vertices are static, not on the stack: the compiler inlines the function in `render_model`, and the Windows build probed the stack a page at a time for every model drawn. |
| `interface/hud.c` | In multiplayer, players' names are drawn above their heads (`display.player_names`, `display.player_name_scale`). |
| `render/render_objects.c`, `objects/objects.c` | In first person on a flat view, your own body is drawn from the waist down with the pose `first_person_legs.c` makes (`display.first_person_legs`); the waist it found and the mesh `object_mesh.c` read are forgotten on a new map. `game/first_person_view.c` keeps it out of stereo frames. |
| `rasterizer/rasterizer_text.c`, `text/draw_string.c` | Text is drawn from an atlas of the fonts' glyphs, rasterized at the resolution the game draws at (`src/text_hires.c`), when the font has every character of the string. Text can be drawn scaled about a point (`rasterizer_text_set_scale`), as the players' names are. Each glyph's advance is centred on the font tag character's, so the layout is the same, and a glyph is cut at a text box only where the font tag's character visibly was. |
| `cutscene/cinematics.c` | A scenario's titles (some Custom Edition maps show their name for the whole game) are not drawn over a menu, as the letterbox is not: the screen's overlays are drawn after the players' menus. |
| `rasterizer/rasterizer.h` | A frame holds 2048 transparent geometry groups, not 384 (`include/halo_port_capacity.h`): a co-op game with many added enemies asked for more, and the Xbox's pool drew the frame without the rest. |
| `cache/cache_files.c`, `networking/network_client_manager.c`, `networking/network_server_manager.c`, `networking/network_server_message_handler.c`, `main/main.c`, `interface/interface.c` | Map sharing (`game/map_share.c`): a client that does not have the game's Custom Edition map asks the host for it instead of leaving, is not given time to precache it until it has it, and adds its players to a game in progress only then; the host gives the game's map its own name and its version (its hash's start), takes the requests from any machine in the game, lobby too, and does not drop a machine that downloads; the transfers run each frame, and their progress is drawn over the screen. |

The x86 inline assembly of the game is replaced by C. Thus the compiler
can optimize that code for each processor:

| File | Assembly | Replacement |
| --- | --- | --- |
| `cseries/cseries.h` | x87 `fistp` (`fast_ftol`) | `__builtin_rint` |
| `bitmaps/bitmaps_inlines.h` | x87 conversions | C conversions |
| `math/matrix_math.c` | SSE `matrix4x3_multiply` | a C loop |
| `effects/decals.c` | an x87 conversion | a C conversion |
| `cseries/profile.c` | `rdtsc` | `QueryPerformanceCounter` |
| `cseries/cseries.c` | naked `stristr` | a C `stristr` |
| `cseries/stack_walk_windows.c` | a read of EBP | `__builtin_frame_address` |
| `interface/hud_draw.c` | a read of `[ebp+4]` | `__builtin_return_address(1)` |
| `bink/bink_playback.c` | `int 3` | `__builtin_trap` |

The x87 control and status words (`_control87`, `_statusfp`, `_clearfp` in
`src/msvc_crt.c`) use `fenv.h`. On Android, they use the FPCR and FPSR.
