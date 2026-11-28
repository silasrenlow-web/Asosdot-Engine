# Headless checks for QuickMeshBrush3D (bend, detail, seams, normals, order, rotation,
# collision, apply, save/load, timing). Run from an empty project folder holding this file:
#   bin/asosdot.windows.editor.x86_64.exe --headless --path <folder> --script res://test_quick_mesh_brush.gd
extends SceneTree

var failures := 0
var checks := 0

func check(ok: bool, what: String) -> void:
	checks += 1
	if ok:
		print("  ok   ", what)
	else:
		failures += 1
		print("  FAIL ", what)

func tri_count(mesh: Mesh) -> int:
	var n := 0
	for s in mesh.get_surface_count():
		var a = mesh.surface_get_arrays(s)
		n += a[Mesh.ARRAY_INDEX].size() / 3 if a[Mesh.ARRAY_INDEX] else a[Mesh.ARRAY_VERTEX].size() / 3
	return n

func key(v: Vector3) -> String:
	return "%.5f,%.5f,%.5f" % [v.x, v.y, v.z]

# Every edge (by position) must be used by exactly two faces, except boundary edges (one).
# Returns [non_manifold_count, boundary_length].
func edge_stats(mesh: Mesh) -> Array:
	var edges := {}
	var pos := {}
	for s in mesh.get_surface_count():
		var a = mesh.surface_get_arrays(s)
		var v: PackedVector3Array = a[Mesh.ARRAY_VERTEX]
		var idx: PackedInt32Array = a[Mesh.ARRAY_INDEX]
		for t in range(0, idx.size(), 3):
			for e in 3:
				var p0 := v[idx[t + e]]
				var p1 := v[idx[t + (e + 1) % 3]]
				var k0 := key(p0)
				var k1 := key(p1)
				var k := k0 + "|" + k1 if k0 < k1 else k1 + "|" + k0
				edges[k] = edges.get(k, 0) + 1
				pos[k] = p0.distance_to(p1)
	var bad := 0
	var boundary := 0.0
	for k in edges:
		if edges[k] == 1:
			boundary += pos[k]
		elif edges[k] != 2:
			bad += 1
	return [bad, boundary]

func height_at(brush: QuickMeshBrush3D, x: float, z: float) -> float:
	var hit := brush.intersect_ray(Vector3(x, 50, z), Vector3.DOWN)
	return hit.position.y if not hit.is_empty() else NAN

func _init() -> void:
	await process_frame
	var root := Node3D.new()
	get_root().add_child(root)

	print("Flat floor, two triangles")
	var plane := PlaneMesh.new()
	plane.size = Vector2(10, 10)
	var brush := QuickMeshBrush3D.new()
	brush.update_at_runtime = true
	brush.source_mesh = plane
	root.add_child(brush)
	brush.rebuild()
	check(tri_count(brush.mesh) == 2, "no points: the source mesh, %d triangles" % tri_count(brush.mesh))
	check(brush.mesh == plane, "no points: mesh is the source itself")

	var p := brush.add_point(Vector3.ZERO, Vector3.UP, 2.0, 0.0)
	brush.rebuild()
	check(not brush.has_edits(), "a fresh point bends nothing")
	var rest := brush.get_point_rest(p)
	brush.set_point_transform(p, rest.translated(Vector3(0, -0.5, 0)))
	brush.rebuild()
	var tris := tri_count(brush.mesh)
	check(brush.has_edits(), "moved point has edits")
	check(tris > 200 and tris < 20000, "detail added around the point: %d triangles" % tris)
	check(absf(height_at(brush, 0, 0) + 0.5) < 0.01, "centre dips 0.5: %.4f" % height_at(brush, 0, 0))
	check(absf(height_at(brush, 2.5, 0)) < 0.0001, "outside the radius stays flat: %.5f" % height_at(brush, 2.5, 0))
	check(absf(height_at(brush, 4.9, 4.9)) < 0.0001, "far corner untouched")
	var h1 := height_at(brush, 1.0, 0)
	check(h1 < -0.1 and h1 > -0.45, "halfway is partly bent: %.4f" % h1)
	var stats := edge_stats(brush.mesh)
	check(stats[0] == 0, "watertight: %d bad edges" % stats[0])
	check(absf(stats[1] - 40.0) < 0.001, "boundary is the 40 m perimeter: %.3f" % stats[1])

	# Smoothness: heights along a line change gradually (no steps bigger than a few cm).
	var worst := 0.0
	var prev := height_at(brush, -2.5, 0.01)
	for i in range(1, 101):
		var x := -2.5 + 5.0 * i / 100.0
		var hh := height_at(brush, x, 0.01)
		worst = maxf(worst, absf(hh - prev))
		prev = hh
	check(worst < 0.06, "smooth profile, biggest step over 5 cm: %.4f" % worst)

	# Normals: up at the centre, tilted on the slope.
	var arrays := brush.mesh.surface_get_arrays(0)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var norms: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var best_c := 0
	var best_s := 0
	for i in verts.size():
		if verts[i].length() < verts[best_c].length():
			best_c = i
		if absf(Vector2(verts[i].x, verts[i].z).length() - 1.0) < absf(Vector2(verts[best_s].x, verts[best_s].z).length() - 1.0):
			best_s = i
	check(norms[best_c].dot(Vector3.UP) > 0.99, "normal at the centre points up: %s" % norms[best_c])
	check(norms[best_s].dot(Vector3.UP) < 0.97, "normal on the slope is tilted: %s" % norms[best_s])
	var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
	check(uvs.size() == verts.size() and uvs[best_c].distance_to(Vector2(0.5, 0.5)) < 0.05, "UVs carried to new vertices: %s" % uvs[best_c])

	print("Hardness keeps a flat floor")
	brush.set_point_hardness(p, 0.7)
	brush.rebuild()
	check(absf(height_at(brush, 1.2, 0) + 0.5) < 0.005, "inside the hard part fully bent: %.4f" % height_at(brush, 1.2, 0))

	print("Points apply in order")
	var hit := brush.intersect_ray(Vector3(0, 10, 0), Vector3.DOWN)
	var q := brush.add_point(hit.position, hit.normal, 0.5, 0.0)
	check(brush.get_point_rest(q).origin.distance_to(Vector3(0, -0.5, 0)) < 0.01, "second point sits on the dip")
	check(brush.get_point_surface_position(q).distance_to(Vector3.ZERO) < 0.01, "its surface position is on the unbent floor")
	brush.set_point_transform(q, brush.get_point_rest(q).translated(Vector3(0, -0.3, 0)))
	brush.rebuild()
	check(absf(height_at(brush, 0, 0) + 0.8) < 0.01, "deepened to 0.8: %.4f" % height_at(brush, 0, 0))
	check(absf(height_at(brush, 1.2, 0) + 0.5) < 0.005, "rest of the dip unchanged")

	print("Rotation tilts the area")
	var data := brush.get_points_data()
	brush.clear_points()
	var r := brush.add_point(Vector3(2, 0, 2), Vector3.UP, 1.5, 0.6)
	brush.set_point_transform(r, Transform3D(Basis(Vector3(0, 0, 1), deg_to_rad(20)), Vector3(2, 0, 2)))
	brush.rebuild()
	var slope := (height_at(brush, 2.4, 2) - height_at(brush, 1.6, 2)) / 0.8
	check(absf(slope - tan(deg_to_rad(20))) < 0.03, "slope inside the hard part matches 20 degrees: %.3f" % slope)

	print("Undo data round trip")
	brush.set_points_data(data)
	brush.rebuild()
	check(brush.get_point_count() == 2 and absf(height_at(brush, 0, 0) + 0.8) < 0.01, "set_points_data restores the dip")

	print("Box with UV seams")
	var box := BoxMesh.new()
	box.size = Vector3(4, 4, 4)
	var cube := QuickMeshBrush3D.new()
	cube.update_at_runtime = true
	cube.source_mesh = box
	root.add_child(cube)
	cube.rebuild()
	var c := cube.add_point(Vector3(1.9, 2, 0), Vector3.UP, 1.0, 0.0)
	cube.set_point_transform(c, cube.get_point_rest(c).translated(Vector3(0, 0.4, 0)))
	cube.rebuild()
	var cs := edge_stats(cube.mesh)
	check(cs[0] == 0 and cs[1] < 0.0001, "closed cube stays closed across seams: bad %d, open %.4f" % [cs[0], cs[1]])

	print("Collision shape follows")
	var body := StaticBody3D.new()
	var shape_node := CollisionShape3D.new()
	body.add_child(shape_node)
	brush.add_child(body)
	brush.collision_shape = brush.get_path_to(shape_node)
	brush.rebuild()
	var shape := shape_node.shape as ConcavePolygonShape3D
	check(shape != null and shape.get_faces().size() == tri_count(brush.mesh) * 3, "collision has the bent faces")

	print("Apply edits")
	var before := tri_count(brush.mesh)
	var centre := height_at(brush, 0, 0)
	brush.apply_edits()
	check(brush.get_point_count() == 0 and brush.source_mesh != plane, "points gone, new source")
	check(tri_count(brush.mesh) == before and absf(height_at(brush, 0, 0) - centre) < 0.0001, "same shape after apply")

	print("Saved scene needs no work at runtime")
	var save_root := Node3D.new()
	var saved := QuickMeshBrush3D.new()
	saved.name = "Floor"
	saved.update_at_runtime = true
	saved.source_mesh = plane
	save_root.add_child(saved)
	saved.owner = save_root
	root.add_child(save_root)
	var sp := saved.add_point(Vector3(1, 0, 1), Vector3.UP, 1.5)
	saved.set_point_transform(sp, saved.get_point_rest(sp).translated(Vector3(0, -0.3, 0)))
	saved.rebuild()
	var saved_tris := tri_count(saved.mesh)
	saved.update_at_runtime = false
	var packed := PackedScene.new()
	packed.pack(save_root)
	ResourceSaver.save(packed, "user://brush_test.tscn")
	var loaded = load("user://brush_test.tscn").instantiate()
	root.add_child(loaded)
	await process_frame
	var lf: QuickMeshBrush3D = loaded.get_node("Floor")
	check(lf.get_point_count() == 1 and lf.mesh != null and tri_count(lf.mesh) == saved_tris, "loaded scene draws the saved mesh: %d triangles" % (tri_count(lf.mesh) if lf.mesh else -1))
	var text := FileAccess.get_file_as_string("user://brush_test.tscn")
	check(text.contains("points/0/transform") and text.contains("points/0/rest"), "points are saved")

	print("Timing")
	var big := PlaneMesh.new()
	big.size = Vector2(100, 100)
	big.subdivide_width = 99
	big.subdivide_depth = 99
	var ground := QuickMeshBrush3D.new()
	ground.update_at_runtime = true
	ground.source_mesh = big
	root.add_child(ground)
	for i in 10:
		ground.add_point(Vector3(-40 + i * 9, 0, 0), Vector3.UP, 3.0, 0.3)
	var t0 := Time.get_ticks_usec()
	ground.rebuild()
	var t_refine := (Time.get_ticks_usec() - t0) / 1000.0
	ground.set_point_transform(0, ground.get_point_rest(0).translated(Vector3(0, -0.4, 0)))
	t0 = Time.get_ticks_usec()
	ground.rebuild()
	var t_bend := (Time.get_ticks_usec() - t0) / 1000.0
	print("    ground: %d source tris -> %d, refine+bend %.1f ms, bend only %.1f ms" % [tri_count(big), tri_count(ground.mesh), t_refine, t_bend])
	check(t_bend < 150.0, "a gizmo drag frame on a 20k triangle ground is quick: %.1f ms" % t_bend)

	print("%d checks, %d failed" % [checks, failures])
	quit(1 if failures > 0 else 0)
