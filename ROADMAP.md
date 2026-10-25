# VISR roadmap

## VR

- [x] Stereo with head tracking through Compositor Services (`display.stereo = "head"`)
- [x] Stick turning: snap (default), smooth or off, with an optional comfort vignette (`input.turn`, `input.snap_angle`, `input.smooth_turn_speed`, `input.comfort_vignette`)
- [x] Foveated rendering through the Compositor's rate maps, on by default (`display.foveation`)
- [x] A default render quality chosen from a headset sweep (`display.render_quality`)
- [x] The HUD split by what draws each piece, with the crosshair on its own layer
- [x] The HUD in the periphery, in CE's own corners, turning with the body only (`display.hud_scale`, `display.hud_resolution`)
- [x] The HUD drawn in front of what it covers, with a set resting distance (`display.hud_depth`, `display.hud_depth_floor`, `display.hud_depth_share`, `display.hud_distance`)
- [x] Scope zoom fills the view
- [x] Cutscenes as a 3D film on a 16:9 screen, opening out to the world at the end
- [x] Vehicles stay immersive in third person
- [x] The paused scene stays fixed in the room, and menus get their own layer
- [x] The sky at infinity in each eye, and mirror reflections per eye
- [x] Model detail chosen for the headset's pixels (`display.model_lod`)
- [x] A separate random seed for rendering, so render settings don't change the game
- [x] An adjustable first-person weapon height for the headset's taller view (`display.weapon_offset_down`)
- [x] The first-person body drawn below the view (`display.first_person_body_offset`)
- [x] The first-person body in vehicle seats, and depth clamping on the body
- [ ] Immersive cutscenes, with the scene outside the director's frame blurred
- [x] Stereo on the theater screen, as a 3D TV (`display.stereo = "screen"`)
- [x] 2D play in a window at the window's resolution
- [x] 2D play on a screen in your room on visionOS 26 (`display.immersive`, `display.theater_width`, `display.theater_distance`, `display.theater_environment`)

## Controls

- [x] Any game controller, plus keyboard navigation in the menus
- [x] Touch controls on iPhone and iPad, sharing player one with a connected game controller
- [x] Aiming with tracked PlayStation VR2 Sense controllers on visionOS 27: local first-person seats aim with the hand; third-person seats and all network seats use the stick (`input.aim`, `input.aim_hand`)
- [x] The game's rumble on the Sense controllers (`input.controller_rumble`)
- [x] Bullet aim assist kept for hand aiming, tunable in single player and fixed in multiplayer (`input.aim_assist`)

## Performance

- [x] CPU savings in the Metal draw path, and visibility tests that don't wait for the GPU
- [x] Relaxed-math pixel shaders
- [x] The Compositor paces stereo frames (`display.frame_repeat`)
- [x] A frame rate cap on iPhone, iPad, Apple TV and Mac, with an unlimited option on Mac (`display.frame_rate`: `"display"`, a quoted number or `"unlimited"`; set in `config.toml`, or the Mac app's Video Setup screen)
- [x] Shader and pipeline warm-up at map load
- [x] Device profiles: graphics settings chosen per device model or class, with the main menu and cutscenes budgeted apart from play (`profiles/`, `debug.device_profile`)
- [x] Device profiles can set shadow resolution and anti-aliasing, plus separate frame-rate targets for the main menu, cutscenes and play (`display.shadow_map_size`, `display.anti_aliasing`, `display.frame_rate`)
- [ ] Profile knobs still to build: a shadow budget, a level-of-detail bias outside stereo, pixel math as a setting
- [x] Choose the strongest compatible measured hardware profile at or below the current device, with exact profiles taking precedence and unknown hardware retaining its class fallback
- [x] Signed profile-data updates independent of app releases, with compatible snapshots, offline defaults, and rollback
- [x] A benchmark testers can run from the app, with live timing and display-variant runs, whose report a profile is derived from (tester builds; `debug.benchmark_suite`)
- [x] A recent render-target cache (a port of upstream's `197c1994`)
- [x] Consecutive immediate-mode draws merged when their state matches, and collision queries reuse nearby candidates
- [x] Undrawn textures released after a device-specific idle period and on memory pressure (`display.texture_idle_seconds`)
- [x] Distant clutter can be left undrawn by a device profile without changing the game (`display.clutter_distance`, `0` draws it all)
- [x] Device profiles can cap Apple TV's built-in output, including 720 lines on Apple TV 4K (1st generation)

## Graphics

- [x] Rendering at the display's native resolution, with MetalFX upscaling (`display.upscaler`, `display.render_scale`)
- [x] Compressed textures, 16x anisotropic filtering and reversed-Z depth
- [x] Upstream's redrawn high-resolution HUD and text
- [x] FXAA and SMAA on flat Metal views, and adjustable shadow resolution (`display.anti_aliasing`, `display.shadow_map_size`)
- [ ] MSAA, SSAA and per-pixel lighting on Metal
- [x] Mirror reflections and active camouflage at Xbox, half or full resolution (`display.effect_resolution`: `"xbox"`, `"half"` by default, or `"full"`)
- [ ] Mirror reflections at full resolution by default
- [ ] AI-upscaled textures
- [x] The intro and credits movies (Bink video, decoded with FFmpeg; `display.movies`)
- [x] Your own legs when you look down on a flat screen, from OpenCE (`display.first_person_legs`, off by default; set it in `config.toml`). The headset draws the whole body instead (`display.first_person_body`)

## Multiplayer

- [x] Split-screen co-op, LAN play, and internet play with a server browser, from OpenCE (`network.online`)
- [x] Delta's public games in the server browser, with joins to network versions 29 through 32; optional host announcements are off by default (`network.delta_browse`, `network.delta_announce`)
- [x] `halo://join` invite links on every Apple platform
- [x] A grace period before other players drop an internet game's backgrounded app (`network.background_grace`, 60 seconds by default)
- [x] LAN discovery over Bonjour on iPhone, iPad and Vision Pro, relaying UDP discovery broadcasts to resolved peers without the multicast networking entitlement
- [ ] LAN discovery over Bonjour on Apple TV
- [x] Sharing the host's invite through the system share sheet on iPhone, iPad and Vision Pro
- [ ] Sharing the host's invite from Apple TV
- [ ] Internet play on by default
- [x] Custom Edition maps downloaded from a game's players, from OpenCE; off by default (`network.map_download`, DOWNLOAD MAPS in Settings > Network Setup)
- [x] Helping send a Custom Edition map to other players is off by default (`network.map_seed`, HELP SEND MAPS in Settings > Network Setup)
- [ ] A question with the map's name and size before a map download starts, then downloads on by default
- [ ] Network version 36 compatibility for 0.5.0: implement 33 (co-op ally friendly fire) and 34 (scenery shields and tripmines); accept 35's options format while refusing its new gametypes, and accept 36's shield-effect messages while drawing and hosting stock effects

## App

- [ ] An in-game settings screen for these options
- [x] Wired external-display play on iPhone and iPad with Metal, keeping touch controls on the device
- [ ] AirPlay support for 0.5.0
