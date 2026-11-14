# Offline trade evolutions

This local mod replaces trade evolution requirements with level-based evolution at **level 36 or higher** in
Pokémon Legends: Z-A 2.0.2. Required trade items are removed. The game retains its normal party-menu **Evolve** action.

The generator reads your own game files and creates a loose RomFS mod for one local profile. It does not change
the extracted originals or edit a save. MonZaMac excludes RomFS mods from Lumiose Online sessions, including
online preparation. Restart the game after installing or disabling the mod.

## Generate the mod

Run from the repository root, using the `mods` directory of the profile you intend to play:

```sh
python3 tools/za/offline_trade_evolutions.py \
  --romfs /path/to/extracted/romfs \
  --out /path/to/profile/mods/OfflineTradeEvolutions \
  --level 36
```

The tool can discover the packaged `monzamac-shader-decode` helper. Use `--decoder /path/to/monzamac-shader-decode`
if your app is installed elsewhere. `--dry-run` validates the inputs and reports the proposed changes before
installing anything. An identical repeat is harmless; unknown existing output and conflicting active mods are
rejected. Rename the installed folder to `_OfflineTradeEvolutions` to disable it before the next offline launch.

Only the verified original 2.0.2 personal-data resource is accepted. Modified or unsupported input is rejected. Generated
game files stay local: do not commit, upload or redistribute the mod's `romfs` files.

## Evolution rules

All 13 present trade-evolution routes are covered, including the Mega Dimension Pokémon:

| Pokémon | Evolution | Additional condition |
|---|---|---|
| Kadabra | Alakazam | None |
| Machoke | Machamp | None |
| Haunter | Gengar | None |
| Phantump | Trevenant | None |
| Pumpkaboo | Gourgeist | All four size forms are preserved |
| Slowpoke | Slowking | Night |
| Onix | Steelix | None |
| Scyther | Scizor | None |
| Spritzee | Aromatisse | None |
| Swirlix | Slurpuff | None |
| Porygon | Porygon2 | None |
| Porygon2 | Porygon-Z | None |
| Feebas | Milotic | None |

Kantonian Slowpoke evolves into **Slowbro at level 37 or higher during the day**, or **Slowking at level 36 or
higher at night**. This avoids two overlapping level rules. Galarian Slowpoke, unavailable species, other
evolution methods and unrelated personal data are preserved.

The generator uses the game's normal level and time-of-day methods. Target species, forms and unrelated fields
are preserved. It does not simulate a trade or award Link Trade research progress.

## Validation

The pure transformation and installation tests use authored fixtures, without game data. Filesystem regressions
exercise actual offline overlays, exclusion during online preparation, and reset back to offline mode.

In-game checks should include formerly plain and item-based trade evolutions, the level-35/36 boundary, day and
night, cancellation, and Slowpoke's two branches. Data validation alone does not establish those gameplay results.
