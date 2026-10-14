# Platforms

One folder per export target. Each holds that target's packaging notes and its **handoff
document** (`HANDOFF.md`), the description passed to whoever builds that platform's runtime
template. The template for these documents is in `midgard_roadmap.md`, Appendix A.

| Folder | Targets |
|---|---|
| `windows/` | Games (x86_64) and the dedicated server |
| `linux/` | Games (x86_64, `.tar.gz`) and the dedicated server (x86_64 and arm64) |
| `android/` | Games (arm64-v8a). Android never hosts and has no editor. |

The runtime templates themselves are built from Phase 2 onwards.
