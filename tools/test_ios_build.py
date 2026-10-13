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
