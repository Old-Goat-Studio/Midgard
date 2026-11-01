-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 11: terrain stamps (roadmap §7; milestone 5.9).
--
-- Sculpting and painting a scene's terrain: each row is one stroke of a brush, applied in order
-- (by id) on top of the generated ground. See midgard/terrain.h's TerrainStamp:
--   {"tool": "raise" | "lower" | "smooth" | "flatten" | "paint",
--    "paint": "lush" | "dry" | "sand" | "rock" | "clear",   (paint only)
--    "radius": metres, "strength": metres (raise, lower) or 0 to 1 (the others),
--    "height": metres (flatten), "face": 0, "points": [[u, v], ...]}
-- World types will place stamps too, and players' changes to the ground (saved per body) use
-- the same form.

CREATE TABLE terrain_stamp (
    id    INTEGER PRIMARY KEY,   -- the order they apply in
    scene TEXT NOT NULL REFERENCES scene (id) ON DELETE CASCADE,
    stamp TEXT NOT NULL CHECK (json_valid(stamp))
);

CREATE INDEX terrain_stamp_scene ON terrain_stamp (scene, id);
