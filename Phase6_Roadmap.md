# Phase 6 Roadmap: Gameplay data systems

**Midgard** · Old Goat Studio · 4 November 2026 · status: **approved**

This document breaks Phase 6 of `midgard_roadmap.md` (§10) into milestones that can be built,
tested and reviewed one at a time, as `Phase5_Roadmap.md` did for Phase 5. The same two rules
hold:

1. **Foundation before features.** What every later system needs comes first: the screens and
   HUD, input, saving, worlds and characters. Combat, quests and ships come after, built on them.
2. **Testing all the way.** Every milestone adds automated tests at each level it touches, runs
   in CI on every platform, and ends with an exit you can try, with a phone check where marked 📱.

Phase 6 is much bigger than Phase 5: it turns Midgard from a world builder into a game maker. It
is built in **stages** (§4). Each stage ends with something playable, so priorities can change
between stages without leaving anything half-made. Each milestone becomes a step-by-step plan
(like 5.17's and 5.18's) when work on it starts.

---

## 1. Decisions for this phase (4 November 2026)

| Question | Decision |
|---|---|
| The order of Phase 6 | **In stages** (§4): the UI and the player's basics first, then the role-playing core, then the living world, then building, then travel; the phase's exit comes last. |
| How much to plan now | **This document now** (stages, milestones, exits); each milestone's steps when it starts. |
| Midgard's default UI font | **An OFL font from your Master library**, bundled with its licence file: the one exception to "bundled assets are CC0 only". Games can use their own fonts. |
| Playing on phones | **On-screen touch controls arrive in 6.0** (until then a phone needs a game controller). |
| Data | Every new system keeps its data in the project's database (tables with migrations and upgrade tests), is checked when the game is cooked, and has a `midgard` script API with `.pyi` stubs. |

### New libraries (all permissive, all build for Windows, Linux and Android)

| Library | Licence | Used for |
|---|---|---|
| RmlUi | MIT | Menus, screens and the HUD (HTML/CSS-like documents) |
| FreeType (via RmlUi) | FreeType License (BSD-like) | Font rendering |
| ICU (a small part) or a compact plural-rules table | Unicode License | Plurals and number formats in translations (6.4) |
| Rhubarb Lip Sync (cook-time tool, editor only) | MIT | Mouth shapes from recorded voice lines (6.10) |

Text-to-speech for generated words in voiced dialogue (6.10) is decided when 6.10 starts (§7).

---

## 2. How Phase 6 is tested

Phase 5's six kinds of test continue (unit, reference, determinism, image, integration and live
link, performance budgets and device checks). Phase 6 adds four:

### 2.1 Screen tests
The runtime draws RmlUi screens offscreen like any image test (`--screenshot`), at desktop and
phone sizes, and compares them with references. Focus navigation (every control reachable with a
gamepad), touch targets (big enough, inside the safe area) and text that doesn't fit are checked
automatically for every bundled screen template.

### 2.2 Play-through tests
Scripted runs of the game without a window (`--simulate`, `--hold`, scripts that act as the
player, `--dump-scene`), extended to read stats, inventories, wallets, quests and screens. Each
milestone's exit is written as one of these where it can be: "take the quest, kill the wolves,
get paid" runs in CI, not just by hand.

### 2.3 Save tests
Every system that saves state round-trips it (save, load, compare) and survives a save made by
the previous version (migration tests with saved fixtures). A save made on Windows loads on Linux
and the phone.

### 2.4 Balance and data checks
Cook-time checks for each system's data (a quest that can't be finished, a recipe no station can
make, a skill no one can reach, a shop that never opens, a dialogue line with no way out), plus
balance reports (the economy simulator, crafting chains, combat damage) that warn but don't fail.

### Definition of done (every milestone)
As in Phase 5: CI green on every platform; migrations with upgrade tests; cook checks; schema
docs; script API with stubs; the Help guide updated in plain language; the progress list updated;
a phone check where marked 📱.

---

## 3. Carried over from Phase 5

Moved here in Phase 5 because they need this phase's systems (each is placed in a milestone in
§4): cold, heat and soaked **status effects** and the rested bonus; **clothing** survival stats;
craftable **campfires and heaters**; the **HUD temperature gauge** and screen-edge frost and
sweat; `survival_scale` from the difficulty; weather reducing **AI perception**; **space
weather**; **forecast devices**; camera **lock-on**; the in-game **map screen**. Also from Phase
5's reviews: shadows that follow a surface shader's cutouts (5.17), and on-screen touch controls
(5.18).

---

## 4. Milestones, in stages

Sizes: M (about a week of work), L (two), XL (three or more), as in Phase 5.

### Stage A: the player's basics

#### 6.0 UI foundation · XL
RmlUi on desktop and Android (mouse, keyboard, gamepad focus, touch, UI scale, safe areas);
designer-defined **player stats** (the Stats editor, regeneration, `midgard.stats`) and **scores
and counters**; the **HUD** with data binding (stat bars, numbers, counters and timers, prompts,
the notification feed, the crosshair, the compass and the minimap, both shape-aware);
`midgard.ui`; **on-screen touch controls**; the **Menu editor** (a canvas, anchors, styles,
desktop and phone previews, live changes in the running game); screen templates for the title and
main menu, pause, settings, and game over and respawn. Steps:
1. the UI engine; 2. stats, scores and counters; 3. the HUD; 4. compass and minimap; 5. touch
controls; 6. the Menu editor; 7. the screen templates; 8. exit.

**Exit:** a game opens at its title screen and starts from *New*. The player sees health and
stamina bars, a score a script raises, a pick-up notification and a compass that stays true
walking round a planet. They pause, change the volume and a key binding, die and respawn. 📱 All
of this works on the phone, by touch.

#### 6.0b Terrain textures · M
Added at the user's request (Nov 2026), before 6.1: picture textures on the terrain, which 5.9's
procedural ground (colours by slope, height, biome and season) never had. Each ground layer
(grass, dry grass, sand, rock, snow, and each biome's ground) can take a **texture set** from the
Asset Library's Textures section (colour, normal and roughness maps) and a tile size in metres;
a layer without one keeps the procedural colour. The textures blend exactly where the colours
blend now (slope, height above the sea, biome, the paint brushes), with snow cover, autumn and
dry-grass tints laid over them. On every world shape, by **triplanar** sampling (no single "up"
to lay them from on a planet or inside a cylinder; no stretching on slopes, no seams at the
cube-sphere's faces); sampled at two scales so they're detailed up close without visible repeats
far off; a cheaper path on phones. Steps:
1. ground layers take a texture set (the World Type editor's Climate and Biomes; cooked as
   textures are); 2. the renderer samples them (texture arrays, triplanar, blended as the
   colours are); 3. detail and distance (two scales, fading the repeat; the phone tier);
4. painting a texture (a brush that paints a chosen layer, stored as terrain stamps);
5. exit.
**Exit:** a world type's grass, rock and snow given texture sets shows them on a bounded area, a
planet, a cylinder and a hollow world, blended by slope and height with no seams or stretching,
snow lying over them in winter; a path painted across the grass shows its texture; picture tests
on each shape; it runs smoothly on the phone.

#### 6.1 Input mapping · M
The Input editor (actions and their keys, buttons and touch controls), contexts (walking, flying,
menus, vehicles), the touch layout editor, the player's rebinding screen, and prompts that show
the right key or button for the device in use.
**Exit:** you add an action in the Input editor, a script uses it, the player rebinds it in the
game, and its prompt shows a key, a gamepad button or a touch control as the device changes. 📱

#### 6.2 Worlds and characters · L
World folders and `world.json`; the **New World** screen (shape and size, seed, name and
description, difficulty); several worlds per player; character profiles that travel between
worlds; the **Difficulty editor** and modifiers (including `survival_scale`); menu templates for
the world list, New World and character select.
**Exit:** a player makes two worlds of different shapes and difficulties, plays one, switches to
the other with the same character, and comes back to find the first as they left it.

#### 6.3 Save and load · M
The Save editor (autosave triggers, slots, quicksave), manual save slots with thumbnails in
single-player, the save/load screen, and save migrations carried in the package.
**Exit:** autosave on a room change, a manual save with its thumbnail, load it; then a game
update with a changed save format loads the old save (a copy kept). 📱

#### 6.4 Localisation · L
The English string table behind every piece of text; the Localisation editor; pseudo-localisation
(long and accented text, to find what doesn't fit); the **translation kit** in every export;
language packs the player installs, with English as the fallback; plurals and placeholders; font
fallback; the in-game *Export strings* button.
**Exit:** pseudo-localised, every bundled screen still fits; a language pack installed by a player
changes the game's text, and anything missing falls back to English.

**Stage A's exit:** a small game with its own title screen, HUD, saves, worlds and a second
language, played with keyboard, gamepad and touch.

### Stage B: the role-playing core

#### 6.5 Items and inventory · L
Item definitions (the Items editor, built on a new **property-grid widget**); stacking, weight and
slots; equipment; pick-ups in the world; the **inventory** and **character sheet** screens.
**Exit:** pick up items, equip a sword and a coat, see the character sheet change; drop and pick
them up again; all saved.

#### 6.6 Calendar events · M
The recurrence editor (daily, weekly, by season, by the calendar's own months), `calendar_event`
prerequisites, shop hours, NPC schedules, monthly events, and time skips (sleeping, waiting).
**Exit:** a market that opens only on market days by daylight, and an NPC who goes home at dusk.

#### 6.7 Wallet, shops and banks · L
The wallet (and gold on the HUD); shops (stock, prices, buying and selling, opening hours); banks
(the Banking editor, several banks per universe with interest and risk profiles, monthly risk
events, ratings and history, joint accounts, recovery quests after losses); the economy
simulator report.
**Exit:** sell loot, buy from a shop that's only open by day, deposit gold and see interest at
month end.

#### 6.8 Quests · L
The Quest editor (steps, objectives, conditions, rewards), the quest log screen and HUD markers,
quest state in saves, and the cook checks (no quest that can't be finished).
**Exit:** take a quest, follow its marker on the compass, kill the wolves (a placeholder fight
until 6.12), return and be paid.

#### 6.9 Skills and progression · M
XP and level as stats; skill trees (the Skill editor); skill points; the character sheet's skills.
**Exit:** the quest's XP raises the level; a skill point unlocks a skill that changes a stat.

#### 6.10 Dialogue · XL
The Dialogue editor; response trees with conditions and actions (start a quest, pay, change a
stat); role dialogue with generator slots (names from the world); recorded voice with optional
subtitles; voice profiles; text-to-speech for slot words; recording scripts; lip-sync tracks; the
conversation window and its camera.
**Exit:** the quest is given by a voiced conversation whose answers change the reward.

**Stage B's exit:** the roadmap's first loop: talk, take a quest, kill the wolves, earn gold,
bank it, buy from a shop by day, spend a skill point. Saved and loaded.

### Stage C: the living world

#### 6.11 AI behaviour · XL
The AI Behaviour editor (behaviour trees), perception (sight, hearing, and the weather reducing
it), navigation links (jumps, doors, ladders), groups and factions, Calendar schedules, spawn
tables, and live behaviour-tree debugging over the live link.
**Exit:** a wolf pack that patrols at night, spots the player, flanks and calls for help.

#### 6.12 Combat · XL
The Combat editor; weapons and abilities; hit boxes on the skeleton's bones; damage types and
status effects; dodge, block and parry; camera **lock-on**; the combat sandbox (try a weapon
against a dummy in the editor).
**Exit:** fight the wolf pack in each camera view, locked on, with dodges and blocks.

#### 6.13 Companions and approval · L
Recruiting (by dialogue, quest or hire), commands, companion skill trees, downed and revive,
following through doors, teleports and onto ships; traits and approval (tags in the Dialogue and
Quest editors, refusals, leaving and winning back, personal quests, the approval simulator).
**Exit:** hire a companion who fights beside you, disapproves of a choice, and leaves if pushed.

#### 6.14 Survival · L
Status effects for cold, heat, being soaked and being rested; clothing's warmth and protection;
craftable campfires and heaters (with 6.16's crafting); the HUD temperature gauge and screen-edge
frost and sweat; hunger and thirst (meters, food and drink, cooking, sickness, companions eating);
difficulty scales.
**Exit:** caught in a blizzard without a coat, the player gets cold, warms up at a campfire, and
cooks a meal.

#### 6.15 Hostile atmospheres · M
Atmosphere kinds per world type; suits and breathing gear; air supply and refills; life support
and airlocks; depressurisation; spacewalks with magnetic boots and thruster packs; warnings on
arrival. Space weather (radiation storms) here too.
**Exit:** step out of an airlock onto an airless moon in a suit, watch the air run down, and get
back in time.

**Stage C's exit:** the wolf fight with a companion, in weather that matters, saved mid-fight and
loaded.

### Stage D: building

#### 6.16 Gathering and crafting · L
The Resources editor; resource nodes placed from the seed; tools and mining lasers; depletion and
regrowth; deposit scanning; the Crafting editor (recipes, stations, production chains, quality
tiers, station queues); the balance checker.
**Exit:** mine ore, refine it and craft a turret at a workbench.

#### 6.17 Bases and raids · XL
The Base editor; snap placement on every world shape; functional pieces (storage, stations,
power, beds, teleport and landing pads, defences); permissions, budgets and no-build zones; raids
(attention and schedules, warnings, waves, damage and repair, theft and recovery, the
difficulty's `raid_scale`). Garages and boatyards come with their vehicles (6.21).
**Exit:** build a small base with a teleport pad and a turret, and defend it against a raid with a
companion.

### Stage E: travel

#### 6.18 Space and teleportation · XL
The Star Field editor and the 9 generated systems (stars, bodies, names); skies from the star
field on every body and in space; discovery and the **star map**; the in-game **map screen**;
teleporters (machines, ship modules, artifacts, player-placed pads), the Teleport editor,
interference zones and safe arrival; world mode and per-body days; forecast devices.
**Exit:** teleport with an artifact to a hollow world in another system, seen first on the star
map.

#### 6.19 Ships · XL (in three parts)
1. **Flying:** the Ship editor, the flight model with flight assist, atmosphere drag and lift,
   compressed system space with cruise, flying between space and planet skies, docks into
   habitats and hollow worlds, landing and parking, crewed interiors.
2. **Getting ships:** hull frames, modules as items, shipyards (slot building and refitting with
   live checks, blueprints), ships as rewards.
3. **Wrecks and ship combat:** module damage, wreck beacons, salvage, weapon and shield modules,
   enemy ships, blueprint sharing.
**Exit:** fly a ship from the planet into space and dock inside a habitat.

#### 6.20 Bounded surface maps · M
The ocean boundary and storm wall, 1 to 3 continents, archipelagos and islands from the New World
screen, ships in both world modes.

#### 6.21 Other craft, vehicles and mounts · XL
Aircraft, airships, sailing ships and small craft; land vehicles with garages; mounts (stables,
taming, riding, flying mounts, breeding, the roster). Built in the order you choose when we get
there; any can wait for a later phase.

#### 6.22 Quests in generated worlds · L
Quest anchors with placement rules, quest templates with slots, the quest generator, *Preview in
seed…*, and the cook check that every anchor is placed.

### Stage F: finishing

#### 6.23 Assistant v2 · M
`propose_changes`: the Assistant drafts items, quests and skill trees as a diff to Apply or Reject,
one Ctrl+Z to undo.

#### 6.24 Phase 6 sample and exit · L
A sample game that plays the roadmap's whole loop, checked automatically where it can be, with a
checklist for you and a phone check.

**Exit (the roadmap's):** a playable loop: take a quest, kill the wolves, earn gold, bank it (and
earn interest at month end), buy an item from a shop open only by day, spend a skill point. Fly a
ship from the planet into space and dock inside a habitat; teleport with an artifact to a hollow
world in another system; take that character into a second universe on a harder difficulty.
Along the way: a voiced dialogue whose answers change the outcome, a wolf pack that flanks and
calls for help alongside a hired companion, all three camera views mid-fight, the attack key
rebound, a community language pack, a manual save loaded again. Finally: mine ore, craft a
turret, build a base with a teleport pad, and defend it against a raid with a companion. 📱

---

## 5. Order and what can wait

| Milestone | Needs | Can overlap with |
|---|---|---|
| 6.0 UI foundation | Phase 5 | — |
| 6.1 Input mapping | 6.0 | 6.2 |
| 6.2 Worlds and characters | 6.0 | 6.1, 6.3 |
| 6.3 Save and load | 6.0, 6.2 | 6.4 |
| 6.4 Localisation | 6.0 | 6.3, Stage B |
| 6.5 Items and inventory | 6.0, 6.3 | 6.6 |
| 6.6 Calendar events | Phase 5's calendar | 6.5 |
| 6.7 Wallet, shops and banks | 6.5, 6.6 | 6.8 |
| 6.8 Quests | 6.5 | 6.7, 6.9 |
| 6.9 Skills and progression | 6.0 stats | 6.8 |
| 6.10 Dialogue | 6.8 | 6.11 |
| 6.11 AI behaviour | Phase 5 navigation | 6.10 |
| 6.12 Combat | 6.0 stats, 6.11 | 6.13 |
| 6.13 Companions and approval | 6.10, 6.11, 6.12 | 6.14 |
| 6.14 Survival | 6.5, 6.0 stats | 6.13 |
| 6.15 Hostile atmospheres | 6.14 | 6.16 |
| 6.16 Gathering and crafting | 6.5 | 6.15 |
| 6.17 Bases and raids | 6.16, 6.11 | 6.18 |
| 6.18 Space and teleportation | 6.2 | 6.17 |
| 6.19 Ships | 6.18, 6.5 | 6.20 |
| 6.20 Bounded surface maps | 6.2 | 6.19 |
| 6.21 Other craft, vehicles, mounts | 6.19 | — |
| 6.22 Quests in generated worlds | 6.8 | Stage E |
| 6.23 Assistant v2 | 6.5, 6.8, 6.9 | anything after Stage B |
| 6.24 Sample and exit | everything | — |

**What could move to a later phase** if you'd rather reach multiplayer (Phase 7) sooner: 6.21
(other craft, vehicles and mounts), parts of 6.19 (ship combat), 6.20 and 6.22. None of the
others depend on them.

---

## 6. Risks and how the plan handles them

| Risk | Handling |
|---|---|
| The phase is very large | Stages, each ending playable; 6.21 and parts of 6.19 can move to a later phase (§5) |
| RmlUi on Android (touch, fonts, Vulkan) | Built and checked on the phone in 6.0's first step, before anything depends on it |
| Systems inventing their own numbers | One Stats table from 6.0 that combat, survival, hunger, progression and scripts all share |
| Saves breaking as systems are added | Save tests from 6.3 on (§2.3): every system round-trips, and old saves load |
| Text that doesn't fit in other languages | Pseudo-localisation checks of every bundled screen from 6.4 on |
| Balance (economy, combat, crafting) | Balance reports in the editor and in CI (§2.4), warnings rather than failures |
| Phone performance with HUD, AI and combat | Budgets per milestone (as in Phase 5), the phone's quality tier, and the self-test gaining checks as systems arrive |

---

## 7. Questions to settle when we reach them

- **6.0:** which of your Master's OFL fonts becomes the default (I'll suggest a clear one).
- **6.10:** text-to-speech for generated words (on the device, offline): which engine and voices
  (licences vary), or recorded words only.
- **6.12:** how combat feels by default (action like a brawler, or slower and deliberate), as a
  starting point the Combat editor then changes.
- **6.21:** which craft, vehicles and mounts are wanted in this phase, and in what order.
- **Art for the samples:** the sample games need more CC0 models (wolves, a shop, a ship); I'll
  list candidates when each is needed, as we did for 5.13 and 5.18.
