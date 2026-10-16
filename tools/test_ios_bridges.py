"""tools/ios_bridges.py: the host side of guest imports"""
import pytest

from tools import ios_bridges


def test_host_gpu_import_calls_the_backend_function():
    name, source = ios_bridges.bridge("uint32_t", "host_gpu_draw",
                                      "const struct gpu_draw *draw, const struct gpu_constant_store *constants, "
                                      "const struct gpu_uniforms *uniforms")
    assert name == "host_gpu_draw"
    assert source.startswith("static uint32_t ios_bridge_host_gpu_draw(uint64_t a0, uint64_t a1, uint64_t a2) {")
    assert "return gpu_draw((const struct gpu_draw *)host_pointer(a0), " \
           "(const struct gpu_constant_store *)host_pointer(a1), " \
           "(const struct gpu_uniforms *)host_pointer(a2));" in source
    assert "host_gpu_draw(" not in source.split("{", 1)[1]


def test_other_host_imports_call_themselves():
    name, source = ios_bridges.bridge("void", "host_log", "int priority, const char *text")
    assert name == "host_log"
    assert "host_log(a0, (const char *)host_pointer(a1));" in source


def test_posix_imports_keep_their_prefix_and_limit():
    name, source = ios_bridges.bridge("int", "posix_close", "int fd")
    assert name == "hostposix_close"
    assert "return posix_close(a0);" in source
    # posix_socket_select takes nine; its guest stub (tools/guest_posix_stubs.py) is its own contract
    ios_bridges.bridge("int", "posix_nine", ", ".join(f"int a{index}" for index in range(9)))


def test_more_than_eight_integer_or_pointer_arguments_is_an_error():
    parameters = ", ".join(f"unsigned int a{index}" for index in range(9))
    with pytest.raises(ValueError, match="host_too_many"):
        ios_bridges.bridge("void", "host_too_many", parameters)


def test_eight_arguments_and_floats_are_allowed():
    parameters = ", ".join(f"unsigned int a{index}" for index in range(8)) + ", float extra"
    ios_bridges.bridge("void", "host_eight", parameters)


def test_render_validation_imports_bridge_to_the_validator():
    name, source = ios_bridges.bridge("void", "host_gpu_validation_identity",
                                      "const struct gpu_validation_identity *identity")
    assert name == "host_gpu_validation_identity"
    assert "gpu_validation_identity((const struct gpu_validation_identity *)host_pointer(a0))" in source
    name, source = ios_bridges.bridge("void", "host_gpu_validation_note", "uint32_t reason, uint32_t detail, uint32_t value")
    assert "gpu_validation_note(a0, a1, a2)" in source


def test_replay_touch_notification_preserves_the_integer_flag():
    name, source = ios_bridges.bridge("void", "host_touch_replay_active", "int active")
    assert name == "host_touch_replay_active"
    assert "host_touch_replay_active(a0);" in source
