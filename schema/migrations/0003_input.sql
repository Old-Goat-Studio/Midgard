-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 3: input actions and their default bindings.
-- Scripts and systems read actions ("move_y"), never raw keys. Control names:
--   keyboard: SDL scancode names ("W", "Space", "Escape")
--   mouse:    "delta_x", "delta_y", "wheel"
--   gamepad:  SDL gamepad names ("leftx", "lefty", "rightx", "righty", "a", "start")
-- Players can rebind later (Phase 6); their bindings live in their profile, not here.

CREATE TABLE input_action (
    id      TEXT PRIMARY KEY,
    context TEXT NOT NULL DEFAULT 'on_foot',   -- on_foot, ship, menu, dialogue
    kind    TEXT NOT NULL CHECK (kind IN ('button', 'axis'))
);

CREATE TABLE input_binding (
    action  TEXT NOT NULL REFERENCES input_action(id) ON DELETE CASCADE,
    device  TEXT NOT NULL CHECK (device IN ('keyboard', 'mouse', 'gamepad', 'touch')),
    control TEXT NOT NULL,
    scale   REAL NOT NULL DEFAULT 1.0,          -- e.g. -1 for "S" on move_y
    PRIMARY KEY (action, device, control)
);

INSERT INTO input_action (id, kind) VALUES
    ('move_x', 'axis'), ('move_y', 'axis'), ('look_x', 'axis'), ('look_y', 'axis'),
    ('zoom', 'axis'), ('jump', 'button'), ('menu', 'button');

INSERT INTO input_binding (action, device, control, scale) VALUES
    ('move_x', 'keyboard', 'D', 1.0),        ('move_x', 'keyboard', 'A', -1.0),
    ('move_x', 'gamepad', 'leftx', 1.0),
    ('move_y', 'keyboard', 'W', 1.0),        ('move_y', 'keyboard', 'S', -1.0),
    ('move_y', 'gamepad', 'lefty', -1.0),    -- stick up is negative
    ('look_x', 'mouse', 'delta_x', 0.15),    ('look_x', 'gamepad', 'rightx', 120.0),
    ('look_y', 'mouse', 'delta_y', -0.15),   ('look_y', 'gamepad', 'righty', -90.0),
    ('zoom', 'mouse', 'wheel', -1.0),
    ('jump', 'keyboard', 'Space', 1.0),      ('jump', 'gamepad', 'a', 1.0),
    ('menu', 'keyboard', 'Escape', 1.0),     ('menu', 'gamepad', 'start', 1.0);
