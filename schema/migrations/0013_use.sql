-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 13: the "use" action (milestone 5.11a).
-- Pressed inside a scene exit whose activation is "use" (a lever, a teleport pad), it takes the
-- player through. Scripts read it too (midgard.action("use")).
-- The engine gives older packages the same default bindings.

INSERT OR IGNORE INTO input_action (id, kind) VALUES ('use', 'button');

INSERT OR IGNORE INTO input_binding (action, device, control, scale) VALUES
    ('use', 'keyboard', 'E', 1.0),  ('use', 'gamepad', 'x', 1.0);
