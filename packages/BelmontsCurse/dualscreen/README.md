# Unreleased internal companion candidate

Map-first layout for Castlevania: Belmont's Curse, exact supported1.1.0 executable, runtime22.
This is an internal engineering candidate. The first StreetsPoor room is verified on the desktop
harness: installed map geometry and entered-zone fog, native font/player glyph, local pan, Return
to Rose and native menu. Other rooms remain unavailable in this initial slice. Broader coverage,
Android/NCE, physical Thor review and release verification remain pending.

The source includes only JSON, documentation and an authored font-request descriptor. It ships
no game pictures, captured screens, font glyphs or derived map geometry. Native artwork and font
must be decoded from the player's recognized game by the native module. No fallback map fixture
is used. Unready data shows an unavailable state; unsupported modules show the host's actual error.

The module must provide readiness/state flags, discovered-area name, map image/area, native Rose
marker and world-space player/home bindings documented by `module_outputs` and `_map_contract`.
The generic host map handles local pan/zoom and a contextual Return to Rose action. All declared
actions reset a companion-local view; no gameplay button, memory write or guest call is declared.
No controller navigation is captured.

The authored `fonts/latin51.bfont` file asks the module to decode LatinFont at51cap. It contains
no font metrics or glyphs. Runtime text_scale uses five pixels per unit:10/6/5 target50/30/25cap.
Native font coverage, actual visible cap heights and UI alignment need live inspection.

Native libraries are intentionally not staged by this layout change. Both platform references and
hashes must be refreshed against the actual integrated builds before package validation. The exact
build gate, host asset failures, unreadable/missing assets and delayed font readiness must remain
safe. Optional DLC absence alone is not an error; no DLC-dependent feature is claimed here.

The initial renderer freezes time and simplifies decorative noise/water effects. It preserves
installed native contours, colors, skyline and actual entered-zone clipping; exact shader pixel
parity is not claimed. The initial area heading uses the installed English localization table;
automatic language selection is pending.
