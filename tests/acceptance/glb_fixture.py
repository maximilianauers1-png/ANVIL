"""Synthetic test minis as glTF 2.0 binary (GLB), written without dependencies.

`humanoid_glb(path, rigged=True)` writes a 1.8 m humanoid in T-pose, Y up,
metres, front +Z (the glTF convention), built from overlapping 8-sided tubes
(intersecting shells, like AI-generated meshes) with open tube ends (holes).
With `rigged`, a skin with Mixamo bone names ("mixamorig:LeftArm", ...) and
smooth weights is included -- the layout Tripo and Meshy rig exports use.
"""
import json
import math
import struct

# (name, parent, head xyz in metres; glTF frame: +X = the figure's left, Y up, front +Z)
BONES = [
    ("mixamorig:Hips", None, (0.0, 0.95, 0.0)),
    ("mixamorig:Spine", "mixamorig:Hips", (0.0, 1.08, 0.0)),
    ("mixamorig:Spine1", "mixamorig:Spine", (0.0, 1.22, 0.0)),
    ("mixamorig:Neck", "mixamorig:Spine1", (0.0, 1.50, 0.0)),
    ("mixamorig:Head", "mixamorig:Neck", (0.0, 1.58, 0.0)),
    ("mixamorig:LeftShoulder", "mixamorig:Spine1", (0.08, 1.42, 0.0)),
    ("mixamorig:LeftArm", "mixamorig:LeftShoulder", (0.20, 1.42, 0.0)),
    ("mixamorig:LeftForeArm", "mixamorig:LeftArm", (0.49, 1.42, 0.0)),
    ("mixamorig:LeftHand", "mixamorig:LeftForeArm", (0.75, 1.42, 0.0)),
    ("mixamorig:LeftHandMiddle1", "mixamorig:LeftHand", (0.84, 1.42, 0.0)),
    ("mixamorig:RightShoulder", "mixamorig:Spine1", (-0.08, 1.42, 0.0)),
    ("mixamorig:RightArm", "mixamorig:RightShoulder", (-0.20, 1.42, 0.0)),
    ("mixamorig:RightForeArm", "mixamorig:RightArm", (-0.49, 1.42, 0.0)),
    ("mixamorig:RightHand", "mixamorig:RightForeArm", (-0.75, 1.42, 0.0)),
    ("mixamorig:RightHandMiddle1", "mixamorig:RightHand", (-0.84, 1.42, 0.0)),
    ("mixamorig:LeftUpLeg", "mixamorig:Hips", (0.10, 0.92, 0.0)),
    ("mixamorig:LeftLeg", "mixamorig:LeftUpLeg", (0.10, 0.50, 0.0)),
    ("mixamorig:LeftFoot", "mixamorig:LeftLeg", (0.10, 0.08, 0.0)),
    ("mixamorig:RightUpLeg", "mixamorig:Hips", (-0.10, 0.92, 0.0)),
    ("mixamorig:RightLeg", "mixamorig:RightUpLeg", (-0.10, 0.50, 0.0)),
    ("mixamorig:RightFoot", "mixamorig:RightLeg", (-0.10, 0.08, 0.0)),
]

# tubes: (bone at the start, bone at the end or None, start, end, radius)
def _tubes():
    b = {n: h for n, _, h in BONES}
    t = []
    t.append(("mixamorig:Hips", "mixamorig:Spine", (0, 0.86, 0), b["mixamorig:Spine"], 0.15))
    t.append(("mixamorig:Spine", "mixamorig:Spine1", b["mixamorig:Spine"], b["mixamorig:Spine1"], 0.14))
    t.append(("mixamorig:Spine1", "mixamorig:Neck", b["mixamorig:Spine1"], (0, 1.48, 0), 0.17))
    t.append(("mixamorig:Neck", "mixamorig:Head", b["mixamorig:Neck"], b["mixamorig:Head"], 0.06))
    t.append(("mixamorig:Head", None, b["mixamorig:Head"], (0, 1.80, 0), 0.11))
    for s in ("Left", "Right"):
        p = "mixamorig:" + s
        t.append((p + "Arm", p + "ForeArm", b[p + "Shoulder"], b[p + "ForeArm"], 0.055))
        t.append((p + "ForeArm", p + "Hand", b[p + "ForeArm"], b[p + "Hand"], 0.045))
        hx = b[p + "Hand"][0] * 1.24
        t.append((p + "Hand", None, b[p + "Hand"], (hx, 1.42, 0), 0.05))
        t.append((p + "UpLeg", p + "Leg", b[p + "UpLeg"], b[p + "Leg"], 0.085))
        t.append((p + "Leg", p + "Foot", b[p + "Leg"], (b[p + "Foot"][0], 0.02, 0), 0.06))
        t.append((p + "Foot", None, (b[p + "Foot"][0], 0.03, -0.04), (b[p + "Foot"][0], 0.03, 0.20), 0.035))
    return t


def _tube_mesh(a, c, r, sides=8, rings=10):
    ax = [c[i] - a[i] for i in range(3)]
    L = math.sqrt(sum(x * x for x in ax))
    d = [x / L for x in ax]
    # perpendicular frame
    h = (1, 0, 0) if abs(d[0]) < 0.9 else (0, 1, 0)
    u = [d[1] * h[2] - d[2] * h[1], d[2] * h[0] - d[0] * h[2], d[0] * h[1] - d[1] * h[0]]
    lu = math.sqrt(sum(x * x for x in u))
    u = [x / lu for x in u]
    v = [d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]]
    V, T, F = [], [], []
    for k in range(rings + 1):
        t = k / rings
        for s in range(sides):
            ang = 2 * math.pi * s / sides
            V.append([a[i] + ax[i] * t + r * (math.cos(ang) * u[i] + math.sin(ang) * v[i]) for i in range(3)])
            T.append(t)
    for k in range(rings):
        for s in range(sides):
            i0, i1 = k * sides + s, k * sides + (s + 1) % sides
            j0, j1 = i0 + sides, i1 + sides
            F += [(i0, i1, j1), (i0, j1, j0)]
    # cap the far end only: the start stays open (a hole, as in many AI meshes)
    ci = len(V)
    V.append(list(c))
    T.append(1.0)
    top = rings * sides
    for s in range(sides):
        F.append((top + s, top + (s + 1) % sides, ci))
    return V, T, F


def humanoid_glb(path, rigged=True):
    names = [n for n, _, _ in BONES]
    index = {n: i for i, n in enumerate(names)}
    pos, joints, weights, idx = [], [], [], []
    for bone, nxt, a, c, r in _tubes():
        V, T, F = _tube_mesh(a, c, r)
        base = len(pos)
        for p, t in zip(V, T):
            pos.append(p)
            # blend into the next bone over the last 25 % of the tube
            w2 = 0.0 if nxt is None else max(0.0, (t - 0.75) / 0.25) * 0.5
            joints.append((index[bone], index[nxt] if nxt else 0, 0, 0))
            weights.append((1.0 - w2, w2, 0.0, 0.0))
        idx += [base + i for f in F for i in f]
    blob = bytearray()

    def add(data, fmt, count, comp, typ, target=None, minmax=False):
        off = len(blob)
        blob.extend(data)
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), **({"target": target} if target else {})})
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ}
        if minmax:
            acc["min"] = [min(p[i] for p in pos) for i in range(3)]
            acc["max"] = [max(p[i] for p in pos) for i in range(3)]
        accessors.append(acc)
        return len(accessors) - 1

    views, accessors = [], []
    a_pos = add(b"".join(struct.pack("<3f", *p) for p in pos), None, len(pos), 5126, "VEC3", 34962, True)
    a_idx = add(b"".join(struct.pack("<I", i) for i in idx), None, len(idx), 5125, "SCALAR", 34963)
    attrs = {"POSITION": a_pos}
    nodes = []
    doc = {"asset": {"version": "2.0", "generator": "anvil test fixture"}, "scene": 0}
    if rigged:
        attrs["JOINTS_0"] = add(b"".join(struct.pack("<4H", *j) for j in joints), None, len(pos), 5123, "VEC4", 34962)
        attrs["WEIGHTS_0"] = add(b"".join(struct.pack("<4f", *w) for w in weights), None, len(pos), 5126, "VEC4", 34962)
        # inverse bind matrices: joints carry only translations (world = head)
        ibm = b""
        for _, _, h in BONES:
            m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -h[0], -h[1], -h[2], 1]
            ibm += struct.pack("<16f", *m)
        a_ibm = add(ibm, None, len(BONES), 5126, "MAT4")
        for n, parent, h in BONES:
            ph = (0, 0, 0) if parent is None else BONES[index[parent]][2]
            nodes.append({"name": n, "translation": [h[i] - ph[i] for i in range(3)]})
        for i, (n, parent, _) in enumerate(BONES):
            if parent is not None:
                nodes[index[parent]].setdefault("children", []).append(i)
        mesh_node = len(nodes)
        nodes.append({"name": "Body", "mesh": 0, "skin": 0})
        doc["skins"] = [{"joints": list(range(len(BONES))), "inverseBindMatrices": a_ibm, "skeleton": 0}]
        doc["scenes"] = [{"nodes": [0, mesh_node]}]
    else:
        nodes.append({"name": "Body", "mesh": 0})
        doc["scenes"] = [{"nodes": [0]}]
    doc["nodes"] = nodes
    doc["meshes"] = [{"name": "Body", "primitives": [{"attributes": attrs, "indices": a_idx, "mode": 4}]}]
    doc["bufferViews"] = views
    doc["accessors"] = accessors
    doc["buffers"] = [{"byteLength": len(blob)}]
    js = json.dumps(doc, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))
    return path


if __name__ == "__main__":
    import sys
    humanoid_glb(sys.argv[1] if len(sys.argv) > 1 else "humanoid.glb", rigged="--unrigged" not in sys.argv)
