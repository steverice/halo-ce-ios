# VISR roadmap

## VR

- [x] Stereo with head tracking through Compositor Services (`display.stereo = "head"`)
- [x] Stick turning: snap (default), smooth or off, with an optional comfort vignette (`input.turn`, `input.snap_angle`, `input.smooth_turn_speed`, `input.comfort_vignette`)
- [x] Foveated rendering through the Compositor's rate maps, on by default (`display.foveation`)
- [x] A default render quality chosen from a headset sweep (`display.render_quality`)
- [x] The HUD split by what draws each piece, with the crosshair on its own layer
- [x] The HUD in the periphery, in CE's own corners, turning with the body only (`display.hud_scale`, `display.hud_resolution`)
- [x] The HUD drawn in front of what it covers, at a set distance (`display.hud_depth_floor`, `display.hud_depth_share`, `display.hud_distance`)
- [x] Scope zoom fills the view
- [x] Cutscenes as a 3D film on a 16:9 screen, opening out to the world at the end
- [x] Vehicles stay immersive in third person
- [x] The paused scene stays fixed in the room, and menus get their own layer
- [x] The sky at infinity in each eye, and mirror reflections per eye
- [x] Model detail chosen for the headset's pixels (`display.model_lod`)
- [x] A separate random seed for rendering, so render settings don't change the game
- [x] The first-person weapon lowered for the headset's taller view
- [x] The first-person body drawn below the view (`display.first_person_body_offset`)
- [x] The first-person body in vehicle seats, and depth clamping on the body
- [ ] Immersive cutscenes, with the scene outside the director's frame blurred
- [x] Stereo on the theater screen, as a 3D TV (`display.stereo = "screen"`)
- [x] 2D play in a window at the window's resolution
- [x] 2D play on a screen in your room on visionOS 26 (`display.immersive`, `display.theater_width`, `display.theater_distance`, `display.theater_environment`)

## Controls

- [x] Any game controller, plus keyboard navigation in the menus
- [ ] Aiming with tracked PlayStation VR2 Sense controllers: first-person seats aim with the hand, third-person seats with the stick
- [ ] The game's rumble on the Sense controllers
- [ ] Aim assist and magnetism kept for hand aiming, tunable in single player and fixed in multiplayer

## Performance

- [x] CPU savings in the Metal draw path, and visibility tests that don't wait for the GPU
- [x] Relaxed-math pixel shaders
- [x] The Compositor paces stereo frames (`display.frame_repeat`)
- [x] A frame rate cap, or no cap, in the Mac app (`display.frame_rate`: `"display"`, a number or `"unlimited"`; set in `config.toml`, not yet a row in the settings screens)
- [x] Shader and pipeline warm-up at map load
- [x] Device profiles: graphics settings chosen per device model or class, with the main menu and cutscenes budgeted apart from play (`profiles/`, `debug.device_profile`)
- [ ] Profile knobs still to build: a shadow budget, a level-of-detail bias outside stereo, anti-aliasing, a frame-rate target for a scene class, pixel math as a setting
- [x] Choose the strongest compatible measured hardware profile at or below the current device, with exact profiles taking precedence and unknown hardware retaining its class fallback
- [ ] Signed profile-data updates independent of app releases, with compatible snapshots, offline defaults, and rollback
- [x] A benchmark testers can run from the app, whose report a profile is derived from (tester builds; `debug.benchmark_suite`)
- [x] A recent render-target cache (a port of upstream's `197c1994`)

## Graphics

- [x] Rendering at the display's native resolution, with MetalFX upscaling (`display.upscaler`, `display.render_scale`)
- [x] Compressed textures, 16x anisotropic filtering and reversed-Z depth
- [x] Upstream's redrawn high-resolution HUD and text
- [ ] Upstream's anti-aliasing (FXAA, SMAA, MSAA, SSAA), shadow resolution and per-pixel lighting on Metal
- [ ] Mirror reflections at full resolution by default (`display.mirror_resolution`)
- [ ] AI-upscaled textures
- [x] The intro and credits movies (Bink video, decoded with FFmpeg; `display.movies`)
- [ ] The attract movies
- [x] Your own legs when you look down on a flat screen, from OpenCE (`display.first_person_legs`, off by default; set it in `config.toml`: its menu row comes with OpenCE's FOV and Viewmodels screen, not ported yet). The headset draws the whole body instead (`display.first_person_body`)

## Multiplayer

- [x] Split-screen co-op, LAN play, and internet play with a server browser, from OpenCE (`network.online`)
- [x] `halo://join` invite links on every Apple platform
- [x] A grace period that keeps a network game running while the app is in the background (`network.background_grace`)
- [ ] LAN discovery over Bonjour: advertise a Bonjour service and relay the game's UDP discovery broadcasts to the resolved peers, so iPhone, iPad and Apple TV find local games without the multicast networking entitlement
- [ ] Sharing the host's invite from the app
- [ ] Internet play on by default
- [x] Custom Edition maps downloaded from a game's players, from OpenCE; off by default (`network.map_download`, DOWNLOAD MAPS in Settings > Network Setup)
- [x] Helping send a Custom Edition map to other players is off by default (`network.map_seed`, HELP SEND MAPS in Settings > Network Setup)
- [ ] A question with the map's name and size before a map download starts, then downloads on by default

## App

- [ ] An in-game settings screen for these options
