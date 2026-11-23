# MVS Patcher

A Windows program for **MultiVersus** (Steam, final build 17784632) with two parts you patch and unpatch
separately:

- **Client side:** smooth motion on high-refresh screens (fighters, camera, poses and effects drawn between the
  game's 60 simulation steps), a frame rate cap, fast start (skips the intro and title screen) and a hitbox view.
  It changes only what is drawn, never how the game plays, so it is safe to keep on online.
- **Beta gameplay (experimental):** makes the game play more like the 2023 open beta: faster movement and jumps,
  earlier cancel windows, beta-like dodges, gravity and fall speed. **Offline only:** it changes the game
  itself, so it is for the Lab and playing on your own and cannot be played online. While it is patched, OpenVersus
  is turned off (its plugin and paks renamed, nothing deleted) so patched games never join OpenVersus servers; unpatching
  turns it back on.

## Download

Get `MVSPatcher-<version>.zip` from [Releases](../../releases), unzip it anywhere, and run `MVSPatcher.exe`.
Nothing else to install. The `README.txt` inside explains use and removal.

The exe is not code-signed, so Windows SmartScreen may warn the first time ("More info", then "Run anyway").

## How it works

- The patcher ships **no game files**. Beta gameplay is rebuilt on the player's PC from their own copy of the
  game: `recipe.json` holds only offsets, our new values and hashes of the files they apply to. The game's
  encryption key is read from the player's own game exe at patch time and kept in memory only.
- **`helper/`**: the patcher (.NET 10 WinForms, [CUE4Parse](https://github.com/FabianFG/CUE4Parse) to read the
  game's files; `probe/IoStore.cs` writes the override container).
- **`plugin/`**: `BetaFeel.asi`, the client-side plugin. Hooks are tied to build 17784632: the code bytes at every
  address are checked first and nothing is hooked if one differs.
- **`pakloader/`**: `MvsPakLoader.asi`, lets the game mount unsigned override paks (the same patch OpenVersus
  applies) and sets gravity and fall speed from its ini.
- Plugins are loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (MIT), which the
  patcher installs only where no loader is present.

## Building

- Plugins: Visual Studio 2022 C++ build tools, then `plugin\build.cmd` and `pakloader\build.cmd`.
- Patcher and release zip: .NET 10 SDK and Node.js, then `node helper/stage.js`. It publishes a self-contained
  single-file exe and writes `release/MVS Patcher/` and `release/MVSPatcher-<version>.zip`. It needs a
  `recipe.json` (made by the research tools, not in this repository; take it from a release zip) and copies the
  ASI loader from a local game install.

## Licence

MIT, see [`LICENSE`](LICENSE). Bundled third-party software keeps its own licence.

## Credits

Ultimate ASI Loader by ThirteenAG, CUE4Parse and the other libraries listed in
[`helper/THIRD-PARTY-NOTICES.txt`](helper/THIRD-PARTY-NOTICES.txt). Not affiliated with Warner Bros. Games or
Player First Games.
