-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 22: the Input editor (milestone 6.1).
-- Each action gets the name players see (the Controls screen, prompts such as "Press E to
-- use") and whether players may rebind it. Projects add their own actions and bindings in the
-- Input editor; scripts read them by id (midgard.action, down, pressed). Mouse buttons are
-- controls too: "left", "right", "middle", "x1", "x2" (counted while the game has the mouse).

ALTER TABLE input_action ADD COLUMN name TEXT NOT NULL DEFAULT '';
ALTER TABLE input_action ADD COLUMN rebindable INTEGER NOT NULL DEFAULT 1;

-- The built-in actions (every earlier migration's), named.
INSERT OR IGNORE INTO input_action (id, kind) VALUES
    ('move_x', 'axis'), ('move_y', 'axis'), ('look_x', 'axis'), ('look_y', 'axis'),
    ('zoom', 'axis'), ('jump', 'button'), ('menu', 'button'), ('view', 'button'),
    ('run', 'button'), ('use', 'button'), ('descend', 'button');
UPDATE input_action SET name = 'Move sideways' WHERE id = 'move_x';
UPDATE input_action SET name = 'Move forward and back' WHERE id = 'move_y';
UPDATE input_action SET name = 'Look sideways' WHERE id = 'look_x';
UPDATE input_action SET name = 'Look up and down' WHERE id = 'look_y';
UPDATE input_action SET name = 'Zoom' WHERE id = 'zoom';
UPDATE input_action SET name = 'Jump' WHERE id = 'jump';
UPDATE input_action SET name = 'Menu' WHERE id = 'menu';
UPDATE input_action SET name = 'Change view' WHERE id = 'view';
UPDATE input_action SET name = 'Run' WHERE id = 'run';
UPDATE input_action SET name = 'Use' WHERE id = 'use';
UPDATE input_action SET name = 'Go down (flying)' WHERE id = 'descend';
-- Looking and zooming follow the mouse and sticks; the menu key opens the pause menu.
UPDATE input_action SET rebindable = 0 WHERE id IN ('look_x', 'look_y', 'zoom', 'menu');
