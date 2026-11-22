<div align="center">

<img src="assets/icon0.png" width="128" alt="Home Screen Manager icon">

# PS5 Home Screen Manager

**It only does everything on your PS5 home screen.**<br>
Create Folders; Change your Music, Avatars, Tile Icons, Wallpapers;<br>Rearrange, Hide & Rename any Tile and so much more...

![Tested on FW 13.60](https://img.shields.io/badge/tested-FW%2013.60-2ea44f)
![Version 2.2.3](https://img.shields.io/badge/version-2.2.5-7a4bf2)
![License GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-555)

</div>

---

> [!IMPORTANT]
> Your changes to the home screen stay in place even when the PS5 isn't jailbroken. Home Screen Manager writes them into the console's own database, so they survive restarts and boots. Some features, like opening folders, will not work unless Home Screen Manager is loaded either through autoload, a payload manager or the tile in the media tab after the jailbreak. Read the [Installation Guide](https://github.com/lazyboy6791/PS5HomeScreenManager#installation-guide) for more details. For the best experience access the HSM menu from a PC or phone in the same network as the PS5 under `http://<PS5-IP>:8090`.

## Home Screen Manager Features

**NOTHING IS EVER DELETED! A SAFETY BACKUP IS MADE OF EVERYTHING! EVERY CHANGE CAN BE UNDONE!**

   - **Create Your Own Folders**
   - **Rename any Tile**
   - **Hide Anything**
   - **Custom Icons on any Tile**
   - **Custom Wallpapers On any App/Game/System Tile or Folder**
   - **Rearrange Tiles and/or Move Them Between Tabs**
   - **Change Your Home Screen Music**
   - **Clean Up Your Game Names**
   - **Redirect to Where Your System Tiles Open**
   - **Export & Import All of Your Customizations**
   - **Light and Dark Mode**
   - **Quiet Autoload**
   - **Open Anything from the Menu through PC or Phone**


## Requirements

- A jailbroken PS5 (tested on firmware **13.60**)
- [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) and/or [ShadowMount+](https://github.com/drakmor/ShadowMountPlus)
- `HomeScreenManager.elf` and/or **PPSA979797.zip** from the [Releases](../../releases) page


## Installation Guide

There are 2 methods to install home screen manager. For best results use both but you will also have to update them independently.

**Method 1:** 
   - Start [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) on the PS5 and add the latest `HomeScreenManager.elf` from the [Releases](../../releases) page in any one of these ways:
   - **From within Payload Manager:** Go to **Settings > Multiple Payload Sources > Manage Sources**, add [this URL](https://raw.githubusercontent.com/lazyboy6791/PS5HomeScreenManager/main/payloads.json) and download it from there. Updates will also be picked up if you choose this method.
   - **Phone or PC:** In the same network as the PS5 open `http://<PS5-IP>:8084`, go to **Get Payloads > Upload ELF Payload**, and choose the file on your device.
   - **USB drive:** Copy the file to the root of an exFAT or FAT32 drive, in payload manager go to **Settings > Scan USB Payloads**, then go to **Get Payloads > USB Storage > Copy to Internal**.
   - **FTP or File Manager:** Copy the file from your device to `/data/pldmgr/payloads/`.
   - Launch **HomeScreenManager** from the Payload Manager.
   - The menu opens in the PS5 browser and is also accessible on a PC or phone in the same network as the PS5 under `http://<PS5-IP>:8090`.
>  [!Note]
> Add HSM to payload managers autoloader so its features are always available.
> - In Payload Manager, go to **Autoload > Add Item to Sequence** and choose **HomeScreenManager**.
> - In Home Screen Manager, open the **Start-Up Options** and choose **Quiet when started by Autoload**.
> - From then on HSM will start silently in the background. The menu will still be accessible through a PC or phone in the same network as the PS5 under `http://<PS5-IP>:8090` and the menu in the PS5 browser is also still accessible by re-launching HSM again through payload manager or the **tile in the media tab** as described in **Method 2**. 

**Method 2:** 
   - Download the latest **PPSA97979.zip** from the [Releases](../../releases) page or the [Prospero Store](https://prosperostore.net/app/PPSA97979/)
   - Copy the unzipped **PPSA97979 Folder** to `/data/homebrew/` on your PS5 via **FTP or File Manager**.
   - Once it finishes [ShadowMount+](https://github.com/drakmor/ShadowMountPlus) will pick it up and **install a tile in the media tab**
   - Click on the tile and home screen manager will be loaded
   - The menu opens in the PS5 browser and is also accessible on a PC or phone in the same network as the PS5 under `http://<PS5-IP>:8090`.
>  [!IMPORTANT]
> When using only method 2 you will always have to start HSM manually after the jailbreak to use its features.

## Getting Started

| To... | Do this |
|---|---|
| Hide a tile | Find it with the search box or the tab filters, then press **Hide**. **Show** brings it back. |
| Make a folder | Press **+ New folder**, give it a name, choose a tab and colour, tap the games, and press **Save folder**. |
| Rename a tile, or change its picture or background | Press **Edit** next to it. |
| Start a game or app | Press **Open**. It works for hidden ones too. |
| See your changes | Hiding and folders show after you open and close any game, or restart. Names, pictures and backgrounds show after **Refresh home screen now**. |

## The Menu

1. **Home Screen Folders and Tiles**: your folders and tiles, with more **Folder options** inside.
2. **Tile hiding options**
3. **Change and Edit Wallpapers**
4. **Home Screen Music**
5. **Where System Tiles Open**
6. **Clean Up Game Names**
7. **Home Screen Manager Start-Up Options**
8. **Update settings**
9. **Export & Import**
10. **More & troubleshooting**

## Folders

A folder is one tile on the **Games** or **Media** tab. Selecting it opens a page that looks like the Game Library: pick a game and it starts.

- Games in a folder are hidden from the home screen. A game can be in more than one folder, and it stays hidden while it is in any of them. Take it out of its last folder and it comes back, unless you had hidden it before.
- Give the folder a colour, or choose the chequered swatch for **no colour** (see-through).
- Use the games as the folder picture (five styles: Grid 2×2, Grid 3×3, Stack, Feature, Single), or choose your own picture and background. Every picture you choose opens a crop and zoom window first.
- Web apps such as Payload Manager can go in folders too. Selecting one opens its page.
- If another game is running, the folder page asks before closing it.
- The d-pad works on the folder page. The first game is highlighted when a folder opens, the d-pad moves the highlight, and X starts the highlighted game. Moving the pointer takes over again. If it doesn't behave, turn on **Folder page: show what the controller sends** in Folder options to see what the browser receives.
- The folder page shows the folder's picture and name at the top, a row with the count, the **Sort by** button and the **View** menu (**Icon** with the pictures, **Compact** with the picture and title in a row, or **List** with the titles only) and a menu next to it for **Tiles per row** (Icon, 5 to 10, default 6) or **Columns** (Compact and List, 1 to 3, default 1); your choices are remembered, and an **Edit this folder in Home Screen Manager** link under the games. Each can be switched off in **Folder options**.
- Adding a tile goes through the console's installer, which can take about 90 seconds on newer firmware. The folder editor warns about this above its buttons, the menu shows **Creating folder…** and waits, and Home Screen Manager keeps answering meanwhile. When a folder has been created or really changed, Home Screen Manager puts the tile positions right and then restarts the home screen by itself, because the console shuffles tiles while it works and the home screen only shows the repaired layout after a restart. Saving a folder without changing anything does nothing and says **Nothing was changed**.
- Editing a folder that already exists (name, colour, games, picture) is put straight into the home screen database whenever that is possible, which takes seconds. Creating a folder, changing its background, or a folder whose tile can't be updated that way, go through the installer.

## Change and Edit Wallpapers

Open **Change and Edit Wallpapers** to give many games the same background in one go. It shows what it will do first, makes a copy of the home screen database (`app.db.before-bulk`) before it writes anything, and restarts the home screen by itself when it is done, with a message and a counter while it runs.

Choose one picture (from a USB drive, your phone or PC, or the pictures folder). Home Screen Manager crops it to 16:9 from the middle, shows the result with warnings if it is small or not 16:9, and lists every game with a tick box and **Select all** / **Select none** buttons. Games are ticked. Apps and system tiles that can take a background are listed and not ticked. Games that have no background entry in the database can't take one and are skipped, and folders are not included. **Overwrite backgrounds I've already set** is off by default, so your own work is never undone by accident.

The picture is converted once for each file format and the result is shared between the games (linked when the drive allows it, copied when not), so a big library takes seconds to minutes, not hours. The log says how many files were linked and how many were copied. If the console's drive gets almost full, the job stops by itself and what is done stays. **Use the original for all** puts every original background back, with the same message and counter.

**Games without a background entry.** They are in the normal list, ticked like the others, and get your picture too: HSM fills their empty slots in the home screen database, in the format the other tiles use in the same place (the log reports what it found and chose). They carry the note "game has no own wallpaper", because **Use the original** gives them back no wallpaper at all. System tiles (Game Library, Media Gallery and so on) are not included: the home screen does not keep a background that is given to them. The share box does not apply to these games, they get files of their own.

**Share one file between games (experimental).** Instead of one converted file per game, one file per format is kept in a folder of its own (`hsm_shared` in `appmeta`) and every game points at it. It is much smaller and faster. It is off by default: try it on a few games first, restart the console once, and check that the backgrounds are still there. The shared files are removed when no game uses them any more.

## Clean Up Game Names

Open **Clean Up Game Names** to tidy many names in one go. Tick the rules you want: remove &trade;, &reg; and &copy;, remove platform tags such as (PS4) and (PS5), clean up spaces, and **Fix capital letters**. Add your own words and phrases to remove, such as "Remastered", in any language, or add one of the suggestions with a tap.

**Fix capital letters** changes names that are all upper case or all lower case, so GOD OF WAR becomes God of War. In a name that has both cases it fixes only words of four or more capital letters (RESIDENT EVIL requiem becomes Resident Evil Requiem); shorter capital words such as GTA or NBA are left alone. Small words (a, an, and, as, at, but, by, for, in, nor, of, on, or, the, to, up, vs) stay small, except as the first or last word or right after a colon or dash. Roman numerals stay capitals, and words with digits are left as written. It is off until you tick it. Under **Keep name as intended** you list words that must keep the capitals you write, such as FIFA, NBA or inFAMOUS (suggestions are one tap away, and you can type your own); the spelling in your list wins. The list shows every name that would change, old next to new, with **Select all** and **Select none**. If two games would end up with the same name, you get a warning and they are not ticked. Names that would end up empty or longer than 64 characters are skipped.

**Names you've changed** lists every game whose name is changed, with the original, a label that says whether the cleanup or you (by hand) made the change, and a **Use the original** button on each (that one always goes back to the console's own name). In the preview, a game you renamed by hand before shows "Renamed by hand before", and it is still ticked so you can untick it.

**Use the original names** only undoes what the cleanup changed. If the cleanup had tidied a name you gave by hand, you get your own name back. Tick **Also names I changed by hand** next to the button to put every name back to the original. A line under the button says how many names it will put back. A copy of the database is made first, and the home screen restarts by itself when the names are saved.

Custom names, pictures and backgrounds stay in place: when the PS5 writes a game's own name back (it does that when you start the game), Home Screen Manager puts yours back within a moment.

## Export & Import

Open **Export & Import** in the menu to save everything Home Screen Manager manages in one file: your folders (with their pictures and backgrounds), the tiles you hid, custom names, pictures and backgrounds, the home screen order and the settings. Your games are not in it.

- **Export to USB drive** writes `HomeScreenManager-backup-<date>.zip` to the top of a drive. **Download to this phone or PC** saves the same file there.
- **Import (experimental)** reads a backup from a USB drive or from your phone or PC, shows what is in it, and only changes anything after you confirm. Folders are added next to the ones you have, the hidden tiles and custom names, pictures and backgrounds are restored, and your saved order and settings are replaced by the backup's. Games that aren't installed are skipped.
- Creating folders uses the console's installer, so an import can take up to about 10 minutes depending on your firmware. The menu shows a timer and which folder it is on, and the home screen refreshes by itself at the end.
- The import hasn't been tested on a console yet. Export your current setup first.

## Names, pictures and backgrounds

Press **Edit** next to any game, app or system tile:

- **Name:** type a new name, up to 64 characters.
- **Picture:** choose a picture. Tick **Keep see-through parts transparent** to keep a PNG's transparent areas. Otherwise they get a dark background.
- **Background:** choose a wide picture. Home Screen Manager saves it in the same size and format as the game's own background, so the home screen reads it the same way. Saving takes up to 10 seconds.

Each part has a **Use the original** button. The game's own files are never touched: Home Screen Manager only changes what the home screen shows, and it saves every original first.

<details>
<summary><b>Pictures guide</b></summary>

<br>

**Where pictures come from.** Home Screen Manager copies the picture it uses, so you can unplug the drive afterwards.

- **USB drive:** put pictures anywhere on the drive (up to four folders deep), then press **Choose from USB drive**. PS5 screenshots show up too.
- **Phone or PC:** open the menu at `http://<PS5-IP>:8090` and use the file picker.
- **FTP:** put pictures in `/data/homescreenmanager/pictures`.

| | |
|---|---|
| Formats | PNG, JPG, BMP, GIF, TGA (up to 32 MB). Convert HEIC and WebP to JPG first. |
| Icons | Square, 512&times;512 or bigger. Other shapes are cropped from the middle. |
| Backgrounds | Wide, 16:9, ideally 3840&times;2160. Other shapes are cropped from the middle. |
| Phone photos | Turned upright automatically. |
| GIFs | The first frame is used. Home screen tiles can't play animation. |

</details>

## Reorder the home screen

The PS5 sorts each tab by most-recently-played and has no manual reorder. The home-screen list shows your tiles in that same order, and lets you set your own instead.

1. Use the **▲ / ▼** arrows on any tile to move it earlier or later on its tab (hidden tiles stay greyed at the end).
2. Press **Save order**, then **Refresh home screen now**.
3. **Automatic order** goes back to the PS5's recency sort.

Because the PS5 re-sorts by recency, Home Screen Manager re-applies your order every time it runs, so **add it to Autoload** to keep it. Everything is saved and fully reversible.

## Keep folders first

Turn on **Keep folders first** (in **Folder options**, inside Home Screen Folders and Tiles) and your folders stay in the first spots of their tab, ahead of every game and app, even after you play something else. Folders still reorder among themselves: the one you open moves to the front of the folders (when you also have a saved order, it moves to the front a moment later, when Home Screen Manager next looks). A game that is running stays in front of the folders until you close it. A hidden game (one inside a folder, say) also stays on screen while it runs, so you can close it from the home row, and is hidden again after you close it (**Show a running game** in **Tile hiding options** turns that off). Home Screen Manager keeps this in place while it's running, so **add it to Autoload**. Switch it off and the PS5's own order comes back. Press **Refresh home screen now** (or open and close any game) to see a change.

**Other apps that change the home screen.** Apps such as PS4 Mount or ShadowMountPlus re-register games, and the console then reshuffles tiles while they work: folders can show up on the Media tab and hidden games can appear for a moment. Home Screen Manager waits until the other app is done (a missing folder tile is only reinstalled after it has been missing for 45 seconds and the home screen database has been quiet for 30 seconds), then puts everything back by itself. It does not restart the home screen, so open and close any game or folder to redraw it sooner.

## Renamed from Home Hider

This project used to be called **Home Hider**. The first time the renamed version starts, it moves its data folder from `/data/homehider` to `/data/homescreenmanager`: it copies everything (settings, original database backup, pictures, log), checks every file, and only then removes the old folder. If anything goes wrong it keeps working from the old folder and says so in the log. Your folders, hidden tiles and update setup keep working. The ids of your folder tiles (`HHFD…`) and the internal code names did not change. A Payload Manager or Autoload entry that still says `HomeHider.elf` keeps working and is updated in place.

## Updating

Go to **Updates** in the menu:

- **USB:** copy the new `HomeScreenManager.elf` to the top of a USB drive and plug it in.
- **GitHub:** enter this repository under **Update settings**. Downloads are checked against GitHub's SHA-256 checksum.
- **File:** upload it from your phone or PC.

Opening **Update settings** looks for a newer version on a USB drive; nothing checks in the background. Nothing installs until you press **Install**. The old version is kept in `/data/homescreenmanager/backup/`.

## Undo

| To undo | Do this |
|---|---|
| One tile | **Show**, or **Edit > Use the original…** |
| Everything you hid | **More & troubleshooting > Show everything I hid** |
| Everything Home Screen Manager changed | **More & troubleshooting > Undo everything I changed**. Takes back folders, moved tiles, order, hidden tiles, pictures, backgrounds and names, and leaves the rest of the database alone |
| The whole database (last resort) | **More & troubleshooting > Restore original backup**, then restart the PS5. Games deleted since the backup can come back as blank tiles that only Safe Mode > Rebuild Database removes |
| Without the jailbreak | Safe Mode > **Rebuild Database** |

The first-run backup of the home-screen database is kept in `/data/homescreenmanager/backup/app.db.original`.

<details>
<summary><b>Troubleshooting</b></summary>

<br>

| Problem | Fix |
|---|---|
| A change doesn't show | Hiding and folders: open and close any game, or restart. Names, pictures and backgrounds: **Refresh home screen now**. |
| A folder shows "can't connect" | Home Screen Manager isn't running. Start it, or add it to Autoload. |
| A web app in a folder shows "can't connect" | The payload behind it (for example, Payload Manager) isn't running. |
| The menu opens at Autoload | **Home Screen Manager Start-Up Options > Quiet when started by Autoload**. |
| "Choose from USB drive" finds nothing | Use exFAT or FAT32, keep pictures within four folders of the top, and press **Refresh**. |
| A background "can't be written yet" | That game uses an unusual texture format. |
| "Port 8090 is busy" | Restart the PS5 and launch Home Screen Manager again. |
| Anything else | **More & troubleshooting > Diagnostics** shows a log of every step, and lists of the tiles and directories that are not games, and **Copy log** copies it with one tap. Please include it when you open an issue. |

</details>

<details>
<summary><b>Questions</b></summary>

<br>

**Does it delete anything?** No. Hidden games stay installed, and every original value is saved so it can be put back.

**Do changes survive without the jailbreak?** Yes. Only a system update or Rebuild Database can undo them, and Home Screen Manager puts them back the next time it runs.

**Can GIFs or backgrounds animate?** No. Tile pictures and backgrounds are still pictures.

**Why can't Game Library and App Library be hidden?** The home screen adds those two tiles itself.

**Can I use it from my phone?** Yes. Open `http://<PS5-IP>:8090` on any device in the same network.

</details>

## Native app (experimental)

The `native-app/` folder holds a small PS5 app (title ID PPSA97979) that starts Home Screen Manager and opens its menu, so it has a tile of its own on the home screen. It is a source package: build it on Linux or WSL with `native-app/build.sh` (see `native-app/README.txt`). After an update from GitHub or USB, the tile starts the updated copy (kept in `/data/homescreenmanager`) when it is newer than the one bundled in the app.

## Building from source

See [BUILDING.md](BUILDING.md). It covers the PS5 build, the desktop test suite (`make test`) and how the home-screen changes work.

## Credits

- **[HomeHider](https://github.com/Euphoric-Dev/HomeHider) by Euphoric-Dev**: developer of the original HomeHider on which this project is based upon
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom
- [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) by itsPLK
- Home-screen research: [PS5-TabShifter](https://github.com/xvvulcanvx/PS5-TabShifter), [psgfx](https://github.com/elripalda/psgfx), [PS5 Art Swap](https://github.com/griches/PS5_ArtSwap) and the [PS5 Developer Wiki](https://www.psdevwiki.com/ps5/Title_ID)
- [SQLite](https://sqlite.org) and [stb](https://github.com/nothings/stb) (public domain)

## License

Copyright © 2026 lazyboy6791. Home Screen Manager is a modified version of [HomeHider](https://github.com/Euphoric-Dev/HomeHider) by Euphoric-Dev (GPL-3.0), changed and renamed in 2026.

Home Screen Manager is free software under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)).

<div align="center">
<sub>Use at your own risk. Home Screen Manager only changes the home-screen layout, keeps backups, and can undo everything it does.</sub>
</div>
