# Telling Discord what I'm working on

Notes for later: two ways to show Midgard work on Discord. Nothing here is built yet.

## 1. On my Discord profile: Rich Presence

The "Playing Midgard: editing HelloCube" line under my name. Discord reads it from programs
running on the PC; nothing to set up on Discord's side.

- **By hand, no code:** Discord ▸ User Settings ▸ Registered Games ▸ *Add it!* and pick
  Midgard. Only the program's name shows, no project or activity.
- **From the editor itself:**
  - Register a free application at <https://discord.com/developers/applications> to get an
    **application ID** (not secret; it names the app in Discord).
  - The editor talks to the Discord app running on the same PC through its local connection
    (Discord's IPC). No login, password or token is involved, and nothing goes over the
    internet from the editor.
  - It would show the project's name and what I'm doing, e.g. "Editing a material",
    "Placing objects", "Playing", with the time since I started.
  - Plan if we build it: a Preferences switch, **off by default**. It stays silent when
    Discord isn't running, and never shows file paths.

## 2. In a Discord channel: webhooks

Posts in a server channel, e.g. a devlog channel for followers.

- In the channel: **Edit Channel ▸ Integrations ▸ Webhooks ▸ New Webhook ▸ Copy Webhook
  URL**. Anything that sends a short message to that URL appears in the channel.
- **The URL works like a password:** anyone who has it can post in the channel. Keep it out of
  the repository and out of project files.
- **Simplest, no code: GitHub posts each push.**
  - In the repository on GitHub: **Settings ▸ Webhooks ▸ Add webhook**.
  - Payload URL: the Discord webhook URL with **`/github`** added to the end.
  - Content type: `application/json`. Events: *Just the push event* (or pick others, such as
    releases).
  - Every push then appears in the channel with its commit messages. The Phase commit
    messages already read well as a devlog.
- **Possible editor feature:** a "Post Update…" command that sends a short message, and
  perhaps a screenshot of the viewport, to the channel. The webhook URL would be kept in the
  editor's own settings, never in the project.

## Recommendation

Start with the GitHub webhook (about five minutes, no code). Add Rich Presence to the editor
later if wanted.
