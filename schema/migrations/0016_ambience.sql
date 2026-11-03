-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 16: ambience sets (milestone 5.14).
--
-- An ambience set is the background sound of somewhere (scenes, world types, biomes; empty:
-- anywhere), chosen as music is: the one naming the most that hold, then the highest priority.
-- Its layers are sound events that loop under everything, each at its own times of day (dawn,
-- day, dusk, night; none: always), fading in and out over its crossfade as the day goes by; its
-- sprinkles are one-shot events now and then, at a random spot around the listener
-- (midgard/ambience.h).

CREATE TABLE ambience (
    id       TEXT PRIMARY KEY,
    name     TEXT NOT NULL,
    ambience TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(ambience))
                           -- {"layers": [{"event": "<id>", "times": [], "volume": 1}],
                           --  "sprinkles": [{"event": "<id>", "times": [], "every": [10, 30],
                           --                 "distance": [5, 30]}],
                           --  "scenes": [], "world_types": [], "biomes": [],
                           --  "priority": 0, "crossfade": 4}
);
