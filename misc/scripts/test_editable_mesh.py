#!/usr/bin/env python3
"""Check EditableMeshInstance3D, headless: a MeshInstance3D whose mesh CSG shapes cut into.

A box with vertex colors, UV2 and its own material gets a hole cut through it: the hole's
walls take the cutter's material and face into the hole, the rest of the box keeps its
colors, UV2 and normals. A low-poly sphere that is cut keeps its own smooth normals instead
of CSG's. Union and intersection work, an open mesh is shown unchanged with a warning,
hidden shapes are ignored, apply_edits() makes the result the new source, a MeshInstance3D
changed to this type brings its mesh along, the node's own MeshInstance3D settings survive,
and nothing is recomputed at runtime unless update_at_runtime is on. Level meshes made of
closed blocks touching along edges, with T-junctions closed by zero-area slivers, can be cut."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node3D

var fails := 0


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func material(p_color: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = p_color
	return m


# A 2 m box as an ArrayMesh with red vertex colors, UV2 and one material.
func colored_box(p_material: Material) -> ArrayMesh:
	var box := BoxMesh.new()
	box.size = Vector3(2, 2, 2)
	var arrays := box.surface_get_arrays(0)
	var count: int = arrays[Mesh.ARRAY_VERTEX].size()
	var colors := PackedColorArray()
	var uv2 := PackedVector2Array()
	for i in count:
		colors.append(Color(1, 0, 0, 1))
		uv2.append(Vector2(0.25, 0.75))
	arrays[Mesh.ARRAY_COLOR] = colors
	arrays[Mesh.ARRAY_TEX_UV2] = uv2
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, p_material)
	return mesh


func cutter(p_size: Vector3, p_position: Vector3, p_operation: int, p_material: Material = null) -> CSGBox3D:
	var shape := CSGBox3D.new()
	shape.size = p_size
	shape.position = p_position
	shape.operation = p_operation
	shape.material = p_material
	return shape


# Distances at which a ray meets a mesh.
func through(p_mesh: Mesh, p_from: Vector3, p_direction: Vector3) -> Array:
	var faces := p_mesh.get_faces()
	var found := []
	for i in range(0, faces.size(), 3):
		var hit = Geometry3D.ray_intersects_triangle(p_from, p_direction, faces[i], faces[i + 1], faces[i + 2])
		if hit != null:
			found.append(snappedf((hit - p_from).length(), 0.001))
	return found


# Two 1 m cubes in one mesh, touching along the edge x = 0.5, z = 0.5.
func two_blocks_touching() -> ArrayMesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for offset in [Vector3.ZERO, Vector3(1, 0, 1)]:
		var arrays := BoxMesh.new().surface_get_arrays(0)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for index in arrays[Mesh.ARRAY_INDEX]:
			st.add_vertex(vertices[index] + offset)
	return st.commit()


# A 2 m cube whose front face is split at the middle of its top edge while the top face
# keeps that edge whole, closed with a zero-area sliver.
func cube_with_t_junction() -> ArrayMesh:
	var p := func(x, y, z): return Vector3(x, y, z)
	var triangles := []
	var quad := func(a, b, c, d): triangles.append_array([[a, b, c], [a, c, d]])
	quad.call(p.call(-1, -1, -1), p.call(1, -1, -1), p.call(1, -1, 1), p.call(-1, -1, 1)) # Bottom.
	quad.call(p.call(-1, 1, 1), p.call(1, 1, 1), p.call(1, 1, -1), p.call(-1, 1, -1)) # Top, edge (-1,1,1)-(1,1,1) whole.
	quad.call(p.call(-1, -1, -1), p.call(-1, 1, -1), p.call(1, 1, -1), p.call(1, -1, -1)) # Back.
	quad.call(p.call(-1, -1, -1), p.call(-1, -1, 1), p.call(-1, 1, 1), p.call(-1, 1, -1)) # Left.
	quad.call(p.call(1, -1, -1), p.call(1, 1, -1), p.call(1, 1, 1), p.call(1, -1, 1)) # Right.
	var mid: Vector3 = p.call(0, 1, 1)
	triangles.append_array([[p.call(-1, -1, 1), p.call(1, -1, 1), mid], [p.call(-1, -1, 1), mid, p.call(-1, 1, 1)], [p.call(1, -1, 1), p.call(1, 1, 1), mid]]) # Front, split.
	# Godot winds front faces clockwise: a triangle's (b - a) x (c - a) points into the cube.
	for tri in triangles:
		var center: Vector3 = (tri[0] + tri[1] + tri[2]) / 3.0
		if (tri[1] - tri[0]).cross(tri[2] - tri[0]).dot(center) > 0.0:
			var swap = tri[1]
			tri[1] = tri[2]
			tri[2] = swap
	# The sliver runs against the halfedges already there, so every edge pairs up.
	var directed := {}
	for tri in triangles:
		for k in 3:
			directed[[tri[k], tri[(k + 1) % 3]]] = true
	var tl: Vector3 = p.call(-1, 1, 1)
	var tr: Vector3 = p.call(1, 1, 1)
	triangles.append([tr, tl, mid] if directed.has([tl, tr]) else [tl, tr, mid])
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for tri in triangles:
		for corner in tri:
			st.add_vertex(corner)
	return st.commit()


func frames(p_count: int) -> void:
	for i in p_count:
		await get_tree().process_frame


func _ready() -> void:
	var red := material(Color.RED)
	var blue := material(Color.BLUE)

	# 1. A hole through a box.
	var node := EditableMeshInstance3D.new()
	node.update_at_runtime = true
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	node.material_overlay = material(Color(1, 1, 1, 0.1))
	var source := colored_box(red)
	node.source_mesh = source
	add_child(node)
	await frames(2)
	check("without shapes the mesh is the source itself", node.mesh == source)
	node.add_child(cutter(Vector3(1, 1, 4), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION, blue))
	await frames(2)
	var mesh: Mesh = node.mesh
	check("a shape makes a new mesh", mesh != null and mesh != source)
	check("two surfaces: the box's, then the cutter's", mesh.get_surface_count() == 2, mesh.get_surface_count())
	check("surface 0 keeps the box's material, surface 1 has the cutter's", mesh.surface_get_material(0) == red and mesh.surface_get_material(1) == blue)
	var walls := mesh.surface_get_arrays(1)
	var inside := true
	var facing := 0
	var wall_vertices: PackedVector3Array = walls[Mesh.ARRAY_VERTEX]
	var wall_normals: PackedVector3Array = walls[Mesh.ARRAY_NORMAL]
	for i in wall_vertices.size():
		var v := wall_vertices[i]
		inside = inside and absf(v.x) <= 0.501 and absf(v.y) <= 0.501
		# The hole's walls face into the hole: towards its axis.
		var to_axis := Vector3(-v.x, -v.y, 0)
		if to_axis.length() > 0.01 and wall_normals[i].dot(to_axis.normalized()) > 0.5:
			facing += 1
	check("the cutter's faces are the hole's walls", inside and wall_vertices.size() > 0, wall_vertices.size())
	check("and they face into the hole", facing == wall_vertices.size(), [facing, wall_vertices.size()])
	var box := mesh.surface_get_arrays(0)
	var colors: PackedColorArray = box[Mesh.ARRAY_COLOR]
	var uv2: PackedVector2Array = box[Mesh.ARRAY_TEX_UV2]
	var box_vertices: PackedVector3Array = box[Mesh.ARRAY_VERTEX]
	var box_normals: PackedVector3Array = box[Mesh.ARRAY_NORMAL]
	var kept_color := colors.size() == box_vertices.size()
	for c in colors:
		kept_color = kept_color and c.is_equal_approx(Color(1, 0, 0, 1))
	var kept_uv2 := uv2.size() == box_vertices.size()
	for uv in uv2:
		kept_uv2 = kept_uv2 and uv.is_equal_approx(Vector2(0.25, 0.75))
	check("the box keeps its vertex colors", kept_color, colors.size())
	check("and its UV2", kept_uv2, uv2.size())
	var outward := 0
	for i in box_vertices.size():
		# Every normal is one of the six axes, and the vertex lies on that face of the box.
		var n := box_normals[i]
		var axis := Vector3(roundf(n.x), roundf(n.y), roundf(n.z))
		if n.dot(axis) > 0.99 and absf(box_vertices[i].dot(axis) - 1.0) < 0.001:
			outward += 1
	check("and its normals, pointing out of each face", outward == box_vertices.size(), [outward, box_vertices.size()])
	check("the front face now has a hole", mesh.get_faces().size() > source.get_faces().size(), [source.get_faces().size() / 3, mesh.get_faces().size() / 3])
	check("the node's own settings are untouched", node.cast_shadow == GeometryInstance3D.SHADOW_CASTING_SETTING_OFF and node.material_overlay != null)

	# 2. A union sticking out, and a hidden shape, which is ignored.
	var bump := cutter(Vector3(1, 1, 1), Vector3(1.4, 0, 0), CSGShape3D.OPERATION_UNION)
	node.add_child(bump)
	var hidden := cutter(Vector3(3, 3, 3), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION)
	hidden.visible = false
	node.add_child(hidden)
	await frames(2)
	var aabb: AABB = node.mesh.get_aabb()
	check("a union adds to the mesh", absf(aabb.end.x - 1.9) < 0.01, aabb)
	check("a hidden shape changes nothing", node.mesh.get_aabb().size.y > 1.9)
	check("a union without a material takes the box's", node.mesh.get_surface_count() == 2 and node.mesh.surface_get_material(0) == red, node.mesh.get_surface_count())
	bump.queue_free()
	hidden.queue_free()
	await frames(2)

	# 3. apply_edits(): the result becomes the source, the shapes go.
	var before_apply: Mesh = node.mesh
	node.apply_edits()
	await frames(2)
	var shapes := node.get_children().filter(func(c): return c is CSGShape3D)
	check("apply_edits removes the shapes", shapes.is_empty(), shapes.size())
	check("and makes the result the new source", node.source_mesh == before_apply and node.mesh == before_apply)

	# 4. Intersection with a sphere-shaped cutter leaves a rounded box.
	var rounded := EditableMeshInstance3D.new()
	rounded.update_at_runtime = true
	rounded.source_mesh = colored_box(red)
	add_child(rounded)
	var ball := CSGSphere3D.new()
	ball.radius = 1.3
	ball.operation = CSGShape3D.OPERATION_INTERSECTION
	rounded.add_child(ball)
	await frames(2)
	var corner_gone := true
	for v in rounded.mesh.get_faces():
		corner_gone = corner_gone and v.length() <= 1.31
	check("an intersection trims the box to the sphere", corner_gone)

	# 5. A low-poly sphere keeps its own smooth normals where it isn't cut.
	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 12
	sphere.rings = 6
	var smooth := EditableMeshInstance3D.new()
	smooth.update_at_runtime = true
	smooth.source_mesh = sphere
	add_child(smooth)
	smooth.add_child(cutter(Vector3(1.5, 3, 3), Vector3(1.25, 0, 0), CSGShape3D.OPERATION_SUBTRACTION, blue))
	await frames(2)
	var shell := smooth.mesh.surface_get_arrays(0)
	var shell_vertices: PackedVector3Array = shell[Mesh.ARRAY_VERTEX]
	var shell_normals: PackedVector3Array = shell[Mesh.ARRAY_NORMAL]
	var worst := 0.0
	for i in shell_vertices.size():
		if shell_vertices[i].length() > 0.9:
			worst = maxf(worst, (shell_normals[i] - shell_vertices[i].normalized()).length())
	check("the cut sphere keeps its smooth normals (worst deviation %.3f)" % worst, worst < 0.05, worst)
	var cut_face := false
	for v in smooth.mesh.surface_get_arrays(1)[Mesh.ARRAY_VERTEX]:
		cut_face = cut_face or absf(v.x - 0.5) < 0.001
	check("and the cut is flat, with the cutter's material", cut_face and smooth.mesh.surface_get_material(1) == blue)

	# 6. An open mesh can't be cut: shown unchanged, with a warning.
	var plane := PlaneMesh.new()
	var open := EditableMeshInstance3D.new()
	open.update_at_runtime = true
	open.source_mesh = plane
	add_child(open)
	open.add_child(cutter(Vector3(0.5, 0.5, 0.5), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION))
	await frames(2)
	check("an open mesh is shown unchanged", open.mesh == plane)
	check("and says it is not closed", not open.is_source_closed())

	# 7. A MeshInstance3D changed to this type brings its mesh: it becomes the source.
	var adopted := EditableMeshInstance3D.new()
	var plain_box := BoxMesh.new()
	adopted.mesh = plain_box
	add_child(adopted)
	await frames(1)
	check("the mesh it had becomes the source", adopted.source_mesh == plain_box and adopted.mesh == plain_box)

	# 8. At runtime nothing is recomputed unless update_at_runtime is on: the saved result is used.
	var still := EditableMeshInstance3D.new()
	still.source_mesh = colored_box(red)
	add_child(still)
	await frames(1)
	var saved: Mesh = still.mesh
	still.add_child(cutter(Vector3(1, 1, 4), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION))
	await frames(2)
	check("at runtime the shapes don't recompute the mesh by themselves", still.mesh == saved)
	still.rebuild()
	check("rebuild() does it on request", still.mesh != saved and still.mesh.get_faces().size() > saved.get_faces().size())
	check("a cutter without a material joins the box's surface", still.mesh.get_surface_count() == 1, still.mesh.get_surface_count())

	# 9. Plain CSG is unchanged: a combiner root with a subtracting box still builds its mesh.
	var combiner := CSGCombiner3D.new()
	var block := CSGBox3D.new()
	block.size = Vector3(2, 2, 2)
	combiner.add_child(block)
	combiner.add_child(cutter(Vector3(1, 1, 4), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION))
	add_child(combiner)
	await frames(3)
	var meshes := combiner.get_meshes()
	check("plain CSG roots still build their mesh", combiner.is_root_shape() and meshes.size() == 2 and (meshes[1] as Mesh).get_faces().size() > 36, meshes.size())
	check("and a shape under an editable mesh is not a root", not (node.get_child_count() > 0 and node.get_child(0) is CSGShape3D and node.get_child(0).is_root_shape()))

	# 10. Level meshes: closed blocks in one mesh that touch along an edge, the way wall
	# pieces meet at corners. Manifold refuses such a mesh as a whole; it is cut block by block.
	var corner_mesh := two_blocks_touching()
	var corner := EditableMeshInstance3D.new()
	corner.update_at_runtime = true
	corner.source_mesh = corner_mesh
	add_child(corner)
	corner.add_child(cutter(Vector3(0.4, 2, 0.4), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION))
	await frames(2)
	check("blocks touching along an edge can be cut", corner.is_source_closed() and through(corner.mesh, Vector3(0, -2, 0), Vector3.UP).is_empty(), through(corner.mesh, Vector3(0, -2, 0), Vector3.UP))
	check("and the other block is untouched", through(corner.mesh, Vector3(1.13, -2, 0.91), Vector3.UP).size() == 2, through(corner.mesh, Vector3(1.13, -2, 0.91), Vector3.UP))

	# 11. A T-junction closed with a sliver whose corners lie on one line, as exporters leave
	# them: the sliver keeps the mesh closed, so it must not be thrown away.
	var t_mesh := cube_with_t_junction()
	var t_node := EditableMeshInstance3D.new()
	t_node.update_at_runtime = true
	t_node.source_mesh = t_mesh
	add_child(t_node)
	t_node.add_child(cutter(Vector3(0.5, 3, 0.5), Vector3.ZERO, CSGShape3D.OPERATION_SUBTRACTION))
	await frames(2)
	check("a cube with a T-junction sliver can be cut", t_node.is_source_closed() and through(t_node.mesh, Vector3(0, -2, 0), Vector3.UP).is_empty())

	print("EDITABLE_MESH fails=%d" % fails)
	get_tree().quit()
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-editable-mesh-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        if args.engine.name.endswith(".console.exe"):
            main_exe = args.engine.resolve().with_name(args.engine.name.replace(".console.exe", ".exe"))
            shutil.copy2(main_exe, runtime / main_exe.name)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        (project / "test.gd").write_text(TEST, encoding="utf-8")
        (project / "main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n\n[ext_resource type="Script" path="res://test.gd" id="1"]\n\n'
            '[node name="Main" type="Node3D"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Editable mesh {work.name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "EDITABLE_MESH fails=0", work)
        log = (work / "EDITABLE_MESH fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL", "INFO"))), flush=True)
        print("EDITABLE_MESH_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
