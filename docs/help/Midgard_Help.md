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
- **Help**: this guide, and **About Midgard**.

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
Rigid body, Physics material, Scene exit, Arrival point, Character, Sound. Each component has an **✕**
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

1. Choose files, a folder or a .zip file.
2. Choose the type (Textures, Models, Music…), a category, and a name.
3. Choose the **licence** and fill in who made it (see below).
4. For textures, a table shows every picture: choose what each one is for (colour, normal,
   roughness, metal, ambient occlusion, glow) or "not used". Midgard renames them for you.

Midgard copies the files in. It never changes your originals. Files it can't use (say, a
Photoshop file inside a zip) are skipped and listed.

### Licences and credits

Only use assets you're allowed to. Midgard accepts two kinds:

- **CC0**: free for anything, no credit needed.
- **CC-BY**: free, but you must credit the author. Midgard does this for you: the **Project
  Page** shows the credits your game needs, and every exported game includes a `CREDITS.txt`.

### Looking at assets

Select an asset to see its details and a preview:

- **Models** turn slowly; if they're animated, choose a clip and press Play.
- **Sounds** are listed file by file with their sizes; click one to play it.
- **Textures** are shown as a material: choose **Cube, Sphere or Plane**, and **1:1, 1:2 or
  1:4** to see how the texture repeats. **Drag** to turn it, **wheel** to zoom, **double-click**
  to reset. A lamp beside the camera shows how shiny it is.

The first time a big texture set is shown, Midgard prepares it (it says "Preparing the
textures…"); you can carry on working meanwhile. After that it shows at once.

### Using assets

- In the **Inspector**, rows that take a model or picture have a **Library…** button.
- A character's **Model** and **Animations from** come from the library.
- In the Material editor, **Library…** puts a whole texture set on a material in one step.

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

### About repeating (tiling)

The **1:1, 1:2, 1:4** choice in the library's texture preview is for looking only. In the game,
how often a texture repeats depends on the shape's own texture layout. The built-in Cube, Plane
and other shapes show the texture once per metre of face, so a bigger cube shows more repeats.

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
- **Paint**: **Lush grass**, **Dry grass**, **Sand**, **Rock**, or **Clear paint**.

Each stroke is one Undo step.

### Ground textures

The ground's look comes from Midgard itself: grass from dry to lush, sand on beaches, rock on
steep slopes, and snow on high ground or in cold climates. It changes with the seasons. You
control it through the World Type's **Climate and Biomes** (colours for each zone) and the paint
brushes. *Putting your own picture textures onto terrain isn't available yet.* For a special
patch (a stone courtyard), place a textured **Plane** or flattened **Cube** on the ground.

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

### In the finished game

When the game is built, Midgard makes every sound it uses smaller: music and ambience become
**Ogg Vorbis**, effects become **Opus**. Each is checked to be sure the game can play it. CC-BY
sounds are credited automatically, like models and textures.

*Coming in the next updates:* sounds heard from where they are in 3D (louder close up, muffled
behind walls, echoing in caves), music that changes with the place and time of day, ambience by
biome, footsteps that match the ground, and an **Audio** panel to edit sound events in detail.

---

## 13. Time, calendars, day and night

Every game has a clock and a **calendar** (the **Calendar** panel): how long a day is, the
months, the weekdays, the seasons and moons. You can design your own calendar for a fantasy
world. Drag the Calendar panel's **scrubber** to preview any time of day in the Viewport.

Scripts can read and change the time, speed it up, stop it, skip ahead (sleeping in an inn),
and react at dawn, dusk or each new day (section 19, *Time*).

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
- **Physics material**: **Friction** (0 is ice) and **Bounciness** (0 stops dead, 1 bounces
  back fully).
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

| Action | Keyboard and mouse | Gamepad |
|---|---|---|
| Walk | W A S D | Left stick |
| Look | Mouse | Right stick |
| Run | Hold Left Shift | Click left stick |
| Jump | Space | A |
| Change view | V | Y |
| Zoom | Mouse wheel | |
| Use (doors, switches) | E | X |
| Menu | Escape | Start |
| Show colliders | F4 | |

Walking is relative to the camera, and the player turns to face the way it walks.

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

**Add Sample Assets** (Assets menu) gives you free characters and a big animation library to
practise with.

### The animation graph

A single looping animation is fine for a statue. A player needs to *switch*: idle when
standing, walk when moving, run when fast, jump when jumping. That's what an **animation graph**
does. Click **Animation Graph…** in the Character section to open the **Animation** panel.

- **States** are boxes, each playing an animation (or a blend of several by speed, so it eases
  from walk to run smoothly).
- **Transitions** are arrows between states, each with a condition, such as "speed > 0.2".
- **Parameters** are the values conditions test. **speed** is filled in automatically from how
  fast the character moves; scripts can set others ("is_swimming", "health").
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

The picture fades out and in during the change.

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
| `on_update(entity, dt)` | Every frame; `dt` is the seconds since the last one. |
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
| `entity.move(x, y, z)` | Moves it by that much. |
| `entity.add_force(x, y, z)` | Pushes a dynamic body (call every frame for a steady push). |
| `entity.add_impulse(x, y, z)` | A sudden shove (a kick, an explosion). |
| `entity.set_animation(name, value)` | Sets an animation graph parameter. |
| `entity.trigger_animation(name)` | Fires an animation graph trigger. |
| `entity.animation(name)` | Reads a parameter ("speed" is always there). |
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

Built-in actions: `move_x`, `move_y`, `look_x`, `look_y`, `zoom`, `jump`, `run`, `view`, `use`,
`menu`.

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

### Navigation: `midgard.nav`

| What | Meaning |
|---|---|
| `.path(start, end, partial=False)` | A list of points to walk along, or None. |
| `.nearest(point, reach=2)` | The nearest walkable spot. |
| `.ready` | True once the walkable area round the player is worked out. |

### Sound: `midgard.audio`

| What | Meaning |
|---|---|
| `midgard.audio.play("Bell", volume=1)` | Plays a sound event by name; gives a **Sound** (or None). |
| `entity.play_sound("Bell", volume=1)` | The same, from that entity. |
| `sound.stop()`, `sound.playing`, `sound.set_volume(v)` | Stop it, check it, change its volume. |
| `midgard.audio.stop_all()` | Stops every sound. |
| `midgard.audio.set_volume(bus, v)` / `.volume(bus)` | A bus's volume: "master", "music", "effects", "voice", "ambience", "interface". |
| `midgard.audio.playing` | The sound events playing now. |
| `midgard.audio.enabled` | False when the game has no sound (a server). |

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
- **Cook** (Ctrl+B): just packs the game, to check for problems; the Log lists any.
- **Export…** (Ctrl+E): makes a finished game for **Windows**, **Linux** or **Android** in a
  folder you choose, ready to share.

The first Play after adding a big model takes longer while its textures are converted; later
Plays are quick.

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
