# Profiling builds

A profiling build records where the game spends its CPU time. A recording is a
set of Chrome trace files that [Perfetto](https://ui.perfetto.dev) opens as a
timeline, and that `tools/net_report.py` reads as tables. Normal builds are
not affected: the recorder, its settings and every scope compile to nothing
without the profiling switch (`tools/test_profile.py` checks that the changed
sources preprocess to the same tokens as before).

## Building

| Build | Command |
| --- | --- |
| Linux, Windows | `python3 configure.py --profile`, then build as usual |
| iPhone, iPad | `python3 tools/ios_build.py --profile --team TEAM` (or `--unsigned`) |
| Apple TV | `python3 tools/ios_build.py --profile --tvos --team TEAM` |
| Mac (Catalyst runner) | `python3 tools/ios_build.py --profile --mac` |
| Mac (AppKit) | `python3 tools/ios_build.py --profile --native-mac` |

On the Apple builds the switch applies to the game, which runs as the guest
image; the host app is the same as a normal build's. `--profile` cannot be
combined with `--app-store-package`, and a profiling build does not train
optimization profiles (`--pgo=train`).

## Recording

Set the recording up in `config.toml`:

```toml
[debug]
profile_record = true        # record without a command
profile_record_when = "game" # "start": from the first frame until the first map change;
                             # "game": each game that is not the main menu, one recording each
profile_memory = 256         # megabytes kept in memory, 4 to 1024
```

The console (and `init.txt`) also take `profile_record [seconds]` and
`profile_stop`. A recording stops at `profile_stop`, after its seconds, when a
map loads, or when the game quits. It has no length limit: each time half of
its memory fills, that half is written as a numbered part.

Parts are written to `profiles/` in the game's data folder, as
`profile_<time>_<role>.part<n>.json`:

| Platform | Data folder |
| --- | --- |
| iPhone, iPad | the app's Documents folder |
| Apple TV | the app's `Library/Caches/Halo` folder |
| Mac (Catalyst, AppKit) | `HALO_DATA_ROOT`, else `Application Support/<bundle ID>` (inside the app's container when it is sandboxed) |
| Linux, Windows | the data root (`paths.data`) |

To copy a device's parts to a Mac:

```bash
xcrun devicectl device copy from --device DEVICE_ID --domain-type appDataContainer \
    --domain-identifier org.steverice.visr --source Documents/profiles --destination ./profiles
```

On an Apple TV the source is `Library/Caches/Halo/profiles`. `tools/mac_run.py`
copies a Mac run's parts into its output folder's `profiles/`.

## Reading

```bash
python3 tools/net_report.py profiles/profile_<time>_<role>.part1.json
```

The report finds the recording's other parts, prints its sections as tables
(count, total, mean and longest time, calls a frame, time a game tick) and the
slowest frames, and writes `<recording>.summary.txt`. Open a part in Perfetto
for the timeline.

## Sections

The game's own profile sections (`source/cseries/profile.c`), the main loop's
steps, each game tick's steps and the network tick's steps are scopes of the
game thread's track; the internet play thread has a track of its own. Names
that run many times a frame are summed a frame rather than recorded one by one;
their row's longest time is the largest sum in one frame.

The port adds:

| Section | What it times |
| --- | --- |
| `gpu_draw`, `gpu_clear`, `gpu_texture`, `gpu_buffer`, `gpu_shader_create`, `gpu_visibility` (an occlusion query's start and end), `gpu_visibility_result` (reading one back), `gpu_pipeline_warm` (summed a frame) | each call into the GPU backend (`gpu.h`), from the call until the backend returns; on the Apple builds, the Metal backend's encoding in the host |
| `gpu_frame_start`, `gpu_present`, `gpu_flush`, `gpu_upscale`, `gpu_anti_alias`, `gpu_warm`, `gpu_initialize` | the backend's frame start, presentation and the other whole-frame calls |
| `d3d8_draw_vertices`, `d3d8_draw_indexed_vertices`, `d3d8_draw_quad_ranges`, `d3d8_end`, `d3d8_clear`, `d3d8_set_vertex_shader_constant`, `d3d8_set_render_target` (summed a frame) | the Direct3D 8 front end's outermost calls (`d3d8_device.c`), translation and backend call together; less the `gpu_*` time inside them, the translation's own |
| `d3d8_immediate_flush` (summed a frame) | a held immediate-mode draw sent when the next call needs it; inside that call's own row |
| `object_update.type`, `.damage`, `.export_functions`, `.node_matrices`, `.functions`, `.change_colors`, `.lights`, `.postprocess` (summed a tick) | the parts of each object's update (`objects.c`); `.type` holds the per-type updates (`biped_update` and the rest) |
| `objects_update.activation`, `objects_update.created_deleted`, `objects_update.garbage` | `objects_update`'s scan for objects to activate and deactivate, its pass over created and deleted objects, and its garbage collection |
| `benchmark_suite_update` | a benchmark suite's actions in the main loop |
| `port_network_update` | the network tests, map sharing, away players and aim assist before the network frame |
| `network_distributed_tick.sync_log` | the network sync log |

The Apple builds read the CPU's counter register for the recording's clock,
rather than asking the host for the time.
