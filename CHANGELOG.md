# Changelog

fanMO's releases, newest first. Versions follow `MAJOR.MINOR.PATCH`; while
it's 0.x, a minor release can change how things work.

## 0.2.0 — 2026-11-10

- The Nexus API key is encrypted. It's sealed with AES-256-GCM under a
  password you choose (scrypt derives the key), never kept in plain text,
  and fanMO asks for the password when it opens. Not now works without
  Nexus until Game, Unlock Nexus; a download from the browser waits for it.
  A key 0.1 stored in plain text is offered a password on first start.
  `fanmo-cli` asks for the password in the terminal.
- fanMO uses personal API keys only, as the README now says.
- Memory no longer creeps up while the window sits idle: large buffers are
  handed back the moment they're freed, and freed memory is returned every
  ten minutes, not only after a scan or a job.
- Building now needs OpenSSL's libcrypto.

## 0.1.2 — 2026-11-09

- Much less memory with big collections: about 220 MB with Gate to
  Sovngarde loaded, down from over 1 GB. Each rescan used to re-read the
  whole deployment record (61 MB for that collection) to spot unmanaged
  files; it's now read only after a deploy changes it. Memory freed after a
  scan, install or deploy is handed back to the system.
- Rescans after reordering or switching mods take about half as long.

## 0.1.1 — 2026-11-09

- Endorsing a batch of mods now waits one second between requests, so a
  whole collection's worth goes to Nexus at a steady, person-like pace.
  A single endorsement still goes through at once, and Cancel stops a batch
  straight away.

## 0.1.0 — 2026-11-09

The first release: a native C++/Qt 6 mod organizer for Skyrim Special Edition
on Linux, tested end to end on Gate to Sovngarde (1,976 mods, about 234,000
files).

- **Mods and plugins**: a priority list and a load order, both drag to
  reorder, with per-file conflict counts, a conflict viewer, load indexes for
  regular and light plugins, missing-master warnings and "Move after its
  masters".
- **Collections**: install from a Nexus URL, an `nxm://` link, a collection
  bundle or a `collection.json`. Recorded FOMOD choices are replayed, binary
  patches and INI tweaks applied, and mods ordered by the curator's rules. A
  newer revision only touches what changed. Premium accounts download on
  their own; free accounts get each file through the website.
- **Wabbajack lists**: `FromArchive`, `PatchedFromArchive`, `InlineFile` and
  `RemappedInlineFile`. A list needing anything else is refused before
  anything is written.
- **Deployment**: hard links into the game, as a diff against the last run.
  Nothing outside fanMO's manifest is deleted, displaced files are backed up
  and restored, an interrupted deploy finishes on the next run, and purging
  restores the game folder exactly. Root mods such as SKSE go beside the game.
- **Profiles**: separate mod lists and load orders, optionally separate saves
  and game settings.
- **INI tweaks**: from collections, and from an `INI Tweaks` folder in any
  mod, merged into the game's INIs on every deploy.
- **LOOT**: run natively or through Proton, its order read back afterwards,
  with the collection's rules merged into LOOT's userlist beside your own.
- **Tools**: Windows programs through Proton in the game's prefix, Linux
  programs directly; new files they write go to a mod you name or Overwrite.
- **Nexus**: `nxm://` links from the browser download into fanMO, keeping
  each mod's id, name, version, author and category. The ♥ column shows
  which mods you've endorsed, and right-click Endorse votes for one mod or a
  whole selection.
- **fanmo-cli**: the same operations without the window.

Known limits: Skyrim Special Edition only; C# script FOMODs don't run;
separate saves per profile and collections with binary patches have had
little real-world use so far.
