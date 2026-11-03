-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 17: the game's bus volumes (milestone 5.14).
--
-- How loud each bus is when a game starts (the Audio panel's Mixer): "master" (the whole game),
-- "music", "effects", "voice", "ambience" and "interface". A bus not here is at full volume (1).
-- Scripts change them while the game plays (midgard.audio.set_volume).

CREATE TABLE audio_bus (
    bus    TEXT PRIMARY KEY CHECK (bus IN ('master', 'music', 'effects', 'voice', 'ambience',
                                           'interface')),
    volume REAL NOT NULL DEFAULT 1 CHECK (volume >= 0 AND volume <= 4)
);
