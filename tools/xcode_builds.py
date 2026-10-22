"""Xcode builds a release accepts. The `xcode-27` runner fleet serves more than one image, so the
release jobs select whichever of these the image has (`tools/xcode_select.py`) and the release tools
check the actual build against this list."""

# Newest first. Both carry the 27.2 SDKs.
XCODE_BUILDS = ("27B5038j", "27B5028f")
