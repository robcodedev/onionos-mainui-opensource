# Open MainUI

An open-source replacement for MainUI, the main launcher screen on Onion for the Miyoo Mini family.

Written in C11 against SDL 1.2, the same libraries Onion already ships.

Based on functionality from the [patched MainUI project](https://github.com/robcodedev/onionos-mainui-patcher).

**Version 1.0.2.** It runs on hardware and is usable, but keep a copy of your original SD card.

## Why

MainUI ships as a closed-source binary, so every improvement has to be made by patching machine code from the outside. That puts a ceiling on the project: bugs cannot be fixed at the source, new behaviour has to fit whatever space the binary leaves, and each firmware release risks breaking the patch set.

An open implementation removes that ceiling. Anyone can read it, fix it, extend it, and check what it does on their own device.

It also turns out to be smaller and quicker. Early hardware testing shows faster startup and faster launching and returning from games. The device binary is around 200 KB against the original's 1.4 MB - the original statically linked SQLite, while this build links the copy Onion already ships.

## Status

Supported devices: Mini Plus, Mini, and Mini Flip.

Working: console and ROM browsing against the SQLite ROM caches, Recents, Favorites and folder editing, search, the Apps list, game launch and return, themes, the stock settings screens, and the `miyoogamelist.xml` metadata used for display names and box art.

Known gaps: behaviour has been checked on a Mini Plus more than on the other two models. Bug reports naming the model and describing the SD card layout are the most useful thing you can send.

If something goes wrong, see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md), including how to enable logging and how to resolve an interrupted ROM deletion.

How the cursor and the list window move, including where this differs from stock on purpose, is described in [docs/NAVIGATION.md](docs/NAVIGATION.md).

Some newer labels (folder actions, Tweaks, the About device rows) appear in English regardless of the selected language, because their translation IDs do not exist in Onion's language files yet. See [lang/README.md](lang/README.md).

## Themes

Themes made for stock MainUI or the patched MainUI work unchanged. Optional settings, assets, font behaviour and safe margins are described in [docs/THEMES.md](docs/THEMES.md).

## Building

```sh
make          # host development binary at build/MainUI-dev
make check    # build and run the whole test suite
make device   # cross-compile the device launcher (recommended)
```

The host build needs a C11 compiler, SQLite, and SDL 1.2, SDL_image and SDL_ttf. The device build requires `dev-miyoomini-toolchain` and can be compiled inside Docker. See the [device build instructions](docs/BUILDING.md#device-build) for the command and required paths, or [running the preview on a host](docs/BUILDING.md#running-the-preview-on-a-host) to try it against a mounted card.

To install on a device, follow the [SD-card installation instructions](docs/BUILDING.md#installing-on-a-device).

## Layout

| Path | Contents |
| --- | --- |
| `src/` | The launcher. Domain layout in [src/README.md](src/README.md). |
| `tests/` | Unit suites plus `tests/integration/` fixture cases. |
| `vendor/` | Unmodified upstream cJSON. |
| `device/` | Device-side wrapper script. |

## Sample

![New About device screen](./docs/screen-about.png)

New "About device" screen with model name, max resolution and Onion version now visible.

## Licence

GPL-3.0-only, the same licence as Onion. `vendor/` keeps its own upstream notices. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for cJSON and SQLite attribution, and [CONTRIBUTING.md](CONTRIBUTING.md) before sending a patch.

## Disclaimer

As with the patched MainUI project, most of this source code was created with help of AI. Some code has been reviewed by humans, but far from everything.

This project is unofficial and is not affiliated with Miyoo or the OnionUI maintainers. This launcher replaces Onion's existing MainUI and may contain bugs. Use backups and test carefully.
