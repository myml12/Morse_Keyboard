import bpy
import math
import os
from mathutils import Vector


# Morse keyer concept model, dimensions are millimetres.
# SS-10GL13 dimensions are based on the Omron SS-series datasheet.
OUTPUT_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "outputs", "blender_keyer")
)
os.makedirs(OUTPUT_DIR, exist_ok=True)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for datablocks in (bpy.data.meshes, bpy.data.curves, bpy.data.materials, bpy.data.cameras, bpy.data.lights):
        pass


def material(name, color, metallic=0.0, roughness=0.45, transmission=0.0, alpha=1.0):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.diffuse_color = (*color, alpha)
    mat.use_nodes = True
    bsdf = next(
        node for node in mat.node_tree.nodes if node.type == "BSDF_PRINCIPLED"
    )
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    if "Transmission Weight" in bsdf.inputs:
        bsdf.inputs["Transmission Weight"].default_value = transmission
    elif "Transmission" in bsdf.inputs:
        bsdf.inputs["Transmission"].default_value = transmission
    if transmission > 0:
        bsdf.inputs["IOR"].default_value = 1.47
    return mat


def assign(obj, mat):
    obj.data.materials.append(mat)
    return obj


def rounded_box(name, size, location, radius, mat, bevel_segments=5):
    bpy.ops.mesh.primitive_cube_add(location=location)
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bevel = obj.modifiers.new("Edge radius", "BEVEL")
    bevel.width = radius
    bevel.segments = bevel_segments
    bevel.limit_method = "ANGLE"
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=bevel.name)
    assign(obj, mat)
    return obj


def cylinder(name, radius, depth, location, rotation, mat, vertices=64):
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=vertices,
        radius=radius,
        depth=depth,
        location=location,
        rotation=rotation,
    )
    obj = bpy.context.object
    obj.name = name
    assign(obj, mat)
    return obj


def boolean_difference(target, cutter):
    mod = target.modifiers.new("Cut", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.solver = "EXACT"
    mod.object = cutter
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.data.objects.remove(cutter, do_unlink=True)


def ribbon_mesh(name, points, width, thickness, mat):
    # A rectangular-section ribbon following an x/z polyline. Width is local Y.
    verts = []
    for x, z in points:
        verts.extend([
            (x, -width / 2, z - thickness / 2),
            (x, width / 2, z - thickness / 2),
            (x, width / 2, z + thickness / 2),
            (x, -width / 2, z + thickness / 2),
        ])
    faces = []
    for i in range(len(points) - 1):
        a, b = i * 4, (i + 1) * 4
        faces.extend([
            (a, b, b + 1, a + 1),
            (a + 1, b + 1, b + 2, a + 2),
            (a + 2, b + 2, b + 3, a + 3),
            (a + 3, b + 3, b, a),
        ])
    faces.append((0, 1, 2, 3))
    n = (len(points) - 1) * 4
    faces.append((n + 3, n + 2, n + 1, n))
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    bevel = obj.modifiers.new("Stamped edge softening", "BEVEL")
    bevel.width = 0.10
    bevel.segments = 2
    assign(obj, mat)
    return obj


def parent_local(obj, parent):
    obj.parent = parent
    return obj


def create_ss10gl13(name, location, rotation_y, mirror_leaf=False):
    root = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(root)
    root.location = location
    root.rotation_euler = (0.0, rotation_y, 0.0)

    black = bpy.data.materials["Switch black"]
    steel = bpy.data.materials["Stainless"]
    orange = bpy.data.materials["Plunger orange"]

    # Omron nominal body envelope: 19.8 x 6.4 x 10.2 mm.
    body = rounded_box(name + "_BODY", (19.8, 6.4, 10.2), (0, 0, 0), 0.7, black, 3)
    parent_local(body, root)

    # Face mounting holes, represented with true dark bores.
    for hx in (-4.75, 4.75):
        bore = cylinder(
            name + "_MOUNT_HOLE",
            1.18,
            0.35,
            (hx, -3.23, -0.1),
            (math.radians(90), 0, 0),
            bpy.data.materials["Hole black"],
            40,
        )
        parent_local(bore, root)

    # Three solder lugs; they are part of the switch and remain inside the enclosure.
    for idx, x in enumerate((-5.0, 0.0, 5.0), start=1):
        lug = rounded_box(
            f"{name}_TERMINAL_{idx}",
            (1.6, 0.55, 4.8),
            (x, 0, -7.45),
            0.15,
            steel,
            2,
        )
        parent_local(lug, root)

    plunger = rounded_box(
        name + "_YELLOW_PLUNGER",
        (2.1, 3.0, 1.35),
        (-1.5, 0, 5.65),
        0.35,
        orange,
        3,
    )
    parent_local(plunger, root)

    # One continuous stamped steel actuator. The first points form the folded,
    # fixed root on the body. It then passes over the yellow plunger and curls
    # at the distal end. It is never detached from this switch assembly.
    points = [
        (-8.7, 5.05),
        (-8.7, 5.75),
        (-7.5, 5.85),
        (-5.5, 6.00),
        (3.8, 8.25),
        (6.4, 8.35),
        (7.35, 8.10),
        (7.90, 7.55),
        (8.00, 6.90),
        (7.70, 6.35),
        (7.25, 6.15),
    ]
    if mirror_leaf:
        points = [(-x, z) for x, z in points]
    leaf = ribbon_mesh(name + "_ATTACHED_LEAF", points, 3.6, 0.30, steel)
    parent_local(leaf, root)

    rivet_x = 8.15 if mirror_leaf else -8.15
    rivet = cylinder(
        name + "_LEAF_RIVET",
        0.75,
        0.40,
        (rivet_x, 0, 5.95),
        (math.radians(90), 0, 0),
        steel,
        32,
    )
    parent_local(rivet, root)
    return root


def add_camera(name, location, target, lens=54):
    bpy.ops.object.camera_add(location=location)
    cam = bpy.context.object
    cam.name = name
    cam.data.lens = lens
    direction = Vector(target) - cam.location
    cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    return cam


def add_area_light(name, location, energy, size, color=(1, 1, 1)):
    bpy.ops.object.light_add(type="AREA", location=location)
    light = bpy.context.object
    light.name = name
    light.data.energy = energy
    light.data.shape = "DISK"
    light.data.size = size
    light.data.color = color
    return light


def point_light_at(light, target):
    light.rotation_euler = (Vector(target) - light.location).to_track_quat("-Z", "Y").to_euler()


clear_scene()

# Materials
graphite = material("Graphite enclosure", (0.25, 0.29, 0.34), roughness=0.36)
black = material("Switch black", (0.020, 0.024, 0.030), roughness=0.30)
hole_black = material("Hole black", (0.004, 0.005, 0.007), roughness=0.45)
steel = material("Stainless", (0.56, 0.60, 0.63), metallic=0.92, roughness=0.20)
orange = material("Plunger orange", (0.92, 0.34, 0.025), roughness=0.38)
screen_mat = material("OLED glass", (0.003, 0.008, 0.015), metallic=0.05, roughness=0.12)
screen_glow = material("OLED blue", (0.02, 0.45, 0.95), roughness=0.22)
clear = material("Clear knob", (0.62, 0.74, 0.84), roughness=0.10, transmission=0.36, alpha=0.72)
magnet_mat = material("Nickel magnets", (0.47, 0.50, 0.52), metallic=0.94, roughness=0.18)
rubber = material("Rubber", (0.008, 0.009, 0.010), roughness=0.78)
pcb_mat = material("PCB green", (0.015, 0.17, 0.09), metallic=0.05, roughness=0.35)

# Enclosure: 65 x 60 x 40 mm, table at z=0.
body = rounded_box("HOUSING_BODY", (65, 60, 40), (0, 0, 20.5), 4.0, graphite, 7)

# Front service cavity. The rear plate closes the cavity while a narrow central
# shroud hides both complete switch bodies. Only the distal ends of the original
# attached leaves extend sideways beyond that shroud.
cavity = rounded_box("CAVITY_CUTTER", (52, 15, 25), (0, -27.0, 25.0), 2.0, graphite, 3)
boolean_difference(body, cavity)

front_panel = rounded_box("FRONT_CAVITY_BACKPLATE", (55, 1.8, 25), (0, -26.0, 25.0), 2.2, graphite, 5)
switch_shroud = rounded_box("SWITCH_BODY_SHROUD", (36.0, 8.0, 25.0), (0, -29.1, 25.0), 1.6, graphite, 6)

# Bottom seam/base plate.
base = rounded_box("BOTTOM_PLATE", (64, 59, 4.0), (0, 0, 2.2), 3.5, graphite, 6)

# PCB inside, kept low.
pcb = rounded_box("MAIN_PCB", (55, 47, 1.6), (0, 2, 10.0), 0.8, pcb_mat, 3)

# Complete, inseparable switch assemblies. Rotated so their plunger motion is
# horizontal; the original attached leaves become the actual squeeze paddles.
left_switch = create_ss10gl13(
    "SS10GL13_LEFT_DOT", (-11.5, -29.0, 22.5), -math.pi / 2, mirror_leaf=False
)
right_switch = create_ss10gl13(
    "SS10GL13_RIGHT_DASH", (11.5, -29.0, 22.5), math.pi / 2, mirror_leaf=True
)

# OLED and a recessed bezel on the top.
oled_bezel = rounded_box("OLED_BEZEL", (29.0, 17.0, 1.5), (-10.5, 2.0, 41.0), 2.0, hole_black, 5)
oled = rounded_box("OLED_SCREEN", (25.5, 13.5, 0.9), (-10.5, 2.0, 41.85), 1.2, screen_mat, 5)
for i, width in enumerate((15.5, 18.0, 11.0)):
    line = rounded_box(
        f"OLED_PIXEL_LINE_{i}",
        (width, 0.42, 0.08),
        (-10.5, -1.0 + i * 2.5, 42.34),
        0.12,
        screen_glow,
        2,
    )

# Encoder shaft and clear knob.
cylinder("ENCODER_COLLAR", 3.6, 2.0, (16.5, 2.0, 41.5), (0, 0, 0), steel)
knob = cylinder("CLEAR_ENCODER_KNOB", 8.0, 13.0, (16.5, 2.0, 48.0), (0, 0, 0), clear, 72)
bevel = knob.modifiers.new("Knob edge", "BEVEL")
bevel.width = 0.8
bevel.segments = 4

# Side 3.5 mm jack.
cylinder("TRS_JACK_RING", 3.5, 1.5, (33.0, 7.0, 23.0), (0, math.pi / 2, 0), steel)
cylinder("TRS_JACK_HOLE", 2.2, 1.7, (33.5, 7.0, 23.0), (0, math.pi / 2, 0), hole_black)

# Rear USB-C visual opening.
usb = rounded_box("USB_C_OPENING", (11.0, 1.3, 4.2), (-16.0, 30.1, 15.0), 1.6, hole_black, 5)

# Four flush magnetic retention points and separate anti-scratch feet.
for x in (-24.0, 24.0):
    for y in (-20.0, 20.0):
        cylinder("RECESSED_MAGNET", 4.5, 1.6, (x, y, 0.65), (0, 0, 0), magnet_mat)
for x in (-28.0, 28.0):
    for y in (-24.0, 24.0):
        cylinder("RUBBER_PAD", 2.5, 1.0, (x, y, 0.15), (0, 0, 0), rubber, 48)

# Ground plane / desk.
desk = rounded_box("DESK", (240, 200, 4), (0, 0, -3.0), 2.0, material("Desk", (0.42, 0.31, 0.20), roughness=0.60), 4)

# Studio setup.
bpy.context.scene.render.engine = "BLENDER_EEVEE_NEXT"
bpy.context.scene.render.resolution_x = 1400
bpy.context.scene.render.resolution_y = 1000
bpy.context.scene.render.resolution_percentage = 100
bpy.context.scene.render.image_settings.file_format = "PNG"
bpy.context.scene.render.film_transparent = False
bpy.context.scene.world.use_nodes = True
world_bg = next(node for node in bpy.context.scene.world.node_tree.nodes if node.type == "BACKGROUND")
world_bg.inputs["Color"].default_value = (0.12, 0.14, 0.18, 1.0)
world_bg.inputs["Strength"].default_value = 0.55
bpy.context.scene.view_settings.look = "AgX - Medium High Contrast"
bpy.context.scene.view_settings.exposure = 1.75

key = add_area_light("Key light", (-65, -70, 105), 1450, 70)
point_light_at(key, (0, 0, 22))
fill = add_area_light("Fill light", (75, -15, 65), 1100, 55, (0.80, 0.88, 1.0))
point_light_at(fill, (0, 0, 20))
rim = add_area_light("Rim light", (0, 75, 90), 1200, 50, (1.0, 0.90, 0.76))
point_light_at(rim, (0, 0, 24))

# Exterior render: the central shroud hides every switch body and terminal;
# only the original attached actuator ends extend from its left and right sides.
exterior_cam = add_camera("Exterior camera", (48, -125, 76), (0, -3, 23), 58)
bpy.context.scene.camera = exterior_cam
bpy.context.scene.render.filepath = os.path.join(OUTPUT_DIR, "morse_keyer_exterior.png")
bpy.ops.render.render(write_still=True)

# Mechanical proof render with the removable cover hidden.
front_panel.hide_render = True
switch_shroud.hide_render = True
cutaway_cam = add_camera("Switch mechanism camera", (0, -132, 55), (0, -23, 24), 64)
bpy.context.scene.camera = cutaway_cam
bpy.context.scene.render.filepath = os.path.join(OUTPUT_DIR, "morse_keyer_switch_mechanism.png")
bpy.ops.render.render(write_still=True)
front_panel.hide_render = False
switch_shroud.hide_render = False

# Underside render to verify magnetic base.
bottom_keep_prefixes = ("BOTTOM_PLATE", "RECESSED_MAGNET", "RUBBER_PAD")
bottom_hidden_state = {}
for obj in bpy.context.scene.objects:
    if obj.type == "MESH":
        bottom_hidden_state[obj.name] = obj.hide_render
        obj.hide_render = not obj.name.startswith(bottom_keep_prefixes)
bottom_cam = add_camera("Bottom camera", (78, -78, -86), (0, 0, 1), 62)
bpy.context.scene.camera = bottom_cam
desk.hide_render = True
bpy.context.scene.render.filepath = os.path.join(OUTPUT_DIR, "morse_keyer_magnetic_base.png")
bpy.ops.render.render(write_still=True)

# Restore normal viewport state and save an editable .blend.
for name, was_hidden in bottom_hidden_state.items():
    bpy.data.objects[name].hide_render = was_hidden
desk.hide_render = False
bpy.context.scene.camera = exterior_cam
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUTPUT_DIR, "morse_keyer_concept.blend"))

print("Created:", OUTPUT_DIR)
