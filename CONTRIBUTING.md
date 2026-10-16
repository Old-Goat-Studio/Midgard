# Contributing to Midgard

Thank you for helping build Midgard. This page covers what you need to know before opening a
pull request.

## Licence

Midgard is licensed under the **Apache License 2.0** (see `LICENSE` and `NOTICE`).
By submitting a contribution, you agree that it is licensed under the same terms
(*inbound = outbound*, as described in section 5 of the licence). Only submit work you have the
right to license this way.

Every source file starts with an SPDX line, which CI checks:

```text
# SPDX-License-Identifier: Apache-2.0      (Python, CMake, YAML)
// SPDX-License-Identifier: Apache-2.0     (C++)
-- SPDX-License-Identifier: Apache-2.0     (SQL)
```

## Third-party code

Adding a dependency means adding it to `tools/third_party.json` with its licence and where it
ships (`runtime`, `editor` or `build`). `python tools/licenses.py check` must pass:

- **Runtime** dependencies end up inside every exported game, so they must be permissive
  (MIT, BSD, zlib, Apache-2.0, public domain and similar). No GPL or LGPL.
- **Editor** dependencies may also be LGPL (Qt / PySide6), because the editor is distributed so
  users can replace those libraries.
- Proprietary SDKs (such as Steamworks) are never committed.

## Getting set up

```sh
# Editor (Python 3.12+)
python -m venv .venv
.venv/bin/pip install -e "editor[dev]"          # Windows: .venv\Scripts\pip
.venv/bin/pytest editor tools

# Engine (C++20, CMake ≥ 3.25, Ninja)
python tools/setup_vcpkg.py                     # fetches vcpkg at the pinned baseline
cmake --preset linux-release                    # or windows-release from a VS developer prompt
cmake --build --preset linux-release
ctest --preset linux-release
```

## Kinds of tests

Every change comes with tests at the levels it touches (see `Phase5_Roadmap.md`, §2):

- **Unit tests:** doctest in `engine/tests/`, pytest in `editor/tests/` and `tools/tests/`.
- **Reference tables:** formulas with a specification (hashing, deterministic maths and
  world shapes now; solar phase and calendars later) have a Python version in `tools/reference/` and a table of cases in
  `schema/tests/`, checked against both the Python and the engine.
- **Image tests:** `python tools/images.py run --runtime <midgard_runtime>` draws the scenes in
  `engine/tests/images/scenes.json` and compares them with the reference images beside it. CI
  runs them on Linux with software Vulkan. When a change is meant to alter the pictures, run
  `tools/images.py update` and review the new images like code.
- **Budgets:** `midgard_bench --json r.json` then `python tools/budgets.py r.json` checks timings
  against `schema/budgets.json`. Every benchmark needs a budget.
- **Device checks:** `midgard_runtime --selftest` (with `--show` on screen), and on Android the
  self-test APK from CI (`midgard-editor export … --selftest`).

## Third-party assets

Models, sounds and textures from others go in `assets/third_party/`, and only under **CC0**.
List each file (or a folder from one pack) in `assets/third_party/SOURCES.toml` with its title,
author and source; `python tools/licenses.py check` fails otherwise.

## Style

- Python: `ruff check .` must pass (settings in `ruff.toml`).
- C++: `clang-format --dry-run --Werror` must pass, using **clang-format 18.1.8** (installed by
  `pip install -e "editor[dev]"`, the same version CI uses) and the repository's `.clang-format`.
- Anything that must come out the same on every target (world generation above all) uses
  `midgard/det.h`: its hashing, random streams, fixed-point noise and `det::sin`/`det::cos`,
  never `std::sin`, `std::pow` or `rand()`. The engine builds with strict floating point; don't
  add fast-math or FMA options.
- Positions are doubles (`glm::dvec3`); convert to floats only relative to the camera
  (`render::Camera::origin`, `render::relative_to`).
- Nothing assumes "y is up": ask the body's shape (`midgard/shape.h`, `midgard/body.h`) for
  positions on the surface, up, gravity and the local frame. Surface grids that must meet
  exactly at seams use power-of-two steps.
- Keep the contract in `schema/` the single source of truth: never duplicate its definitions in
  the editor or the engine. Released migrations are never edited, only added to.

## Pull requests

- One topic per pull request, with tests where behaviour changes.
- CI must be green: lint, licences, editor tests on Linux and Windows, engine builds and tests on
  Windows, Linux x86_64, Linux arm64 and Android, image tests, budgets and the self-test.
- Describe what changed and why, and link the roadmap phase it belongs to.

## Code of conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).
