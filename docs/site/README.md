Edit `index.md` for prose and `docs/features.toml` for shared facts, then run
`python3 tools/build_site.py`. This updates `README.md`'s marked blocks and
`docs/site/index.html` together. Commit the sources and generated files.
Install the build dependencies from `tools/requirements-ci.txt` first.
`python3 tools/build_site.py --check` reports stale outputs without writing;
the portable suite runs this check too. Python 3.11 or newer is required.

Include a platform's bullets with a standalone line:

```md
{{ features: iphone-and-ipad }}
```

The other platform names are `vision-pro`, `apple-tv` and `mac`.
`{{ shared: every-device }}` includes the common features and the cloud-save
conditions and exceptions from the facts file. The legal footer also reads
from that file. Minimum OS versions are resolved from `tools/ios_build.py` and, for the
native AppKit Mac app, `tools/macos_build.py`. The iOS requirement in the
feature bullets uses that version instead of a second handwritten number.
The other deployment versions and device lists remain available as facts
metadata without adding requirements to the site copy.

README blocks use `<!-- generated: features: apple-tv -->` and
`<!-- end generated -->` around generated content. Edit the facts file to
change those blocks; the build preserves prose outside them. The `copy`
section temporarily preserves existing README/site wording differences for
review rather than resolving them during generation.

Add a device frame to a standalone gameplay image with `{device=iphone}`, `{device=ipad}`, `{device=tv}` or `{device=mac}`:

```md
![Halo on iPhone with touch controls](./iphone.jpg){device=iphone}
```

Use local PNG or JPEG images with descriptive alt text. The build reads their dimensions and groups framed images beside their section's text. Images stack below the text on narrow screens.
