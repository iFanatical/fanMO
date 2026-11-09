# fanMO

A native C++/Qt6 mod organizer for Skyrim Special Edition on Linux. It has
MO2's layout and a priority list you drag around, deploys with hard links the
way Vortex does, and installs Nexus collections and Wabbajack lists without
Wine. Windows tools still run, through the game's Proton prefix; Linux
programs, a native LOOT among them, run directly.

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
- **Root mods**: SKSE, preloaders and other files that belong beside the game
  go to the game folder, not `Data`. Collections mark these themselves; for an
  archive you install yourself, fanMO recognises `skse64_loader.exe` and the
  DLLs Windows loads from there, and says so in the log.
- **Profiles**: separate mod lists and load orders, optionally separate saves
  and INIs.
- **Tools**: Windows programs run through Proton in the game's prefix; Linux
  programs (anything that isn't `.exe`, `.bat`, `.cmd`, `.com` or `.msi`) run
  as they are. New files in `Data` go to a mod you name, or to Overwrite. A
  tool marked as a sorter (LOOT) has its load order read back afterwards.

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

- **Run LOOT.** Add LOOT in Settings: fanMO fills in
  `--game --game-path --auto-sort` and reads the result back when it exits.
  - *Native LOOT* (recommended): a Linux build of LOOT, such as the one
    `../loot/build.sh` makes. Add its `LOOT` program. It runs without Proton,
    finds the game's prefix through Steam, and sorts the same `Plugins.txt`.
    It keeps its data in `~/.local/share/LOOT` (or `--loot-data-path`).
  - *Windows LOOT*: add `LOOT.exe`. LOOT 0.29 needs one file Proton doesn't
    provide: copy Proton's `files/lib/wine/x86_64-windows/icu.dll` into LOOT's
    program folder as `icuuc.dll`, or LOOT exits at once with no window.

  On deploy, the curator's rules are merged into every LOOT's
  `userlist.yaml`: the prefix's, and each native LOOT tool's. A native LOOT
  with no userlist yet starts from a copy of the prefix's, so your own rules
  come along; after that the two are separate files.
  If a sorter exits with an
  error, or changes nothing, fanMO doesn't read the order back, and the log
  says so; each tool's output is kept in `logs/` in the instance.
- **Import an order.** Plugins tab, Import load order: any LOOT-sorted
  `loadorder.txt`. Plugins it doesn't list stay where they are, so a previous
  revision's order works for the next one.

Updating a collection in a profile keeps the existing order for plugins it
already knows, and asks for sorting again. An imported order that loads a
master after a plugin needing it also counts as unsorted.

For a plugin flagged as missing masters, right-click, Move after its masters,
puts it just below them, and takes along anything that move would leave above
its own master. A master that isn't installed or is switched off is reported
in the log instead: moving can't fix those.

The userlist is shared with you: rules you add in LOOT stay. fanMO records what
it added in `state/loot-userlist.json` (and `state/loot-userlist-native-*.json`
for a native LOOT) and takes back only that, so a rule a new
revision dropped goes away, and a profile without a collection gets your list
back. Where you and a collection disagree on something with one answer, such as
a plugin's group, yours stays and the deploy log says so. A rule of the
curator's that you delete comes back on the next deploy; to override one, set
your own value instead. The first version of your list is kept as
`userlist.yaml.fanmo-backup`.

If the list uses YAML that fanMO can't rewrite faithfully, such as anchors, it
leaves the file alone and warns, and the collection's rules aren't added.

## INI tweaks

Every deploy merges INI tweaks into the INIs in `My Games`. The name in
brackets picks the file: `My Changes [Skyrim].ini` goes into `Skyrim.ini`,
`[SkyrimPrefs]` into `SkyrimPrefs.ini`, and a name without brackets into
`Skyrim.ini`. Each key replaces the same key in the same section, ignoring
case; a key that isn't there is added to its section. Nothing else in the INI
changes, and removing a key from a tweak doesn't remove it from the INI.

Tweaks come from two places, applied in this order:

1. The collection's, copied into the profile's `ini-tweaks/` folder when the
   collection is installed. Installing it again copies them fresh, so edits
   made there don't survive an update.
2. Any enabled mod's `INI Tweaks/` folder, Vortex's convention, in mod
   priority order. That folder isn't deployed into `Data`. Since these go
   last, a mod of your own overrides the collection's values and survives
   reinstalls. Vortex makes you turn each of a mod's tweaks on; fanMO applies
   them whenever the mod is enabled, and the deploy log lists each one.

Editing a tweak doesn't mark the profile as changed, so press Deploy to apply
it; Run deploys first only when the mod or plugin lists have changed.

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
  userlist     merging a collection's rules into LOOT's userlist.yaml
  yaml         the YAML reader and writer for it
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
- fanMO doesn't sort plugins itself: it runs LOOT, natively or through
  Proton. xEdit and the like stay Windows tools run through Proton.
- A FOMOD whose output depends on which plugins are active gets the state of
  the finished collection. Vortex uses whatever happened to be active at that
  moment, so the two can differ on such a mod.
- Right-click, Deploy to game folder, covers a root mod fanMO didn't recognise.
  It treats the mod's folder as the game folder and moves nothing, so a mod
  installed with an extra folder around its files needs that folder removed.
  FOMOD archives are never detected as root mods; they say where files go.
- Hard links are real files: a tool that edits a deployed file in place edits
  the mod's copy. Vortex has this too. A tool that replaces the file is fine.
