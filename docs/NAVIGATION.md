# Navigation

How the cursor and the visible window move in Open MainUI. Where this differs from stock MainUI, it is on purpose: the aim is a UI that behaves predictably, not a copy of every stock quirk.

## Button mapping

At start, as stock does, MainUI hands the `keymap` in `system.json` (default `L2,L,R2,R,X,A,B,Y`) to the device's button driver, which remaps the physical buttons for MainUI and every other program. A remapped button, such as X set to act as B, then does on every screen what the button it is mapped to does. A missing `keymap`, or one that is not eight comma-separated names from the default (`L2`, `L`, `R2`, `R`, `X`, `A`, `B`, `Y`, repeats allowed), hands over the default. Stock hands over any string, which could leave the buttons unusable everywhere until MainUI starts again. The tables below name the buttons after mapping.

## Console grids

The Games grid shows 4 columns by 2 rows per page, the Expert grid 3 by 3.

| Button | Moves to |
| --- | --- |
| Right / Left | The next / previous console in reading order: along the row, then on to the start of the next row, then on to the next page. At the end of the list it wraps to the first console, and Left on the first console wraps to the last. |
| Down / Up | The console below / above in the same column, also onto the next or previous page. It stops at the first and last console instead of wrapping; on a short last page it lands on the last console. |
| R / L | The same position one page forward / back, stopping at the first and last console. |

**Differs from stock:** in stock MainUI, Right on the last console of the top row opens the next page at its top-left console. Here Right always takes the next console in order, which is the first console of the row below on the same page. Every console is then reached by pressing Right repeatedly, and the cursor never skips the rest of a page.

## Lists

This applies to ROM lists, Favorites, Recents, Apps and Search results.

| Button | Moves to |
| --- | --- |
| Down / Up | The next / previous row. From the last row it wraps to the first, and from the first to the last. |
| R / L | One page (a screen of rows) down / up, stopping at the first and last row. |
| Right | Opens the details of the selected game in game lists. |
| Y | Launches the selected game as A does, in ROM lists, Favorites, Recents and Search results, as stock does. With Onion's default keymap (`mainui_button_y` set to `glo`), Onion then opens Game List Options for that game instead of starting it. Y on a folder or `..` does nothing. Game List Options removes the first line of `recentlist.json`, taken to be this launch, so Y puts the game's line first and leaves the other lines as they are: backing out leaves Recents unchanged. That line is written before the launch: if it cannot be, nothing is launched (A still is). When a Y launch fails, keymon's Y flag is cleared, and every other launch clears it too: keymon sets it on any Y press, also one MainUI ignores (on a folder, say), and only B or X clear it, so A starts the game instead of opening Game List Options. |
| Menu | Opens the context menu, as Select does, when Menu is released. With Onion's default "Context menu" long press (Tweaks > Button shortcuts), keymon sends MainUI only that release, as for stock. A second release, such as the physical one after keymon's, does not close the menu; B or Select does. Keymon also sends an L1 press after its release, after each repeat of Menu while it is held and after the physical release. Every L1 within 3 s of a Menu event, with no other key between, is ignored with its repeats, so holding Menu does not move the selection in a list. |

Apps are listed in the order the SD card lists their folders, as in stock, not by name: on FAT that is usually the order the folders were created in, so Quick Guide may come first. An empty file named `.appsort` in `.tmp_update/config` sorts them by name instead, with the same case rule as the other lists. Consoles and ROMs are sorted by name (see [CATALOG_CACHE.md](CATALOG_CACHE.md#differences-from-stock)).

The window scrolls only as far as needed to keep the selected row on screen. A row that is already visible never moves the window.

## When a list changes

Edits change the list under the window: Create, Rename, Sort, Delete, Move and Paste in Favorites, removing a Favorite or Recent, and deleting a ROM. After an edit:

- **The window stays where it is** if the selected row still fits on it. Moving a Favorite a few rows within the screen does not scroll the list.
- **Otherwise it scrolls just far enough** to show the selected row, as moving the cursor there would.
- **No gap is left at the bottom.** When a removal shortens a long list, the rows above move into view rather than leaving an empty row under the last one.
- **A short list keeps its top row.** In a folder, the `..` row stays in view, also after pasting or creating the first entry there.

Paste puts the moved Favorite where the cursor is: in place of the selected row, or at the end of the open folder on "..". A game pasted on a folder goes into that folder instead, after its games, and the cursor stays on the folder. A folder pasted on a folder takes that folder's place; to put it inside, open the folder and paste there.

Folders remember where you were. Back returns to the row and window you entered the folder from, also after editing inside it, at any depth. The remembered place of a folder you have visited survives edits elsewhere, and is dropped only when that folder is removed.

This also holds when the folders were reordered outside MainUI while a game ran: Back selects the folder you came from at its new row.

## Confirmations and messages

Delete ROM, Clear Recents and Shutdown ask before they act. Holding A after it opened the question never answers it: release A and press it again. Likewise, A that confirms or dismisses a message acts once; holding it does nothing more until it is released. Leaving MainUI's focus or going to sleep cancels an open question.

A message shown over game details takes the keys first: A or Back dismisses it, and the details stay open behind it.
