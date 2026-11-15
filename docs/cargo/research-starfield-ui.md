# Starfield UI language: research notes and Slate tokens

Reference study for the drone cargo-loading UI. Compiled 2026-11-14. Starfield shipped 2023-09-06, and
its *Shattered Space* expansion came out 2024-09. Quotes are paraphrased. Bracketed numbers point to Sources.

**Evidence levels used below**
- **[S]** stated by a source: developer, agency, a guide, or a mod author who unpacked the game files.
- **[O]** our own observation of the game's look. It is plausible but not checked against a source, so verify it on screenshots.
- **[P]** our proposal for this project.

Not found: a GDC talk or public interview by Bethesda's UI/UX team. No published colour values, grid, or
type scale exist either. Every token in §7 is therefore [P], informed by [S] and [O].

## 1. Design philosophy: "NASApunk"

- **Grounded tech [S].** Art director Istvan Pely coined NASA-punk for a sci-fi world that is grounded and relatable, with
  future tech extrapolated from today's spaceflight hardware [1][2]. In the Hypebeast interview (2023-08-30) he
  explains the two halves:
  - "NASA" is heavily engineered, over-built survival gear.
  - "Punk" is the cultures and cities.

  The game was treated as a future period piece, with a pre-CGI 1970s–80s tactile undercurrent [3].
- **Shared reference [S].** Lead animator Rick Vicens said the label gave the whole art team one reference point, and that this mattered early in the project
  (VGC, 2022-02-18) [2].
- **Brand identity [S].** The identity was built by AKQA. Its principles were scientifically inspired, warm, optimistic, and bold. Visually it was
  minimalist and clinically simple, but still looked human-made. Its cornerstones were the starmap, the gravitational wave, and
  the Constellation emblem, *a stripe of four earthly colours* [4].
- **Combat UI [S].** The space-combat HUD had to stay clean so dogfights don't overload the player. It also had to separate friend from foe, showing
  both intent and threat (Tim Lamb, Space.com) [5].
- **Diegetic screens [S].** Players praised the in-world consoles, kiosks, and terminals as convincing and functional-looking
  (2024-03) [6]. Accessibility reviews criticised the same screens [7].
- **What this means for us [P].**
  - Flat print-like panels, like instrument placards or NASA checklists, rather than holograms.
  - Solid colour bars and thin rules, with no glow, bloom, or scanlines.
  - Plenty of negative space.
  - Colour used to carry meaning, not as decoration.

## 2. Typography

| Aspect | Finding | Level |
|---|---|---|
| Families | The UI uses two Neubau Berlin faces, **NB Architekt** and **NB Grotesk (R)**, both in `Interface/fonts_en.swf`, mapped through `fontconfig.txt`. NB Architekt Bold is a default face in several places. A Consolas-based `$DebugTextFont` also exists. JP builds ship "JP_NB Grotesk Regular/Bold". | [S] mod authors [8][9][10] |
| Character | NB Architekt is a monoline tribute to Letraset-era architectural-plan lettering (Stefan Gandl). Mod authors describe the game's fonts as rounded and less readable for long text. | [S] [8][11] |
| Size | Widely criticised as too small ("abysmally small" in GamesRadar's report of advocates' complaints). The menu text-size option didn't cover dialogue subtitles at launch. | [S] [12][13] |
| Case / tracking | Headers and category labels are uppercase and widely tracked. Body and descriptions are sentence case. Numbers are large and set apart from their units. | [O] |

## 3. Colour

| Element | Finding | Level |
|---|---|---|
| Emblem | Constellation stripe of four earthly colours. Names and hex values are not published. To the eye it reads as rust-red, orange, ochre/gold, and blue. | [S] [4] / [O] |
| On-foot HUD | White O₂ bar (red when empty). Health bar turns red when low. Blue power bar. White reload bars in ship mode. | [S] [14] |
| Radar | Red dots = enemies. White squares = locations. Blue circles = mission objectives. | [S] [14] |
| Scanner | Interactables get a blue outline. Fully scanned targets turn green. Unsurveyed resources are grey in the list and turn coloured once surveyed. The route to a quest marker is drawn as white ground arrows. | [S] [15][16][14] |
| Kiosks | Brand-coloured buttons (Ryujin red, for example). The focus state is only a slightly lighter shade, which reviewers flagged as low contrast. Scrollbars are thin grey lines. | [S] [7] |
| Menus | White or off-white type on a dark translucent scrim over the blurred world. The selected row is inverted (light bar, dark text). Thin light-grey rules. Flat rectangular stat and capacity bars. | [O] |

## 4. Layout and screens

- **Grid.** No published grid. To the eye: a list column on the left, a detail or item card on the right, a category icon row on
  top, and a footer with key hints and status [O].
- **Inventory.**
  - The current/max mass ratio sits at the bottom-left, in kg [S][17].
  - Categories stack, and players lose track of where they are. Digipicks are hidden under Misc [S][13].
- **Transfer (container or cargo hold).**
  - Opening the transfer screen (R / X) shows the container. Q / LB toggles to the personal inventory [S][17][18].
  - Selecting a stack opens a **quantity pop-up with a slider** [S]. StarUI's "skip the quantity pop-up" feature (Ctrl/Shift for one or all) shows that the pop-up exists [S][19].
  - "Store All Resources" moves every resource in one action [S][18].
  - Both sides show capacity as current/max mass, and a transfer fails silently when the hold is full [S][18].
- **Cargo hold.** Open it from the blue cockpit panel or from the Ship menu ("Cargo Hold"). It also works remotely near the ship or in major cities [S][18].
  Capacity comes from Cargo modules added in the Ship Builder at a ship technician, and the Payloads skill adds up to +50 % [S][18].
- **Ship builder.** An orbit camera around a 3D ship, with module lists, snap points, and attachment markers on the model. A side panel
  shows stats such as cargo, mass, and mobility, with deltas when a part is swapped [O].
- **Outpost cargo link.**
  - Two containers stand with a control panel between them. The player picks the target outpost from a list.
  - Two kinds of link: in-system, and inter-system (fuelled by He-3).
  - The number of links per outpost is disputed [S][20][21].
- **Scanner mode** (F / LB). A circular scan window sits at centre. The quest title is at the top, and the planet survey % with its category list (resources, flora,
  fauna, features) is on the left. Unknown-location markers and small type icons (manmade, science outpost, …) appear in the world
  [S][14][22]. Aiming at a target shows its name and the scan result next to the reticle [O].
  - **Leader-line callouts are not documented.** No source describes Starfield callouts with leader lines attached to 3D
    objects, or any declutter or stacking rules. The scanner mainly uses outlines and icons.
  - Clutter is a known issue: the popular "Scanner Reworked" mod shrinks the icons and removes the circle and distance read-out [S][23][24].
  - The leader-line rules in §7 are therefore our own, in the spirit of the game's terminal and starmap info panels.

## 5. Screenshot references (links only)

- Scanner HUD: https://www.gamepressure.com/starfield/gfx/word/359176750.jpg
- On-foot HUD: https://www.gamepressure.com/starfield/gfx/word/359176734.jpg
- Starmap: https://www.gamepressure.com/starfield/gfx/word/359176765.jpg
- Planet map: https://www.gamepressure.com/starfield/gfx/word/359176781.jpg
- Ship HUD: https://www.gamepressure.com/starfield/gfx/word/359176796.jpg
- Kiosk and terminal examples, with annotated screenshots: https://cloudfour.com/thinks/starfield-accessibility/
- Game UI Database (search "Starfield"; the site blocks fetching): https://www.gameuidatabase.com/

## 6. Free fonts that approximate the look (all SIL OFL 1.1, Google Fonts, can be embedded in UE)

| Role | Starfield | Suggested | Why |
|---|---|---|---|
| Labels, headers (caps, tracked) | NB Architekt Bold | **Barlow** / **Barlow Semi Condensed** (Medium–SemiBold) | A slightly rounded, DIN-like monoline grotesk with an engineering feel. Holds up at small sizes. |
| Body, descriptions | NB Grotesk R | **Inter** (Regular/Medium) | A neutral grotesk with excellent hinting and tabular figures (`tnum`). |
| Alternative display | NB Architekt | **Exo 2** (used by a community font-replacement mod), **Space Grotesk** | Geometric, technical |
| Numbers, IDs, SCU read-outs | — | **JetBrains Mono** or **IBM Plex Mono** | Fixed-width columns for manifests |

## 7. Design tokens for Unreal Slate

These are 1080p reference values. Scale them with DPI (`UUserInterfaceSettings` DPI curve). Every value here is **[P]**. Where a value
follows a sourced or observed trait, the column says so.

### Colour

| Token | Hex | Use | Basis |
|---|---|---|---|
| `paper` | `#ECEAE4` | Panel fill (off-white placard) | [O] inverted panels and light selection |
| `paper-dim` | `#D6D3CB` | Secondary panel, disabled fill | — |
| `ink` | `#1B1C20` | Text on paper | — |
| `ink-2` | `#5B5D64` | Secondary text, units | — |
| `rule` | `#A9A69E` | 1 px dividers on paper | [S] thin grey rules [7] |
| `scrim` | `#0C0E12` @ 72 % | Backdrop behind full-screen panels | [O] |
| `on-scrim` | `#F2F1EC` | Text and lines over the world or scrim | [O] |
| `accent-blue` | `#3A78C2` | Interactable, selection, leader lines on target | [S] blue outlines and objectives [14][15] |
| `ok-green` | `#5FAF6A` | Loaded, scanned, complete | [S] scanned = green [16] |
| `warn` | `#E3A23A` | In progress, near capacity | — |
| `alert` | `#C8452F` | Blocked, over capacity, obstructed | [S] red = critical [14] |
| `stripe-1..4` | `#B4422F` `#DB7A2C` `#E5B43C` `#3B6EA8` | Four-stripe brand motif: 4 px bars on a panel's top-left edge or in section headers | [S] four earthly colours [4], hues [O] |

Rules: colour always pairs with text or an icon (this answers the kiosk-contrast criticism [7]). Selection is an inverted bar
(`ink` on `paper`, or `paper` on `scrim`), never just a lighter tint. Contrast stays at or above 4.5:1 for text.

### Type scale (px at 1080p)

| Token | Size / line | Weight | Case / tracking | Font |
|---|---|---|---|---|
| `display` | 32 / 36 | SemiBold | UPPER, +6 % | Barlow |
| `title` | 22 / 28 | SemiBold | UPPER, +8 % | Barlow |
| `section` | 15 / 20 | Medium | UPPER, +14 % | Barlow Semi Condensed |
| `body` | 16 / 22 | Regular | Sentence, 0 | Inter |
| `label` | 13 / 16 | Medium | UPPER, +12 % | Barlow Semi Condensed |
| `numeric-lg` | 28 / 32 | Medium | tabular | Inter (tnum) or JetBrains Mono |
| `numeric` | 16 / 20 | Regular | tabular | JetBrains Mono |

The minimum is 13 px for caps labels and 16 px for running text. That stays deliberately above Starfield's criticised sizes [12][13].

### Geometry

| Token | Value |
|---|---|
| Base grid | 8 px. Internal padding 16 px, panel gutter 24 px, screen-safe margin 48 px |
| Strokes | Rules 1 px. Leader lines 1.5 px (2 px when selected). Focus outline 2 px `accent-blue` |
| Corner radius | **0** everywhere (flat placards). 2 px only on toggle pills if needed |
| Bars (capacity, progress) | 6 px tall, flat fill on a `paper-dim` track. A tick every 25 %. Value shown as text to the right, e.g. `48 / 64 SCU` |
| Stripe motif | 4 bars, each 4 × 24 px with a 2 px gap, or one 4 px horizontal band split 4 ways. Max one per panel |
| Icons | 16 / 24 px, 1.5 px monoline, square caps |

### World-space callouts (leader lines)

- **Anchor.** A 5 px filled dot (`on-scrim`, or `accent-blue` when targeted) at the object's projected pivot or grid cell
  centre. A 1 px dark halo keeps it readable on bright backgrounds.
- **Line.** It runs from the anchor at 45° (or vertical) for 24–64 px to an elbow, then horizontally (the *shelf*) for at least 16 px into the
  label edge. Only 0°, 45°, and 90° segments are allowed, and lines never cross other lines or labels.
- **Placement.** The label sits outside the object's projected screen bounds plus a 16 px margin, so the object itself is never
  covered. Candidate slots go NE → NW → SE → SW. Keep the current side unless it collides.
- **Stacking.**
  - Labels on one side line up in a column with a 6 px gap.
  - Order is by anchor screen-y, so that lines don't cross.
  - The column slides as one block, and the elbows absorb the offset.
- **Declutter priority.** Selected → active drone / its box → mission or manifest cargo → grids → others.
  - At most 8 labels are visible. Lower-priority labels collapse to a dot only.
  - Below 20 px of projected object size, show the dot only. Beyond the distance cut-off, hide.
- **Occlusion.** When the anchor is hidden, draw the line dashed (4 on, 3 off) at 50 % alpha. After 0.5 s occluded, hide the label.
- **Stability.**
  - Re-solve the layout at most every 100 ms.
  - Change side only after a 12 px or 150 ms hysteresis.
  - Labels move by lerp (τ ≈ 80 ms), never by snapping.
- **Content.** One `label` line (UPPER) with the name or ID. One `numeric` line with a value and unit (`16 SCU · 4×2×2`). Optionally, a
  6 px status bar.

### Motion

| Event | Timing | Easing |
|---|---|---|
| Callout in | Line draws 140 ms (anchor → elbow → shelf), then the label fades in over 120 ms with a 6 px slide from the shelf side | ease-out cubic |
| Callout out | 90 ms fade, line retracts 90 ms | ease-in quad |
| Panel open / close | 200 ms fade + 12 px slide / 140 ms | ease-out / ease-in |
| List row select | 80 ms inverted-bar fill | linear |
| Stagger (lists, stacked callouts) | 25 ms per item, max 8 | — |
| Capacity / progress bar | Tracks the value with τ 120 ms. A step greater than 25 % flashes `warn` or `ok-green` for 300 ms | ease-out |
| Reduced motion | Slides off, fades ≤ 80 ms | — |

## Sources

1. TechRadar, "Starfield's developers are aiming for a NASA-Punk aesthetic": https://www.techradar.com/news/starfields-developers-are-aiming-for-a-nasa-punk-aesthetic ; GamesRadar: https://gamesradar.com/starfield-is-aiming-for-a-very-realistic-and-nasa-punk-sci-fi-world
2. VGC, 2022-02-18, "Starfield developers discuss the game's NASA-Punk aesthetic": https://www.videogameschronicle.com/news/starfield-developers-discuss-the-games-nasa-punk-aesthetic
3. Hypebeast, 2023-08-30, interview with Istvan Pely: https://hypebeast.com/2023/8/starfield-art-launch-interview-istvan-pely-bethesda ; Domus, 2023-09-01, "NASApunk": https://www.domusweb.it/en/news/2023/09/01/nasapunk-the-new-aesthetics-of-spacial-exploration.html
4. AKQA, "Designed for Exploration" (Starfield identity): https://www.akqa.com/work/bethesda/designed-for-exploration/
5. Space.com, interview with Tim Lamb on space combat and UI: https://www.space.com/entertainment/space-games/finding-the-fun-was-always-our-guiding-star-how-starfield-developers-balanced-realism-with-arcade-fun-when-approaching-space-combat-exclusive
6. Shazoo, 2024-03-07, fan praise for Starfield's consoles and displays (RU): https://shazoo.ru/2024/03/07/154336/fanat-interfeisov-poxvalil-otlicnyi-ui-dizain-razlicnyx-konsolei-i-displeev-starfield
7. Cloud Four (Scott Vandehey), 2023-09-19, "Starfield's Accessibility Problems": https://cloudfour.com/thinks/starfield-accessibility/
8. Starfield String Sharpener (font mod description): https://allmods.net/starfield-mods/starfield-user-interface/starfield-string-sharpener-a-font-overhaul-v2-6-1/ ; Nexus: https://www.nexusmods.com/starfield/mods/1393
9. Nexus mod 299 (JP font replacement, NB Grotesk mapping): https://www.nexusmods.com/starfield/mods/299
10. Font-mod file notes (fonts_en.swf / fontconfig.txt): https://www.nexusmods.com/starfield/mods/15915
11. NB Architekt (Neubau / Stefan Gandl): https://maxibestof.one/typefaces/nb-architekt ; https://luc.devroye.org/fonts-37346.html
12. GamesRadar, "abysmally small UI text": https://gamesradar.com/starfield-is-taking-heat-from-accessibility-advocates-for-its-abysmally-small-ui-text
13. Siliconera (Cody Perez), 2023-09-02: https://www.siliconera.com/starfields-biggest-ui-problem-isnt-the-start-screen/
14. Gamepressure, Starfield HUD guide: https://www.gamepressure.com/starfield/hud/z810f0a
15. GamerGuides, scanner highlighting: https://gamerguides.com/starfield/guide/gameplay/basics/how-to-highlight-loot-and-scan-objects-in-starfield
16. Gfinity, scanner guide: https://www.gfinityesports.com/article/starfield-scanning ; Game8 scanner markers: https://game8.co/games/Starfield/archives/421265
17. Deltia's Gaming / GamerJournalist, transfer and mass: https://gamerjournalist.com/how-to-transfer-items-from-personal-inventory-to-ship-inventory-in-starfield-storage-guide
18. GamerGuides, cargo hold: https://gamerguides.com/starfield/guide/gameplay/basics/how-to-store-items-on-your-ship-how-to-access-and-upgrade-the-cargo-hold-in-starfield
19. StarUI Inventory (Nexus 773): https://www.nexusmods.com/starfield/mods/773 ; PC Gamer: https://www.pcgamer.com/the-same-modder-who-fixed-starfields-inventory-has-now-sorted-out-the-rest-of-the-ui
20. PC Gamer, cargo link: https://pcgamer.com/starfield-cargo-link ; GamerGuides, outpost links: https://gamerguides.com/starfield/guide/outposts/bases/a-guide-to-starfields-outpost-links-and-connecting-outposts-together
21. UESP Starfield Outposts: https://starfieldwiki.net/wiki/Starfield:Outposts
22. mein-mmo, 2023-09-07, what the scanner shows: https://mein-mmo.de/en/i-was-surprised-that-the-scanner-in-starfield-can-do-much-more-than-just-display-resources,1033208
23. Wccftech, scanner and inventory mods: https://wccftech.com/starfield-mods-inventory/amp/
24. Nexus scanner-declutter mod: https://www.nexusmods.com/starfield/mods/8986
