# SPDX-License-Identifier: Apache-2.0
"""A .gltf and the .bin files (or data URIs) beside it, packed into one .glb (milestone 5.13).

Game packages carry one file a model; character packs often come as .gltf with a .bin and
images. Buffers are joined into the .glb's binary chunk; images given by file are left out
(materials keep their factors: textures go through the editor's own materials), so the result
needs nothing beside it.

    python -m midgard_editor.gltf_pack model.gltf model.glb
"""
import base64
import json
import struct
import sys
from pathlib import Path


class PackError(Exception):
    pass


def _read_buffer(buffer: dict, folder: Path) -> bytes:
    uri = buffer.get("uri")
    if uri is None:
        raise PackError("a buffer has no uri (a .glb's own chunk can't be packed again)")
    if uri.startswith("data:"):
        try:
            return base64.b64decode(uri.split(",", 1)[1])
        except (IndexError, ValueError) as e:
            raise PackError(f"a buffer's data uri can't be read: {e}") from e
    path = (folder / uri).resolve()
    if folder.resolve() not in path.parents and path.parent != folder.resolve():
        raise PackError(f"buffer {uri!r} is outside the model's folder")
    try:
        return path.read_bytes()
    except OSError as e:
        raise PackError(f"buffer {uri!r} can't be read: {e}") from e


def pack(gltf_path: Path) -> bytes:
    """The .gltf at gltf_path as .glb bytes."""
    try:
        doc = json.loads(Path(gltf_path).read_text("utf-8"))
    except (OSError, ValueError) as e:
        raise PackError(f"not a glTF file: {e}") from e
    folder = Path(gltf_path).parent
    # One binary chunk: the buffers one after another (4-byte aligned), views moved to match.
    blob = bytearray()
    starts = []
    for buffer in doc.get("buffers", []):
        while len(blob) % 4:
            blob.append(0)
        starts.append(len(blob))
        blob += _read_buffer(buffer, folder)
    for view in doc.get("bufferViews", []):
        view["byteOffset"] = view.get("byteOffset", 0) + starts[view["buffer"]]
        view["buffer"] = 0
    doc["buffers"] = [{"byteLength": len(blob)}] if blob else []
    # Images given by file go (and the textures and material slots that use them).
    images = doc.get("images", [])
    gone = {i for i, im in enumerate(images) if "uri" in im}
    if gone:
        textures = doc.get("textures", [])
        lost = {t for t, tex in enumerate(textures) if tex.get("source") in gone}
        for material in doc.get("materials", []):
            pbr = material.get("pbrMetallicRoughness", {})
            for owner, key in ((pbr, "baseColorTexture"), (pbr, "metallicRoughnessTexture"),
                               (material, "normalTexture"), (material, "occlusionTexture"),
                               (material, "emissiveTexture")):
                if key in owner and owner[key].get("index") in lost:
                    del owner[key]
        for key in ("images", "textures", "samplers"):
            doc.pop(key, None)
    text = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    blob += b"\0" * (-len(blob) % 4)
    chunks = struct.pack("<II", len(text), 0x4E4F534A) + text
    if blob:
        chunks += struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    return struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks


if __name__ == "__main__":  # pragma: no cover
    if len(sys.argv) != 3:
        sys.exit("usage: python -m midgard_editor.gltf_pack model.gltf model.glb")
    Path(sys.argv[2]).write_bytes(pack(Path(sys.argv[1])))
