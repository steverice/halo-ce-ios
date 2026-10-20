# Cost model files

`tools/profile_cost_model.py` writes both files. Edit them only through the tool.

`runs.csv`: one row per benchmark run, keyed by (`report`, `index`). Columns: `report`, `index`, `device` (hardware model identifier), `chip`, `workload` (catalog scene id), `settings` (JSON object of setting name to value), `render_width`, `render_height`, `simulation_cpu_ms`, `render_cpu_ms`, `gpu_ms` (run means), `thermal` (worst thermal state, 0 nominal to 3 critical), `timing_basis`.

`model.toml`: one `[[fit]]` per device and workload.
- `baseline`: the setting values the fit is relative to.
- `simulation_ms`: simulation CPU time per 1/30 s tick.
- `render_cpu_ms`: render CPU time per frame.
- `gpu_fixed_ms` and `gpu_ms_per_megapixel`: GPU time per frame is `gpu_fixed_ms + gpu_ms_per_megapixel * megapixels`.
- `[[fit.knob]]`: `cpu_factor` and `gpu_factor` for one knob at one value, relative to the baseline.
- `excluded`: counts of rows left out: `thermal` (Serious or Critical), `timing_basis` (not `offscreen_uncapped`), `gpu_zero` (no GPU time reported; skipped for the GPU fit only).
- `correction`: multiplier from a real-time check when the prediction is off by more than 10%.

Predicted frame time at a target rate: `max(render_cpu_ms + simulation_ms * 30 / hz, gpu_ms) * correction`.
