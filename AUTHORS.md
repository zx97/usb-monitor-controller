# Authors and credits

## This project

`USB-monitor-controller` — a C++17 controller and system monitor for the small
USB LCD screens (Turing Smart Screen / TURZX, XuanFang, Kipye Qiye, WeAct).

- **Manuel Flury** — C++ port, theme engine, sensors, daemon, Qt6 applications,
  Windows packaging.

## Upstream project (the origin of the protocols and the theme format)

This project is a **derived work** of
[turing-smart-screen-python](https://github.com/mathoudebine/turing-smart-screen-python),
distributed under GPL-3.0. The wire protocols of every screen revision, the
`theme.yaml` format, the drawing helpers and a large part of the layout logic
were ported from it, sometimes line by line.

- **Matthieu Houdebine** (`@mathoudebine`) — original author, LCD drivers,
  system monitor, theme system.
- **Rollbacke**, **Ebag333**, **w1ld3r**, **Charles Ferguson (gerph)**,
  **Russ Nelson** — early contributions to the Python project.
- **Alexander Wauleit**, **Arthur Ferrai**, **hicwic**, **amiltonjr**,
  **takattila**, **Ebag333** and the other theme authors — the themes bundled in
  `res/themes/` (each `theme.yaml` names its author).

Please support the upstream project: <https://github.com/mathoudebine/turing-smart-screen-python>.

## Third-party assets

| Asset | Origin | Licence |
|---|---|---|
| `res/themes/*` | turing-smart-screen-python `res/themes/` | GPL-3.0, per-theme author |
| `res/fonts/roboto`, `roboto-mono` | Google Roboto | Apache-2.0 |
| `res/fonts/jetbrains-mono` | JetBrains | SIL OFL 1.1 |
| `res/fonts/generale-mono` | upstream project | see `res/fonts/generale-mono/LICENSE.txt` |
| `res/icons/monitor-icon-17865` | upstream project | GPL-3.0 |
| `res/themes/demo-320x480` | this project | GPL-3.0-or-later |

## Libraries

libpng, libjpeg-turbo, FreeType, libusb, OpenSSL, yaml-cpp, Qt 6 — see
`docs/BUILD.md` for the exact list and the build instructions.
