MVS Patcher 1.0.0
=================

A small Windows program for MultiVersus (Steam). It has two parts that you patch and unpatch separately.

Client side
  Changes how the game looks and starts, never how it plays:
  - Smoothing: fighters, camera, poses and effects are drawn between the game's 60 steps a second, so motion is
    smooth on 120/144/240 Hz screens. Turn on Vertical Sync in the game's settings for the best result.
  - Frame rate cap: screen rate, or 2x/3x/4x the game's 60 steps (120/180/240).
  - Fast start: skips the intro videos and the title screen; the game opens on the main menu.
  - Hitboxes: shows the game's own hit and hurt boxes in matches.
  Because it does not change gameplay, it is safe to keep on when you play online (OpenVersus) or with players
  who do not have it.

Beta gameplay (EXPERIMENTAL)
  Makes the game play more like the 2023 open beta: faster movement and jumps, earlier cancel windows, beta-like
  dodges, gravity and fall speed.
  IMPORTANT: this changes the game. Only play with people who have the same patch: compare the gameplay
  fingerprint shown in the patcher. Unpatch it before playing on OpenVersus or with anyone who does not have it.
  Online play with it has not been tested.


Requirements
------------
- Windows 10 or 11, 64-bit.
- MultiVersus from Steam, the final build (17784632). Other builds are refused or the affected feature stays off.
- Nothing else to install.


How to use
----------
1. Unzip this folder anywhere (not inside the game folder). Keep all files together.
2. Close the game.
3. Start MVSPatcher.exe. It finds the game through Steam; if not, use "Change folder".
4. Click Patch on the part(s) you want. Patching needs the game closed.
5. Press Play, or start the game from Steam as usual.

The switches on the Client side card take effect at once, even while the game is running. Keep the patcher open
beside the game if you want to change them during play; you can also close it, the game keeps working.


Removing it
-----------
Open MVS Patcher and click Unpatch on each part. It removes only the files it put in and leaves your own mods and
settings alone. Deleting this folder afterwards removes the patcher itself.

What it installs, if you ever need to remove it by hand (game closed):
- <game>\MultiVersus\Binaries\Win64\plugins\BetaFeel.asi, BetaFeel.ini, BetaFeel.log, BetaFeel.installed.txt
- <game>\MultiVersus\Binaries\Win64\plugins\MvsPakLoader.asi, MvsPakLoader.ini, MvsPakLoader.log
- %LOCALAPPDATA%\MultiVersus\Saved\Paks\BetaFeel_* (Beta gameplay)
- <game>\MultiVersus\Binaries\Win64\xinput1_3.dll, only if the game had no ASI loader before (the patcher
  removes it again on Unpatch only if it put it there)


Warnings from Windows or antivirus
----------------------------------
MVSPatcher.exe is not code-signed, so Windows SmartScreen may show "Windows protected your PC": click
"More info", then "Run anyway". Some antivirus programs flag game plugins (.asi files) and ASI loaders because
they load code into the game; that is how every mod of this kind works.


Good to know
------------
- The patcher ships no game files. Beta gameplay is built on your PC from your own copy of the game.
- If something is not right, the Activity page lists everything the patcher did; copy it when asking for help,
  together with plugins\BetaFeel.log from the game folder.


Credits
-------
- Ultimate ASI Loader by ThirteenAG (MIT licence; see loader\LICENSE_Ultimate_ASI_Loader.txt).
- CUE4Parse (Apache 2.0) for reading the game's files.
- Other open-source libraries inside MVSPatcher.exe: see THIRD-PARTY-NOTICES.txt.
