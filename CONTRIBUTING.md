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

## Style

- Python: `ruff check .` must pass (settings in `ruff.toml`).
- C++: `clang-format --dry-run --Werror` must pass, using **clang-format 18.1.8** (installed by
  `pip install -e "editor[dev]"`, the same version CI uses) and the repository's `.clang-format`.
- Keep the contract in `schema/` the single source of truth: never duplicate its definitions in
  the editor or the engine. Released migrations are never edited, only added to.

## Pull requests

- One topic per pull request, with tests where behaviour changes.
- CI must be green: lint, licences, editor tests on Linux and Windows, engine builds on Windows,
  Linux x86_64, Linux arm64 and Android.
- Describe what changed and why, and link the roadmap phase it belongs to.

## Code of conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).
