-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 24: the "map" action (the full map: M, a gamepad's Back).
-- Pressed, the game shows the whole world's map over 3/4 of the window, with the player and
-- the scene's exits on it; pressed again (or Esc), it goes. Players can rebind it.
-- The engine gives older packages the same default bindings.

INSERT OR IGNORE INTO input_action (id, kind, name, rebindable, context)
    VALUES ('map', 'button', 'Map', 1, 'always');

INSERT OR IGNORE INTO input_binding (action, device, control, scale) VALUES
    ('map', 'keyboard', 'M', 1.0), ('map', 'gamepad', 'back', 1.0);
