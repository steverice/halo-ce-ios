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


def cloud_profile(aps_environment=None):
    entitlements = {
        "com.apple.developer.kernel.extended-virtual-addressing": True,
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
    }
    if aps_environment is not None:
        entitlements["aps-environment"] = aps_environment
    return {"Entitlements": entitlements}


def test_cloudkit_profile_validation_is_required_only_for_uploads() -> None:
    from tools.release_signing import validate_profile_entitlements

    profile = {"Entitlements": {"com.apple.developer.kernel.extended-virtual-addressing": True}}
    validate_profile_entitlements(profile, "ios")
    with pytest.raises(ValueError, match="CloudKit"):
        validate_profile_entitlements(profile, "ios", benchmark_upload=True)


def test_upload_profile_requires_exact_cloudkit_container_and_production_aps() -> None:
    from tools.release_signing import validate_profile_entitlements

    validate_profile_entitlements(cloud_profile("production"), "ios", benchmark_upload=True)
    for profile in (
        cloud_profile(),
        cloud_profile("development"),
        {**cloud_profile("production"), "Entitlements": {
            **cloud_profile("production")["Entitlements"],
            "com.apple.developer.icloud-container-identifiers": ["iCloud.example.other"],
        }},
        {**cloud_profile("production"), "Entitlements": {
            **cloud_profile("production")["Entitlements"],
            "com.apple.developer.icloud-services": ["CloudDocuments"],
        }},
    ):
        with pytest.raises(ValueError):
            validate_profile_entitlements(profile, "tvos", benchmark_upload=True)


def test_profile_capability_error_is_safe_to_report() -> None:
    from tools.release_signing import ProfileCapabilityError, validate_profile_entitlements

    with pytest.raises(ProfileCapabilityError, match="production APS"):
        validate_profile_entitlements(cloud_profile(), "tvos", benchmark_upload=True)


def test_upload_profile_accepts_ios_cloudkit_service_wildcard() -> None:
    from tools.release_signing import validate_profile_entitlements

    profile = cloud_profile("production")
    profile["Entitlements"]["com.apple.developer.icloud-services"] = "*"
    validate_profile_entitlements(profile, "ios", benchmark_upload=True)


@pytest.mark.parametrize("field,value", [
    ("com.apple.developer.icloud-services", []),
    ("com.apple.developer.icloud-container-identifiers", ["iCloud.example.other"]),
    ("aps-environment", "development"),
])
def test_cloud_saves_profile_requires_cloudkit_container_and_production_aps(field, value):
    from tools.release_signing import validate_profile_entitlements

    profile = cloud_profile("production")
    validate_profile_entitlements(profile, "ios", cloud_saves=True)
    profile["Entitlements"][field] = value
    with pytest.raises(ValueError):
        validate_profile_entitlements(profile, "ios", cloud_saves=True)
