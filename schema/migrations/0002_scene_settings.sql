-- SPDX-License-Identifier: Apache-2.0
-- Contract v0, migration 2: per-scene settings (sky colour, ambient light), and the component
-- documentation the editor and engine share (see schema/components.md).

ALTER TABLE scene ADD COLUMN settings TEXT NOT NULL DEFAULT '{}' CHECK (json_valid(settings));
