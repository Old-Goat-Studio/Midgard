-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 5: the project's physics collision layers.
-- A rigidbody component names its layer by number (0 to 15); everything collides with
-- everything unless a pair of layers is listed in physics_ignore.

CREATE TABLE physics_layer (
    id   INTEGER PRIMARY KEY CHECK (id BETWEEN 0 AND 15),
    name TEXT NOT NULL UNIQUE
);

-- Pairs of layers that pass through each other (a <= b; each pair once).
CREATE TABLE physics_ignore (
    a INTEGER NOT NULL REFERENCES physics_layer(id) ON DELETE CASCADE,
    b INTEGER NOT NULL REFERENCES physics_layer(id) ON DELETE CASCADE,
    PRIMARY KEY (a, b),
    CHECK (a <= b)
);

INSERT INTO physics_layer (id, name) VALUES (0, 'default'), (1, 'player'), (2, 'debris');
-- Debris (small things that tumble about) never gets in the player's way.
INSERT INTO physics_ignore (a, b) VALUES (1, 2);
