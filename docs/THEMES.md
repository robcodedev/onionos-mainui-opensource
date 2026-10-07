# Notes for theme authors

Open MainUI works with themes that provide only the normal stock assets. This page lists the optional settings and assets it adds, and a few places where the behaviour is worth knowing when designing a theme. Most of it matches the [patched MainUI](https://github.com/robcodedev/onionos-mainui-patcher) theme-author guide, so a theme made for one works on the other.

The active theme's `config.json` is read. If the active theme has no `config.json`, the stock theme's configuration is used as a whole; fields are not merged. Onion's theme overrides from Tweaks (Appearance > Theme overrides), saved in `Saves/CurrentProfile/theme/config.json`, are then applied over it field by field, as Onion does: each field in an object of the overrides (for example `batteryPercentage.size` or `hideLabels.hints`) replaces the theme's, and the object's other fields stay as the theme set them. A missing override file changes nothing; one that cannot be read or is not a JSON object is logged and ignored. Files in Onion's profile override folder (`Saves/CurrentProfile/theme/skin/`) take precedence over the theme's own `skin/` files.

## Optional settings

A theme can set both game-list options in its top-level `config.json`:

```json
{
  "gamelist": {
    "bold": false,
    "iconLeftMargin": 12
  }
}
```

- **Game-list icon left margin:** `gamelist.iconLeftMargin` places the folder and game icons, and with them the start of the title, at an absolute distance from the left edge. It applies at every row count, including the stock six rows. `0` is valid and lets artwork start at the list edge; values above 300 are treated as 300. A missing, negative or invalid value keeps the normal layout, which depends on the row count. One value controls both folder and game rows.
- **Game-list font weight:** `gamelist.bold` set to `false` draws ROM, Favorites, Recents and Search result rows in the normal (not bold) style. Missing, invalid or `true` keeps the stock bold style. Other text (Settings, Apps, titles, game details, the keyboard) is not affected.
- **Hidden labels:** `hideLabels.icons` and `hideLabels.hints` hide the main-menu icon labels and the footer hint text, as in Onion. The older `hideIconTitle` still hides both.
- **Font size 0:** as in stock, a size of 0 hides that text: `hint.size` the footer hints and page counter, `grid.grid1x4` the main-menu labels and the Games console labels, `grid.grid3x4` the Expert console labels, and `title.size` the screen title. Dialogs, messages and details that share these fonts stay readable at the default size. Only an explicit 0 counts: a missing `hint.size` follows `title.size` but stays visible.

## Optional assets

- **Selection background per row count:** with a row count `N` from 6 to 20, ROM, Favorites and Recents lists first try `skin/bg-list-s_N.png` from the active theme, then the ordinary `skin/bg-list-s.png`. Keep the stock asset width, horizontal alignment and transparency. The row height is `floor(360 / N)`.
- **Popup selection background:** context-menu rows use `skin/bg-list-popup-s.png` from the active theme if present, otherwise `skin/bg-list-s.png`. Make it as wide as `bg-pop-menu-1.png` to `bg-pop-menu-6.png`; a height of 60 px, matching a popup row, is the safest choice. This is the highlighted-row strip; the numbered `bg-pop-menu-N.png` files remain the popup backgrounds.
- **Numbered popup backgrounds:** `bg-pop-menu-1.png` to `bg-pop-menu-6.png` should share width, border placement, transparency and horizontal padding. Heights may differ. A missing larger one is built from the largest smaller one that exists.
- **Wide game-icon spacers:** a `skin/icon-game.png` at least 120 px wide and at least three times as wide as it is tall is treated as a deliberate spacer. Its full width is kept and only extra height is cropped; titles start after it, and Favorite markers move to the far right so they do not overlap it. Normal icons are unaffected. Theme resizing tools should use the same rule (`width >= 120 && width >= 3 * height`) and leave such assets unchanged.
- **Settings icons:** supply `skin/icon-theme.png` for the **Themes** row and `skin/fixit.png` for the **Tweaks** row in Settings.

## List colours

All list rows, including the selected one, use `list.color`. `list.selectedcolor` has no effect on list rows in stock MainUI, with one exception: stock draws the first row of its Wi-Fi screen (the on/off toggle) in that colour. Open MainUI does not read `list.selectedcolor` at all, so its Wi-Fi toggle row uses `list.color` like every other row. Make sure `list.color` is readable on the selection background.

## Background orientation

`skin/background.png` is stored upside down, as for stock MainUI and Onion's own apps: it is rotated 180 degrees when loaded. Check the result on the device or in a screenshot, not in an image viewer. The other images are used as stored.

## Fonts and languages

The theme's `font` entries are used in every language. The `fontascii` key that stock MainUI also reads is not supported; no theme in the Onion theme repository uses it.

If a theme font cannot be opened, Open MainUI falls back to a built-in font chosen by language, as stock does:

- a language other than English (any language file whose name does not start with `en.lang`): `wqy-microhei.ttc`, then `Exo-2-Bold-Italic.ttf`;
- English: `Exo-2-Bold-Italic.ttf`.

These are looked up in `miyoo/app` and then, on the device, in `/customer/app` on internal flash.

Stock reloads its fonts immediately when the language is changed. Open MainUI chooses the fallback when the theme is loaded, so when a theme relies on a fallback font and the change switches between English and another language, it restarts itself: Onion starts MainUI again and it reopens Settings on the language row. Without a runtime handoff folder (host development builds), the new fallback applies from the next start instead. Themes whose fonts all load are not affected.

A theme font must contain the characters of the languages its users choose; neither stock nor Open MainUI substitutes another font for missing characters. A Latin-only font shows boxes for Chinese, Japanese, Korean or Cyrillic labels. No font shipped with Onion contains Arabic or Bengali, and the SDL_ttf version Onion uses cannot join or reorder those scripts, so those languages do not display correctly with any theme.

## Safe margins and borders

The 640x480 screen reserves a 360 px band for the game list, between the title and the footer:

```text
y =   0..59    title
y =  60..419   game list
y = 420..479   footer (hints and counter)
```

The areas above `y = 60` and from `y = 420` down are outside list rows, though the title and footer use them. Inside the list band there is no fixed top or bottom margin: the row height changes with the row count, and selection and icon artwork may fill the whole row. Borders that must never touch a row should stay outside the 360 px band. Borders inside `bg-list-s_N.png` should fit within that row's own `floor(360 / N)` height.

Horizontally, do not rely on the stock 20 px inset. At high row counts the outer padding shrinks to 15 px, and `gamelist.iconLeftMargin` can move icons and titles further left, down to zero. For a permanent left border of `B` pixels, set `iconLeftMargin` to at least `B` plus the gap you want. On the right, keep important border artwork conservative: dense rows can approach the 15 px inset, Favorite markers can use the far-right lane, and wide-spacer layouts use more width than stock. Test border-heavy themes at the smallest and largest row counts they support.

Main-menu labels may wrap to two centred lines within a 136 px width. Dialog action labels use the theme's hint style. Hint text (footer hints, counter, dialog actions) uses `hint.font`; without it, the default font (Exo 2 Bold Italic, or the language font), as in stock, not `title.font`. Its style is the font file's own. The Games grid uses `bg-game-item-n.png` and `bg-game-item-f.png` for every console; Expert draws only `bg-ra-list-item.png` (214x120), behind the selected console. Behind the context menu the screen above the footer is darkened with black at 0xAA alpha, as in stock. Test unusually large fonts, borders, icons and preview art on the device: assets are cropped, not rearranged, when they do not fit.
