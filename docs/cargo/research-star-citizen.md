# Star Citizen cargo: research notes

Research for the drone cargo-loading vertical slice. Compiled 2026-11-14. LIVE is 4.10.x
(our pinned data is 4.10.2; 4.10.3 went LIVE on 2026-11-12). Each fact carries the patch or date of its
source. Quotes are paraphrased. Bracketed numbers point to the Sources list.

**Project rule reminder.** These numbers are for orientation only. The viewer must read grid sizes,
container sizes, and ship dimensions from DataCore / P4k (AGENTS.md: no hard-coded game values). Where
sources disagree (Hull A dimensions, for example), the game data decides.

## 1. Units and containers

| Term | Fact | Source / patch |
|---|---|---|
| SCU (Standard Cargo Unit) | A 1.25 m cube: 1 m³ of usable volume plus 125 mm of shell on every side. One SCU holds 100 units of a commodity. Commodities don't mix inside one crate. | Shipyard: SCU and Cargo Capacity, 2017-10-25 [1]; 1-SCU container page, 4.x [2] |
| µSCU / cSCU | Sub-units used for personal and item inventories. For example, the Hull A has a 1,400,000 µSCU personal inventory, and a 2-SCU box is listed as 200 cSCU. | Wiki, 4.10.2 [4][3] |
| Container sizes | 1/8, 1, 2, 4, 8, 16, 24, 32 SCU | Wiki Container category, 4.x [3] |

### Standard container dimensions (exterior)

The cell is one SCU, 1.25 m on each side. Footprints are L × W × H.

| SCU | Metres (L×W×H) | Cells (L×W×H) | Source |
|---|---|---|---|
| 1 | 1.25 × 1.25 × 1.25 | 1×1×1 | [2] |
| 2 | 2.50 × 1.25 × 1.25 | 2×1×1 | [5] |
| 4 | 2.50 × 2.50 × 1.25 (flat) | 2×2×1 | [6] |
| 8 | 2.50 × 2.50 × 2.50 | 2×2×2 | [7] |
| 16 | 5.00 × 2.50 × 2.50 | 4×2×2 | [8] |
| 24 | 7.50 × 2.50 × 2.50 | 6×2×2 | [9] |
| 32 | 10.00 × 2.50 × 2.50 | 8×2×2 | [10] |

Every size from 8 SCU up has a 2×2-cell cross-section (2.5 × 2.5 m) and grows only in length. The 1/8-SCU
box exists, but its dimensions were not verified.

## 2. Cargo grids

- **What a grid is.** Ships, elevators, and some vehicles carry *cargo grids*: axis-aligned volumes measured
  in SCU cells. Each grid also has a max box size. Containers "fix themselves to the cargo plates" [3]. Since
  3.24, cargo snaps onto a grid when its dimensions fit [11]. In 4.10 there is a snap indicator that shows whether a crate can attach,
  and crates are released from the grid with the tractor beam or by hand [12][13].
- **Manifest.** Cargo that is locked on the grid is "declared" on the manifest. Loose cargo is not
  broadcast, which is the hook for smuggling (2017 design) [1].
- **Grid shapes.** Grids are rectangular boxes. Large ships use several grids, and grids can be rebuilt: the RAFT
  was merged into a single 192 SCU grid (10 × 2.5 × 15 m, 16-SCU max) in 4.1.1 [14]. Examples from 4.7–4.8 item data:
  the Zeus grid is 4 SCU with a 4-SCU max, and the Constellation Taurus tail grid is 6 SCU [14].
- **Max container per grid** is a property of each grid, not of each ship. For the Hull A it is 16 SCU (§4).

## 3. Freight elevators (hangar and station)

| Item | Fact | Source / patch |
|---|---|---|
| Introduced | Alpha 3.24.0 (Cargo Empires, 2024-08), together with physicalised cargo. External station and Levski freight elevators came in 4.4.0 (2025-11-19). | [11][13] |
| Where | Personal and staging hangars, outposts, scrapyards, distribution-centre wings, and the external cargo deck of equipped stations | [11][13] |
| Per hangar | Small hangar (S1–S2 ships): 1× small elevator. Medium (S3–S4): 2× medium. Large (S5): 2× large. XL (S6): 2× XL. | Hangars page, 4.10.1 [15] |
| Platform grid | **Small 6×5×4 cells = 120 SCU** (7.5 × 6.25 × 5.0 m). **Medium 8×7×4 = 224 SCU** (10 × 8.75 × 5 m). **Large 16×14×4 = 896 SCU** (20 × 17.5 × 5 m). **XL 20×14×4 = 1120 SCU** (25 × 17.5 × 5 m). The wiki lists width × depth × height without units. The figures only work out as SCU cells (6·5·4 = 120), so the metre values are our conversion. | Freight elevator page [13] |
| Size limit | A community 3.24 FAQ reports that the small elevator accepts boxes only up to 16 SCU, and that a 32-SCU box breaks it. The FAQ itself says this is unverified. A 32-SCU box (8 cells long) can't lie flat on a 6×5 platform anyway. | [16] |

**Kiosk UI (the "Freight Elevator Kiosk", also called "Freight Manager").**
- **Two inventories.** The kiosk shows the location's stored inventory (the warehouse / local inventory) and the
  elevator platform as separate lists. The player assigns items to the platform, then raises the elevator. To store cargo, the player
  places crates on the platform, lowers it, and deposits them [11][12][13].
- **Item details.** The kiosk shows sub-inventories (for example purchasable cargo boxes) and details for mission items [11].
- **Mission tags.** Mission cargo is tagged, and the list can be filtered to the cargo a mission needs [12].
- **Submission screen (4.10).** Sending the platform down at a destination opens a submission screen. It lists the recognised
  and missing mission cargo, then asks the player to confirm [12].
- **Claim mode.** One guide describes a "Claim Mode" step in the hangar Freight Manager [17] (third party, undated).
- **Errors.**
  - "Elevator Obstructed" appeared in 3.24 when returned cargo was blocking the platform [11].
  - Transfers between the Item Bank and the Freight Manager could fail in 3.24 [11].
- **Item Bank vs. Freight Manager.** FPS items go through Item Banks and Storage Access kiosks. Cargo goes through the Freight Manager [11].

## 4. Loading and unloading paths (4.x)

| Path | How it works | Facts / limits |
|---|---|---|
| **Automated loading** | At a commodity kiosk the player picks the stored ship as the destination, and the location loads it. | 3.20 added automated loading at LEO stations and boxes up to 32 SCU. In 3.24 it became a paid option: it costs aUEC, it is slower than loading by hand, the ship can't be used until it finishes, the duration depends on box count and size, and progress shows on the ASOP terminal [11][18][19]. It needs a location with hangars and a legal landing. The 4.x cargo guide says only that it costs "time and aUEC" [19]. **No public fee or timing table was found.** |
| **Hull C cargo loading areas** | The Hull C docks at station cargo decks for arm-based loading. | Exclusive to the Hull C [11]. Its loading has been repeatedly broken and fixed (4.0.2, 4.4, 4.5) [20]. |
| **Manual: freight elevator + tractor beam** | Raise the platform, tractor each box into the ship, snap it to the grid. Reverse at the destination. | This is the standard path for most haulers, the Hull A included [12][21] |
| Multi-tool + TruHold attachment | Handheld, one-handed | In 3.24 its cargo mass was limited to boxes under 24 SCU, with less range and speed [11][22]. A 3.24.1 Q&A cites 16 SCU [23]. |
| MaxLift | Handheld, two-handed (Greycat) | **16 SCU with one beam. Two beams combined: 32 SCU.** Range 100 m, force 440 kN, max angle 70°, tether break after 2.5 s. Art refactor in 4.6.0. | MaxLift page, 4.10.2 [24] |
| ATLS power suit | Pulls the target next to the user, selects grids faster, snaps from range | 32 SCU limit (CIG Q&A) [23][12] |
| Ship / vehicle tractor | Ship-mounted turrets (Hull B, Hermes rail, etc.) | Up to 32 SCU [21][19] |

**Hauling missions.**
- 3.24 launched three grades:
  - Small: boxes of 4 SCU or less.
  - Supply: 8 or 16 SCU.
  - Bulk: no limit, reputation-gated.
- Mission variants: A→B, multi→single, single→multi [11].
- By 4.10 the tiers are:
  - XS: up to 10 SCU in 1-SCU boxes.
  - S: about 24 SCU in 4-SCU boxes.
  - M: up to 600 SCU in 8–16-SCU boxes.
  - L: over 600 SCU in 32-SCU boxes.
- Distances are Direct, Planetary, Solar, and Interstellar. Interstellar runs (Stanton, Pyro, Nyx) arrived in 4.4 and allow partial completion [21][19].
- Mission cargo appears on the hangar elevator, and the destination freight terminal collects it automatically [21].

## 5. MISC Hull A

| Field | Value | Source |
|---|---|---|
| Role / size | Light freight, size class Small (S2), 1 crew. In the PU since 3.17.0 (2022-04-29). | [4] |
| Capacity | **64 SCU, all external** | [4][25] |
| Grids | **4 open external grids** (`MISC_Hull_A_CargoPlate`), **each 2.5 × 2.5 × 5.0 m = 16 SCU**, box size 16 SCU | API wiki, 4.10.3 [25]; [4] |
| Accepted boxes | Any mix of 1, 2, 4, 8, 16 SCU. **Max 16 SCU.** The Impound says 32 SCU, but that is an outlier. | [4] |
| Layout | A telescoping central spindle, compact when retracted, that extends to carry the full 64 SCU. Each grid holds exactly one 16-SCU box (4×2×2 cells), so it reads as four 16-SCU slots around or along the spindle. **The exact placement and orientation of each plate must come from the ship's geometry and DataCore.** | [4][26] |
| Dimensions | Conflicting: 24 × 10 × 8 m, with (17 × 4 × 4) in brackets, probably the retracted state [4]; or 30.0 × 12.5 × 8.0 m in 4.10.3 data [25]. The RSI pledge page says 24 m long [27]. | [4][25][27] |
| Mass | 122,650 kg | [4] |
| Loading in game | Manual. Its S2 size fits a small hangar, whose small elevator (6×5×4, ≤16-SCU boxes) matches the Hull A's 16-SCU max box. Players load from the elevator with a MaxLift (16 SCU, one beam) or a ship tractor. Automated loading of a stored Hull A is available at hangar locations. Hull C-style station loading areas don't apply. Hull-series design intent: the spindle trades protection for fast EVA or ground loading [26]. | [13][15][21][24][26] |

## 6. mobiGlas

- **What it is.** A wrist-band computer made by microTech. The user raises the forearm, unlocks it with a fingerprint, and it
  projects a holographic UI in front of the face. It can also pair with lenses, glasses, visors, or screens. The projector
  sits on top of the wrist case, and the UI is diegetic: the character sees it too [28]. Lore: the first
  generation (2818) was much bulkier than today's thick bracelet [28].
- **Opening it.** The Home app is on F1 by default; it was F9 back in 2.0 [28].
- **3.23 rework (2024-05-10).**
  - mobiGlas, Visor, and Lens all moved to *Building Blocks*, CIG's UI framework, for performance and easier app development.
  - Reworked apps: Home, Starmap/Maps (search, trip planning), Contract Manager, mo.Trader, NikNax, and Journal [29][30].
  - The Home screen shows tracked missions, notifications, reputation, and connected-ship status (community summary) [31].
  - The same patch reworked the PIT (Personal Inner Thought) inventory and the FPS loot screen. Personal inventory lives in PIT, not in the mobiGlas [30].
  - **"mobiGlas 2.0" as a name was not found in official sources.** The Building Blocks rework is the redesign.
- **Apps (as of 4.4).**
  - Home, Health, Commlink (comms/contacts), Contracts Manager, Maps, and Journal.
  - Assets (the NikNax asset manager), Reputation, and Wallet (send aUEC for a fee).
  - Landing Services, and the Vehicle Loadout Manager / Vehicle Management (edit a loadout only when the ship is stored locally) [28].
  - Fixes in 4.10.x: the Wallet Send button, contracts not showing, and stuck Landing Services requests [32].
- **Cargo-related functions.**
  - **Contracts Manager:** accept and share hauling contracts. Each contract shows goods, SCU/box count, and pickup/drop-off objectives [19].
  - **Journal:** commodity price and supply/demand notes [19].
  - **Assets (NikNax):** items across locations, the closest thing to a manifest. It had infinite-loading bugs in 4.1.1 [32]. Historically `mG.cargoManager` (3.0) consolidated hangar, ship cargo, and personal inventory [28].
  - Buying and selling commodities, ASOP ship retrieval, and freight transfer all happen on **physical kiosks**, not in the mobiGlas [11][19].
  - Roadmap, Nov 2026: guild contract boards are moving from the app to physical in-world boards [33].
- **Not found.** No dedicated in-mobiGlas cargo manifest or grid view exists in 4.x. That leaves room for our viewer's own "manifest"
  panel, styled as a mobiGlas app.

## 7. Pain points (brief)

- **Automated loading is opaque.**
  - It is paid and slow by design: the 3.24 AMA wanted it to be like refining, with a cost and the ship locked away [18].
  - It shows no visual process, only a timer on ASOP [11].
  - It fails often: "Cargo Transfer Failed" reports, Hull C stalling at 00:00 in 3.24, more Hull C autoload fixes in 4.4 and 4.5, and lost SCU at the end of a run reported in 4.10 [11][20][21][34].
- **Manual loading is tedious.**
  - It's one box at a time. Ship tractors rotate slowly, and 16-SCU boxes are hard to manoeuvre (4.3.1 review) [35].
  - The handheld beams were nerfed in 3.24 [11].
  - Elevator obstruction and missing mission cargo still turn up in 4.10 [11][21].
  - Players ask for something like a "drop cargo" option [35].
- **Takeaway for the slice.** A visible, automated, but legible process fills the gap between instant opaque loading and tedious
  manual loading. Drones moving real boxes onto real grid cells, with a manifest that updates as they go, would do that.

## Sources

1. RSI Comm-Link "The Shipyard: SCU and Cargo Capacity" (2017-10-25), wiki mirror: https://starcitizen.tools/Comm-Link:The_Shipyard_-_SCU_and_Cargo_Capacity
2. 1-SCU container: https://www.starcitizen.tools/1-SCU_container
3. Container category: https://starcitizen.tools/Container
4. Hull A (wiki, 4.10.2): https://starcitizen.tools/Hull-A
5. 2-SCU container: https://www.starcitizen.tools/2-SCU_container
6. 4-SCU container: https://starcitizen.tools/4-SCU_container
7. 8-SCU container: https://starcitizen.tools/8-SCU_container
8. 16-SCU container: https://starcitizen.tools/16-SCU_container
9. 24-SCU container: https://starcitizen.tools/24-SCU_container
10. 32-SCU container: https://starcitizen.tools/32-SCU_container
11. Alpha 3.24.0 update notes (wiki): https://starcitizen.tools/Star_Citizen_Alpha_3.24.0
12. ExpCarry, "Freight Elevator: Cargo, ATLS and Fixes" (4.10): https://expcarry.com/star-citizen-freight-elevator-cargo-atls-fixes
13. Freight elevator (wiki): https://starcitizen.tools/Freight_elevator
14. RAFT grid change, 4.1.1 PTU (Reddit mirror): https://lr.in.psf.lt/r/starcitizen/comments/1k06vbf/raft_gets_a_real_grid ; item data: https://api.star-citizen.wiki/items/placeholder-5387
15. Hangars (wiki): https://starcitizen.tools/Hangars
16. "3.24 Hangar Function FAQ (Unofficial)", Spectrum: https://robertsspaceindustries.com/spectrum/community/SC/forum/3/thread/3-24-hangar-function-faq
17. The Impound, inventory and personal hangars guide: https://theimpound.com/blogs/guides/star-citizen-inventory-and-personal-hangars-finding-storing-and-recovering-gear
18. RSI "Cargo Refactor AMA Recap": https://api.star-citizen.wiki/comm-links/18986
19. RSI "Cargo Guide" (as of 4.1, updated through 4.10): https://api.star-citizen.wiki/comm-links/20040
20. Alpha 4.4.0 PTU notes: https://robertsspaceindustries.com/spectrum/community/SC/forum/190048/thread/star-citizen-alpha-4-4-0-ptu-patch-notes ; 4.5 PTU: https://robertsspaceindustries.com/spectrum/community/SC/forum/190048/thread/star-citizen-alpha-4-5-ptu-patch-notes-4 ; wiki 4.4: https://starcitizen.tools/4.4
21. Timesaver.gg, "Hauling Guide (Alpha 4.10)": https://timesaver.gg/blog/star-citizen-hauling-guide-alpha-4-10 ; SC Quick Guides (4.5): https://scquickguides.com/guides/hauling/
22. Alpha 3.24 "Cargo Empires" Comm-Link: https://api.star-citizen.wiki/comm-links/19985
23. RSI "Q&A: Argo ATLS": https://api.star-citizen.wiki/comm-links/20159
24. MaxLift Tractor Beam (wiki, 4.10.2): https://www.starcitizen.tools/MaxLift_Tractor_Beam
25. Star Citizen Wiki API, MISC Hull A (4.10.3-LIVE): https://api.star-citizen.wiki/vehicles/misc-hull-a
26. RSI "Q&A: MISC Hull B" (2026-04-08): https://starcitizen.tools/Comm-Link:Q%26A_-_MISC_Hull_B_-_2026-04-08
27. RSI pledge page, Hull A: https://robertsspaceindustries.com/pledge/ships/hull/Hull-A
28. mobiGlas (wiki): https://starcitizen.tools/MobiGlas
29. Roadmap Roundup, 2024-02-07: https://api.star-citizen.wiki/comm-links/19777
30. Alpha 3.23.0 update notes: https://starcitizen.tools/Update:Star_Citizen_Alpha_3.23.0 ; PTU feedback thread: https://robertsspaceindustries.com/spectrum/community/SC/forum/4/thread/3-23-ptu-feedback-mobiglas-rework
31. Community discussion of the 3.23 mobiGlas: https://lemmy.dbzer0.com/post/17872726
32. 4.10.0 / 4.10.3 / 4.1.1 update notes: https://starcitizen.tools/Update:Star_Citizen_Alpha_4.10.0 , https://starcitizen.tools/Update:Star_Citizen_Alpha_4.10.3 , https://starcitizen.tools/Update:Star_Citizen_Alpha_4.1.1
33. Roadmap Roundup, 2026-11-04: https://robertsspaceindustries.com/spectrum/community/SC/forum/3/thread/roadmap-roundup-november-4-2026
34. Auto-loading failures write-up: https://pupuweb.com/why-is-cargo-auto-loading-failing-in-star-citizen ; Hull C 4.0.2 thread: https://robertsspaceindustries.com/spectrum/community/SC/forum/4/thread/ptu-4-0-2-current-hull-c-situation-need-help-testi
35. 4.3.1 hotfix review (2025-09-23): https://recapio.com/digest/is-it-hot-this-time-latest-4-3-1-hotfix-testing-star-citizen-alpha-4-3-1-live-sep-23-2025-4k-by-cpt- ; ggwtb 4.9 Hull C guide: https://ggwtb.com/blog/star-citizen-4-9-hull-c-cargo-hauling-guide-make-millions-auec-without-autoload
