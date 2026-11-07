# fanMO

A native C++/Qt6 mod organizer for Skyrim Special Edition on Linux. It has
MO2's layout and a priority list you drag around, deploys with hard links the
way Vortex does, and installs Nexus collections and Wabbajack lists without
Wine. Windows tools still run, through the game's Proton prefix.

## What it does

- **Mods and plugins**: priority list on the left, load order on the right,
  both drag-to-reorder, with per-file conflict counts and a conflict viewer.
- **Collections**: from a Nexus URL, an `nxm://` link, a `collection_*.7z`
  bundle or a `collection.json`. Replays recorded FOMOD choices, applies
  binary patches and INI tweaks, and orders mods by the curator's rules.
  Installing a newer revision only touches what changed. Plugin order takes
  one more step: see [Load order](#load-order).
- **Wabbajack lists**: `FromArchive`, `PatchedFromArchive`, `InlineFile` and
  `RemappedInlineFile`. A list needing anything else (BSA building, texture
  recompression, files nested in BSAs) is refused before anything is written.
- **Deployment**: hard links into `Data`, as a diff against the last run, so
  a reorder touches a handful of files. Folders that differ only by case
  merge into one spelling.
- **Profiles**: separate mod lists and load orders, optionally separate saves
  and INIs.
- **Tools**: anything run through Proton in the game's prefix. New files in
  `Data` go to a mod you name, or to Overwrite. A tool marked as a sorter
  (LOOT) has its load order read back afterwards.

## Build

Needs Qt 6 (Core, Gui, Widgets, Network, Concurrent), bzip2, zlib and xxhash,
and 7-Zip at runtime.

```sh
make            # fanmo, fanmo-cli, and both test programs
make test       # no game needed; everything runs in temporary folders
make install    # ~/.local/bin; doesn't touch your nxm:// handler
```

`fanmo-cli register-nxm` makes fanMO your `nxm://` handler. Nothing else does.

## First run

fanMO finds Skyrim through Steam and asks where the instance should go. An
instance holds mods, downloads and profiles, and must be on the same drive as
the game, because hard links can't cross filesystems.

Add your Nexus API key in Settings. With Premium, collection downloads run on
their own. On a free account Nexus requires a click per file: the Collection
tab lists what's needed and opens the pages, and archives already on disk (see
Archive folders in Settings) are found by MD5 and never downloaded.

## Load order

A collection doesn't contain a load order. Its plugin list only says which
plugins are on; Vortex gets the order from LOOT, using the curator's rules.
fanMO uses LOOT the same way rather than reimplementing it.

After installing a collection, a profile has an order the game can load
(masters first, the curator's rules applied) but not a good one, and the status
bar says so. Then either:

- **Run LOOT.** Add `LOOT.exe` in Settings: fanMO fills in
  `--game --game-path --auto-sort` and reads the result back when it exits. On
  deploy, the curator's rules are copied to LOOT's `userlist.yaml` in the
  prefix, keeping any existing list as `.fanmo-backup`.
- **Import an order.** Plugins tab, Import load order: any LOOT-sorted
  `loadorder.txt`. Plugins it doesn't list stay where they are, so a previous
  revision's order works for the next one.

Updating a collection in a profile keeps the existing order for plugins it
already knows, and asks for sorting again.

## How deployment stays safe

- Nothing is deleted that isn't in fanMO's manifest. A file in the way moves to
  `state/backup` and comes back when the mod that displaced it goes.
- A deployed file with a new inode was replaced by a program. It moves to
  Overwrite instead of being lost.
- An interrupted deploy leaves a journal; the next run finishes from it.
- Purging restores `Data` exactly, including removing folders it created.
- fanMO refuses to deploy to a game folder Vortex is deploying to. Purge in
  Vortex first, or use another install.
- `Plugins.txt`, `loadorder.txt` and any INI it edits get a `.fanmo-backup`
  copy the first time.

## Layout

```text
src/core/    no UI; everything testable headless
  fomod        installer XML, option and flag state, copy instructions
  installer    archive -> mod folder, binary patches
  collection   collection.json, rule sorting, load order
  engine       whole operations: install a collection, activate a profile
  deploy       hard links, manifest, conflicts, capture
  plugins      TES4 headers, Plugins.txt
  wabbajack    .wabbajack lists
  nexus        REST and GraphQL client, downloads
  steam, launcher  finding the game, running through Proton
src/ui/      Qt widgets; theme.h and style.cpp are shared with fanFM
src/cli/     fanmo-cli
tests/       fanmo-tests (core) and fanmo-uitests (widgets, off screen)
```

`fanmo-cli verify-collection` and `verify-deploy` compare what fanMO produces
with an existing Vortex install, which is how the installer was checked.

## Limits

- Skyrim Special Edition only.
- C# script FOMODs don't run (Vortex collections don't record their choices
  either). XML FOMODs do.
- LOOT, xEdit and the like stay Windows tools run through Proton; fanMO doesn't
  sort plugins itself.
- A FOMOD whose output depends on which plugins are active gets the state of
  the finished collection. Vortex uses whatever happened to be active at that
  moment, so the two can differ on such a mod.
- Hard links are real files: a tool that edits a deployed file in place edits
  the mod's copy. Vortex has this too. A tool that replaces the file is fine.
