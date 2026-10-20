"""Check that the tested zoom ray reaches the headset reticle quad."""

from harness import function, read


def in_order(body, *calls):
    positions = [body.find(call) for call in calls]
    return all(position >= 0 for position in positions) and positions == sorted(positions)


render = function(read("source/render/render.c"), "render_player_frame_stereo")
assert in_order(
    render,
    "halo_stereo_zoom_orient(&zoom_camera.forward.i",
    "halo_stereo_head_orient(&camera->forward.i",
    "zoomed = halo_stereo_zoom_begin(",
    "if (halo_zoom_ordinary_aim_ray(window->local_player_index, aim))",
    "halo_stereo_zoom_reticle(aim, &zoom_camera.forward.i, &zoom_camera.up.i, magnification)",
    "for (eye = first_pass; eye < pass_end; eye++)",
), "zoom reticle must use the picture's pre-eye camera and ordinary player ray before drawing"

present = function(read("port/linux/src/d3d8_device.c"), "D3DDevice_Present")
assert in_order(
    present,
    "halo_stereo_reticle(present.reticle);",
    "stereo_reticle_aimed(&present);",
    "gpu_present_stereo(&present)",
), "the headset present must read the projected reticle before submission"

metal = read("port/ios/host/gpu_metal.m")
assert "present->hud_ui, present->reticle, &aim," in metal, "Metal must forward the same reticle"
headset = function(read("port/ios/host/host_stereo.m"), "host_stereo_present")
assert "host_stereo_hud_layout_aimed(layout_width, hud_ui, reticle," in headset, \
    "the headset must place the forwarded reticle in the HUD plane"

print("stereo camera wiring: PASS")
