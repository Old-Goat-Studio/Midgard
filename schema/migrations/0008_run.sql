-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 8: the "run" action.
-- Held, the player moves at its speed times the player component's run_multiplier.
-- The engine gives older packages the same default bindings.

INSERT OR IGNORE INTO input_action (id, kind) VALUES ('run', 'button');

INSERT OR IGNORE INTO input_binding (action, device, control, scale) VALUES
    ('run', 'keyboard', 'Left Shift', 1.0),  ('run', 'gamepad', 'leftstick', 1.0);
