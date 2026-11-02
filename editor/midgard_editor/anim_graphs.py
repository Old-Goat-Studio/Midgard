# SPDX-License-Identifier: Apache-2.0
"""Animation graphs as the editor changes them (milestone 5.13; the engine plays them:
midgard/anim_graph.h, documented with the character component in schema/components.md).

A graph is JSON kept in the character component's "graph". Each function here returns a changed
copy, leaving the graph it was given alone (so an undo step can hold both). States keep where
they sit on the Animation editor's canvas in "pos"; each layer's Any State box in "any_pos"
(the engine ignores both)."""
import copy
from typing import Any

from . import engine
from .cook import mesh_asset_data
from .project import Project

AUTOMATIC = ("speed", "vertical_speed")  # parameters the game sets from how a character moves
OPS = ("==", "!=", "<", "<=", ">", ">=")
ANY = "*"


def skinned_info(project: Project, asset: str) -> dict | None:
    """A skinned model file's joints, clips and rig (none if it can't be read, or isn't one)."""
    module = engine.module()
    data = mesh_asset_data(project, asset) if asset else None
    if module is None or data is None:
        return None
    try:
        return module.skinned_info(data)
    except ValueError:
        return None


def clips_of(project: Project, character: dict) -> list[str]:
    """The clips a character can play: its clips file's, or its model's own."""
    info = skinned_info(project, character.get("clips") or character.get("model", ""))
    return [c["name"] for c in info["clips"]] if info else []


def unique_name(base: str, taken) -> str:
    taken = set(taken)
    if base not in taken:
        return base
    n = 2
    while f"{base} {n}" in taken:
        n += 1
    return f"{base} {n}"


def new_graph(clips: list[str]) -> dict:
    """A graph that plays the first clip (or "Idle_Loop" if there is one)."""
    clip = "Idle_Loop" if "Idle_Loop" in clips else (clips[0] if clips else "")
    return {"parameters": {}, "triggers": [], "root_motion": False,
            "layers": [{"name": "Base", "start": "Idle",
                        "states": {"Idle": {"clip": clip, "pos": [0, 0]}},
                        "transitions": [], "any_pos": [-220, -120]}]}


def problem(project: Project, character: dict) -> str:
    """What the engine says is wrong with a character's graph, or a clip it hasn't got ("": all
    well)."""
    import json
    graph = character.get("graph")
    module = engine.module()
    if not graph or module is None:
        return ""
    said = module.anim_graph_problem(json.dumps(graph))
    if said:
        return said
    clips = set(clips_of(project, character))
    missing = sorted(clips_used(graph) - clips)
    if missing:
        return "Clips the character hasn't got: " + ", ".join(missing)
    return ""


def clips_used(graph: dict) -> set[str]:
    out = set()
    for layer in graph.get("layers", []):
        for state in layer.get("states", {}).values():
            if state.get("clip"):
                out.add(state["clip"])
            for clip, *_where in state.get("clips", []):  # ([clip, x] or [clip, x, y])
                out.add(clip)
    return out


def blend_parameters(state: dict) -> list[str]:
    """What a blend state blends by: one parameter, or two (a 2D blend space)."""
    blend = state.get("blend")
    if blend is None:
        return []
    return list(blend) if isinstance(blend, list) else [blend]


def set_blend(state: dict, by: str, and_by: str = "") -> dict:
    """A blend state blending by one parameter, or two (and_by: a 2D blend space, its clips
    given a y; back to one: their y dropped)."""
    state = copy.deepcopy(state)
    two = bool(and_by)
    state["blend"] = [by, and_by] if two else by
    state["clips"] = [[c[0], c[1], (c[2] if len(c) > 2 else 0.0)] if two else [c[0], c[1]]
                      for c in state.get("clips", [])]
    return state


def names(graph: dict) -> list[str]:
    """Parameters and triggers, as scripts and conditions name them (the automatic ones first)."""
    return [*AUTOMATIC, *graph.get("parameters", {}), *graph.get("triggers", [])]


def numbers(graph: dict) -> list[str]:
    """The parameters (not triggers): what blends, speeds and weights can follow."""
    return [*AUTOMATIC, *graph.get("parameters", {})]


# ---- states -----------------------------------------------------------------------------------
def _layer(graph: dict, layer: int) -> dict:
    return graph["layers"][layer]


def add_state(graph: dict, layer: int, pos: tuple[float, float], clip: str = "",
              blend: str | None = None, name: str = "State") -> tuple[dict, str]:
    g = copy.deepcopy(graph)
    lay = _layer(g, layer)
    name = unique_name(name, lay["states"])
    state: dict[str, Any] = {"pos": [round(pos[0]), round(pos[1])]}
    if blend is not None:
        state.update({"blend": blend, "clips": [[clip, 0.0]] if clip else []})
    else:
        state["clip"] = clip
    lay["states"][name] = state
    if not lay.get("start"):
        lay["start"] = name
    return g, name


def rename_state(graph: dict, layer: int, old: str, new: str) -> dict:
    """Renamed, and everything naming it (its transitions, the start) with it. A name already
    taken, or empty, changes nothing."""
    g = copy.deepcopy(graph)
    lay = _layer(g, layer)
    new = new.strip()
    if not new or new in (old, ANY) or new in lay["states"]:
        return g
    lay["states"] = {(new if k == old else k): v for k, v in lay["states"].items()}
    for t in lay.get("transitions", []):
        for end in ("from", "to"):
            if t.get(end) == old:
                t[end] = new
    if lay.get("start") == old:
        lay["start"] = new
    return g


def remove_states(graph: dict, layer: int, states) -> dict:
    """Gone, with their transitions; another state starts if the start went."""
    g = copy.deepcopy(graph)
    lay = _layer(g, layer)
    states = set(states)
    if len(states) >= len(lay["states"]):  # (a layer keeps a state)
        states -= {next(iter(lay["states"]))}
    for s in states:
        lay["states"].pop(s, None)
    lay["transitions"] = [t for t in lay.get("transitions", [])
                          if t.get("from") not in states and t.get("to") not in states]
    if lay.get("start") not in lay["states"]:
        lay["start"] = next(iter(lay["states"]))
    return g


def set_state(graph: dict, layer: int, name: str, state: dict) -> dict:
    g = copy.deepcopy(graph)
    _layer(g, layer)["states"][name] = copy.deepcopy(state)
    return g


def move_states(graph: dict, layer: int, positions: dict) -> dict:
    """Moved on the canvas (ANY: the layer's Any State box)."""
    g = copy.deepcopy(graph)
    lay = _layer(g, layer)
    for name, (x, y) in positions.items():
        if name == ANY:
            lay["any_pos"] = [round(x), round(y)]
        elif name in lay["states"]:
            lay["states"][name]["pos"] = [round(x), round(y)]
    return g


def set_start(graph: dict, layer: int, name: str) -> dict:
    g = copy.deepcopy(graph)
    _layer(g, layer)["start"] = name
    return g


# ---- transitions ------------------------------------------------------------------------------
OPPOSITE = {">": "<=", ">=": "<", "<": ">=", "<=": ">", "==": "!=", "!=": "=="}


def add_transition(graph: dict, layer: int, src: str, dst: str) -> tuple[dict, int]:
    """A transition; its index. From ANY: from any state. It starts with a condition (the engine
    wants one): once the character moves; from any state, the graph's first trigger (if it has
    one); back the way another goes, the opposite of that one's first (so the two don't flip to
    and fro), or once a cycle has played."""
    g = copy.deepcopy(graph)
    lay = _layer(g, layer)
    transition: dict[str, Any] = {"from": src, "to": dst, "when": [["speed", ">", 0.1]],
                                  "blend": 0.2}
    back = next((t for t in lay.get("transitions", [])
                 if t.get("from") == dst and t.get("to") == src), None)
    if src == ANY and g.get("triggers"):  # (from anywhere: when something asks for it)
        transition["when"] = [[g["triggers"][0]]]
    elif back is not None:
        first = (back.get("when") or [None])[0]
        if first and len(first) == 3 and first[1] in OPPOSITE:
            transition["when"] = [[first[0], OPPOSITE[first[1]], first[2]]]
        else:
            del transition["when"]
            transition["after"] = 1.0
    lay.setdefault("transitions", []).append(transition)
    return g, len(lay["transitions"]) - 1


def set_transition(graph: dict, layer: int, index: int, transition: dict) -> dict:
    g = copy.deepcopy(graph)
    _layer(g, layer)["transitions"][index] = copy.deepcopy(transition)
    return g


def remove_transitions(graph: dict, layer: int, indices) -> dict:
    g = copy.deepcopy(graph)
    lay = _layer(g, layer)
    gone = set(indices)
    lay["transitions"] = [t for i, t in enumerate(lay.get("transitions", [])) if i not in gone]
    return g


# ---- layers -----------------------------------------------------------------------------------
def add_layer(graph: dict, clips: list[str]) -> tuple[dict, int]:
    g = copy.deepcopy(graph)
    name = unique_name("Layer", [lay.get("name", "") for lay in g["layers"]])
    clip = clips[0] if clips else ""
    g["layers"].append({"name": name, "mask": "", "weight": 1.0, "start": "Pose",
                        "states": {"Pose": {"clip": clip, "pos": [0, 0]}},
                        "transitions": [], "any_pos": [-220, -120]})
    return g, len(g["layers"]) - 1


def remove_layer(graph: dict, layer: int) -> dict:
    g = copy.deepcopy(graph)
    if len(g["layers"]) > 1:
        del g["layers"][layer]
    return g


def set_layer(graph: dict, layer: int, **values) -> dict:
    g = copy.deepcopy(graph)
    _layer(g, layer).update(values)
    return g


# ---- parameters -------------------------------------------------------------------------------
def add_parameter(graph: dict, trigger: bool = False) -> tuple[dict, str]:
    g = copy.deepcopy(graph)
    name = unique_name("trigger" if trigger else "parameter", names(g))
    if trigger:
        g.setdefault("triggers", []).append(name)
    else:
        g.setdefault("parameters", {})[name] = 0.0
    return g, name


def set_parameter(graph: dict, name: str, value: float) -> dict:
    g = copy.deepcopy(graph)
    if name in g.get("parameters", {}):
        g["parameters"][name] = value
    return g


def _renamed(value, old: str, new: str):
    return new if value == old else value


def rename_parameter(graph: dict, old: str, new: str) -> dict:
    """Renamed everywhere it is used (conditions, blends, speeds, weights). A name taken, or
    one of the automatic ones, changes nothing."""
    g = copy.deepcopy(graph)
    new = new.strip()
    if not new or new == old or new in names(g) or old in AUTOMATIC:
        return g
    if old in g.get("parameters", {}):
        g["parameters"] = {(new if k == old else k): v for k, v in g["parameters"].items()}
    elif old in g.get("triggers", []):
        g["triggers"] = [_renamed(t, old, new) for t in g["triggers"]]
    else:
        return g
    for lay in g["layers"]:
        if isinstance(lay.get("weight"), str):
            lay["weight"] = _renamed(lay["weight"], old, new)
        for state in lay["states"].values():
            if isinstance(state.get("blend"), list):  # (a 2D blend: two parameters)
                state["blend"] = [_renamed(b, old, new) for b in state["blend"]]
            elif "blend" in state:
                state["blend"] = _renamed(state["blend"], old, new)
            if isinstance(state.get("speed"), str):
                state["speed"] = _renamed(state["speed"], old, new)
        for t in lay.get("transitions", []):
            for c in t.get("when", []):
                c[0] = _renamed(c[0], old, new)
    return g


def uses(graph: dict, name: str) -> int:
    """How many places use a parameter or trigger."""
    count = 0
    for lay in graph.get("layers", []):
        count += lay.get("weight") == name
        for state in lay.get("states", {}).values():
            blend = state.get("blend")
            count += (name in blend) if isinstance(blend, list) else blend == name
            count += state.get("speed") == name
        for t in lay.get("transitions", []):
            count += sum(1 for c in t.get("when", []) if c and c[0] == name)
    return count


def remove_parameter(graph: dict, name: str) -> dict:
    """Gone, if nothing uses it (else the graph is left as it was)."""
    g = copy.deepcopy(graph)
    if uses(g, name):
        return g
    g.get("parameters", {}).pop(name, None)
    if name in g.get("triggers", []):
        g["triggers"].remove(name)
    return g
