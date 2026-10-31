-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 7: the "view" action (milestone 5.6, Cameras).
-- It steps the player's camera through its views: third person, first person, top-down.
-- The engine gives older packages the same default bindings.

INSERT OR IGNORE INTO input_action (id, kind) VALUES ('view', 'button');

INSERT OR IGNORE INTO input_binding (action, device, control, scale) VALUES
    ('view', 'keyboard', 'V', 1.0),          ('view', 'gamepad', 'y', 1.0);
