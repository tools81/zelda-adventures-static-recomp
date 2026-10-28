ZELDA'S ADVENTURE (CD-i) FOR WINDOWS
====================================

A native Windows version of the Philips CD-i game Zelda's Adventure, made by
recompiling the original program. You need your own copy of the game disc as
a disc image (.cue + .bin, raw 2352-byte sectors). It is not included.

Requirements: Windows 10 or 11 (64-bit). Nothing else to install.


STARTING
--------
zelda.exe       the original picture
zelda_hd.exe    the same game with the Cartoon HD and Cartoon HD+ upscalers

The first time, the game asks for your disc image (choose the .cue or .bin
file). You can also put the image in the "rom" folder, or change the path
later in game.ini (rom=...).


EVERYTHING STAYS IN THIS FOLDER
-------------------------------
The game writes nothing to your user profile. Next to the program you find:
  saves\        game saves, quicksaves, display settings, recomp.log
  game.ini      where your disc image is, and update settings
To move the game, move the whole folder.


CONTROLS
--------
Thumbpad: arrow keys (Shift = faster) or a gamepad's stick / D-pad
Button 1: Z, Space or A (gamepad A)     Button 2: X, Ctrl or S (gamepad B)
Both buttons: C or Enter (gamepad X or Start)

F2  quicksave            F3  next quicksave slot (0-9)    F4  quickload
F5  invincibility        F6  run-ahead (less input lag)   F7  60 FPS sprites
F9  aspect               F10 filter                       F11 full screen
Tab (hold) run fast      Esc quit


UPDATES
-------
When the game starts it checks for a newer version and offers to install it.
Your saves, settings and game.ini are kept. To turn this off, set
check_updates=0 in game.ini (or start the game with --no-update).
