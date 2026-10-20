# Unreleased internal companion candidate

Map and equipped-loadout companion for Castlevania: Belmont's Curse, the exact supported1.1.0 executable and
Eden Duo runtime22. The second screen is laid out as the game's pause-menu Map tab, always open:
full-screen map, area ribbon (biome and sub-area), gilt corners, a World/Area/Sub-area zoom rail
and Focus on Rose. Discovered landmarks add native engravings and black/gold name ribbons; the
current main objective uses its installed English text. A touch tab opens the native Equipment
composition: equipped Arcana card, weapon illustration, relic icons/names and final stats.
Switching pages preserves the map view. Native geometry, explored-zone fog, font, biome and area names, menu art and
the Rose marker are read from the player's recognized installation. Local touch pan/zoom, the
zoom rail and Focus on Rose leave gameplay controls available. All declared actions change only the companion view; the module reads game
memory and RomFS without guest writes or calls.

Desktop live checks cover the opening area, walking/reveal changes, transition into the Sewers,
loading/title, native Equipment pause, touch inspection/reset and unsupported-build refusal.
A PC-origin test fixture also loaded a late Sanctuary of Solitude checkpoint; Equipment values
were checked against the native menu there. Broader native source readers cover ten biome registries. Static source comparisons and renderer
checks are distinct from live progression coverage. Physical AYN Thor, Android/NCE execution and
full-game review remain pending; this is not a published or device-verified release.

Source files contain JSON, this documentation and an authored font-request descriptor. They
contain no game artwork, screenshots, font glyphs, localized label text or derived map geometry.
Packaged native libraries decode installed assets at runtime. There is no fallback map fixture.
Unready or unsupported data remains unavailable; unsupported modules display the host's error.

The generic host map handles local pan/zoom. No controller navigation is captured. Sub-area
follows Rose; Area frames every revealed zone of the current region; World stays dimmed until a
world overview is available. Focus on Rose returns to Sub-area and recentres the local view. Title/loading clear stale map, marker, name and coordinates;
a coherent native pause keeps the current map available.

The authored fonts/latin51sc.bfont descriptor requests the native LatinFont at51cap with small
capitals (lowercase drawn as 80% capitals, as in the game's menus); it contains no metrics or
glyphs. Runtime text_scale7/5 targets36/25cap. Desktop screenshots and all217
native English area-label widths were checked using the actual font. Physical readability still
requires the target device. English area labels are explicit; automatic language selection is
pending.

The explored-zone edge fades softly as in the native map. Decorative noise, water and moving
ornament effects are simplified at frozen time. Landmark visibility requires coherent native
discovery and navigation; landmarks with unresolved reveal conditions remain hidden. Engravings
scale with the map while name ribbons retain their native screen-relative size. Objective and
equipment readers fail closed during loading or incoherent data. Equipped weapon, spell, skin
and charm slots join their installed native English names and sprites on a worker. The equipped
spell selects its native revealed Arcana card; weapon illustrations remain separate from icons.
The supported gold portrait material uses its verified static opacity treatment; animated gold
highlights are omitted.
Final ATK/DEF follow native modifier, floor and bonus-sum semantics; HP/MP current/max and level
are read coherently. Unsupported modifiers, weighted/nonlinear curves or unknown wrap modes
leave the affected stat unavailable. A displayed dash means unavailable data, never an invented
base value. Only equipped items are resolved; no future inventory list is exposed. Source room
contours, palette, entered-zone shapes and selected navigation remain native. Exact shader pixel
parity, separate secret-item overlays, FastTravel map geometry and the world-overview screen are
not claimed. Optional DLC absence alone is not an error; no DLC-dependent companion feature is
claimed. Module libraries and their manifest hashes must match the actual build being packaged.
