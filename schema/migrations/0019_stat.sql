-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 19: the player's stats, scores and counters (milestone 6.0).
--
-- A stat is a number the designer names, of one of three kinds:
--   stat     between a least and a most (health, stamina, mana); it may refill (or drain) by
--            itself, "regen" a second, once it has gone "delay" seconds without dropping
--   score    a whole number that never goes below its least (points)
--   counter  a whole number from its least up to a target ("max"; 0: none) (keys, coins)
-- Scripts read and change them (midgard.stats), the HUD shows them, and the world's save keeps
-- them. midgard/stats.h reads them, in "position" order (the Stats panel's).
--
-- A project starts with Health, Stamina and Score, which the designer can change, delete or
-- add to.

CREATE TABLE stat (
    id       TEXT PRIMARY KEY,
    name     TEXT NOT NULL,
    position INTEGER NOT NULL DEFAULT 0,
    stat     TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(stat))
                          -- {"kind": "stat", "min": 0, "max": 100, "start": 100,
                          --  "regen": 0, "delay": 0}
                          -- (a score's and a counter's start at their least by default)
);

INSERT INTO stat (id, name, position, stat) VALUES
    ('health', 'Health', 0,
     '{"kind": "stat", "min": 0, "max": 100, "start": 100, "regen": 1, "delay": 5}'),
    ('stamina', 'Stamina', 1,
     '{"kind": "stat", "min": 0, "max": 100, "start": 100, "regen": 20, "delay": 1}'),
    ('score', 'Score', 2, '{"kind": "score", "min": 0, "start": 0}');
