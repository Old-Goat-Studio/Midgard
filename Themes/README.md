# Themes

Editor colour themes. Every `.qss` file here appears in the editor's **View ▸ Themes** menu,
named after the file (`solarized-light.qss` → *Solarized Light*), below *System Light* and
*System Dark*. Images a theme needs go in a `<name>-icons/` folder beside it and are loaded as
`url(renoir:<file>)`. See the Theme Scheme section of `midgard_roadmap.md`.

| Theme | Description |
|---|---|
| `yggdrasil.qss` | Midgard's signature dark theme, coloured after the logo: night blue, bark, leaf green and knotwork gold. |

You can add your own themes without touching the install: put `.qss` files in the `Themes`
folder of your Midgard settings folder (`%APPDATA%\Midgard\Themes` on Windows,
`~/.local/share/Midgard/Themes` on Linux). A theme there with the same name replaces the bundled
one. The menu is rebuilt each time it opens, so new files appear straight away.

Themes must style the docking panels too (`ads--CDockWidgetTab`, `ads--CDockAreaTitleBar`,
`ads--CDockSplitter`, `ads--CFloatingDockContainer`); see `yggdrasil.qss` for an example.
