-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 20: the on-screen touch controls (milestone 6.0).
-- A phone plays by touch: a stick on the left, dragging on the right to look, and buttons
-- (midgard/ui/touch.h). They are a "touch" device, bound to the actions like the keyboard and
-- gamepad: stick_x and stick_y (-1..1), drag_x and drag_y (dp dragged this frame; degrees per
-- dp, as the mouse's are degrees per pixel), and the buttons jump, use, run, view and descend.
-- The engine gives older packages the same default bindings.
--
-- Also "descend" (going down while flying, milestone 5.16), which until now only had the
-- engine's default controls: as an action with all its bindings, so its keys stay bound.

INSERT OR IGNORE INTO input_action (id, kind) VALUES ('descend', 'button');

INSERT OR IGNORE INTO input_binding (action, device, control, scale) VALUES
    ('descend', 'keyboard', 'Left Ctrl', 1.0),  ('descend', 'gamepad', 'b', 1.0);

INSERT OR IGNORE INTO input_binding (action, device, control, scale) VALUES
    ('move_x', 'touch', 'stick_x', 1.0),  ('move_y', 'touch', 'stick_y', 1.0),
    ('look_x', 'touch', 'drag_x', 0.3),   ('look_y', 'touch', 'drag_y', -0.3),
    ('jump', 'touch', 'jump', 1.0),       ('use', 'touch', 'use', 1.0),
    ('run', 'touch', 'run', 1.0),         ('view', 'touch', 'view', 1.0),
    ('descend', 'touch', 'descend', 1.0);
