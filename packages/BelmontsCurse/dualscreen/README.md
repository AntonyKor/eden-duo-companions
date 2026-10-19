# Unreleased internal companion candidate

Map-first companion for Castlevania: Belmont's Curse, the exact supported1.1.0 executable and
Eden Duo runtime22. Native geometry, explored-zone fog, font, area name and Rose marker are read
from the player's recognized installation. Local touch pan/zoom and Return to Rose leave gameplay
controls available. All declared actions change only the companion view; the module reads game
memory and RomFS without guest writes or calls.

Desktop live checks cover the opening area, walking/reveal changes, transition into the Sewers,
loading/title, native Equipment pause, touch inspection/reset and unsupported-build refusal.
Broader native source readers cover ten biome registries. Static source comparisons and renderer
checks are distinct from live progression coverage. Physical AYN Thor, Android/NCE execution and
full-game review remain pending; this is not a published or device-verified release.

Source files contain JSON, this documentation and an authored font-request descriptor. They
contain no game artwork, screenshots, font glyphs, localized label text or derived map geometry.
Packaged native libraries decode installed assets at runtime. There is no fallback map fixture.
Unready or unsupported data remains unavailable; unsupported modules display the host's error.

The generic host map handles local pan/zoom. No controller navigation is captured. Contextual
Return to Rose resets the local view. Title/loading clear stale map, marker, name and coordinates;
a coherent native pause keeps the current map available.

The authored fonts/latin51.bfont descriptor requests the native LatinFont at51cap; it contains
no metrics or glyphs. Runtime text_scale10/6/5 targets50/30/25cap. Desktop screenshots and all217
native English area-label widths were checked using the actual font. Physical readability still
requires the target device. English area labels are explicit; automatic language selection is
pending.

Decorative noise, water and moving ornament effects are simplified at frozen time. Source room
contours, palette, entered-zone shapes and selected navigation remain native. Exact shader pixel
parity, separate secret-item overlays, FastTravel map geometry and the world-overview screen are
not claimed. Optional DLC absence alone is not an error; no DLC-dependent companion feature is
claimed. Module libraries and their manifest hashes must match the actual build being packaged.
