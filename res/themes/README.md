# Bundled themes

Every directory here is a theme for the USB LCD screens (see
[`docs/THEMES.md`](../../docs/THEMES.md)):

```
<theme>/theme.yaml        layout + stats, read as-is
<theme>/background.png    background image
<theme>/preview.png       preview used by upstream tooling
<theme>/theme_example.png screenshot of the rendered theme
```

## Provenance and licence

Except `demo-320x480`, the themes are copied **unmodified** from
[turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python)
(`res/themes/`), which is distributed under **GPL-3.0**; the per-theme `author`
field in each `theme.yaml` names the contributor. They are included here as
theme *data* for the GPL-3.0-or-later C++ renderer that reads them.

`demo-320x480` is written for this project (same licence, no third-party art).

## What is bundled

Only the themes whose background is **320x480** — the resolution of the
hardware this project is validated on (revision A: Turing 3.5", UsbPCMonitor
3.5"). That keeps the repository small (~2 MB) and the self test fast.

## Importing more

```sh
# another resolution (480x800, 800x480, 1920x480, ...)
tools/import-themes.py --source ~/src/turing-smart-screen-python --size 480x800

# everything (~56 MB, all resolutions)
tools/import-themes.py --source ~/src/turing-smart-screen-python --all

# what would be copied
tools/import-themes.py --source ~/src/turing-smart-screen-python --all --dry-run
```

The tool also copies the font families the imported themes reference (see
[`../fonts/README.md`](../fonts/README.md)). Themes never overwrite
`demo-320x480`.

Alternatively, point the tools at an external checkout and copy nothing:

```sh
./build/lcd-monitor --theme 8inchTheme2 \
    --root ~/src/turing-smart-screen-python/res/themes \
    --fonts ~/src/turing-smart-screen-python/res/fonts
```
