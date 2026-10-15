"""Tests for tools/ios_build.py's pure helpers."""

import pytest

from tools import ios_build

# the iPhoneOS SDK's OpenGLES.tbd as Xcode 27 ships it, cut down to two symbols
STUB = """--- !tapi-tbd
tbd-version:     4
targets:         [ arm64e-ios, arm64e.x1-ios ]
install-name:    '/System/Library/Frameworks/OpenGLES.framework/OpenGLES'
exports:
  - targets:         [ arm64e-ios, arm64e.x1-ios ]
    symbols:         [ _EAGLGetVersion, _glActiveTexture ]
...
"""


def test_the_stub_links_against_the_mac_s_maccatalyst_opengles():
    stub = ios_build.maccatalyst_stub(STUB)
    assert stub.count("[ arm64-maccatalyst, arm64e-maccatalyst ]") == 2
    assert "-ios" not in stub
    assert "install-name:    '/System/iOSSupport/System/Library/Frameworks/OpenGLES.framework/OpenGLES'" in stub
    assert "_EAGLGetVersion, _glActiveTexture" in stub


def test_a_stub_with_other_targets_is_refused():
    with pytest.raises(ValueError, match="does not list"):
        ios_build.maccatalyst_stub(STUB.replace("arm64e.x1-ios", "arm64e.x2-ios"))


def test_a_stub_with_another_install_name_is_refused():
    with pytest.raises(ValueError, match="install name"):
        ios_build.maccatalyst_stub(STUB.replace("/System/Library/Frameworks/OpenGLES", "/usr/lib/OpenGLES"))


def test_a_stub_naming_an_ios_target_the_rewrite_misses_is_refused():
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(STUB + "uuids:\n  - target: arm64e-ios\n")


def test_the_catalyst_build_goes_in_the_checkout_by_default():
    assert ios_build.mac_build_folder({}) == ios_build.ROOT / "build/mac/app"


def test_a_host_s_designated_folder_takes_the_catalyst_build(tmp_path):
    assert ios_build.mac_build_folder({"HALO_MAC_BUILD": str(tmp_path)}) == tmp_path / ios_build.ROOT.name / "app"


def test_an_app_store_package_is_archived_then_exported_and_uploads_only_when_asked(tmp_path):
    import plistlib
    archive, export = ios_build.app_store_commands(tmp_path / "build", "tvOS", "TEAM123456", tmp_path / "out", False)
    assert archive[0] == "xcodebuild" and archive[-1] == "archive" and "generic/platform=tvOS" in archive
    assert "-allowProvisioningUpdates" in archive and "-exportArchive" in export
    options = plistlib.loads((tmp_path / "out" / "ExportOptions.plist").read_bytes())
    assert options["method"] == "app-store-connect" and options["destination"] == "export" and options["teamID"] == "TEAM123456"
    # the build stamps its own version: Xcode must not renumber it
    assert options["manageAppVersionAndBuildNumber"] is False
    ios_build.app_store_commands(tmp_path / "build", "tvOS", "TEAM123456", tmp_path / "out", True)
    assert plistlib.loads((tmp_path / "out" / "ExportOptions.plist").read_bytes())["destination"] == "upload"


def test_cloud_saves_are_off_unless_asked_for():
    assert ios_build.cloud_definitions("org.steverice.visr", False, None) == [
        "-DHALO_CLOUD_SAVES=OFF", "-DHALO_BENCHMARK_UPLOAD=OFF"]


def test_the_project_s_bundle_ids_share_its_container():
    for bundle_id in ("org.steverice.visr", "org.steverice.visr.tvos", "org.steverice.visr.visionos", "org.steverice.visr.mac"):
        assert ios_build.cloud_definitions(bundle_id, True, None) == [
            "-DHALO_CLOUD_SAVES=ON", "-DHALO_CLOUD_CONTAINER=iCloud.org.steverice.visr", "-DHALO_BENCHMARK_UPLOAD=OFF"]


def test_another_bundle_id_must_name_its_own_container():
    with pytest.raises(ValueError, match="needs --cloud-container"):
        ios_build.cloud_definitions("com.example.visr", True, None)
    with pytest.raises(ValueError, match="needs --cloud-container"):
        ios_build.cloud_definitions("org.steverice.visrx", True, None)
    assert ios_build.cloud_definitions("com.example.visr", True, "iCloud.com.example.visr")[1] == \
        "-DHALO_CLOUD_CONTAINER=iCloud.com.example.visr"


def test_a_container_that_is_not_an_icloud_identifier_is_refused():
    for container in ("com.example.visr", "iCloud.", "iCloud.visr", "iCloud.com.example.visr;rm"):
        with pytest.raises(ValueError, match="must look like"):
            ios_build.cloud_definitions("com.example.visr", True, container)


def test_a_container_without_the_option_is_refused():
    with pytest.raises(ValueError, match="for --cloud-saves builds"):
        ios_build.cloud_definitions("org.steverice.visr", False, "iCloud.org.steverice.visr")


def test_benchmark_upload_takes_the_container_without_turning_cloud_saves_on():
    assert ios_build.cloud_definitions("org.steverice.visr.tvos", False, None, True) == [
        "-DHALO_CLOUD_SAVES=OFF", "-DHALO_CLOUD_CONTAINER=iCloud.org.steverice.visr", "-DHALO_BENCHMARK_UPLOAD=ON"]
    assert ios_build.cloud_definitions("org.steverice.visr.tvos", True, None, True)[0] == "-DHALO_CLOUD_SAVES=ON"
    with pytest.raises(ValueError, match="needs --cloud-container"):
        ios_build.cloud_definitions("com.example.visr", False, None, True)
