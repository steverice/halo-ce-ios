import pytest

from tools.release_signing import XcodeManagedProfileError, reject_xcode_managed_profile


def test_rejects_xcode_managed_profile() -> None:
    profile = {"Name": "Synthetic Xcode-managed sample", "IsXcodeManaged": True}

    with pytest.raises(
        XcodeManagedProfileError, match="Xcode-managed.*manually managed profile"
    ) as error:
        reject_xcode_managed_profile(profile, "ios")

    assert profile["Name"] not in str(error.value)


def test_accepts_manually_managed_profile() -> None:
    reject_xcode_managed_profile({"IsXcodeManaged": False}, "ios")


def test_missing_marker_keeps_existing_validation_behavior() -> None:
    reject_xcode_managed_profile({}, "ios")


def test_profile_name_does_not_override_false_marker() -> None:
    profile = {"Name": "Xcode-managed sample", "IsXcodeManaged": False}

    reject_xcode_managed_profile(profile, "ios")
