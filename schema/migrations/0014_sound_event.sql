-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 14: sound events (milestone 5.14).
--
-- A sound event is a named sound a game plays: "Door creak", "Footstep on grass", "Theme". It
-- holds one or more sounds (variations, picked by weight each time it plays), with a volume and
-- a pitch picked within their ranges, so a sound heard again and again never repeats exactly.
-- Entities play them with a sound component ({"event": "<id>"}), and scripts by name or id
-- (midgard.audio.play). Their sounds are files in the project ("snd:<path>"), which the cook
-- makes into Ogg Vorbis or Opus (midgard/audio_encode.h).

CREATE TABLE sound_event (
    id    TEXT PRIMARY KEY,
    name  TEXT NOT NULL,   -- what the editor shows, and scripts may call it by
    event TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(event))
                           -- {"sounds": [{"sound": "snd:<path>", "weight": 1}],
                           --  "bus": "effects", "volume": [min, max], "pitch": [min, max],
                           --  "loop": false, "cooldown": 0, "max_voices": 0,
                           --  "spatial": true, "min_distance": 1, "max_distance": 40}
                           -- (midgard/sound_events.h)
);
