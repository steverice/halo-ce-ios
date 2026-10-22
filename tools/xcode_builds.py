"""Xcode builds a release accepts. The `xcode-27` runner fleet serves more than one image, so the
release jobs select whichever of these the image has (`tools/xcode_select.py`) and the release tools
check the actual build against this list."""

# Newest first. All carry the 27.2 SDKs; App Store Connect accepts uploads only from the RC, 27B48.
XCODE_BUILDS = ("27B48", "27B5038j", "27B5028f")
