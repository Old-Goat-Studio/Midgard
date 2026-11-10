-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 21: screens made in the Menu editor (milestone 6.0).
--
-- A screen is a menu, the title screen, pause or game over: elements (panels, text, buttons,
-- sliders and checks) each hung on an anchor of its parent and placed in dp, with its own look.
-- Buttons close their screen, open another, quit, or tell scripts (midgard.ui.on_click). The
-- format is in engine/core/include/midgard/screen.h; the runtime draws screens with RmlUi.

CREATE TABLE screen (
    id       TEXT PRIMARY KEY,
    name     TEXT NOT NULL,
    position INTEGER NOT NULL DEFAULT 0,
    screen   TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(screen))
                          -- {"modal": true, "pauses": false, "background": [r, g, b, a],
                          --  "elements": [{"id", "kind", "parent", "anchor", "x", "y", "w",
                          --                "h", "text", "style", "action", ...}]}
);
