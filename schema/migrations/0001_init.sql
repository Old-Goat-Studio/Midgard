-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 1: the tables every Midgard project and package starts with.
-- The editor applies migrations in order and sets PRAGMA user_version to the last one applied.

-- Key/value facts about the project (name, created, game version, world mode).
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- Localisation: every player-facing text field stores a key into this table (English is the
-- original language). Translation packs supply the same keys in other languages.
CREATE TABLE string (
    key     TEXT PRIMARY KEY,
    english TEXT NOT NULL,
    context TEXT NOT NULL DEFAULT ''  -- note for translators
);

-- Scenes and the entities placed in them.
CREATE TABLE scene (
    id       TEXT PRIMARY KEY,
    name_key TEXT NOT NULL REFERENCES string(key)
);

CREATE TABLE entity (
    id         TEXT PRIMARY KEY,
    scene      TEXT NOT NULL REFERENCES scene(id) ON DELETE CASCADE,
    name       TEXT NOT NULL,
    parent     TEXT REFERENCES entity(id) ON DELETE CASCADE,
    components TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(components))  -- {"transform": {...}, ...}
);

CREATE INDEX entity_scene ON entity(scene);
