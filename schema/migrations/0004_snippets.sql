-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 4: the project's snippet library for the Script editor.
-- Editor-only: the cook step drops this table from the game's copy of the database.

CREATE TABLE snippet (
    id   TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    tags TEXT NOT NULL DEFAULT '',   -- space-separated, e.g. "trigger quest"
    body TEXT NOT NULL               -- Python source, inserted at the cursor
);
