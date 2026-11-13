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
- **Fewer messages in the bar at the top of the menu.** The result of an action in the lower part of the page now appears where you are: in the status line of Update settings, as a small pop-up near the bottom (Copy log, the USB export, an import without problems, Show everything I hid, Quit, errors), or under the buttons (Restore original backup, an import with problems). Show everything I hid and Quit now ask for a second press.
- **USB drives are checked when Update settings is opened,** not every 20 seconds.
- **When the console can't connect to GitHub but the browser can,** the menu shows what the browser found instead of the error.
- **The log has a line for every update check,** with the failing step and the error code when GitHub can't be reached.
- **The folder editor** keeps the attention message right below the games grid, with the counter and messages below it.
- The menu is arranged in sections: Folder options, Tile hiding options, Update settings, Export & Import, More & troubleshooting.
