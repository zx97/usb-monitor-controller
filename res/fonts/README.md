# Bundled fonts

Themes reference a font by `family/file.ttf` (for example
`jetbrains-mono/JetBrainsMono-Bold.ttf`). `lcdctl` resolves that path against
this directory first, then the theme directory, then the default font
(`RobotoMono-Regular.ttf`, at the root of `res/fonts`).

`tools/import-themes.py` only copies the families the imported themes actually
use, and only `.ttf`/`.otf` files plus their licence. The upstream `*_preview.png`
images are not used by the renderer and are not imported.

| Family | Used by | Licence |
|---|---|---|
| `roboto-mono` | most themes | Apache-2.0 (`LICENSE.txt`) |
| `jetbrains-mono` | `3.5inchTheme2`, gauges | SIL OFL 1.1 (`OFL.txt`) |
| `roboto` | `bash-dark-green*`, line graphs | Apache-2.0 (`LICENSE.txt`) |
| `generale-mono` | Cyberpunk / Simple* themes | see `LICENSE.txt` |
| `RobotoMono-Regular.ttf` | fallback when a font is missing | Apache-2.0 |

All of them are copied unmodified from
[turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python)
(`res/fonts/`).

The upstream `geforce/` family is **not** imported: no licence file ships with
it, so the `Terminal` theme falls back to the default font.

If a theme uses a family this directory does not have, the loader warns
(`lcd-theme show <theme>`) and falls back to the default font rather than
failing.
