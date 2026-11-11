# Midgard Help

*A plain-language guide to making games with Midgard. No programming or 3D experience is
assumed: every term is explained the first time it appears.*

---

## Contents

1. Welcome: what Midgard is, and the words you'll meet
2. A tour of the editor window
3. Your first five minutes
4. Getting around the 3D view
5. Building a scene
6. Which way is "forward"?
7. The Asset Library: storing your pictures, models and sounds
8. Objects: models you place many times
9. Colours, materials and textures
10. Terrain: hills, valleys, sand and snow
11. Light: the sun, the sky, lamps, candles and torches
12. Sound and music
13. Time, calendars, day and night
14. Physics: things that are solid, fall and bounce
15. The player and the camera
16. Characters and animation
17. More than one scene: doors and portals
18. Scripting for beginners
19. Scripting reference: everything a script can do
20. Playing, building and exporting your game
21. Saving, Git, backups and the error log
22. Troubleshooting: when something goes wrong
23. Keyboard shortcuts

---

## 1. Welcome

Midgard is a tool for making 3D games. You build the game's world in the **editor** (the
program you are using now), press **Play** to try it, and when it's ready you **export** it as a
game other people can run on Windows, Linux or Android.

### Words you'll meet

- **Project**: one game. It lives in a folder of its own (by default under *Documents ▸ Midgard
  Projects*). Everything the game needs is kept in that folder.
- **Scene**: one place in your game: a level, a room, a planet. A game can have many scenes, and
  doors between them.
- **Entity**: anything in a scene: a box, a tree, a lamp, the player, the camera. If you can
  point at it in the Scene list, it's an entity.
- **Component**: one ability an entity has. A box has a *mesh* (its shape), a *material* (its
  colour), maybe a *collider* (it's solid). You give an entity abilities by adding components.
  The **Inspector** shows an entity's components and lets you change them.
- **Asset**: a file your game uses: a picture, a 3D model, a sound, a font.
- **Asset Library**: your project's organised store of assets, with their licences.
- **Object**: a model made into a reusable template, placed as many times as you like. Change
  the template and every copy changes.
- **Script**: a small text file of Python instructions that makes an entity *do* something:
  open a door, follow the player, keep score.
- **Cook**: Midgard packs the project into the compact form the game reads. It happens
  automatically when you press Play or Export.
- **Play**: runs your game in its own window while the editor stays open, so you can change
  things and see the result straight away.

---

## 2. A tour of the editor window

The editor is made of **panels** (small windows) arranged round the big 3D view. You can drag
panels anywhere, stack them as tabs, float them on another monitor, or close them. **Window** in
the menu bar lists them all; tick one to bring it back. **Window ▸ Layouts** saves and restores
arrangements ("Two Monitors" is built in).

| Panel | What it's for |
|---|---|
| **Viewport** | The 3D view of the scene you're editing. |
| **Scene** | A list of every entity in the scene. Click one to select it. |
| **Inspector** | The selected entity's settings: name, place, and all its components. |
| **Project** | The files in your project's folder. |
| **Scenes** | All the scenes in your game; open, add, rename them. |
| **Objects** | Your reusable objects (imported models). |
| **Script Editor** | Where you write and edit scripts. |
| **Terrain** | Brushes to sculpt and paint the ground. |
| **World Types** | Recipes for whole worlds: how the ground is shaped and coloured. |
| **Environment** | How the sky, sunlight, fog and water look through the day. |
| **Calendar** | The game's calendar and clock; scrub through the day to preview it. |
| **Animation** | The animation graph editor for characters. |
| **Audio** | Sound events, music playlists, ambience and the mixer, with Play to hear them. |
| **Project Page** | The game's title, version, credits and options. |
| **Log** | Messages from the editor and, while it runs, from your game. |
| **History** | Every change you've made; click one to go back to it. |
| **Assistant** | An AI helper you can ask questions (if switched on in Preferences). |

### The menus

- **File**: new, open and close projects; put a project under Git and commit (see section 21).
- **Edit**: Undo, Redo, and **Preferences** (your settings for the editor itself).
- **Build**: **Cook**, **Play** (F5), **Continue** (Ctrl+F5), **Pause** (F6), **Stop** (Shift+F5)
  and **Export…** (Ctrl+E).
- **World**: **World Shape…** (flat, cylinder, planet, hollow world) and **Physics Layers…**.
- **Assets**: the **Asset Library…** (Ctrl+Shift+L), **Import Into Library…**, and **Add Sample
  Assets** (free sample characters and animations to practise with).
- **View**: show colliders and gizmos; choose a colour **Theme**.
- **Window**: show or hide panels; layouts.
- **Help**: this guide (**F1**), and **About Midgard**. The guide opens in its own window: its
  sections are listed down the left (click one to go there; the list follows as you read),
  the **Search** box finds every place a word appears (Enter for the next, Shift+Enter for the
  one before), and **Save as PDF…** saves the whole guide to print: a title page, the
  contents with each section's page, then the guide with its pages numbered.

---

## 3. Your first five minutes

1. **File ▸ New Project…**. Give it a name and choose a world mode. A folder is made for it.
2. In the **Scene** panel, click **Add ▸ Cube**. A grey cube appears in the Viewport and is
   selected.
3. In the **Inspector**, change its **Position** Y to 3. The cube jumps up three metres.
4. Click the **Colour** button and choose red.
5. Press **F5** (Play). Your game opens in its own window. Walk with **W A S D**, look with the
   mouse, jump with **Space**. Close the game window, or press **Shift+F5**, to stop.

That's the whole loop: change something, press Play, look, repeat. Everything you do in the
editor can be undone with **Ctrl+Z**.

### Sample projects to explore

**File ▸ New Sample Project** makes a ready-made project in your projects folder and opens it
(a second copy gets a number on its name, so yours is never replaced):

- **World Tour**: a paved hub with four doorways, each into a world of a different shape:
  - **Archipelago**: islands in a sea, 3 km across, with an edge.
  - **Habitat**: inside a spinning cylinder 1 km in radius and 5 km long. The land curves
    up on both sides and meets overhead. A **Dock** pad takes you outside into space.
  - **Planet**: a small planet 2 km in radius, cold at the poles and warmer towards its
    middle. The orange **Launch Pad** is a jetpack: fly up through the atmosphere (200 m) into
    space and back down.
  - **Hollow World**: inside a sphere 2.5 km in radius, with the far side overhead through
    the haze. An **Exit Site** pad takes you outside.

  Every world's ground is textured (grass, sand, rock, snow, the Planet's desert and forest,
  the Hollow World's faintly glowing moss), each texture set made with Midgard's own texture
  generator and in the project's Asset Library, and a cobbled **Path** (the world type's
  first paint layer) leads away from where you arrive.

  It opens at its **title screen** (New game, Settings, Quit) and has a pause menu (Esc),
  settings and a game over screen, all made in the Menu editor (section 15). Five gold
  **coins** round the hub raise your Score with a note each; standing in the red **fire pit**
  burns Health away until **You died**, and **Try again** puts you back where you arrived.

  The first time you arrive in a world, its sky is clear for an hour (a small script,
  `fair_welcome`, does it with `midgard.weather.force`); then its own weather comes back.
  Each world has its own day and night, seasons, weather and sounds (the sea on the
  Archipelago, wind on the Planet, a hum in the Habitat), with music throughout. You walk as
  an animated character (press **V** for first person, third person or the view from above),
  with footsteps that sound like the ground under you; another character strolls round the
  hub. Walk all the way round any world and you come back where you started. The glowing pad
  behind where you arrive takes you back to the hub (stand on it and press **E**).

  All its sounds and the character are Midgard's free samples, in the project's Asset Library
  with their credits.
- **Scene Tour**: three small scenes joined by a door and a teleport pad.

---

## 4. Getting around the 3D view

| To do this | Do this in the Viewport |
|---|---|
| Turn round what you're looking at | Drag with the left mouse button |
| Slide the view sideways (pan) | Drag with the middle button, or Shift + drag |
| Zoom in and out | Mouse wheel |

**Gizmos** are the three small controls in the Viewport's top-right corner: **Move**, **Rotate**
and **Scale**. Select an entity in the Scene list, then drag a coloured handle: red is the X
axis (left/right), green is Y (up/down), blue is Z (forwards/backwards). The white centre of
Scale grows or shrinks evenly. Each drag is one Undo step.

The Viewport's toolbar has switches:

- **Colliders**: outlines of everything solid (see section 14).
- **Navigation**: where computer-controlled characters can walk.
- **Lights**: little light bulbs where your lamps are (they never appear in the game).
- **Gizmos**: show or hide the Move/Rotate/Scale controls.

*Selecting things:* choose them in the **Scene** list. (Clicking objects in the 3D view to
select them is not available yet.)

---

## 5. Building a scene

### Adding things

The **Add** button at the top of the **Scene** panel offers:

- **Folder**: just for tidiness; folders group entities in the list and do nothing in the game.
- **Group**: a set of parts that move as one (a cart made of a box and four wheels). Move the
  group and all its parts come along.
- **Scene Exit** and **Arrival Point**: doors between scenes (section 17).
- **Point Light** and **Spot Light**: lamps (section 11).
- **Reverb Zone**: a place that echoes (section 12).
- **Object**: one of your reusable objects (section 8).
- **Sky**: the scene's sky (one per scene).
- **Cube, Sphere, Plane, Cylinder, Wedge**: simple shapes, great for building and testing.

### The Scene list

- Click to select. Press **Delete** to delete (and Undo brings it back).
- **Drag** an entity onto a folder or group to put it inside. It stays where it is in the
  world.
- **Drag a light onto anything** (a candle, a lantern) to attach it: it then moves with that
  thing.
- Right-click for more options.

### The Inspector

The top rows are the same for everything:

- **Name**: what it's called in the list (scripts can find things by name).
- **Position**: where it is, in metres. X is left/right, **Y is up**, Z is forwards/backwards.
- **Rotation**: how it's turned, in degrees. The middle number (Y) turns it round like a
  revolving door.
- **Scale**: its size. 1 is normal, 2 is twice as big.

Below come its components. **Add Component** gives it new abilities: Script, Trigger, Collider,
Rigid body, Physics material, Scene exit, Arrival point, Character, Sound, Reverb zone,
Footsteps, Heat source. Each component has an **✕**
button to take it off again.

*Position, rotation and scale are always relative to the entity's parent.* If a candle is inside
a table group, its position is measured from the table.

---

## 6. Which way is "forward"?

Midgard has one simple rule: **everything faces along its own -Z** (negative Z, the blue
axis pointing away from you).

- A **camera** looks along -Z.
- A **spot light** shines along -Z.
- An **arrival point** faces players along -Z.
- A **character** faces -Z, and the player turns to face the way it walks.

Most 3D model files (glTF) are made facing +Z. Midgard turns characters round for you so they
face the way they walk. If a *plain* model (a statue, a car) faces the wrong way, set its
**Rotation** Y to 180.

---

## 7. The Asset Library

The **Asset Library** (**Assets ▸ Asset Library…**, Ctrl+Shift+L) is your project's store of
everything you collect: models, textures, pictures, icons, music, ambiance, sound effects and
fonts. It's kept inside the project folder (in `library/`), so it travels with the project.

### Bringing things in

**Assets ▸ Import Into Library…** (or **Import** in the library window):

1. Choose files, a folder or a .zip file. The **Preview** list on the right shows everything
   in them that can be seen or heard: click one to see a picture, read a font's sample, play a
   sound or turn a model, before anything is imported.
2. Choose the type (Textures, Models, Music…), where it goes (a texture's kind, or a sound's
   album), and a name.
3. Choose the **licence** and fill in who made it (see below).
4. For textures, a table shows every picture: choose what each one is for (colour, normal,
   roughness, metal, ambient occlusion, glow) or "not used". Midgard renames them for you.

Midgard copies the files in. It never changes your originals. Files it can't use (say, a
Photoshop file inside a zip) are skipped and listed.

### Licences and credits

Only use assets you're allowed to. Midgard accepts these:

- **CC0**: free for anything, no credit needed.
- **CC-BY**: free, but you must credit the author. Midgard does this for you: the **Project
  Page** shows the credits your game needs, and every exported game includes a `CREDITS.txt`.
- **CC-BY-SA** (share-alike): free, with a credit, and if your game changes the asset
  (recolours it, cuts it up), you must offer the changed asset under the same licence.
- **Purchased** (from the Master Asset Library, below): bought assets. Your games may use
  them, but their **original files may never be given away**.
- **SIL Open Font License (OFL)**: fonts free to use and ship with your games (not to sell on
  their own; a changed font gets a new name). No credit needed.
- **Own work (all rights reserved)**: things you made yourself and keep for your own games.
  The artist's name is needed; like purchased ones, their original files are never given away.

### Looking at assets

Select an asset to see its details and a preview. Below its licence, every file it holds is
listed with its size; click one to see it (or play it). Files there's nothing to show of, such
as a licence or a model's data, are greyed.

- **Models** turn slowly; if they're animated, choose a clip and press Play. Their texture
  pictures are listed too.
- **Sounds:** click one to play it (double-click plays it at once).
- **Fonts** show a sample line in the font.
- **Textures** are shown as a material (the first line of the list); click a map to see it
  on its own. As a material: choose **Cube, Sphere or Plane**, and **1:1, 1:2 or
  1:4** to see how the texture repeats. **Drag** to turn it, **wheel** to zoom, **double-click**
  to reset. A lamp beside the camera shows how shiny it is.

The first time a big texture set is shown, Midgard prepares it (it says "Preparing the
textures…"); you can carry on working meanwhile. After that it shows at once.

### Organizing the library

Inside each type (Textures, Music, Models…) you can sort your assets into **collections**, like
folders, and put collections inside collections as deep as you like. Texture kinds (Metal,
Wood…) and albums are collections too. Everything stays in the order you put it in.

- **Drag** assets and collections to move them: drop on a collection to put them inside it,
  or between two rows to put them there. Choose several with Ctrl or Shift first to move them
  together.
- **Drop on a type** in the list on the left to move sounds between Music, Ambiance and FX, or
  pictures between Images, GUI and Icons. (A texture can't become a model.)
- **Right-click**, or press **Organize**, for:
  - **Rename…** (F2): an asset or a collection.
  - **New Collection…** (Ctrl+Shift+N): inside the chosen collection, or beside the chosen
    asset.
  - **Delete Collection**: only an empty one. Move its assets out first.
  - **Move To…** (Ctrl+Shift+M): choose where from a list, without dragging.
  - **Move Up** (Alt+Up) and **Move Down** (Alt+Down).
  - **Sort A-Z**: collections first, then assets, each by name.

**Your game keeps working.** When you rename or move something, Midgard changes everything in
your game that uses it to match, all at once: scenes, materials, sounds, characters, the
project's settings. If any part can't be changed (a file open in another program, say), nothing
is changed and Midgard tells you why. Undo is cleared afterwards, because what it would put
back uses the old names. If one of your scripts names an asset being moved, Midgard shows you
the lines and asks whether to change them too, leave them for you, or cancel.

The **Master Asset Library** program organizes its library the same way, with the same
commands.

### Using assets

- In the **Inspector**, rows that take a model or picture have a **Library…** button.
- A character's **Model** and **Animations from** come from the library.
- In the Material editor, **Library…** puts a whole texture set on a material in one step.

### The Master Asset Library: every asset you have, for all your projects

The **Master Asset Library** is a separate program, installed beside Midgard, with a vault-door
icon. It holds every asset you have, whether downloaded, bought, made yourself or collected from
your projects. Each project's own library takes **copies** from it, so a project is always
complete by itself, even on another computer.

In the Master Asset Library program:

- **Import…** brings in files, folders and zips (or drop them on its window), the same way as
  here, with **tags** to find them by later. Downloads are checked: a zip can't put anything
  outside the library, and a file that isn't what its name says is left out.
- **File ▸ Collect from a Midgard Project…** gathers what a project's library already has.
- **Purchased** assets record who sold them, what, when, the order number, and the seller's
  licence (a file kept with the asset).
- Search, filter by tag or licence, and look at any asset (models and textures in 3D).

In Midgard:

- **Assets ▸ Add from Master…** (or **Add from Master…** in the library window): find assets,
  choose them and press **Add to Project**. The asset you choose is shown on the right first (a
  picture, a font's sample, a sound to play, a model turning). Those the project already has
  are greyed.
- An asset copied from the Master says so in its details. When the Master gets a newer
  version, the library window offers **Update from Master**.
- **Send to Master** gives one of your project's assets to the Master, for your other
  projects. If it came from the Master and you changed it here, it can become the Master's
  next version.
- Midgard finds the Master where its program keeps it. To use another folder, set
  **Edit ▸ Preferences ▸ Master library**.

**Purchased assets, your own work, and Git.** When you commit, Midgard warns if the files of
purchased assets, or of your own all rights reserved work, are going into Git: if the project is ever pushed somewhere others can see (a public GitHub
repository), they would go with it. **Keep Them Out of Git** leaves them in the project but out
of its history.

---

## 8. Objects: models you place many times

The **Objects** panel holds templates made from 3D model files (`.glb` or `.gltf`). Import a
model there once, then use **Add ▸ Object** to place it as often as you like. Every copy uses
the template, so changing the template's materials changes every copy. Each part of the model
keeps its own material, which you can change in the Material editor.

You can also make an object from things you've built: select a **group** and choose **Save as
Object…** in the Inspector.

---

## 9. Colours, materials and textures

### Words first

- A **material** is what a surface is made of: its colour, how shiny, whether it's metal, and
  whether it glows.
- A **texture** is a picture wrapped onto a surface: bricks, wood grain, rust.
- A **texture set** is several pictures for one surface, each doing a job:
  - **Colour** (base colour): the picture you see.
  - **Normal**: the bumps (it looks blue-purple). It makes a flat wall look like it has mortar
    lines.
  - **Roughness**: where it's shiny (dark) and where it's matte (light).
  - **Metal**: where it's bare metal (white) and where it isn't (black).
  - **Ambient occlusion**: soft shadow in the cracks.
  - **Emission**: where it glows.

### Changing a colour

Select the entity; in the Inspector click **Colour**. For everything else, click **Edit…** next
to it to open the **Material editor**.

### The Material editor

It shows a live preview (drag to turn it, wheel to zoom). You can set:

- **Base colour** and **transparency**.
- **Metal** (0 for plastic, wood or stone; 1 for bare metal) and **Roughness** (0 is a mirror
  polish, 1 is completely matte).
- **Emission**: a glow colour and strength (lamps, screens, lava).
- **Casts a shadow**: on unless you turn it off. Turn it off for see-through things (a force
  field, water, a ghost): a shadow is always solid, even where a surface shader cuts holes.
- **Textures**: each slot has **Choose…** (a picture file) and **Library…** (from your Asset
  Library). Picking a whole texture set from the library fills every slot at once.

When you add a big picture, the preview says "Preparing the textures…" for a moment the first
time; the editor stays usable.

### Textures on models

A model file usually brings its own materials and textures. Midgard uses them automatically:

- **A model placed through the Objects panel**: each part keeps its own material. Change any
  part's material in the Material editor.
- **A model used directly as a mesh, or a character's model**: it's drawn with the model's
  first material. Anything you set in the entity's own material (a colour, a texture slot) wins;
  the model's fills in the rest.

The first time a model with big textures is used, Midgard converts its textures for the game
(this can take a minute for a 4K character). Meanwhile it's shown plain, and its textures
appear when they're ready. The converted copy is kept, so next time it's instant.

### Making your own textures: the Texture Generator

**Assets ▸ Texture Generator…** makes textures from patterns, with no painting needed.

- **Start from** one of the ready-made looks: Clouds, Marble, Wood, Rock, Flagstones, Bricks,
  Tiles, Warning stripes, Sand ripples or Hammered metal. Then change anything you like.
- **Pattern**: Noise, Ridged noise, Cells, Gradient, Radial, Rings, Checker, Stripes, Bricks or
  Waves. Only the settings the pattern uses are shown:
  - **Scale**: how many features across the picture. A whole number (4, not 4.5) makes the
    texture **repeat seamlessly**. Tick **Show it tiled** to see four copies side by side.
  - **Detail** and **Roughness** (noise): how many finer layers, and how strong they are.
  - **Warp** bends the pattern; it turns waves into marble or wood grain.
  - **Angle**, **Width**, **Softness**, **Irregular** and **Variation** shape stripes,
    bricks and cells.
  - **Seed** picks a different random version of the same pattern (**New** for another).
- **Colours**: the pattern runs from its lowest parts (0) to its highest (1); each colour
  says what colour it is at that point, blending between them. **Add a colour** for more
  (up to six); **Invert** swaps high and low.
- **Makes**: **Colours**, or **Bumps** (a normal map, the blue-purple kind that gives a
  surface its bumps). With colours, **Its bumps too** also saves a matching bump map.

**Saving**:
- **Save to Project** saves it in your project's **textures/generated** folder (with its
  recipe, so **Open** brings it back to change later). It is then a picture like any other:
  choose it in the Material editor.
- **Add to Library** also puts it in the project's Asset Library (with its bumps, as one
  texture set), as your own work or as CC0. Type your name in the box beside the licence; it's
  remembered.
- **Send to Master** puts it in the project's library and your **Master Asset Library**, for
  all your projects.

### About repeating (tiling)

The **1:1, 1:2, 1:4** choice in the library's texture preview is for looking only. In the game,
how often a texture repeats depends on the shape's own texture layout. The built-in Cube, Plane
and other shapes show the texture once per metre of face, so a bigger cube shows more repeats.

### Surface shaders: your own look for a material

A **surface shader** is a short piece of code that changes how a surface looks: stripes,
glowing cracks, holes that eat away, ripples, anything you can describe with maths. Midgard
still does the hard part, so the sun, shadows, lamps, fog, snow and rain all still work on it.

- Shaders live in your project's **shaders** folder, one file each, ending in **.glsl**.
- In the **Material editor**, pick one under **Surface shader**, or click **Choose…** to bring
  in a .glsl file from elsewhere (it is copied into the shaders folder). Pick **None** to go
  back to Midgard's own material. **Edit…** opens the shader in the **Shader Editor** (with no
  shader chosen yet, it makes a new one for this material first).
- Under the choice, the editor tells you whether the shader works. If it has a mistake, it
  names the line, and anything using it is drawn **bright magenta** (pink-purple) until you
  fix it. A mistake never crashes the editor or the game.
- When you make the game, each shader is turned into a form the game uses directly, so the
  game itself never needs to understand shader code. Mistakes show up in the cook's warnings,
  with their line numbers.

A shader is one function. The material's own colour, textures and settings are its starting
point, and it changes only what it wants to:

```glsl
void surface(SurfaceInput i, inout Surface s) {
    float band = step(0.5, fract(i.uv.x * 8.0));          // 8 stripes across
    s.color = mix(srgb(vec3(0.9, 0.2, 0.1)), s.color, band); // red ones
    s.roughness = 0.3;                                       // a bit shiny
}
```

What it can **read** (`i.`): `uv` (the texture position), `position`, `view` (towards the
camera), `geometry_normal`, `up`, `sun` (towards the sun), and `time` (seconds, repeating
every hour).

What it can **change** (`s.`): `color`, `alpha` with `cutout` (below the cutout a point isn't
drawn, which makes holes), `metallic`, `roughness`, `normal` (bumps), `emission` (glow) and
`occlusion`.

Helpers it can use: `noise(p)` and `fbm(p, layers)` (smooth random patterns from 0 to 1),
`hash(p)` (a random number), `srgb(colour)` (a colour as you'd pick it in a colour
picker), and `bend_normal(normal, slope)` (bumps: the normal tilted by how steeply a pattern
rises across the surface). The material's textures are there too, as `base_color_map`, `normal_map` and so on.

#### Shader settings: one shader, many materials

A shader can declare **settings**: values that each material using it chooses for itself.
Write them at the top of the shader, one per line:

```glsl
setting color tint = vec3(1.0, 0.4, 0.1);   // Glow colour
setting float speed = 2.0;                  // How fast it pulses, 0 .. 10
setting bool sparkle = true;
setting texture pattern;                    // The pattern

void surface(SurfaceInput i, inout Surface s) {
    float pulse = 0.5 + 0.5 * sin(i.time * speed);
    s.emission = tint * pulse * texture(pattern, i.uv).rgb;
}
```

- The value after `=` is the shader's own (its default).
- The comment after a setting is its label in the Material editor. A range such as
  `0 .. 10` in it sets the slider's ends (otherwise 0 to 1).
- The kinds of setting are: `float` (a number), `int` (a whole number), `bool` (on or off),
  `color` (a colour, picked with a colour picker), `vec2`, `vec3` and `vec4` (two, three or
  four numbers), `texture` (a picture) and `data_texture` (a picture holding numbers, such as
  a mask, noise or bumps).
- A shader can have up to 16 number settings and 4 textures.

In the **Material editor**, under the shader's name, **Shader settings** shows a control for
each one: a slider, a tick box, a colour button, number boxes, or a texture with
**Choose…**. A material shows the shader's own values until you change them. The **↺** button
next to a setting puts back the shader's own value. Every change can be undone.

So one "glow" shader can make a red warning light, a slow blue crystal and a flickering green
screen, each just a material with different settings.

If a setting is written wrongly, the editor names its line, just like any other mistake.

#### The Shader Editor

The **Shader Editor** panel (a tab behind the Viewport; **Window** menu if it's closed) is where
you write shaders.

- **New** starts a shader from a starter: **Blank**, **Water** (moving ripples), **Glow**
  (a pulsing glow, brightest at the edges), **Dissolve** (holes eaten away, glowing at their
  edges), **Cartoon** (flat steps of light and an outline; best on rounded shapes) or **Force
  field** (a see-through glowing honeycomb). Each is a copy you can change freely, and each
  has settings to try in the Material editor.
- **Open** lists the project's shaders; each opens in its own tab. **Save** (Ctrl+S) saves
  it. Tabs with unsaved changes show a `*`.
- The code is coloured as you type. Start typing and press **Ctrl+Space**, or type `s.` or
  `i.`, for a list of what fits there.
- Mistakes are found as you type: each is **underlined in red** on its line (hover over it
  to read it), and listed below the code. Click one to jump to its line.
- On the right, a **live preview** shows the shader on a sphere, cube or plane, using the
  shader's own settings. Drag to turn it; the wheel zooms. **Animate** lets the time run, for
  shaders that move. While the code has a mistake, the preview keeps showing the last version
  that worked.
- When you save, the Viewport and any open Material editors show the shader as it is now.
- **While the game is playing** (Play), saving a shader changes it in the running game at once:
  no need to stop and start again. If the saved shader has a mistake, it isn't sent: the game
  keeps drawing the last version that worked, and the Log says so. Fix it and save again.
  Shaders saved from another text editor are picked up the same way.

---

## 10. Terrain: hills, valleys, sand and snow

Terrain is the ground of a large world. It's made in three layers:

1. **World Shape** (**World ▸ World Shape…**): is the world a flat area, a cylinder (a space
   station you live inside), a planet, or a hollow world? This decides which way is "up" and
   how gravity pulls.
2. **World Type** (the **World Types** panel): a *recipe* for the ground: how high the hills,
   how big, where the sea is, and its climate and **biomes** (zones such as forest, desert,
   tundra, each with its own ground colour). It can also scatter objects (trees, rocks)
   automatically. A preview map and 3D view show the result.
3. **Terrain brushes** (the **Terrain** panel): your own hand-made changes on top.

### Sculpting and painting

In the **Terrain** panel choose a tool, set its size and strength, then drag on the ground in
the Viewport:

- **Raise** / **Lower**: build hills or dig valleys.
- **Smooth**: soften bumps.
- **Flatten**: level the ground where you start the stroke (good for building sites).
- **Paint**: **Lush grass**, **Dry grass**, **Sand**, **Rock**, the world type's two paint layers
  (by the names you give them, below: a path, paving…), or **Clear paint**. The last paint
  wins: a path painted over rock replaces it, and rock painted over a path replaces that.

Each stroke is one Undo step.

### Ground textures

The ground's look comes from Midgard itself: grass from dry to lush, sand on beaches, rock on
steep slopes, and snow on high ground or in cold climates. It changes with the seasons. You
control it through the World Type's **Climate and Biomes** (colours for each zone) and the paint
brushes.

**Picture textures**: open them with **World ▸ Ground Textures…**, or the **Ground
Textures…** button in the **Terrain** panel. Both open the **World Types** panel on its
**Ground Textures** tab (the third tab along its top), with your scene's world type chosen. It lists every ground layer (**Grass**, **Dry grass**, **Sand**, **Rock**,
**Snow**), the two **paint layers**, and each biome's lush and dry ground. Double-click a row (or **Choose Texture Set…**)
to pick a **texture set** from your Asset Library's Textures: each set's whole material is
shown on a sphere, a cube or a plane as you pick it. The layer takes all of the set's maps:
its colour, **normal** map, **metal and roughness**, **ambient occlusion (AO)** and **emissive**
(glow) map. The **Maps used** column has a tick box for each: untick one to leave it out (an emissive map that glows where you don't want it, or a metal map that makes the ground shine). It's kept with the layer, so ticking it again brings it back, and maps left out don't go into the finished game. A greyed box is a map the set doesn't have. **Tile** is how
many metres the texture covers before it repeats. **Clear** gives the layer its colour back.
The textures go into the finished game, and CC BY sets are credited.

The ground draws them where each layer lies: grass from dry to lush, sand on beaches, rock on
steep slopes and where you paint rock, snow in the cold, each biome in its zone, blended where
the colours used to blend. They're laid along the world's three directions and blended by the
way the ground faces (*triplanar*), so they look the same on a flat area, a cylinder, a planet
or a hollow world, don't stretch on cliffs, and have no seams. Each textured layer is lit as a
material: its normal map gives it bumps that catch the sun, metal and roughness decide its
shine, ambient occlusion darkens its cracks, and its emissive map glows (at night too). Winter
snow, autumn colours and rain-wet ground still lie over them. A layer without a texture keeps
its colour. The 3D preview in the World Types panel and the Viewport show them as the game does.

Close up you see each texture at its **Tile** size. Further off, a broad, blurred copy of the same
texture (over five times bigger) and a slow light-and-dark variation over some 40 m are mixed in,
so the ground doesn't show a repeating pattern into the distance. On phones the textures are
smaller (512 pixels against 1024) and drawn more simply, to keep the game smooth. When a game
starts, the ground shows its plain colours for a moment while its textures are made ready, then
its textures.

**Paint layers** are ground of your own, laid only where you paint it: a path across the
grass, cobbles in a village, a ploughed field. Type a name in the row (say **Path**), give it
a texture set, then in the **Terrain** panel choose **Paint** and your layer by its name, and
drag over the ground. Without a texture set, the first paints bare earth and the second grey
paving. Each stroke is one Undo step, and the game walks on them as stone.

For a special patch (a stone courtyard), you can also place a textured **Plane** or flattened
**Cube** on the ground.

---

## 11. Light: the sun, the sky, lamps, candles and torches

### The sun and sky

Every new scene has a **Sun** (a directional light: parallel rays from far away) and a **Sky**,
in its World folder.

- Turn the Sun with its Rotation to change where light comes from.
- Tick **Follows the clock** and the sun rises and sets with the game's time.
- The **Sky**'s **Environment** makes the sky colour, sunlight, fog and water follow the time of
  day (edit how in the **Environment** panel).

### Lamps

**Add ▸ Point Light** (a glow in every direction: candles, bulbs, fires) or **Add ▸ Spot Light**
(a cone of light: torches, headlamps, stage lights).

**To put a flame on a candle:** select the candle in the Scene list, then **Add ▸ Point Light**.
The light goes inside the candle, just above its base, and moves wherever the candle goes. You
can also drag any light onto any object in the Scene list.

In the **Inspector**:

- **Light**: Point or Spot.
- **Colour**: warm orange for flames, cool white for electric light.
- **Intensity**: brightness.
- **Reach**: how many metres the light travels before fading to nothing.
- **Cone** (spots): **inner** is fully bright, fading to nothing at **outer**.
- **Flicker**: **Steady**, **Candle**, **Torch** or **Campfire**, or set your own **dims** (how
  much it dims) and **speed**.

To aim a spot light, turn it with the **Rotate** gizmo.

In the Viewport each lamp shows as a little **bulb** in its colour (faint if it's hidden inside
something). A spot shows its cone; the selected lamp shows how far it reaches. Turn the bulbs
off with **Lights** on the Viewport toolbar. They never appear in the game, and the flicker only
shows while the game plays.

*Limit:* the game lights each moment with the **8 lamps nearest the camera**.

---

## 12. Sound and music

Midgard plays sound effects, music and ambience (background sound such as wind or birds).

### Words first

- A **sound** is a sound file in your Asset Library: an effect, a piece of music or ambiance.
- A **sound event** is a named sound your game plays: "Door creak", "Theme", "Wind". It can
  hold several variations of the sound; each time it plays it picks one, and nudges its volume
  and pitch a little, so a sound heard again and again never sounds mechanical.
- A **bus** is like a fader on a mixing desk. Every sound plays on one: **music**, **effects**,
  **voice**, **ambience** or **interface**, all under the **master** (the whole game's volume).

### Giving something a sound

1. Bring the sound into the **Asset Library** (Assets ▸ Import Into Library…) as Music,
   Ambiance or FX.
2. Select the thing that makes the sound (a radio, a waterfall, a campfire) and choose **Add
   Component ▸ Sound**.
3. Click **Library…** next to **Sound** and pick the file. Midgard makes a sound event for it
   (music and ambiance loop by themselves; effects play once).
4. **Plays when the scene starts** plays it as soon as the scene begins (or the thing appears);
   untick it to let only a script play it. **Volume** makes this one louder or quieter.
5. Press **Play** to hear it.

In the Scene list, things with a sound show 🔊.

### Sound in 3D

Effects and ambience (not music) are heard from where the thing making them is, like in real
life:

- **Closer is louder.** Each sound is at full volume within a metre, half as loud at two
  metres, and fades away completely by 40 metres. A sound that moves (a buzzing drone, a
  rolling cart) is followed.
- **Left and right.** A sound to your right comes from your right speaker or headphone.
- **Through walls.** When something solid (anything with a **Collider**) is between you and a
  sound, it becomes quieter and duller, like a party heard through a closed door. It eases in
  and out as you move.

The listener is the game's camera.

### Echoes: reverb zones

Caves, halls and cathedrals echo. To make a place echo:

1. **Add ▸ Reverb Zone** (🏛). A box appears.
2. Move and size it to cover the room (**Size** in the Inspector, or the Scale gizmo). Turn on
   **Colliders** in the Viewport toolbar to see it as a teal box.
3. Choose its **Echo**: **Room**, **Hall**, **Cave**, **Cathedral** or **Tunnel**.
4. **Fades in over** sets how many metres outside the box the echo begins, so walking into a
   cave sounds natural rather than switching on.

While the camera is inside, sound effects, voices and ambience echo; music and menu sounds don't.

### Music that fits the place and time

Music comes from **playlists**. A playlist is a list of tracks (sound events made from your
Music) and when it plays:

- in which **scenes**, **world types** and **biomes** (forest, desert, tundra...), and
- at which **times of day**: **dawn**, **day**, **dusk** or **night**.

Leave a choice empty to mean "anywhere" or "any time". As the player moves about and the day
goes by, the game plays the playlist that fits best: the one that names the most things that
are all true right now (a "Forest at night" playlist beats a plain "Forest" one at night), then
the one with the highest **priority**. When that changes, the old music fades out as the new
fades in over the playlist's **crossfade** time (3 seconds unless you say). A change has to hold
for two seconds first, so walking along the edge of a forest doesn't flip the music back and
forth. Tracks play one after another, in order or shuffled, with an optional **gap** of
silence between them.

A **stinger** is a short piece of music played over the music when something happens (a
fanfare for a found secret): the music dips under it and comes back afterwards.

### Ambience: the sound of a place

**Ambience** is the background sound of somewhere: wind on a hill, a forest's birds, a cave's
drips. An **ambience set** says where it belongs (scenes, world types, biomes; empty means
anywhere) and is chosen just as music is. It has:

- **Layers**: sounds that loop under everything, each at its own **times of day**. Birdsong at
  dawn and by day, crickets at dusk and by night: as the sun sets, the birds fade out as the
  crickets fade in.
- **Sprinkles**: one-off sounds now and then (an owl's hoot, a twig snapping), every so many
  seconds, at a random spot around the player, so they're heard from different directions.

### The Audio panel

The **Audio** panel (a tab behind the Viewport, or **Window ▸ Audio**) is where all of this is
made. It has four tabs; each lists what you've made on the left and shows the chosen one's
settings on the right, with **▶ Play** to hear it exactly as the game would play it and
**■ Stop**.

- **Sounds**: your sound events. **New from Library…** makes one from a sound file. Then:
  - **Sounds**: its variations (**Add Sound…** for more) and how often each is picked
    (**Weight**: 2 is twice as often as 1).
  - **Bus**: music, effects, voice, ambience or interface.
  - **Volume** and **Pitch**: each time it plays, a value is picked between the two numbers.
    Make them a little different (0.9 to 1.1) so repeats don't sound mechanical.
  - **Loops**; **Waits** (seconds before it can play again) and **at most** (how many can
    play at once).
  - **Heard from where it is (3D)**, **Full within** and **silent beyond**: how far it carries.
- **Music**: your playlists. **New Playlist**, then add **Tracks** (sound events), choose the
  **Order**, tick where it plays (**In scenes**, **World types**, **Biomes**; none ticked means
  anywhere) and when (**At**: dawn, day, dusk, night), its **Priority**, **crossfade** and
  **Gap**.
- **Ambience**: your ambience sets. **New Ambience**, then **Layers** (a sound, the times of
  day it plays, its volume) and **Sprinkles** (a sound, its times, about how many seconds apart
  and how many metres away), where it plays, its priority and crossfade. **▶ Play** lets you
  choose the time of day to hear.
- **Mixer**: how loud each bus is when the game starts (master, music, effects, voice,
  ambience, interface).

Everything can be undone, and while your game is playing (**F5**), changes reach it at once:
make a playlist and you hear it start.

### Footsteps

Footsteps sound like what's underfoot. To give something footsteps:

1. Select it (the player, a character, a horse) and **Add Component ▸ Footsteps**.
2. For each surface, choose the sound event a step on it plays: **grass**, **sand**, **rock**,
   **snow**, **water**, **wood**, **stone**, **metal**, and **anything else** for the rest.
   Make one sound event per surface with several variations each, so no two steps sound
   alike.
3. Say when it steps:
   - **Steps on**: the name of the animation event its walk cycle marks at each footfall
   (usually "step"; see *The animation graph* in section 16), or
   - **or every**: a step every so many metres it moves, for things without a walk cycle.

What's underfoot is worked out at each step. On an object (a bridge, a floor), it's the
**Surface** you choose in the object's **Physics material** (wood, stone or metal). On terrain,
it's the ground as it's drawn: sand on beaches, rock on steep slopes, snow high up (or in the
cold), water in the sea, painted sand or rock where you've painted it, and grass everywhere
else.

A character can also play sounds on its other animation events (a sword's "swing"), set in its
component's sounds.

### In the finished game

When the game is built, Midgard makes every sound it uses smaller: music and ambience become
**Ogg Vorbis**, effects become **Opus**. Each is checked to be sure the game can play it. CC-BY
sounds are credited automatically, like models and textures.



---

## 13. Time, calendars, day and night

Every game has a clock and a **calendar** (the **Calendar** panel): how long a day is, the
months, the weekdays, the seasons and moons. You can design your own calendar for a fantasy
world. Drag the Calendar panel's **scrubber** to preview any time of day in the Viewport.

Scripts can read and change the time, speed it up, stop it, skip ahead (sleeping in an inn),
and react at dawn, dusk or each new day (section 19, *Time*).

### Weather

Every project has a list of **weather types**:
Clear, Cloudy, Rain, Thunderstorm, Snow, Blizzard, Fog, Windy, Sandstorm and Ash fall. Each
one says:

- **How it looks:** how cloudy the sky is, how much daylight it takes away, the colour of the
  light, how far you can see (fog, dust or snow), what falls from the sky (rain, snow, ash or
  sand) and how much, and how often lightning flashes.
- **How it feels:** how strong the wind is and how gusty, how wet things get out in the open,
  and how much warmer or colder it is than usual.
- **How it sounds:** sounds that play while it lasts (rain, wind), and the thunder after each
  flash.

**How the weather is chosen.** A world type can have **weather tables**: lists of weather and
how much of the time each one is there ("clear 3, rain 1" means rain a quarter of the time). A
table can be for certain **biomes** (the desert gets sandstorms), certain **seasons** (snow in
winter), both, or everywhere. When several fit, the one for the biome and season wins, then the
one for the biome, then the one for the season. A table can also say what comes **after** a
weather: after a thunderstorm, rain, so a storm doesn't stop dead into sunshine. Each weather
lasts a while (you say how many hours, for example 1 to 6 for rain), then the next is chosen.

The world is divided into **weather cells** a few kilometres across, each with its own weather,
so it can rain over the hills while the valley is dry. Now and then a **front** forms: a band of
weather (a storm) that drifts across the world, over the cells in its path, round a cylinder
and across a planet.

Everything is worked out from the world's **seed** and the clock, so the same world always has
the same weather, and a **forecast** is always right (unless a script changes the weather).
**Inside worlds** such as habitats can have **controlled** weather instead: a timetable that
repeats, such as 20 hours clear, then 4 hours of rain.

**In your game.** The game always knows the weather where the player is standing; walk far
enough and you may walk out of the rain. A weather type's **sounds** (the patter of rain, a
howling wind) play while it lasts, fading in and out over a few seconds as the weather changes.

**Scripts** can ask what the weather is, get the forecast, do something when the weather
changes, and make their own weather: a storm that rolls in when the dragon appears, for an hour
or until the script lets go (section 19, *Weather*). A world with no weather tables has no
weather of its own, but scripts can still force some.

**What you see.** Clouds drift across the sky with the wind, white on a fair day, grey and
heavy in a storm, and hide the sun when they're thick. The daylight dims and takes the weather's
colour (blue-grey in a storm, orange in a sandstorm), and fog closes in to the weather's "how far
you can see". In a thunderstorm, lightning lights everything up for a moment, and its thunder
rolls in a few seconds later (give the weather type a **thunder** sound). When the weather
changes, the sky doesn't snap: a storm rolls in over several seconds.

**What falls.** Rain falls in streaks, snow and ash drift down as flakes, and sand blows
almost sideways, all pushed by the weather's wind and its gusts (a weather type's **falls** and
**amount**). Walk through it and it stays where it is in the world; it doesn't follow you like a
picture on the screen. It eases in and out as the weather changes; rain turning to snow fades
out before the snow comes. Phones draw a quarter as many drops and flakes as a computer, so
heavy weather stays smooth (the phone's self-test has a **Weather** check for this).

Inside a building (a roof and walls all round the player) no rain or snow is drawn. Under a
porch or an open shelter it still falls all round, and also under the roof itself. Under the sea (the
camera below its surface) none falls either.

**What it leaves behind.** Rain wets the ground (up to the weather type's **wetness**, in
about a quarter of an hour of game time), and it dries over a couple of hours after. Wet things
look darker and shinier, catching the sky and the sun, mostly on what faces up. Snow settles while
it snows (covering everything in two to four hours) on whatever faces up: the ground, the tops of
rocks, roofs and trees. It doesn't settle on water, steep rock or anything that glows. Afterwards
it melts in a few hours (at once in rain), and the melted snow leaves the ground wet. In a world
with a climate, snow stays as long as it's below freezing there.

### Staying warm and dry

The game keeps track of how the weather treats the player. This is the groundwork for survival
games; your scripts decide what happens (shivering, a cold, a warning).

- **Shelter:** a few times a second the game looks straight up and round about from the
  player's head (invisible rays). Something solid overhead is a **roof**; things close by on
  every side are **walls**. Under a roof, rain and snow don't reach the player. Walls cut the
  wind. Inside a building (a roof and walls all round) no rain or snow is drawn.
- **Getting wet:** out in the rain the player gets wet in a few game minutes (in snow, less so).
  Under cover they dry off over about an hour, much faster by a fire.
- **Heat sources:** give a campfire, a stove or a brazier **Add Component ▸ Heat source**.
  **Warmer by** is how many degrees warmer it is right next to it (15 to 25 for a campfire);
  **Reaching** is how far the warmth goes (it fades towards the edge). A negative number makes
  something that chills, like a block of ice.
- **Feels like:** the air's temperature, made colder by the wind (the same wind chill the
  weather forecasts use, in cold weather), colder again for being wet (up to 4 degrees, more in
  the wind), and warmer by heat sources near.

Press **F3** while playing to see it on screen: the weather, the air's temperature and how it
feels, the wind, the warmth, whether the player is sheltered, and how wet they and the ground
are.

### The Weather tab

All of this is made in the **Calendar** panel's **Weather** tab, in four parts. Everything can
be undone, and while your game is playing (**F5**), changes reach it at once.

- **Types:** your weather types. Pick one on the left (or **New** for your own: acid rain, a
  heatwave). On the right: **Clouds**, **Darkness** and the **Light colour**; how far you
  **Can see** (0: no limit); what **Falls** and **How much**; **Lightning** a minute; **Wind**,
  **Gusts**, **Wetness** and **Temperature** (how much warmer or colder than usual); and its
  **Sounds** (they loop while it lasts) and **Thunder** (after each flash).
- **Tables:** this scene's weather. Tick **This scene has weather**. Then:
  - **Weather cells:** how big a patch of weather is.
  - A **Table** says how much of the time each weather is there (**3** is three times as much as
    **1**; 0 never). Tick **In biomes** and **In seasons** to make a table just for those (snow
    in the mountains in winter); none ticked means everywhere and all year. **New Table** adds
    another.
  - **What comes after** overrides the table for what follows a weather: after a
    Thunderstorm, Rain.
  - **How long each lasts**, in hours, from least to most.
  - Or tick **Controlled** for a timetable that repeats, for inside worlds: 20 hours clear,
    then 4 of rain.
- **Fronts:** tick it for storms that travel across the world: how often a new one comes, how
  fast it moves, how big it is (in weather cells), how long it lasts, and its weather.
- **Forecast:** the coming days where the player starts, worked out by the engine exactly as
  your game will have it, as a coloured strip (a red band is a front passing) and a list. Below,
  how much of a year each weather is there, so you can check your tables at a glance.

A scene's weather is its own. If its world type has weather, the scene uses that until you
change something here; your first change makes the scene's own copy to edit.

---

## 14. Physics: things that are solid, fall and bounce

Add these with **Add Component**:

- **Collider**: makes it solid. Choose a shape: **box**, **sphere**, **capsule** (a pill),
  **convex** (shrink-wrapped round the model) or **mesh** (exactly the model's triangles: for
  scenery that never moves). An entity with only a collider is solid scenery.
- **Rigid body**: makes it move by physics:
  - **dynamic**: it falls, tumbles and is pushed (a crate, a ball).
  - **kinematic**: a script moves it, and it pushes other things (a lift, a moving platform).
  - **static**: it never moves.
  - **Mass** in kilograms (0 means "work it out from its size").
  - **Buoyancy**: whether it floats or sinks in the sea. Below 1 it sinks (0 like a stone; 0.5,
    the default, sinks slowly), 1 hovers where it is, and above 1 it floats: 2 floats half out of
    the water, like wood, and 1.1 floats almost under, like ice. The water slows everything in
    it, and a floating thing bobs and soon settles. It works on every world shape: on a planet
    the sea is all the way round.
- **Physics material**: **Friction** (0 is ice), **Bounciness** (0 stops dead, 1 bounces
  back fully), and its **Surface** (wood, stone, metal) for footsteps on it.
- **Trigger**: an invisible box that notices when the player walks in or out. Use it for
  pressure plates, alarms or checkpoints, together with a script.

**World ▸ Physics Layers…** decides which kinds of things pass through each other (the player
passes through "debris", for example).

Turn on **Colliders** in the Viewport toolbar to see the outlines: grey-green is scenery,
orange falls and tumbles, blue is moved by scripts, yellow is a trigger, cyan is the player.
In the game, **F4** shows them.

---

## 15. The player and the camera

The entity named **Player** has a **Player** component: speed, run speed, jump height and how
big it is. With a **Character** component it also has a body (section 16).

The **Camera** follows the player in three views: **first person** (through its eyes), **third
person** (behind it) and **top-down**. In the Inspector, **Edit…** next to the camera's field of
view opens the **Camera editor**, where you set each view's distance, height, angles and
smoothing.

### Controls in the game

| Action | Keyboard and mouse | Gamepad | Touch (a phone) |
|---|---|---|---|
| Walk | W A S D | Left stick | The stick, bottom left |
| Look | Mouse | Right stick | Drag on the right half |
| Run | Hold Left Shift | Click left stick | **Run** (tap on, tap off) |
| Jump | Space | A | **Jump** |
| Change view | V | Y | **View** |
| Zoom | Mouse wheel | | |
| Use (doors, switches) | E | X | **Use** |
| Descend (when flying) | Left Ctrl | B | **Down** |
| Menu | Escape | Start | The phone's Back button |
| Show colliders | F4 | | |

Walking is relative to the camera, and the player turns to face the way it walks.

### The Input editor: your game's actions and their keys

The table above is Midgard's ready-made set. The **Input** panel (a tab behind the Viewport, or
**Window ▸ Input**) shows it and lets you change it and add your own **actions**: things the
player does, such as *Honk the horn*, *Crouch* or *Open the map*.

- The list on the left has every action: Midgard's own first, then your game's (marked ★).
  **New Action** adds one; **Delete** removes one of yours (Midgard's are kept).
- **Name** is what players see (the Controls screen, "Press E to use"). **Id** is what your
  scripts read: `midgard.pressed("honk")`. An action of your own can have its id changed;
  Midgard's are fixed.
- **Kind**: a **button** is pressed or not (jump); an **axis** goes from -1 to 1 (moving
  forward and back). An axis's keys each push one way: choose **+** or **−** for each (W
  forward +, S back −). Mouse movement and dragging have a number instead: how far it turns.
- **Counts** says when the action works: **Always**, or only **On foot**, **Flying**, **In
  menus** or **In vehicles**. The game is *in menus* while a menu shows, *flying* while the
  player flies, and otherwise *on foot*; a script can put it *in vehicles* (or any of them):
  `midgard.set_input_context("vehicle")` when the player gets into a car, and
  `midgard.set_input_context(None)` when they get out. So one key can do two things: Left Ctrl
  goes down while flying and brakes in a car. Midgard's own actions count always, except
  **Go down (flying)**.
- **Players can change its keys and buttons** lets players rebind it in the game's settings.
- **Add Key…** then press the key (any key, Escape and Space too: only **Cancel** stops it).
  **Add Mouse**, **Add Gamepad** and **Add Touch** list what you can choose. **✕** removes one.
- Red text under it says what's wrong: an action with nothing that does it, or a key two
  actions share when both can count at once (pressing it would do both). Building the game says so too, and warns about a
  script reading an action your game doesn't have.

Every change is one Undo step. Mouse buttons (left, right, middle, back and forward) count only
once the game has the mouse, so the click that takes the mouse doesn't do anything else.

### Touch controls: playing on a phone

On a phone the game shows **on-screen controls** by itself, so no game controller is needed:

- **The stick**, bottom left. Put a thumb anywhere on the left half of the screen: the stick
  comes to it. Push it the way to walk; the further, the faster. Let go and it goes back to
  its resting place, drawn faint. Pull past its rim and it follows your thumb.
- **Looking**: drag a finger anywhere on the right half (not on a button) to turn the camera.
- **Buttons**, bottom right: **Jump**, **Use** (doors, levers, pads), **Run** (a switch: tap it
  to run, it lights up; tap again to walk), **View** (first person, third person, from above),
  while flying **Down**, and **Menu**, which opens the pause menu (when your game has one: see
  Menus and screens).

Each finger does one thing, so you can walk, look and jump at once. A phone held upright gets a
narrower layout, the buttons closer to the edge. Everything stays clear of the notch and the
rounded corners, and is the same size to a finger on every phone. While the controls show, the
HUD's minimap moves to the top right, above the scores.

Connect a game controller (or a keyboard) and press a button: the controls go away. Touch the
screen and they come back.

**Trying them on your computer:** tick **Build ▸ Play with Touch Controls**, then **Play**. The
controls show, and the **mouse is a finger**: hold the left button and drag on the stick to
walk, drag on the right half to look, click the buttons. The mouse isn't captured, so you
don't need Esc. Untick it to play with the mouse and keyboard again (Midgard remembers which
you chose).

**Swimming.** Where the sea is deeper than the player's shoulders, the player swims: it floats
with the water at its shoulders instead of sinking to the sea floor, and swims along the surface
a little slower than it walks. Where the ground comes up under it (a beach, a gentle bank) it
stands up and walks out. At a steep bank, **Jump** lifts it out. Jumping or falling in from a
height, it goes under for a moment and bobs back up.

### The player's stats, scores and counters

Health, stamina, points, keys: the numbers your game keeps about the player. They're in the
**Stats** panel (Window ▸ Stats; a tab beside the viewport). Every project starts with three:
**Health**, **Stamina** and **Score**. Change them, delete them, or add your own with **New**.
Each is one of three kinds:

- **Stat**: a value between its **least** and **most**, like Health 0 to 100. It can **refill**
  by itself, so much a second, once it has gone a while (**After**) without dropping: Health
  might come back 1 a second after 5 seconds without being hurt. A negative refill drains it
  instead (hunger going down).
- **Score**: a whole number of points that never goes below its least.
- **Counter**: a whole number up to a **target** if it has one (Keys, 3 of 5), or as many as
  there are (Coins).

Two more kinds only show on the HUD. They have no value, so scripts can't read or change them:

- **Position**: where the player is. On a planet or inside a hollow world it shows latitude and
  longitude (`12.35° N, 45.60° E`, as on the World Type editor's map). Elsewhere it shows metres
  east and north of the world's middle (`1240 m E, 380 m S`, or kilometres from 10 km).
- **Date and time**: the game's calendar and clock. Tick the parts to show under **Shows**:
  **Season**, **Weekday**, **Day**, **Month**, **Year** and **Time** (the hour and minute).
  **Looks like** gives an example, such as `Spring, 14 March 1204, 14:05`. The names of the
  months, seasons and weekdays are your calendar's (section 13).

To add one, click **New**, choose **Position** or **Date and time** as its **Kind**, and give
it a name (`Location`, `Today`).

**Move Up** and **Move Down** set their order (the HUD shows them in it). **Show on the HUD**
keeps one on the screen while the game plays, or off it (a hidden stat for your scripts). Under the settings,
**In scripts** shows how a script reads the chosen one; your scripts change them with
`midgard.stats` (section 19). When you edit them while the game plays, the game follows at
once, keeping its values. Cook warns about a stat that starts outside its range, two stats
with the same name, and a script that names a stat the project doesn't have.

The game keeps the player's stats in its saved world, so they're as the player left them next
time. **F3** in the game lists them all, hidden ones too.

### The HUD: what the player sees on top of the game

Every game shows a **HUD** over the game by itself:

- **Bars** at the top left: each stat (Health red, Stamina gold, Mana blue, others green), with
  its value, such as `72/100`.
- **Numbers** at the top right: scores (`Score 1250`), counters (`Keys 3/5`), the player's
  position (`Location 12.35° N, 45.60° E`) and the date and time (`Today Spring, 14:05`).
- **Timers** under them, from your scripts (`Escape 1:05`).
- **Notes** under those: short messages from your scripts ("Picked up a key") that fade after a
  few seconds. Five show at most.
- **A prompt** at the bottom ("Press E to open"), from your scripts.
- **A crosshair** in the middle, in first-person view.
- **A compass** at the top: N, E, S and W slide past the gold mark as the camera turns.
- **A minimap** at the bottom right: the ground around the player from above, north at the top,
  coloured like the World Type editor's map (sea, grass, sand, rock, snow). The player is the
  arrow in the middle, pointing the way the camera faces. Blue squares are scene exits (doors to
  other scenes); gold dots are things your scripts mark (a quest's goal), with their labels.
  Scenes without terrain show the marks on a plain background.

**North on every world shape.** On flat ground north is -Z, the way everything faces (section 6). On a planet or inside a hollow world, north points
towards the world's north pole (the +Y pole, the one its latitude counts from), so walking
round a planet the compass never jumps. On a cylinder, north runs along its length.

Nothing on the HUD takes the mouse, so clicks always reach the game. On a phone it keeps clear of
the notch and rounded corners, and held upright the compass goes on top, the bars and numbers
share the width under it, and the minimap is smaller. Scripts
use it through `midgard.ui` (section 19). Your own menus and screens come from the Menu editor
(below).

### Menus and screens: the Menu editor

The **Menu Editor** (Window ▸ Menu Editor; a tab beside the viewport) makes your game's own
screens: a title screen, a pause menu, settings, game over. On the left are your **Screens**
(**New**, **Duplicate**, **Delete**) and the chosen screen's **Elements**. In the middle is the
**canvas**: the screen drawn by the game's own engine, exactly as players will see it. On the
right are the settings of whatever you've chosen.

**Elements.** Click **Add** for a:

- **Panel**: a box that holds other elements. Move it and everything inside goes with it.
- **Text**: words, such as a title.
- **Button**: something to click (or press with a controller, or tap).
- **Slider**: a value to drag, such as the volume.
- **Check box**: something to tick on or off, such as subtitles.
- **Option button**: one choice among several, such as Easy, Normal and Hard. Options with the
  same **Group** are one choice: choosing one unchooses the others.
- **Picture**: an image on its own, such as your game's logo.

With a panel chosen, Add puts the new element inside it. Elements later in the list are drawn
on top: **Up** and **Down** change the order (a panel takes its parts with it).

**Placing things.** Each element hangs from an **anchor** on its panel, or on the screen: a
corner, the middle of a side, or the very middle. **X** and **Y** say how far from the anchor
it sits (right and down are more), and **Width** and **Height** its size, all in **dp**. A dp is
the same size to the eye on every screen, so a button 56 dp tall is easy to press on a phone and
on a big monitor alike. Because elements hang from anchors, a button anchored bottom right stays
in the bottom right corner on every screen shape. **Fill** stretches an element over its panel
(X and Y become its margins).

On the canvas, **click** an element to choose it, **drag** it to move it, and drag the yellow
square at its **corner** to resize it. Each drag is one step to undo. **Preview** shows the
screen at a desktop size, or on a phone held sideways or upright (clear of its notch).

**The look.** Text, buttons, check boxes and option buttons have a **Font**, a **Text size**,
**Align** (left, middle or right), **Bold**, **Italic** and a **Text colour**. The fonts are
Midgard's own (**Inconsolata**, the default, and **OpenDyslexic**) and the TrueType (.ttf) and
OpenType (.otf) fonts in the **Fonts** section of your Asset Library: import a font family's
folder there, with its licence (most free fonts are under the SIL Open Font License), and it's
in the list. A font with bold and italic files uses them for Bold and Italic. The fonts your
screens use go into the finished game, and are credited like any asset. In the **Font** list
each of the library's fonts is written in itself; rest the pointer on one for a bigger sample.

Panels, text and buttons have a **Fill** colour, a
**Border** colour and width, and **Corners** (how round). Every colour can be see-through (its
opacity), and **Default** goes back to Midgard's usual look.

**Pictures.** Screens and their elements can be drawn with pictures. Every list of pictures
holds the PNG and JPEG pictures in the **GUI** section of your Asset Library (**Assets ▸ Asset
Library**: import button frames, panels, ticks and logos there, with their licence). Each picture
is stretched to fill its place, so a frame keeps its look at any size. **None** takes a picture
away. Each picture in a list has a small thumbnail; rest the pointer on one to see it bigger,
with its size in pixels (the Project Page's Crosshair and Menu pointer lists work the same
way). What each has:

| | Pictures |
|---|---|
| The screen (nothing chosen) | **Background picture**, over the whole screen |
| Panel, text, picture | **Image** |
| Button | **Normal**, **Hover** (the mouse over it), **Pressed** (held down) and **Disabled** (it can't be used) |
| Check box | **Unchecked** and **Checked**: the box's picture, ticked or not |
| Option button | **Unselected** and **Selected** |
| Slider | **Background** (the bar along it) and **Pointer** (the handle you drag) |

A state left empty uses the button's normal picture (Pressed uses Hover first), and a check box
or option with only one picture uses it for both. A button with pictures drops its usual fill
and border, unless you give it a Fill or Border of its own.

**Can be used.** Buttons, sliders, check boxes and options have a **Can be used** tick. Untick it
for one the player can't use yet, such as Load when nothing is saved: it shows its disabled
picture (or greyed) and doesn't respond. A script enables it when it should be
(`midgard.ui.set_enabled`). The pictures your screens use go into the finished game, and
their makers are credited in its `CREDITS.txt` like any other asset.

**What a button does** (**Does**): nothing, **Close this screen**, **Open a screen…** (another
one, on top: closing that one comes back to this, as Settings goes back to Pause), **Quit the
game**, or **Tell scripts only**. Your scripts hear every click whatever it does.

**The screen itself** (with no element chosen) has a **Name**, and two switches:

- **Takes the player's input**: a menu. While it shows, the game's controls wait and the mouse
  is free. Untick it for something to play under, like a score panel.
- **Pauses the game**: everything stands still while it shows (a pause menu), including what
  scripts move: their `on_update` isn't called until it closes. Scripts still hear its buttons.

It also has a **Background** colour over the whole game behind it (a see-through dark one dims
the game).

**Ready-made screens.** **New** offers a **Blank menu**, or four screens that work as they
are, with no scripting:

- **Splash screen**: shows while the game loads, before anything else: your game's name and
  "Loading…" over a dark background, to dress up with your own words, fonts and pictures. With
  the screen chosen (no element), **Shows at least** says how many seconds it stays up however
  quickly the game loads (0: only while it loads), so a quick load doesn't flash it past. (Its
  buttons don't work: nothing has loaded to answer them.) Without a splash screen, the game's
  name shows while it loads.
- **Title screen**: shows when the game starts, the game paused behind it. **Play** closes it,
  **Settings** opens the settings, **Quit** ends the game.
- **Pause menu**: **Esc** (or a controller's **Start**, or a phone's **Back**) opens it while
  playing, the game paused. **Resume** (or Esc again) goes back to the game.
- **Settings**: a **Volume** slider (everything) and a **Music** slider, and buttons for the
  **Jump** and **Use** keys. Click a key button, then press the key you want: it shows
  `Press a key` until you do (Esc gives up). The player's volumes and keys are kept in their
  profile, so they're as they left them next time. **Back** (or Esc) goes back to the menu it
  came from.
- **Game over**: shows when the player's **Health** runs out. **Try again** respawns the player
  where they arrived in the scene, health and the other stats refilled. **Quit** ends the game.

A title screen or pause menu made when the project has no settings screen brings one with it.
Change any of them as you like: colours, pictures, words, more buttons.

**What the game uses a screen for** (**Used as**, with no element chosen): only when shown (by a
script or a button), the **title screen**, the **pause menu**, **settings**, or **game over**
(and **When it runs out**: the stat that, reaching its least, means the player died). Each is
used by one screen; Cook says if two claim the same. Without a pause menu, Esc works as before
(it quits a finished game; under the editor it frees the mouse).

**Settings the game keeps.** A slider's **Setting** can be a volume (everything, music, effects,
voices, ambience or menu sounds), and a button's the key for an action (Jump, Use, Run, View,
Descend, Menu): the slider shows and changes the player's volume, the button shows the key and,
clicked, waits for a new one. **Respawn the player** is one more thing a button **Does**.

**The game's own screen settings** are on the **Project Page** (Window ▸ Project Page), under
**Game screens**, because they're the whole game's:

- **Crosshair**: the aiming mark's picture, in the middle of the screen (without one, a small
  white dot), its **Crosshair size** in dp, and **Crosshair shows**: **In first person** (as
  it starts), **In every view**, or **Never**. Scripts can still show or hide it
  (`midgard.ui.crosshair`).
- **Menu pointer**: the mouse pointer over menus, a picture of its own (its tip is the picture's
  top left corner; 32 x 32 pixels is a good size). Without one, the system's arrow. The crosshair
  and the menu pointer are separate pictures.
- **Menu music**: a playlist (from the Audio panel) that plays while any menu shows, the same for
  all of them, fading in over the scene's music and back out when the menu closes. A screen
  whose **Plays the menu music** tick is off (in the Menu editor, with nothing chosen) keeps the
  scene's music instead. The menu music's playlist only ever plays for menus: the world never
  chooses it by itself.

All their pictures come from the **GUI** section of the Asset Library, and the cook packs them.

**Showing a screen.** A screen shows when a script asks: `midgard.ui.show("scr_pause")` (its
id is under its name). While the game plays under the editor, **Show in Game** shows the chosen
screen there, and every change you make reaches it at once. Problems (an element too small for a
finger, a button a controller can't reach, text too long for its box, a button opening a screen
that's gone) are listed under the canvas, and Cook warns about them too.

A recipe: a title screen when the game starts, its Play button closing it (set Play's **Does**
to **Close this screen**), and a note when it does.

```python
import midgard

def on_start(entity):
    midgard.ui.show("scr_title")
    midgard.ui.on_click("scr_title", "play", lambda: midgard.ui.notify("Off you go!"))
```

---

## 16. Characters and animation

A **character** is an animated model with a skeleton (a "rigged" model). To give an entity a
body, select it, **Add Component ▸ Character**, then:

- **Character** (the model): choose a rigged model with **Library…**.
- **Animations from**: a file of animations, if they're not in the model itself. Midgard can
  share animations between different humanoid characters: it adjusts them to each one's
  proportions automatically.
- **Playing**: which animation plays, and its **Speed**.
- **Rig…**: if Midgard can't recognise the skeleton's bone names, match them up here.
- **IK** (inverse kinematics: limbs that adapt to the world): **Feet on the ground** plants
  feet on slopes and steps; **Looks at** turns the head to the player or another entity.
- **Arms in first person**: shows the player's arms when looking through its eyes.

**Add Sample Assets** (Assets menu) gives you free (CC0) things to practise with: a character
and a big animation library, a small textured model, footsteps on five kinds of ground, eight
ambience loops (birds, night, sea, wind, rain, a station's hum, an eerie drone) and a calm tune.

### The animation graph

A single looping animation is fine for a statue. A player needs to *switch*: idle when
standing, walk when moving, run when fast, jump when jumping. That's what an **animation graph**
does. Click **Animation Graph…** in the Character section to open the **Animation** panel.

- **States** are boxes, each playing an animation (or a blend of several by speed, so it eases
  from walk to run smoothly).
- **Transitions** are arrows between states, each with a condition, such as "speed > 0.2".
- **Parameters** are the values conditions test. Some are filled in automatically: **speed**
  (how fast the character moves), **vertical_speed** (rising or falling) and, for the player,
  **grounded** (1 standing on the ground, 0 in the air) and **swimming** (1 in water deep enough
  to swim). Use them for a jump that leaves the ground and a swim in the sea ("swimming > 0.5").
  Scripts can set others ("health").
- **Triggers** fire once (a "jump" or "attack").
- **Layers** play on top for part of the body (waving while walking).
- **Events** mark moments in an animation ("footstep") that scripts hear.

The panel's preview plays it all live.

---

## 17. More than one scene: doors and portals

The **Scenes** panel lists every scene; one is where the game starts. To join two scenes:

1. In the scene you arrive in, **Add ▸ Arrival Point**: where the player appears, facing its
   -Z.
2. In the scene you leave from, **Add ▸ Scene Exit**: a box. In the Inspector choose where it
   **Leads to** (the scene), **Arriving at** (the arrival point) and how it's **Taken by**:
   walking into it, pressing **Use** (E) inside it, or only when a script says so (a locked
   door).

The picture fades out and in during the change. It stays dark until the ground where you
arrive is all there (a big world can take a moment), so you never see half-made ground or
fall through it; the game keeps running meanwhile.

**The player goes with you.** The player is the one from the scene the game starts in: their
character, speed, footsteps and scripts go into every scene they visit, standing where that
scene's own player stands (or at the arrival point). A scene doesn't need a player of its own,
though giving it one is still how you choose where they appear when the game starts there.
How wet they are, the weather a script has forced, and the time all carry on too.

**Scenes remember.** Leave a scene and come back, and it is as you left it: the crates you
knocked over still lie where they fell, and what a script moved or took away stays moved or
gone. A saved game remembers this too. (Things a script *made* aren't remembered yet.)

### Space above a planet

Above every planet there is **system space**, which Midgard makes for you: the planet hangs
below, its real ground seen from far off (the same seas, hills, snow and ice), the sky is black
and full of stars (placed by the world's seed), and the sun is where it is on the planet. The
player flies a small ship there: **W A S D** go where the camera looks, **Space** climbs and
**Left Ctrl** sinks, and the higher you are the faster you go (hold **Shift** for more). There
is no gravity and no weather in space.

**Getting there.** A planet's atmosphere has a **ceiling**: fly higher than it (with a jetpack
or a ship: a script makes the player fly) and you're in space, carrying on from where you
crossed it. It's a fifth of the planet's radius up, and at least 300 m, unless you set it. Fly
back down through it and you come down below where you descended: on the ground there, or still
flying just under the ceiling if your player flies.

A **habitat** or a **hollow world** has no sky to fly up through. Its ways out are **docks** and
**exit sites**: Scene Exits leading to space. Out there you see your world from outside, a huge
cylinder or ball against the stars, with your ship just outside where you left. Fly back to its
hull and you go back in, arriving at the exit's arrival point. To make one, add a **Scene
Exit**, choose **Space (outside this world)** for where it **Leads to**, and choose where the
player is **Coming back in at** (an arrival point in the same scene).

A planet's ceiling is set in **World ▸ World Shape…**: **Atmosphere ceiling** (leave it at
*Automatic* for a fifth of the radius). To get the player off the ground, a script sets
`player.flying = True` (a jetpack, a launch pad, a ship); `False` walks again.

A whole planet seen from space is a lot of ground to draw at once; the phone's self-test has a
**Space** check to show it stays smooth.

---

## 18. Scripting for beginners

A **script** is a small text file of instructions in **Python**, a programming language
designed to be readable. You don't need to know Python to start: copy the examples below and
change the numbers.

### Adding a script

1. Select an entity. **Add Component ▸ Script**. Midgard creates a new script file named after
   the entity and opens it in the **Script Editor**.
2. Write what should happen (see below). **Ctrl+S** saves.
3. Press **F5**. While the game runs, save again and the change takes effect immediately.

### How a script works

A script is a list of **functions**: named blocks of instructions that Midgard calls when
something happens. You only write the ones you need.

```python
import midgard

def on_start(entity):
    # Runs once, when the game starts.
    midgard.log("Hello from", entity.name)

def on_update(entity, dt):
    # Runs every frame (about 60 times a second).
    # dt is how many seconds passed since the last frame.
    pass
```

- `entity` is the thing the script is attached to. `entity.position`, `entity.rotation`,
  `entity.color` and so on read or change it.
- Lines starting with `#` are notes for people; Midgard ignores them.
- Indentation (the spaces at the start of lines) matters in Python: keep the lines inside a
  function indented by 4 spaces.
- `midgard.log(...)` writes a message to the **Log** panel: the easiest way to see what's
  going on.

### When things go wrong

If a script has a mistake, it pauses and the **Log** shows the file, the line number and what
went wrong. Fix it and save; it starts again.

### Recipes

**A spinning coin**

```python
def on_update(entity, dt):
    x, y, z = entity.rotation
    entity.rotation = (x, y + 90 * dt, z)   # 90 degrees a second
```

**A floating platform that bobs up and down**

```python
import math
import midgard

home = {}   # where each platform started (a script remembers things in its own names)

def on_start(entity):
    home[entity.id] = entity.position

def on_update(entity, dt):
    x, y, z = home[entity.id]
    entity.position = (x, y + math.sin(midgard.time()) * 0.5, z)
```

*Remembering things:* a script keeps its own values in names at the top of the file (like
`home` above). Keep them per entity, by `entity.id`, if the same script is on several things.

**A door that slides open when the player comes near** (give the door a **Trigger**
component sized to the area in front of it):

```python
def on_trigger_enter(entity, other):
    entity.move(0, 3, 0)      # up 3 metres

def on_trigger_exit(entity, other):
    entity.move(0, -3, 0)     # back down
```

**A pressure plate that turns a lamp red**

```python
import midgard

def on_trigger_enter(entity, other):
    lamp = midgard.find("Warning Lamp")
    if lamp:
        lamp.color = (1, 0, 0)
```

**Do something when a key is pressed** (actions are named in the input settings: "use" is E):

```python
import midgard

def on_update(entity, dt):
    if midgard.pressed("use"):
        midgard.log("You pressed Use!")
```

**A ball you can kick** (the ball needs a Collider, a dynamic Rigid body, and a Trigger a
little bigger than the ball, so it notices the player touching it):

```python
import midgard

def on_trigger_enter(entity, other):
    player = midgard.player()
    px, py, pz = player.position
    bx, by, bz = entity.position
    entity.add_impulse((bx - px) * 5, 3, (bz - pz) * 5)
```

**Make a character wave when the player comes close** (its animation graph needs a trigger
called "wave"):

```python
def on_trigger_enter(entity, other):
    entity.trigger_animation("wave")
    entity.look_at(other)            # turn its head to the player

def on_trigger_exit(entity, other):
    entity.look_at(None)
```

**A guard that walks to the player** (uses the navigation mesh):

```python
import midgard

def on_update(entity, dt):
    player = midgard.player()
    path = midgard.nav.path(entity.position, player.position)
    if path and len(path) > 1:
        x, y, z = entity.position
        tx, ty, tz = path[1]
        dx, dz = tx - x, tz - z
        length = (dx * dx + dz * dz) ** 0.5
        if length > 0.1:
            speed = 2.0 * dt
            entity.move(dx / length * speed, 0, dz / length * speed)
```

**Night falls**

```python
import midgard

def on_start(entity):
    midgard.time.on("dusk", lambda: midgard.log("Night falls..."))
    midgard.time.on("dawn", lambda: midgard.log("A new day."))
```

**Teleport to another scene**

```python
import midgard

def on_trigger_enter(entity, other):
    midgard.rooms.change("scn_cave", fade=1.0)
```

**Ring a bell when the player walks in** (make a sound event called "Bell" first, with
**Library…** on any Sound component, or by giving something a sound):

```python
import midgard

def on_trigger_enter(entity, other):
    entity.play_sound("Bell")
```

**Turn the music down in a quiet room**

```python
import midgard

def on_trigger_enter(entity, other):
    midgard.audio.set_volume("music", 0.3)

def on_trigger_exit(entity, other):
    midgard.audio.set_volume("music", 1.0)
```

**A fanfare when a secret is found** (over the music, which dips under it):

```python
import midgard

def on_trigger_enter(entity, other):
    midgard.music.stinger("Fanfare")
```

**Boss music while the boss is near**

```python
import midgard

def on_trigger_enter(entity, other):
    midgard.music.play("Boss fight")

def on_trigger_exit(entity, other):
    midgard.music.release()
```

**Shake the camera when something lands**

```python
import midgard

def on_trigger_enter(entity, other):
    midgard.camera.shake(strength=0.3, seconds=0.6)
```

### Good habits

- Name things clearly in the Scene list: scripts find them by name.
- Use `midgard.log()` freely to check values.
- Change one thing at a time, then press Play.
- The **Assistant** panel can explain errors and suggest scripts.

---

## 19. Scripting reference: everything a script can do

Positions are in metres as `(x, y, z)`; Y is up. Angles are in degrees. Colours are
`(red, green, blue)` or `(red, green, blue, alpha)` from 0 to 1.

### Functions Midgard calls in your script

| Function | When it's called |
|---|---|
| `on_start(entity)` | Once, when the script starts. |
| `on_update(entity, dt)` | Every frame; `dt` is the seconds since the last one. Not while a screen that pauses the game shows. |
| `on_trigger_enter(entity, other)` | The player walked into this entity's trigger. |
| `on_trigger_exit(entity, other)` | The player left it. |
| `on_reload(entity)` | You saved the script while the game was running. |
| `on_room_enter(entity, came_from)` | The player arrived in this scene (from scene `came_from`). |
| `on_room_exit(entity, going_to)` | The player is leaving this scene. |
| `on_animation_event(entity, name)` | Its character's animation passed an event marker. |

The player's own script also gets `on_trigger_enter(player, trigger)` and `on_trigger_exit`.

### An entity

| What | Meaning |
|---|---|
| `entity.id` | Its permanent id, such as "e000004". |
| `entity.name` | Its name. |
| `entity.exists` | False once it has been removed. |
| `entity.position` | Where it is (relative to its parent). Assign to move it. |
| `entity.rotation` | How it's turned (pitch, yaw, roll). |
| `entity.scale` | Its size on each axis. |
| `entity.color` | Its material colour. |
| `entity.velocity` | Its speed and direction (physics bodies). Assign to launch a dynamic body. |
| `entity.on_ground` | True while the player stands on something. |
| `entity.swimming` | True while the player swims in the sea (floating at its surface). |
| `entity.flying` | The player flies (where the camera looks; up with jump, down with Left Ctrl). `False` walks again. Only the player. |
| `entity.speed` | The player's walking speed in metres a second (running multiplies it). Only the player. |
| `entity.follow(points)` | The player walks through these points at its speed, as if steered (a guided tour, a cutscene, click-to-move); `[]` stops it. Only the player. |
| `entity.following` | True while the player is walking a route from `follow`. |
| `entity.move(x, y, z)` | Moves it by that much. |
| `entity.add_force(x, y, z)` | Pushes a dynamic body (call every frame for a steady push). |
| `entity.add_impulse(x, y, z)` | A sudden shove (a kick, an explosion). |
| `entity.set_animation(name, value)` | Sets an animation graph parameter. |
| `entity.trigger_animation(name)` | Fires an animation graph trigger. |
| `entity.animation(name)` | Reads a parameter ("speed", "vertical_speed", "grounded" and "swimming" are always there). |
| `entity.animation_state(layer=0)` | The animation state it's in. |
| `entity.look_at(target)` | Turns its head to a point or an entity; `None` stops. |
| `entity.reach(hand, target, weight=1)` | Reaches "left" or "right" hand to a point or entity; `None` lets go. |

### Finding things

| What | Meaning |
|---|---|
| `midgard.find("Name")` | The first entity with that name, or None. |
| `midgard.entity("e000004")` | The entity with that id, or None. |
| `midgard.entities()` | Every entity in the scene. |
| `midgard.player()` | The player's entity, or None. |

### Input

| What | Meaning |
|---|---|
| `midgard.action("move_y")` | An action's value this frame, -1 to 1. |
| `midgard.down("run")` | True while it's held. |
| `midgard.pressed("jump")` | True only in the frame it's pressed. |
| `midgard.input_context()` | When actions count now: `"on_foot"`, `"flying"`, `"menu"` or `"vehicle"`. |
| `midgard.set_input_context("vehicle")` | Chooses it (in a car, say); `None` gives the game its own back. Actions of other contexts read nothing. |

Built-in actions: `move_x`, `move_y`, `look_x`, `look_y`, `zoom`, `jump`, `run`, `view`, `use`,
`menu`, `descend`; and your game's own, made in the Input editor (by their ids).

### Physics questions

| What | Meaning |
|---|---|
| `midgard.raycast(origin, direction, distance=1000, ignore=None, triggers=False)` | Shoots an invisible line; returns a **Hit** (`.entity`, `.point`, `.normal`, `.distance`) or None. |
| `midgard.overlap_sphere(centre, radius)` | Every solid thing inside a ball. |
| `midgard.overlap_box(centre, size, rotation=(0, 0, 0))` | Every solid thing inside a box. |
| `midgard.gravity(position)` | Which way and how hard gravity pulls there. |
| `midgard.up(position)` | Which way is up there (on a planet it changes as you walk). |

### The camera: `midgard.camera`

| What | Meaning |
|---|---|
| `.view` | "first", "third" or "top". Assign to change it. |
| `.set_view(view, blend=True)` | Changes the view, smoothly or at once. |
| `.shake(strength=0.2, seconds=0.5)` | Shakes the camera. |
| `.look(position, target, seconds=3)` | Takes the camera for a moment (a cutscene close-up). |
| `.release()` | Gives the camera back early. |
| `.position`, `.forward`, `.up`, `.fov` | Where it is and where it looks (read only). |
| `.active`, `.blending`, `.scripted` | Its state (read only). |

### Time: `midgard.time`

| What | Meaning |
|---|---|
| `midgard.time()` | Real seconds since the game started. |
| `.seconds` | The game clock in game seconds. |
| `.now()` | The date and time: `.year`, `.month`, `.day`, `.hour`, `.minute`, `.weekday`, `.month_name`, `.season`… |
| `.season()` | The season's name. |
| `.solar_phase()` | 0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight. |
| `.is_day()` | True between sunrise and sunset. |
| `.set_scale(n)` | Game seconds per real second (0 stops time). |
| `.advance(days=, hours=, minutes=, seconds=)` | Skips ahead (sleeping). |
| `.on("dawn", function)` | Calls your function at "hour", "day", "month", "season", "year", "dawn" or "dusk". |

### Scenes: `midgard.rooms`

| What | Meaning |
|---|---|
| `.current`, `.previous` | The scene ids now and before. |
| `.change(scene, arrival="", fade=0.5)` | Goes to another scene. |
| `.take_exit(exit_entity)` | Uses a Scene Exit (for doors that only open by script). |
| `.changing` | True during the fade. |
| `.enter_space(fade=1.0)` | Into space: above the planet the player is on, or outside the habitat or hollow world. |
| `.in_space`, `.space_from` | Whether the player is in space, and which scene they left for it. |
| `.ceiling`, `.altitude` | A planet's atmosphere ceiling, and how high the player is (metres above sea level). |

```python
import midgard

def on_trigger_enter(entity, other):   # a launch pad
    player = midgard.player()
    player.flying = True               # off the ground: fly up past the ceiling into space
    print("Lift off! Space begins at", round(midgard.rooms.ceiling), "m")
```

### Navigation: `midgard.nav`

| What | Meaning |
|---|---|
| `.path(start, end, partial=False)` | A list of points to walk along, or None. |
| `.nearest(point, reach=2)` | The nearest walkable spot. |
| `.ready` | True once the walkable area round the player is worked out. |

### Sound: `midgard.audio`

| What | Meaning |
|---|---|
| `midgard.audio.play("Bell", volume=1, at=None)` | Plays a sound event by name; gives a **Sound** (or None). `at` is an entity (the sound follows it) or a place `(x, y, z)`; without it, heard the same everywhere. |
| `entity.play_sound("Bell", volume=1)` | The same, from that entity, following it as it moves. |
| `sound.stop()`, `sound.playing`, `sound.set_volume(v)` | Stop it, check it, change its volume. |
| `midgard.audio.stop_all()` | Stops every sound. |
| `midgard.audio.set_volume(bus, v)` / `.volume(bus)` | A bus's volume: "master", "music", "effects", "voice", "ambience", "interface". |
| `midgard.audio.playing` | The sound events playing now. |
| `midgard.audio.enabled` | False when the game has no sound (a server). |

### Music: `midgard.music`

| What | Meaning |
|---|---|
| `midgard.music.play("Boss fight")` | Plays that playlist (crossfading), whatever fits where the player is. |
| `midgard.music.release()` | Back to the playlist that fits where the player is. |
| `midgard.music.stop()` | Fades the music out until `play()` or `release()`. |
| `midgard.music.stinger("Fanfare", duck=0.35)` | Plays a short piece over the music, which dips under it. |
| `midgard.music.playlist`, `midgard.music.track` | What's playing now. |

### Ambience: `midgard.ambience`

| What | Meaning |
|---|---|
| `midgard.ambience.play("Cave")` | Plays that ambience set (crossfading), whatever fits where the player is. |
| `midgard.ambience.release()` | Back to the set that fits where the player is. |
| `midgard.ambience.stop()` | Fades the ambience out until `play()` or `release()`. |
| `midgard.ambience.playing`, `midgard.ambience.layers` | What's playing now. |

### Weather: `midgard.weather`

| What | Meaning |
|---|---|
| `midgard.weather.current()` | The weather where the player is now. `.id` (such as `"rain"`), `.name`, and what it does: `.wind`, `.temperature`, `.wetness`, `.visibility`, `.falls` (`"rain"`, `"snow"`...), `.clouds`, `.lightning`. `.forced` is `True` while a script forces it. |
| `midgard.weather.forecast(hours=24)` | What's coming: a list of spells, each with `.id`, `.in_hours` (how soon), `.hours` (how long). |
| `midgard.weather.on_change(changed)` | Calls your function `changed(before, now)` whenever the weather where the player is changes. |
| `midgard.weather.force("thunderstorm", hours=2)` | That weather everywhere, for two hours, then the weather comes back. Leave out `hours` to keep it until `release()`. |
| `midgard.weather.release()` | Back to the weather there would have been. |
| `midgard.weather.types` | The ids of the weather types. |
| `midgard.weather.wetness`, `midgard.weather.snow` | How wet the ground is, and how much snow has settled, where the player is (0 to 1). You can set them too: `midgard.weather.snow = 1` for a sudden winter. |
| `midgard.weather.temperature` | How warm it is where the player is, in °C (the climate's, with the season and the weather). |
| `midgard.weather.exposure()` | How the weather treats the player: `.feels_like` and `.air` (°C), `.wind` reaching them, `.heat` from fires, `.wetness` (0 dry to 1 soaked), `.sheltered` (under a roof), `.roof` and `.walls` (0 to 1). |
| `midgard.weather.set_player_wetness(0)` | The player is now this wet (dried by the fire, or fell in the river: `1`). |

```python
import midgard

def on_update(entity, dt):
    e = midgard.weather.exposure()
    if e.feels_like < 0 and not e.sheltered:
        print("Brr! Find shelter or a fire.")
```

```python
import midgard

def on_start(entity):
    midgard.weather.on_change(changed)
    for spell in midgard.weather.forecast(12):
        print(spell.name, "in", round(spell.in_hours), "hours, for", round(spell.hours), "hours")

def changed(before, now):
    if now == "rain":
        print("It's starting to rain. Find shelter!")
```

### Stats: `midgard.stats`

Each stat by its id or its name, as the Stats panel has them (`"health"` or `"Health"`).

| What | Meaning |
|---|---|
| `midgard.stats.get("health")` or `midgard.stats["health"]` | Its value now. Scores and counters are whole numbers. |
| `midgard.stats.set("health", 50)` or `midgard.stats["health"] = 50` | Sets it, kept within its least and most. |
| `midgard.stats.add("score", 10)` | Adds to it (a negative number takes away) and gives the new value. |
| `midgard.stats.max("health")`, `midgard.stats.min("health")` | Its most and least (a score has no most: `None`). |
| `midgard.stats.set_max("health", 150)` | A new most: more health as the player grows stronger, or a counter's new target. |
| `midgard.stats.reset("health")` | Back to where it starts. `midgard.stats.reset()` resets them all. |
| `midgard.stats.on_change(changed)` | Calls your function `changed(stat, before, now)` whenever one changes, refilling included. |
| `midgard.stats.ids` | The stats' ids, in the panel's order. `"mana" in midgard.stats` asks if there is one. |

```python
import midgard

def on_start(entity):
    midgard.stats.on_change(changed)

def on_trigger_enter(entity, other):
    midgard.stats.add("health", -25)   # the spikes hurt
    midgard.stats.add("score", 5)      # but there's a coin

def changed(stat, before, now):
    if stat == "health" and now == 0:
        print("You have fallen.")
```

### The HUD: `midgard.ui`

The stats show on the HUD by themselves; these are the rest.

| What | Meaning |
|---|---|
| `midgard.ui.notify("Picked up a key")` | A note on the HUD for 4 seconds (or `seconds=10`), fading as it goes. |
| `midgard.ui.prompt("Press E to open")` | A prompt at the bottom of the screen, until another. `midgard.ui.prompt()` takes it away. |
| `midgard.ui.timer("bomb", 30, label="Bomb", on_done=boom)` | A timer counting down from 30 seconds; your function `boom()` is called at 0. With `count="up"` it counts up instead (to the seconds given, or for ever with 0). The same id again starts it over. |
| `midgard.ui.stop_timer("bomb")` | Takes a timer away (its `on_done` isn't called). |
| `midgard.ui.timer_value("bomb")` | Its seconds left (or passed, counting up); `None` if there's none. |
| `midgard.ui.hud = False` | Hides the whole HUD (a cut scene); `True` shows it again. |
| `midgard.ui.crosshair = True` | Always shows the crosshair; `False` never; `None` in first person only (as it starts). |
| `midgard.ui.compass = False`, `midgard.ui.minimap = False` | Hide the compass or the minimap (`True` shows them again). |
| `midgard.ui.mark(entity, "Shrine")` | Marks an entity on the minimap with a gold dot (and a label, if given). `midgard.ui.unmark(entity)` takes it off. |
| `midgard.ui.heading` | The way the camera faces, in degrees: 0 north, 90 east, 180 south, 270 west. |
| `midgard.ui.show("scr_pause")`, `midgard.ui.hide("scr_pause")` | Shows a screen made in the Menu editor (on top of any others), or takes it away. `midgard.ui.shown("scr_pause")` says whether it shows; `midgard.ui.screens` lists those showing, bottom to top. |
| `midgard.ui.on_click("scr_pause", "resume", resumed)` | Calls `resumed()` when the player clicks that button. `midgard.ui.click("scr_pause", "resume")` presses it as the player would. |
| `midgard.ui.on_change("scr_settings", "volume", set_volume)` | Calls `set_volume(value)` when the player moves that slider (or `(True or False)` when they tick a check box). |
| `midgard.ui.value("scr_settings", "volume")`, `midgard.ui.set_value(...)` | A slider's value or whether a check box is ticked; or sets it. |
| `midgard.ui.set_text("scr_over", "score", "Score: 1250")` | Changes an element's words. |
| `midgard.ui.set_enabled("scr_title", "load", True)` | Lets the player use a button, slider, check box or option (False: not; it shows its disabled picture). `midgard.ui.enabled(...)` says which. |
| `midgard.ui.value("scr_settings", "hard")` | For an option button: whether it's the one chosen. `set_value(..., True)` chooses it (and unchooses the rest of its group). |

```python
import midgard

def on_trigger_enter(entity, other):
    midgard.ui.prompt("Press E to open")

def on_trigger_exit(entity, other):
    midgard.ui.prompt()

def on_start(entity):
    midgard.ui.timer("escape", 120, label="Escape", on_done=too_late)

def too_late():
    midgard.ui.notify("The doors have closed!")
```

### Other

| What | Meaning |
|---|---|
| `midgard.log(...)` | Writes to the Log. `print()` does too. |
| `midgard.warn(...)` | Writes a warning. |
| `midgard.sky()` / `midgard.set_sky(r, g, b)` | Reads or changes the sky colour. |

Scripts can use Python's standard library (`math`, `random` and so on).

---

## 20. Playing, building and exporting your game

- **Play** (F5): starts your game from the beginning in a window beside the editor. Changes you
  make in the editor while it runs (moving things, colours, scripts) appear in the game at once.
- **Continue** (Ctrl+F5): starts where you last left off.
- **Pause** (F6) and **Stop** (Shift+F5).
- **Play with Touch Controls**: when ticked, Play shows a phone's on-screen controls, with
  the mouse as a finger (section 15).
- **Cook** (Ctrl+B): just packs the game, to check for problems; the Log lists any.
- **Export…** (Ctrl+E): makes a finished game for **Windows**, **Linux** or **Android** in a
  folder you choose, ready to share. On an Android phone the game shows its on-screen touch
  controls (section 15), or plays with a game controller (Bluetooth or USB). The phone's Back
  button ends the game.

The first Play after adding a big model takes longer while its textures are converted; later
Plays are quick.

### Menus, screens and the HUD

Every game shows a HUD (section 15), and the screens you make in the Menu editor (section 15,
"Menus and screens"). Screens work with the mouse, the keyboard, a game controller (the d-pad
or stick moves between buttons, A presses) and touch, grow with the player's screen and UI scale,
and keep clear of a phone's notch. Every game carries two fonts for them, **Inconsolata** (the
default) and **OpenDyslexic** (easier for some players to read), both under the SIL Open Font
License; their licence goes in the game's `THIRD_PARTY_NOTICES.txt` for you. Ready-made screens
(a title screen, pause, settings, game over) come in the next step.

---

## 21. Saving, Git, backups and the error log

- **Saving**: Midgard saves every change as you make it. There's no Save button to forget.
- **Backups**: copies of your project are made every few minutes (set in **Edit ▸
  Preferences**), keeping the last several.
- **Git** (optional, for the careful): **File ▸ Put Project Under Git…** keeps a full history
  you can go back to. **File ▸ Commit…** (Ctrl+K) records a checkpoint with a note.
- **Preferences** (**Edit ▸ Preferences…**): your projects folder, the theme, opening the last
  project on start, backups, the Assistant, and the error log.

### The error log

Midgard keeps a file called `error.log` in your project folder. It records the last 100 things
you did and any errors, so if something goes wrong you can see what led up to it. If the editor
ever freezes for 10 seconds, it writes down exactly where it was stuck. If it crashes, the
details are saved too, and the previous session's log is kept as `error.previous.log`. Typing
is never recorded, only counted. You can turn it off in Preferences, or for one project on its
Project Page.

---

## 22. Troubleshooting: when something goes wrong

**The editor takes a long time the first time I open a project or press Play.**
Big model textures are being converted. It happens once per model; afterwards it's quick.
The window stays usable meanwhile.

**I started Midgard twice.**
Only one editor runs at a time. Starting it again brings the running one to the front (and
opens the project you asked for). If the running one is busy, you'll see a message; wait a
moment.

**My character runs backwards.**
Characters face their entity's -Z and the player turns to face its walk. Update to the latest
Midgard (this was fixed). For plain models that face the wrong way, set Rotation Y to 180.

**A character shows "can't load" or nothing at all.**
Its model may have been moved or removed from the library. Choose it again with **Library…**
in the Character section.

**A texture looks wrong: too shiny, too dark, or plastic-like.**
Check each picture's role in the texture set (a roughness map in the colour slot looks grey and
flat). Re-import and choose the roles again, or fix the slots in the Material editor.

**I can't see a lamp in the Viewport.**
Turn on **Lights** in the Viewport toolbar. Select the lamp in the Scene list to see how far it
reaches.

**My lamps go dark when I walk away.**
The game uses the 8 lamps nearest the camera. Use fewer, brighter lamps with a longer Reach.

**My script does nothing.**
Look in the **Log** for an error. Check the function name is spelled exactly (`on_update`, not
`onUpdate`) and that its lines are indented.

**Something fell through the floor.**
The floor needs a **Collider**. Turn on **Colliders** in the Viewport to check.

**The editor froze or crashed.**
Open `error.log` (or `error.previous.log`) in your project folder; it shows what happened. Send
it along when asking for help.

---

## 23. Keyboard shortcuts

| Keys | Does |
|---|---|
| Ctrl+N | New project |
| Ctrl+O | Open project |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+S | Save the script (Script Editor) |
| Ctrl+B | Cook |
| F5 | Play from the start |
| Ctrl+F5 | Continue where you left off |
| F6 | Pause |
| Shift+F5 | Stop |
| Ctrl+E | Export |
| Ctrl+K | Commit (Git) |
| Ctrl+Shift+L | Asset Library |
| Delete | Delete the selected entity (Scene list) |
| F1 | This help |
