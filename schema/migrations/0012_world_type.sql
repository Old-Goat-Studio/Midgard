-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 12: world types (roadmap §7, "World types"; milestone 5.11).
--
-- A world type is a recipe for worlds: the shapes and sizes it suits, its gravity, atmosphere,
-- calendar and wall style, and how its ground is made (midgard/world_type.h). A scene's terrain
-- is a world of one: its settings name a world type, the version of it, and a seed,
--   "terrain": {"world_type": "wt_...", "version": 2, "seed": 7}    (no seed: the body's)
--
-- Every version a world could have been made with is kept, so the world still generates the
-- same after its recipe changes: editing a version that a shipped game used (an export marks
-- them shipped) saves a new version instead, and a shipped version can't be changed or deleted.

CREATE TABLE world_type (
    id      TEXT PRIMARY KEY,
    name    TEXT NOT NULL,                -- for the editor (not shown to players)
    version INTEGER NOT NULL DEFAULT 1    -- the latest: what new worlds are made with
);

CREATE TABLE world_type_version (
    world_type TEXT NOT NULL REFERENCES world_type (id) ON DELETE CASCADE,
    version    INTEGER NOT NULL CHECK (version >= 1),
    recipe     TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(recipe)),
    shipped    INTEGER NOT NULL DEFAULT 0 CHECK (shipped IN (0, 1)),
    PRIMARY KEY (world_type, version)
);

CREATE TRIGGER world_type_version_kept BEFORE DELETE ON world_type_version WHEN OLD.shipped
BEGIN
    SELECT RAISE(ABORT, 'a shipped world type version is kept: worlds made with it must still generate the same');
END;

CREATE TRIGGER world_type_version_fixed BEFORE UPDATE OF recipe ON world_type_version
WHEN OLD.shipped AND NEW.recipe <> OLD.recipe
BEGIN
    SELECT RAISE(ABORT, 'a shipped world type version can''t change: save a new version');
END;

CREATE TRIGGER world_type_kept BEFORE DELETE ON world_type
WHEN EXISTS (SELECT 1 FROM world_type_version WHERE world_type = OLD.id AND shipped)
BEGIN
    SELECT RAISE(ABORT, 'a world type with a shipped version is kept: worlds made with it must still generate the same');
END;

-- Scenes with terrain settings become worlds of their own Custom world type (version 1, the
-- same settings), so they look exactly as before.
INSERT INTO world_type (id, name, version)
    SELECT 'wt_' || id,
           CASE WHEN id = 'scn_main' THEN 'Custom' ELSE 'Custom (' || id || ')' END, 1
    FROM scene
    WHERE json_type(settings, '$.terrain') = 'object'
      AND json_extract(settings, '$.terrain.world_type') IS NULL;

INSERT INTO world_type_version (world_type, version, recipe)
    SELECT 'wt_' || id, 1,
           json_object('terrain', json(json_remove(json_extract(settings, '$.terrain'), '$.seed')))
    FROM scene
    WHERE json_type(settings, '$.terrain') = 'object'
      AND json_extract(settings, '$.terrain.world_type') IS NULL;

UPDATE scene
SET settings = json_set(settings, '$.terrain', json(
        CASE WHEN json_extract(settings, '$.terrain.seed') IS NULL
             THEN json_object('world_type', 'wt_' || id, 'version', 1)
             ELSE json_object('world_type', 'wt_' || id, 'version', 1,
                              'seed', json_extract(settings, '$.terrain.seed'))
        END))
WHERE json_type(settings, '$.terrain') = 'object'
  AND json_extract(settings, '$.terrain.world_type') IS NULL;
