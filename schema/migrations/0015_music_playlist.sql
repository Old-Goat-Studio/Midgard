-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 15: music playlists (milestone 5.14).
--
-- A playlist is music for somewhere and some time: its tracks (sound events on the music bus),
-- and when it plays: in which scenes, world types and biomes, and at which times of day (dawn,
-- day, dusk, night). The game plays the most specific playlist that fits where the player is
-- (then the highest priority), crossfading when that changes (midgard/music.h); scripts can
-- choose one themselves (midgard.music), and play stingers over it.

CREATE TABLE music_playlist (
    id       TEXT PRIMARY KEY,
    name     TEXT NOT NULL,
    playlist TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(playlist))
                           -- {"tracks": ["<sound event id>", ...], "order": "in_order",
                           --  "scenes": [], "world_types": [], "biomes": [], "times": [],
                           --  "priority": 0, "crossfade": 3, "gap": [0, 0]}
                           -- (an empty list: anywhere, any time)
);
