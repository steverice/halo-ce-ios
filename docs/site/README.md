Edit `index.md`, then run `python3 tools/build_site.py`.
Commit both `index.md` and the generated `index.html`.
Install the build dependencies from `tools/requirements-ci.txt` first.

Add a device frame to a standalone gameplay image with `{device=iphone}`, `{device=ipad}`, `{device=tv}` or `{device=mac}`:

```md
![Halo on iPhone with touch controls](./iphone.jpg){device=iphone}
```

Use local PNG or JPEG images with descriptive alt text. The build reads their dimensions and groups framed images beside their section's text. Images stack below the text on narrow screens.
