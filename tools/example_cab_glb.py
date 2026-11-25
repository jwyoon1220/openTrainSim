#!/usr/bin/env python3
"""Writes examples/cab/desk.glb, a small test cab model for the cab API.

A real cab is modelled in Blender (or any tool) and exported as glTF 2.0; this script only
stands in for that so the example works without any downloads. The file has the structure an
artist's export would have:

  * the model origin is the cab floor below the driver's eye (the eye is at 0, 1.2, 0),
  * every moving part is its own node whose origin is its pivot (lever axle, needle centre,
    button base), named so cab.lua can find it,
  * lamps have an off and an on node, the master lever an invisible hit volume.

Python standard library only. Usage: tools/example_cab_glb.py [output.glb]
"""
import json
import math
import struct
import sys
import zlib

# ------------------------------------------------------------------ geometry (openBVE axes: x right)
# Built in openBVE/car coordinates and converted when written (glTF x = -x).


class Mesh:
    def __init__(self):
        self.prims = {}  # material -> (positions, normals, uvs, indices)

    def prim(self, mat):
        return self.prims.setdefault(mat, ([], [], [], []))

    def quad(self, mat, a, b, c, d, n, uv=((0, 1), (1, 1), (1, 0), (0, 0))):
        p, ns, us, ix = self.prim(mat)
        base = len(p)
        p += [a, b, c, d]
        ns += [n] * 4
        us += list(uv)
        ix += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, mat, size, at=(0, 0, 0)):
        hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
        x, y, z = at
        P = [(x + sx * hx, y + sy * hy, z + sz * hz) for sz in (-1, 1) for sy in (-1, 1) for sx in (-1, 1)]
        # P index: bit0 x, bit1 y, bit2 z
        faces = [((0, 2, 3, 1), (0, 0, -1)), ((5, 7, 6, 4), (0, 0, 1)), ((4, 6, 2, 0), (-1, 0, 0)),
                 ((1, 3, 7, 5), (1, 0, 0)), ((2, 6, 7, 3), (0, 1, 0)), ((0, 1, 5, 4), (0, -1, 0))]
        for f, n in faces:
            self.quad(mat, *(P[i] for i in f), n)

    def cylinder(self, mat, radius, length, axis="y", at=(0, 0, 0), segments=24, face_uv=False):
        """Closed cylinder; with face_uv the end caps map the full texture (dial faces)."""
        def place(u, v, w):  # u, v across the axis, w along it
            if axis == "x":
                return (at[0] + w, at[1] + u, at[2] + v)
            if axis == "y":
                return (at[0] + u, at[1] + w, at[2] + v)
            return (at[0] + u, at[1] + v, at[2] + w)

        def nrm(u, v, w):
            return place(u, v, w) if at == (0, 0, 0) else tuple(a - b for a, b in zip(place(u, v, w), at))

        p, ns, us, ix = self.prim(mat)
        h = length / 2
        for cap in (-1, 1):
            c0 = len(p)
            p.append(place(0, 0, cap * h))
            ns.append(nrm(0, 0, cap))
            us.append((0.5, 0.5))
            for i in range(segments):
                t = 2 * math.pi * i / segments
                cu, cv = math.cos(t) * radius, math.sin(t) * radius
                p.append(place(cu, cv, cap * h))
                ns.append(nrm(0, 0, cap))
                if axis == "z":   # x right, y up: texture u right, v down
                    us.append((0.5 + cu / (2 * radius), 0.5 - cv / (2 * radius)))
                else:
                    us.append((0.5 + math.cos(t) / 2, 0.5 + math.sin(t) / 2))
            for i in range(segments):
                a, b = c0 + 1 + i, c0 + 1 + (i + 1) % segments
                ix += [c0, a, b] if cap < 0 else [c0, b, a]
        s0 = len(p)
        for i in range(segments + 1):
            t = 2 * math.pi * i / segments
            cu, cv = math.cos(t), math.sin(t)
            for cap in (-1, 1):
                p.append(place(cu * radius, cv * radius, cap * h))
                ns.append(nrm(cu, cv, 0))
                us.append((i / segments, 0 if cap < 0 else 1))
        for i in range(segments):
            a = s0 + 2 * i
            ix += [a, a + 2, a + 3, a, a + 3, a + 1]


def rot_x(deg):
    t = math.radians(deg)
    return (math.sin(t / 2), 0.0, 0.0, math.cos(t / 2))  # unchanged by the x mirror


def tilt(c, deg, x, y, z):
    """Point on a plane through c tilted by deg about x (local x right, y up, z away)."""
    t = math.radians(deg)
    return (c[0] + x, c[1] + y * math.cos(t) - z * math.sin(t), c[2] + y * math.sin(t) + z * math.cos(t))


# ------------------------------------------------------------------ dial textures (PNG)
def png(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def dial_png(maximum, major, minor, red_from=None, size=256):
    img = bytearray([242, 242, 242, 255] * size * size)
    c = size / 2

    def put(x, y, col):
        if 0 <= x < size and 0 <= y < size:
            i = (y * size + x) * 4
            img[i:i + 4] = bytes(col)

    def ray(v, r0, r1, width, col):
        th = math.radians(135 - 270 * v / maximum)  # 135 deg at 0, -135 at the maximum
        dx, dy = -math.sin(th), math.cos(th)
        steps = int((r1 - r0) * size)
        for s in range(steps + 1):
            r = (r0 + (r1 - r0) * s / max(steps, 1)) * size / 2
            for k in range(-width, width + 1):
                put(int(c + dx * r - dy * k * 0.5), int(c - dy * r - dx * k * 0.5), col)

    for y in range(size):  # black rim
        for x in range(size):
            if math.hypot(x - c + 0.5, y - c + 0.5) > size / 2 - 5:
                put(x, y, (20, 20, 20, 255))
    if red_from is not None:
        v = red_from
        while v <= maximum:
            ray(v, 0.80, 0.90, 2, (210, 30, 30, 255))
            v += maximum / 400
    v = 0
    while v <= maximum + 1e-9:
        is_major = abs(v / major - round(v / major)) < 1e-6
        ray(v, 0.68 if is_major else 0.76, 0.90, 2 if is_major else 1, (15, 15, 15, 255))
        v += minor
    for y in range(size):  # hub
        for x in range(size):
            if math.hypot(x - c + 0.5, y - c + 0.5) < 10:
                put(x, y, (30, 30, 30, 255))
    return png(size, size, img)


# ------------------------------------------------------------------ the cab
MATERIALS = {
    "shell": dict(color=(0.32, 0.34, 0.37, 1)),
    "desk": dict(color=(0.17, 0.18, 0.20, 1)),
    "console": dict(color=(0.26, 0.27, 0.29, 1)),
    "floor": dict(color=(0.12, 0.12, 0.13, 1)),
    "black": dict(color=(0.05, 0.05, 0.05, 1)),
    "metal": dict(color=(0.62, 0.63, 0.66, 1)),
    "white": dict(color=(0.92, 0.92, 0.92, 1)),
    "yellow": dict(color=(0.88, 0.75, 0.12, 1)),
    "blue": dict(color=(0.13, 0.38, 0.88, 1)),
    "red": dict(color=(0.82, 0.12, 0.12, 1)),
    "needle_red": dict(color=(0.9, 0.1, 0.1, 1)),
    "lamp_off": dict(color=(0.18, 0.18, 0.18, 1)),
    "lamp_green": dict(color=(0.2, 0.9, 0.25, 1), emissive=(0.2, 0.9, 0.25)),
    "lamp_red": dict(color=(1.0, 0.2, 0.2, 1), emissive=(1.0, 0.2, 0.2)),
    "lamp_amber": dict(color=(1.0, 0.62, 0.12, 1), emissive=(1.0, 0.62, 0.12)),
    "hit": dict(color=(1, 0, 1, 1)),
    "dial_speed": dict(color=(1, 1, 1, 1), texture=0),
    "dial_bc": dict(color=(1, 1, 1, 1), texture=1),
    "dial_mr": dict(color=(1, 1, 1, 1), texture=2),
}
TEXTURES = [dial_png(320, 40, 10, red_from=300), dial_png(500, 100, 50), dial_png(1000, 200, 50)]

nodes = []  # dicts: name, t, r, mesh, children (indices)


def node(name, t=(0, 0, 0), r=None, mesh=None, parent=None):
    nodes.append(dict(name=name, t=t, r=r, mesh=mesh, children=[]))
    i = len(nodes) - 1
    if parent is not None:
        nodes[parent]["children"].append(i)
    return i


EYE = (0.0, 1.2, 0.0)
DESK_C, DESK_T = (0.0, 0.60, 0.50), -20
DASH_C, DASH_T = (0.0, 0.78, 0.95), -35

root = node("Cab")

# shell, all in the root frame
shell = Mesh()
shell.box("floor", (1.7, 0.04, 1.9), (0, -0.02, 0.15))
shell.box("shell", (0.04, 1.0, 1.9), (-0.85, 0.5, 0.15))       # side walls (below the windows)
shell.box("shell", (0.04, 1.0, 1.9), (0.85, 0.5, 0.15))
shell.box("shell", (0.08, 1.1, 0.08), (-0.81, 1.45, 1.08))     # windscreen pillars
shell.box("shell", (0.08, 1.1, 0.08), (0.81, 1.45, 1.08))
shell.box("shell", (0.08, 1.0, 0.08), (-0.85, 1.5, -0.75))     # rear posts
shell.box("shell", (0.08, 1.0, 0.08), (0.85, 1.5, -0.75))
shell.box("shell", (1.7, 0.10, 0.10), (0, 1.97, 1.08))          # windscreen header
shell.box("shell", (1.78, 0.04, 1.95), (0, 2.02, 0.15))         # roof
shell.box("shell", (1.7, 2.0, 0.04), (0, 1.0, -0.8))            # back wall
shell.box("console", (1.7, 0.70, 0.04), (0, 0.35, 1.12))        # front below the console
node("Shell", mesh=shell, parent=root)

desk = Mesh()
desk.box("desk", (1.5, 0.04, 0.55))
node("Desk", t=DESK_C, r=rot_x(DESK_T), mesh=desk, parent=root)
under = Mesh()
under.box("black", (1.5, 0.60, 0.04))
node("DeskFront", t=(0, 0.28, 0.25), mesh=under, parent=root)

dash = Mesh()
dash.box("console", (1.5, 0.36, 0.04), (0, 0, 0.02))
node("Console", t=DASH_C, r=rot_x(DASH_T), mesh=dash, parent=root)

# --- master controller: axle on the desk, handle up; base plate is part of the body
base = Mesh()
base.box("black", (0.10, 0.02, 0.26))
node("MasterBase", t=tilt(DESK_C, DESK_T, -0.45, 0.025, 0.08), r=rot_x(DESK_T), mesh=base, parent=root)
lever = Mesh()
lever.cylinder("metal", 0.012, 0.22, "y", (0, 0.11, 0))
lever.box("black", (0.12, 0.035, 0.04), (0, 0.23, 0))
master = node("MasterLever", t=tilt(DESK_C, DESK_T, -0.45, 0.03, 0.08), r=rot_x(DESK_T), mesh=lever, parent=root)
hit = Mesh()
hit.box("hit", (0.14, 0.28, 0.08), (0, 0.14, 0))
node("MasterLever_Hit", mesh=hit, parent=master)   # invisible click volume, follows the lever

# --- reverser: rotary knob
knob = Mesh()
knob.cylinder("metal", 0.03, 0.02, "y", (0, 0.01, 0))
knob.box("white", (0.012, 0.03, 0.06), (0, 0.035, 0))
node("Reverser", t=tilt(DESK_C, DESK_T, -0.22, 0.03, 0.08), r=rot_x(DESK_T), mesh=knob, parent=root)

# --- push buttons: origin at the base, pressing moves them along -y
for name, x, z, mat in [("Btn_DoorLeft", 0.25, 0.0, "yellow"), ("Btn_DoorRight", 0.55, 0.0, "yellow"),
                        ("Btn_Horn", 0.05, 0.0, "white"), ("Btn_Ack", 0.05, 0.16, "blue"),
                        ("Btn_Headlights", 0.25, 0.16, "white"), ("Btn_EB", 0.55, 0.16, "red")]:
    b = Mesh()
    b.cylinder(mat, 0.018, 0.02, "y", (0, 0.01, 0))
    ring = Mesh()
    ring.cylinder("black", 0.026, 0.006, "y", (0, 0.003, 0))
    node(name + "_Ring", t=tilt(DESK_C, DESK_T, x, 0.02, z), r=rot_x(DESK_T), mesh=ring, parent=root)
    node(name, t=tilt(DESK_C, DESK_T, x, 0.03, z), r=rot_x(DESK_T), mesh=b, parent=root)

# --- gauges: textured faces in the body, needles rotate about their node's z
for name, x, face, limit in [("Speed", -0.40, "dial_speed", True), ("BC", 0.15, "dial_bc", False), ("MR", 0.40, "dial_mr", False)]:
    f = Mesh()
    f.cylinder(face, 0.10, 0.01, "z", (0, 0, 0), segments=48, face_uv=True)
    node("Dial_" + name, t=tilt(DASH_C, DASH_T, x, 0.03, -0.005), r=rot_x(DASH_T), mesh=f, parent=root)
    n = Mesh()
    n.box("black", (0.006, 0.09, 0.004), (0, 0.04, 0))
    n.cylinder("black", 0.012, 0.006, "z")
    node("Needle_" + name, t=tilt(DASH_C, DASH_T, x, 0.03, -0.013), r=rot_x(DASH_T), mesh=n, parent=root)
    if limit:
        m = Mesh()
        m.box("needle_red", (0.01, 0.02, 0.004), (0, 0.093, 0))
        node("Needle_" + name + "Limit", t=tilt(DASH_C, DASH_T, x, 0.03, -0.011), r=rot_x(DASH_T), mesh=m, parent=root)

# --- indicator lamps: an off and an on node at the same place
for name, x, mat in [("DoorsClosed", -0.36, "lamp_green"), ("EB", -0.24, "lamp_red"), ("ATP", -0.12, "lamp_amber"),
                     ("Overspeed", 0.0, "lamp_red")]:
    for state, m in (("Off", "lamp_off"), ("On", mat)):
        l = Mesh()
        l.box(m, (0.05, 0.03, 0.01))
        node(f"Lamp_{name}_{state}", t=tilt(DASH_C, DASH_T, x, -0.125, -0.005), r=rot_x(DASH_T), mesh=l, parent=root)


# ------------------------------------------------------------------ glTF writer
def write(path):
    blob = bytearray()
    views, accessors, meshes, materials, images = [], [], [], [], []
    mat_index = {}

    def view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        v = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target:
            v["target"] = target
        blob.extend(data)
        views.append(v)
        return len(views) - 1

    def accessor(values, kind, comp=5126):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[kind]
        flat = [c for v in values for c in (v if n > 1 else (v,))]
        fmt = "<%d%s" % (len(flat), "f" if comp == 5126 else "I")
        a = {"bufferView": view(struct.pack(fmt, *flat), 34963 if comp == 5125 else 34962),
             "componentType": comp, "count": len(values), "type": kind}
        if kind == "VEC3" and comp == 5126:
            a["min"] = [min(v[i] for v in values) for i in range(3)]
            a["max"] = [max(v[i] for v in values) for i in range(3)]
        accessors.append(a)
        return len(accessors) - 1

    for i, png_bytes in enumerate(TEXTURES):
        images.append({"bufferView": view(png_bytes), "mimeType": "image/png", "name": f"dial{i}"})

    def material(name):
        if name not in mat_index:
            d = MATERIALS[name]
            m = {"name": name, "doubleSided": True,
                 "pbrMetallicRoughness": {"baseColorFactor": list(d["color"]), "metallicFactor": 0.0, "roughnessFactor": 0.8}}
            if "emissive" in d:
                m["emissiveFactor"] = list(d["emissive"])
            if "texture" in d:
                m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": d["texture"]}
            materials.append(m)
            mat_index[name] = len(materials) - 1
        return mat_index[name]

    gl_nodes = []
    for nd in nodes:
        g = {"name": nd["name"]}
        if nd["children"]:
            g["children"] = nd["children"]
        if nd["t"] != (0, 0, 0):
            g["translation"] = [-nd["t"][0], nd["t"][1], nd["t"][2]]
        if nd["r"]:
            g["rotation"] = list(nd["r"])
        mesh = nd["mesh"]
        if mesh:
            prims = []
            for mat, (p, n, uv, ix) in mesh.prims.items():
                # mirror x (openBVE -> glTF); that also turns the clockwise faces counter-clockwise
                prims.append({"attributes": {"POSITION": accessor([(-a, b, c) for a, b, c in p], "VEC3"),
                                             "NORMAL": accessor([(-a, b, c) for a, b, c in n], "VEC3"),
                                             "TEXCOORD_0": accessor(uv, "VEC2")},
                              "indices": accessor(ix, "SCALAR", 5125), "material": material(mat)})
            meshes.append({"name": nd["name"], "primitives": prims})
            g["mesh"] = len(meshes) - 1
        gl_nodes.append(g)

    gltf = {"asset": {"version": "2.0", "generator": "openTrainSim tools/example_cab_glb.py"},
            "scene": 0, "scenes": [{"nodes": [0]}], "nodes": gl_nodes, "meshes": meshes, "materials": materials,
            "textures": [{"source": i, "sampler": 0} for i in range(len(images))], "images": images,
            "samplers": [{"wrapS": 33071, "wrapT": 33071}],
            "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    while len(blob) % 4:
        blob.append(0)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    with open(path, "wb") as f:
        f.write(out)
    print(f"{path}: {len(nodes)} nodes, {len(meshes)} meshes, {len(out)} bytes")


write(sys.argv[1] if len(sys.argv) > 1 else "examples/cab/desk.glb")
