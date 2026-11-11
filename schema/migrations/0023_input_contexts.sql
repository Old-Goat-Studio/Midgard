-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 23: input contexts (milestone 6.1).
-- An action counts only while its context is the game's: "on_foot", "flying", "menu" (a menu
-- shows), "vehicle" (a script says so), or "always". So one key can do one thing on foot and
-- another flying. The game chooses the context each frame; a script can choose its own
-- (midgard.set_input_context). Until now every action said "on_foot" and none was kept to it:
-- Midgard's own actions count always, but going down, which is for flying.

UPDATE input_action SET context = 'always'
    WHERE id IN ('move_x', 'move_y', 'look_x', 'look_y', 'zoom', 'jump', 'menu', 'view', 'run',
                 'use');
UPDATE input_action SET context = 'flying' WHERE id = 'descend';
