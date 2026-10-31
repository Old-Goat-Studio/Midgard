# The Midgard contract

Everything the Python editor and the C++ engine must agree on lives in this folder. Both sides
read these files; neither side owns a private copy.

| File | What it defines |
|---|---|
| `manifest.schema.json` | The `manifest.json` at the root of every cooked package (JSON Schema 2020-12). |
| `migrations/NNNN_*.sql` | The project and package database. Applied in order by the editor; `PRAGMA user_version` holds the last one applied. |
| `live_link.schema.json` | Messages between the editor and a running game (length-prefixed JSON over TCP). |
| `examples/` | Valid examples, used by the tests of both halves. |

## Objects

The `object` table (migration 6) holds templates: `parts` is a JSON list of
`{"name", "components"}`, each part's components as in `components.md`. An entity with an
`object` component is an instance of one. Objects imported from a model file (`source`, under
the project's `objects/` folder, with `settings`: `scale`, `z_up`, `collider` (`box`, the
default, `convex`, `mesh` or `none`) and `lods` (`[{"source", "distance"}]`, simpler model files
with the same materials)) have
their parts written by the cook step: a part per material, its mesh as content
`obj:<id>/<n>` (`models/<id>/<n>.glb` in `game.pak`) and its textures as `obj:<id>/tex<n>`
(`models/<id>/tex<n>.ktx2`); levels of detail are `obj:<id>/<n>/lod<k>`. Changes made
in the Material editor are kept in `settings.materials` (`{"<material index>": a material
component}`) and replace the model's materials in the cooked parts. Image files that materials
use (`tex:<path>`, the path relative to the project folder) are cooked to
`images/<path>.ktx2` (`images/<path>.linear.ktx2` for `?linear`).

## Calendars

The calendar tables (migration 9) hold the project's calendar templates
(`engine/core/include/midgard/calendar.h`): `calendar` (hours per day, minutes per hour, leap
rule, start date, real minutes per game day, daylight, solar noon, twilight, and the ranges body
calendars are rolled from), with its `calendar_month`, `calendar_weekday`, `calendar_season`
(share of the year, daylight, temperature range) and `calendar_moon` rows. Names are string keys.
Every project starts with `cal_standard`, the standard calendar; exactly one calendar is standard.

The clock counts game seconds from 0 = year 1, month 1, day 1, 00:00 in every calendar; a
minute is always 60 seconds. The formulas (dates, leap years, seasons, the daylight blended
through the year, solar phase, sunrise and sunset, moon phases) are specified in
`tools/reference/calendar.py` and checked against `tests/calendar_dates.json` on both sides. The
cook refuses calendars with a month without days, no weekdays, seasons whose shares don't add up
to 1, daylight outside 1-99%, ranges the wrong way round, or a start date the calendar doesn't
have.

## Player profile

The runtime keeps a player's own settings in `profile.json`
(`engine/scene/include/midgard/player_profile.h`), in `profiles/default/` in the user's data
folder for the game (`%APPDATA%\Midgard\<game>` on Windows, `~/.local/share/Midgard/<game>`
on Linux, the app's internal storage on Android), or in the folder given with
`midgard_runtime --player-profile <dir>`. Play in the editor and tests use none unless that is
given. For now it holds the camera's last view and zoom:

```json
{"camera": {"view": "first", "third_distance": 6.0, "top_distance": 18.0}}
```

It is saved when the view changes and when the game ends. Keys the engine doesn't know are
kept. Phase 6's profile system takes the file over.

## Versioning

- `format_version` in the manifest is `major.minor`. The engine refuses a package whose **major**
  version it doesn't support, and accepts newer minor versions.
- Migrations are never edited after they are released. Changes go in a new, higher-numbered file.
- The live-link `protocol` number changes whenever a message changes incompatibly.

Contract v0 is the Phase 0 starting point described in `midgard_roadmap.md` (§2, Q1).
