# Unreleased internal companion candidate

Map and equipped-loadout companion for Castlevania: Belmont's Curse, the exact supported1.1.0 executable and
Eden Duo runtime22. The second screen is laid out as the game's pause-menu Map tab, always open:
map above the footer controls, area ribbon (biome and sub-area), gilt corners, an Area/Sub-area zoom rail
and Reset. Discovered landmarks add native engravings and black/gold name ribbons; the
current main objective uses its installed English text. A touch tab opens the native Equipment
composition: equipped Arcana card, weapon illustration, relic icons/names and final stats.
Switching pages preserves the map view. Native geometry, explored-zone fog, font, biome and area names, menu art and
the Rose marker are read from the player's recognized installation. Local touch pan/zoom, the
centered zoom rail and Reset adjust the companion view. Use Pin at the top right to choose one of nine native marker symbols, then tap the map
to place it. Ordinary map taps do not open the chooser. Tap an existing pin to edit or remove
it; Cancel abandons a pending placement. Pins last for the current session only. Equipment category taps open
a scrolling grid of owned native items. Selecting an item navigates the game’s own equipment
menu using ordinary controller inputs, checked against fresh native selection and menu state.
Relics use the native two-step item and destination-slot selection; choices from another native
Relics page first navigate to that page and verify its selection. The compact Equipment stat plate
sits at the top, with tightly spaced native health/mana upgrade fragments inside the same
plate, supporting stat text, and the equipment categories and artwork below. Map stays at the bottom right in every Equipment view. On the
map, Equipment is at the bottom left. The module reads game
memory and RomFS without guest writes or calls. Equipment changes are real gameplay changes.

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

Equipment and owned-inventory acquisition run only while the Equipment view is open; position sampling stays continuous. Each reader deduplicates repeated spans within one sample and rechecks source coherence before publishing.

The generic host map handles local pan/zoom. Sub-area follows Rose; Area frames every revealed
zone of the current region. Reset appears only after pan or pinch; it returns to Sub-area and recentres the local view.
Manual inspection persists through room/loading interruptions and newly revealed map images;
the tier controls explicitly apply their selected view. Title/loading
clear stale map, marker, name and coordinates; a coherent native pause keeps the current map
available. Compatible cinematic/dialogue interruptions retain a dimmed, noninteractive bottom
view; a brief incoherent sample holds the previous brightness and compatible view for at most180ms.
Retained views block equipment and pin actions until a fresh coherent sample succeeds. Explicit
loading, changed identity or unsupported state still clears it. Map motion uses a validated
lightweight refresh between full snapshots. Separate scaled presentation bindings work around
runtime22’s coarse float redraw thresholds; marker_scale converts back to the exact native
world coordinates, while all map bounds, pins and gameplay position values keep native units.

The authored fonts/latin51sc.bfont descriptor requests the native LatinFont at51cap with small
capitals (lowercase drawn as 80% capitals, as in the game's menus); it contains no metrics or
glyphs. Runtime text_scale7/5 targets36/25cap. Desktop screenshots and all217
native English area-label widths were checked using the actual font. Physical readability still
requires the target device. English area labels are explicit; automatic language selection is
pending.

Newly explored sections within a compatible native map fade through four native-color frames. Replacement images are prefetched while the old map stays visible, and recent immutable image keys remain available to in-flight requests. Same-source navigation-room changes retain the compatible map. Actual loading and unsupported contexts still clear it.

The explored-zone edge fades softly as in the native map. Native water and wall ornament material terms are included at frozen time; moving
noise and displacement remain approximations. Landmark visibility requires coherent native
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
base value. The owned inventory and current native picker determine which choices are exposed;
unowned items are not listed. Source room
contours, palette, entered-zone shapes and selected navigation remain native. Exact shader pixel
parity, separate secret-item overlays, FastTravel map geometry and the world-overview screen are
not claimed. Optional DLC absence alone is not an error; no DLC-dependent companion feature is
claimed. Module libraries and their manifest hashes must match the actual build being packaged.

Map marker coverage includes Rose, session pins, verified discovered landmarks, used native save points, discovered non-secret elevator endpoints, and unlocked native blue fast-travel points. Save points use the native normal or last-used glyph only when coherent room usage, last-save identity, map-revelation policy and navigation agree. Unknown conditions stay hidden. Elevator endpoints require an exact coherent entered-zone match; secret-linked and used-only reveals remain hidden. Their native base responsive sizes are retained; native maximum-zoom growth is not mapped to companion pinch zoom. Fast-travel points require a coherent unlocked LevelID plus the native map-revelation and navigation gates. Unknown or locked destinations stay hidden. This is a read-only overlay, separate from the game’s FastTravel map geometry. General door, activable, biome-transition, full elevator/link, mirror, character, death and secret overlays still need separate live-reader coverage.

Navigation and action buttons use the native dark leather tab material, with a brighter armed/active state. Their existing touch bounds and actions are retained.
