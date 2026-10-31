-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 10: environments (roadmap §6, "From clock time to light"; milestone 5.8).
--
-- An environment is how a world looks through its day: the sky's colours, the sunlight's colour
-- and strength, the ambient light and the fog, each a curve over the solar phase (0 sunrise,
-- 0.25 noon, 0.5 sunset, 0.75 midnight). Curves that aren't set come from the engine's analytic
-- sky (an Earth-like atmosphere) at the sun's height, so the empty Earth-like environment below
-- already gives a whole day. The scene's Sky entity links to one ("environment": id).

CREATE TABLE environment (
    id       TEXT PRIMARY KEY,
    name     TEXT NOT NULL,   -- for the editor (not shown to players)
    curves   TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(curves)),
                              -- {"<curve>": [[phase, value], ...], ...}: values are numbers or
                              -- [r, g, b] (sRGB); see midgard/environment.h for the curves
    settings TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(settings))
                              -- what isn't over the day: sun_size, glow_tightness,
                              -- fog_height_falloff, fog_sun_scatter
);

INSERT INTO environment (id, name) VALUES ('env_earth', 'Earth-like');
