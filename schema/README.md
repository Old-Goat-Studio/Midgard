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
the project's `objects/` folder, with `settings` such as `scale`, `z_up` and `collider`) have
their parts written by the cook step: a part per material, its mesh as content
`obj:<id>/<n>` (`models/<id>/<n>.glb` in `game.pak`) and its textures as `obj:<id>/tex<n>`
(`models/<id>/tex<n>.ktx2`).

## Versioning

- `format_version` in the manifest is `major.minor`. The engine refuses a package whose **major**
  version it doesn't support, and accepts newer minor versions.
- Migrations are never edited after they are released. Changes go in a new, higher-numbered file.
- The live-link `protocol` number changes whenever a message changes incompatibly.

Contract v0 is the Phase 0 starting point described in `midgard_roadmap.md` (§2, Q1).
