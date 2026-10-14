# Midgard

![Yggdrasil](midgard512.png)

## A Game engine with an action focused editor.

### Overview

Midgard is an editor for video gaming. The editor runs on Linux and Windows. Games are exported to Linux, Windows and Android from either editor host. The original application is being developed on Linux. Please keep a description of the application that will be passed to Windows or Android to create platform specific distributions. 

### Specifics
 
* The application will be built using Python
* The GUI for the editor will be built using PySide6
* The Game engine will be built using C++
* Configuration files for both the editor and resulting game projects will be stored in JSON files.
* Databases for the editor and resulting game packages will be stored in an SQLite file using the filename structure of {project}.db
* Scripting for the games will be allowed in Python.
* Game packages will be network capable either by self hosting, in the game, or on a dedicated game server.

### Deployment

* Windows, Linux and Android
* Runtime templates are built on each OS in Midgard's CI. Game exports need no compiler.


### Editor GUI

* The main editor window will be a project window. from this window you will be able to edit all of the project specific game settings. It's titlebar will be "Midgard - {project_name}". Project settings will be stored in a JSON file in the root project directory under the name of {project_name}.config 
* The main editor window will store it's application settings in the data directory of the application. For windows this will be under "User/Appdata/Roaming", for Linux this will be under ~/.local/share/Midgard/midgard.json

### Main Functions

Midgard will have many game construction functions built into one package. The most important are:

* A scripting editor for Python scripts, complete with a snippet library.
* A menu editor, including widgets for Text entry, Buttons, drop boxes, Lists, Trees, Radio buttons, Checkboxes, Panels, Tabs, Progress bars and menu window control functions (close, minimize movement, etc...)
* Menu windows will be player sizable and have movement and placement functions.   
* A world Environment editor, including functions for skyboxes, lighting water, fog, etc...
* A Physics editor
* A Vulkan shader system and editor with texture generation capability
* A terrain system for both procedural terrain generation and editing
* An object editor for simple geometry 
* A Player inventory management system
* A Player wallet and banking system
* A Quest management system including goals and quest prerequisites.
* A skill tree management system. (Define skills, skill modifiers, prerequisites etc...)
* A player game progression system
* An AI console.

### Questions

* How should the game, designed in the editor, be communicated to the game engine, written in C++?
* How can I handle building for Android?