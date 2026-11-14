# Changelog

Every release has an entry here. `make release` copies the entry for the current version into the release note.

## 2.2.0

- **Renamed to Home Screen Manager.** The data folder moved from `/data/homehider` to `/data/homescreenmanager` (copied, checked file by file, then the old one is removed; the old folder is kept if anything fails). The file is now `HomeScreenManager.elf`. Existing Payload Manager and Autoload entries keep working and are updated in place.
- **Folders.** A game can be in several folders. Folders can stay first on the home screen. The folder page can hide its picture and name, its count and sort row, and its edit link. Two new folder colours, dark gray and black.
- **Faster folder edits.** Editing an existing folder no longer needs the console's installer: seconds instead of about 90. Creating a folder and changing a background still use the installer.
- **The home screen refreshes by itself** after a folder is created or really changed. Saving a folder without changes does nothing and says so.
- **Clear warnings and a running timer** while a folder is created or saved. Creating a folder runs in the background, so the menu no longer times out.
- **A game you are playing** stays in front of your folders until you close it, and is put back right after.
- **Update settings.** A default GitHub repository is built in (`lazyboy6791/PS5HomeScreenManager`). "Check for updates" now says how the check went.
- **Export & Import.** One file holds your folders (with their pictures and backgrounds), hidden tiles, custom names, pictures and backgrounds, the home screen order and the settings. Import is experimental.
- **Copy log** in More & troubleshooting copies the log with one tap.
- **The folder page works with the d-pad.** The first game is highlighted when a folder opens. The d-pad moves the highlight between the games, the Sort button and the edit link, and X starts the highlighted game. Moving the pointer takes over again. There is one highlight at a time. A switch in Folder options shows what the controller sends, for testing.
- **Change and Edit Wallpapers.** One picture as the background of all the games you tick, in seconds, with a preview, warnings, Select all / Select none, a choice about backgrounds you set yourself, and one undo for all. The picture is converted once per file format and shared between the games. A message and a counter show while it applies or undoes, and the home screen restarts by itself when it is done. If the drive gets almost full, the job stops safely.
- **Clean Up Game Names.** Removes ™ ® ©, platform tags and your own list of words (with suggestions) from game names, shows every change first, warns when two games would get the same name, and lists the names you've changed, each with a button to put the original back. Select all / Select none work on the list.
- **Fix capital letters** in Clean Up Game Names (GOD OF WAR becomes God of War; small words stay small; Roman numerals and words with digits are kept), with a **Keep name as intended** list (FIFA, NBA, inFAMOUS and your own words).
- **Names you changed by hand are protected.** Use the original names only undoes what the cleanup changed (a name you gave by hand that the cleanup tidied comes back), with a tick box for the ones changed by hand, counts, labels on every row, and a "Renamed by hand before" notice in the preview.
- **The home screen refreshes right after a bulk change.** The repair of custom names and backgrounds waits until after the refresh, and the log says exactly which entry it fixes and why.
- **Experiment: a background for games without a background entry.** A button per game (system tiles are report only). Experimental.
- **Experiment: share one file between games** for the wallpapers (off by default).
- **Fixed:** a request that arrived while a bulk run or an import was writing could fail with "database is locked". The state database now waits for a writer, like the other connection.
- **Fixed:** "Use the original for all" (wallpapers) could crash on the console. Shared files are now cleaned up once at the end instead of after every game, big tables no longer sit on the job thread's stack, and the job threads have a larger stack.
- **Fixed:** games that share one home screen entry (like CUSA04607 and CUSA02000) no longer keep rewriting each other's background. The file of the other game counts as ours, and is never saved as an "original".
- **Fixed:** a game renamed in the tile editor now shows in Clean Up Game Names and in the list of changed names right away, without reloading the page.
- **Fixed:** a saved "original" background that was really one of our own files (an older build could save one when two games shared an entry) is no longer put back. The console's own value from the first backup is used instead, and the log says so.
- **The log is bigger:** it is kept up to 1 MB before it starts again, and the log page shows up to 600 KB, so a whole test run fits in one copy.
- **A new menu.** Everything is a closed section now: Home Screen Folders and Tiles (with Folder options inside), Tile hiding options, Change and Edit Wallpapers (with Live wallpaper), Clean Up Game Names, Home Screen Manager Start-Up Options, Update settings, Export & Import, More & troubleshooting. The line under the title is "Take control of your PlayStation 5's home screen". The controller test switch moved to More & troubleshooting.
- **Custom names, pictures and backgrounds stay in place.** The PS5 writes a game's own name back when you start it. Now HSM looks at the database four times a second and puts them back at once when it changes (and right after a game closes), with almost no cost when nothing is wrong. It doesn't do this while a bulk run or an import is going, and only one background change runs at a time.
- **Light and dark theme** with a switch at the top. The choice is kept in HSM, so the menu on every device and the folder page on the TV follow it.
- **Fewer messages in the bar at the top of the menu.** The result of an action in the lower part of the page now appears where you are: in the status line of Update settings, as a small pop-up near the bottom (Copy log, the USB export, an import without problems, Show everything I hid, Quit, errors), or under the buttons (Restore original backup, an import with problems). Show everything I hid and Quit now ask for a second press.
- **USB drives are checked when Update settings is opened,** not every 20 seconds.
- **When the console can't connect to GitHub but the browser can,** the menu shows what the browser found instead of the error.
- **The log has a line for every update check,** with the failing step and the error code when GitHub can't be reached.
- **The folder editor** keeps the attention message right below the games grid, with the counter and messages below it.
- The menu is arranged in sections: Folder options, Tile hiding options, Update settings, Export & Import, More & troubleshooting.
