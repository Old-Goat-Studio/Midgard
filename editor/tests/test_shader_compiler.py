# SPDX-License-Identifier: Apache-2.0
"""The shader compiler from the editor's side (milestone 5.17 step 1)."""
import pytest

from midgard_editor import engine

needs_compiler = pytest.mark.skipif(
    engine.module() is None or not hasattr(engine.module(), "compile_shader"),
    reason="needs the engine module with the shader compiler")

GLOW = """#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
void main() {
    float pulse = 0.5 + 0.5 * sin(uv.x * 6.0);
    colour = vec4(pulse, pulse * 0.6, 0.1, 1.0);
}
"""


@needs_compiler
def test_the_editor_compiles_a_shader():
    out = engine.compile_shader(GLOW)
    assert out["ok"] and len(out["spirv"]) > 100 and out["messages"] == []


@needs_compiler
def test_the_editor_hears_each_mistake_on_its_line():
    out = engine.compile_shader(GLOW.replace("sin(", "sinn("), name="glow")
    assert not out["ok"]
    first = out["messages"][0]
    assert first["line"] == 5 and first["error"] and "sinn" in first["message"]


@needs_compiler
def test_lines_are_counted_from_the_users_own_first_line():
    wrapped = "#version 450\n// template\n" + GLOW.split("\n", 1)[1]
    out = engine.compile_shader(wrapped.replace("sin(", "sinn("), first_line=3)
    assert out["messages"][0]["line"] == 4  # (line 6 of the whole, the user's line 4)


def test_without_the_engine_the_editor_is_told(monkeypatch):
    monkeypatch.setattr(engine, "module", lambda: None)
    assert engine.compile_shader(GLOW) is None
