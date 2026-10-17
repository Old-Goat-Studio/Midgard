-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 6: objects, things made once and placed many times.
-- An object is a template: a list of parts, each a set of components like an entity's. An
-- entity with an "object" component is an instance of one: the engine fills in the template's
-- parts as its children, so changing the template changes every instance.

CREATE TABLE object (
    id       TEXT PRIMARY KEY,                -- "obj_crate"
    name     TEXT NOT NULL,
    source   TEXT,                            -- an imported model in objects/, or NULL (made in the editor)
    settings TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(settings)),  -- import settings, collider, overrides
    parts    TEXT NOT NULL DEFAULT '[]' CHECK (json_valid(parts))      -- [{"name", "components"}]
);
