# Headless checks for MeshPainter / MeshPaintBrush (the Mesh Paint tool's engine side):
# loading, slots, islands, smart unwrap, packing, selection, commit (attributes, blend shapes,
# skin), 3D and UV painting, fills, channel images, timing.
# Run from an empty project folder holding this file:
#   bin/asosdot.windows.editor.x86_64.exe --headless --path <folder> --script res://test_mesh_painter.gd
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

func to_array_mesh(mesh: Mesh) -> ArrayMesh:
	var out := ArrayMesh.new()
	for s in mesh.get_surface_count():
		out.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, mesh.surface_get_arrays(s))
	return out

func uv_area(a: Vector2, b: Vector2, c: Vector2) -> float:
	return absf((b - a).cross(c - a)) * 0.5

func tri_count(mesh: Mesh) -> int:
	var n := 0
	for s in mesh.get_surface_count():
		n += mesh.surface_get_arrays(s)[Mesh.ARRAY_INDEX].size() / 3
	return n

func position_set(mesh: Mesh) -> Dictionary:
	var set := {}
	for s in mesh.get_surface_count():
		for v in mesh.surface_get_arrays(s)[Mesh.ARRAY_VERTEX]:
			set["%.4f,%.4f,%.4f" % [v.x, v.y, v.z]] = true
	return set

func uvs_inside(p: MeshPainter) -> bool:
	for uv in p.get_face_uvs():
		if uv.x < -0.0001 or uv.y < -0.0001 or uv.x > 1.0001 or uv.y > 1.0001:
			return false
	return true

func islands_single_slot(p: MeshPainter) -> bool:
	for i in p.get_island_count():
		var faces := p.get_island_faces(i)
		var slot := p.get_face_slot(faces[0])
		for f in faces:
			if p.get_face_slot(f) != slot:
				return false
	return true

func face_center(p: MeshPainter, f: int) -> Vector2:
	return (p.get_face_uv(f, 0) + p.get_face_uv(f, 1) + p.get_face_uv(f, 2)) / 3.0

func pixel_at(img: Image, uv: Vector2) -> Color:
	return img.get_pixel(clampi(int(uv.x * img.get_width()), 0, img.get_width() - 1), clampi(int(uv.y * img.get_height()), 0, img.get_height() - 1))

func big_sphere() -> SphereMesh:
	var m := SphereMesh.new()
	m.radial_segments = 64
	m.rings = 32
	return m

func white_image(size: int) -> Image:
	var img := Image.create_empty(size, size, false, Image.FORMAT_RGBA8)
	img.fill(Color.WHITE)
	return img

func _init() -> void:
	print("MeshPainter checks")

	# Loading.
	var box := BoxMesh.new()
	var p := MeshPainter.new()
	check(p.load_mesh(box) == OK, "loads a BoxMesh")
	check(p.get_face_count() == 12, "box has 12 faces (%d)" % p.get_face_count())
	check(p.get_slot_count() == 1, "one slot per surface")
	check(p.has_uvs(), "box has UVs")
	check(p.get_island_count() >= 1, "box has UV islands (%d)" % p.get_island_count())

	# Smart unwrap of a sphere.
	var sphere := SphereMesh.new()
	sphere.radial_segments = 32
	sphere.rings = 16
	var sphere_mesh := to_array_mesh(sphere)
	p = MeshPainter.new()
	p.load_mesh(sphere_mesh)
	var faces := p.get_face_count()
	var t0 := Time.get_ticks_msec()
	check(p.smart_unwrap() == OK, "smart unwrap works (%d ms, %d faces)" % [Time.get_ticks_msec() - t0, faces])
	check(p.get_face_count() == faces, "unwrap keeps the faces")
	check(uvs_inside(p), "unwrapped UVs are inside 0..1")
	var area := 0.0
	for f in p.get_face_count():
		area += uv_area(p.get_face_uv(f, 0), p.get_face_uv(f, 1), p.get_face_uv(f, 2))
	check(area > 0.3 and area <= 1.0, "islands fill a good part of the square (%.2f)" % area)
	check(p.get_island_count() > 1, "a sphere unwraps into several islands (%d)" % p.get_island_count())

	# Slots: islands never cross materials.
	var top := PackedInt32Array()
	for f in p.get_face_count():
		if p.get_face_position(f, 0).y + p.get_face_position(f, 1).y + p.get_face_position(f, 2).y > 0.0:
			top.push_back(f)
	var red := StandardMaterial3D.new()
	red.albedo_color = Color.RED
	var slot := p.add_slot(red, "Top")
	p.set_faces_slot(top, slot)
	check(p.get_slot_face_count(slot) == top.size(), "faces moved to the new slot")
	check(p.smart_unwrap() == OK, "unwrap with two slots")
	check(islands_single_slot(p), "no island spans two materials")

	# Commit.
	var out := p.commit_mesh()
	check(out.get_surface_count() == 2, "commit makes one surface per slot (%d)" % out.get_surface_count())
	check(tri_count(out) == faces, "commit keeps every triangle (%d)" % tri_count(out))
	check(out.surface_get_material(1) == red, "slot material on its surface")
	check(out.surface_get_name(1) == "Top", "slot name on its surface")
	check(position_set(out).size() == position_set(sphere_mesh).size(), "same vertex positions as the source")
	var p2 := MeshPainter.new()
	p2.load_mesh(out)
	check(p2.get_face_count() == faces and p2.get_slot_count() == 2, "the committed mesh loads back")
	check(p2.get_island_count() == p.get_island_count(), "islands survive a reload (%d / %d)" % [p2.get_island_count(), p.get_island_count()])
	var same_order := true
	for f in p.get_face_count():
		if p.get_face_slot(f) != p2.get_face_slot(f) or not p.get_face_uv(f, 0).is_equal_approx(p2.get_face_uv(f, 0)):
			same_order = false
			break
	check(same_order, "face order and UVs match after a reload (selection keeps working)")

	# Packing with an even texel density.
	check(p.pack_islands(4, 1024, true, true) == OK, "pack islands")
	check(uvs_inside(p), "packed UVs are inside 0..1")
	var ratios := []
	for i in p.get_island_count():
		var a2 := 0.0
		var a3 := 0.0
		for f in p.get_island_faces(i):
			a2 += uv_area(p.get_face_uv(f, 0), p.get_face_uv(f, 1), p.get_face_uv(f, 2))
			var v0 := p.get_face_position(f, 0)
			a3 += (p.get_face_position(f, 1) - v0).cross(p.get_face_position(f, 2) - v0).length() * 0.5
		if a3 > 0.001:
			ratios.push_back(a2 / a3)
	ratios.sort()
	check(ratios.size() > 0 and ratios[ratios.size() - 1] / ratios[0] < 1.6, "islands share a texel density (spread %.2f)" % (ratios[ratios.size() - 1] / ratios[0] if ratios.size() else 0.0))
	var before_islands := p.get_island_count()
	check(p.pack_islands(4, 1024, true, false) == OK and p.get_island_count() == before_islands, "packing keeps the islands (%d)" % p.get_island_count())

	# Moving an island.
	var island := p.get_island_faces(0)
	var c0 := face_center(p, island[0])
	p.transform_faces_uv(island, Transform2D(0.0, Vector2(0.1, 0.0)))
	check(face_center(p, island[0]).is_equal_approx(c0 + Vector2(0.1, 0.0)), "transform_faces_uv moves the island")
	p.transform_faces_uv(island, Transform2D(0.0, Vector2(-0.1, 0.0)))

	# Selection.
	p.select_all(false)
	p.set_face_selected(0, true)
	p.grow_selection()
	var grown := p.get_selected_face_count()
	check(grown > 1, "grow adds the neighbours (%d)" % grown)
	p.shrink_selection()
	check(p.get_selected_face_count() < grown, "shrink takes them away (%d)" % p.get_selected_face_count())
	p.select_all(false)
	p.set_face_selected(island[0], true)
	p.select_islands_of_selection()
	check(p.get_selected_face_count() == island.size(), "select whole island")
	p.select_all(false)
	p.set_face_selected(0, true)
	p.select_pieces_of_selection()
	check(p.get_selected_face_count() == p.get_face_count(), "a sphere is one linked piece")
	p.select_all(false)

	# Picking.
	var hit := p.intersect_ray(Vector3(0, 0, 3), Vector3(0, 0, -1))
	check(not hit.is_empty() and absf(hit["position"].z - 0.5) < 0.02, "ray hits the front of the sphere")
	var behind := p.intersect_ray(Vector3(0, 0, -3), Vector3(0, 0, 1))
	check(not behind.is_empty() and behind["position"].z < -0.45, "ray hits the back from behind")

	# Painting with a 3D brush.
	var img := white_image(256)
	var brush := MeshPaintBrush.new()
	brush.color = Color.RED
	brush.hardness = 1.0
	brush.strength = 1.0
	var eye := Vector3(0, 0, 3)
	p.begin_stroke()
	var rect := p.paint_dab(img, brush, hit["position"], 0.15, eye, Vector3.UP)
	check(rect.has_area(), "a dab paints texels (%s)" % str(rect))
	var front_uv := p.get_uv_at(hit["face"], hit["barycentric"])
	check(pixel_at(img, front_uv).is_equal_approx(Color.RED), "texel under the brush is red")
	var back_uv := p.get_uv_at(behind["face"], behind["barycentric"])
	check(pixel_at(img, back_uv).is_equal_approx(Color.WHITE), "the far side stays white")
	check(p.get_stroke_original(img).get_pixelv(Vector2i(front_uv * 256)).is_equal_approx(Color.WHITE), "the stroke keeps the untouched image for undo")
	check(p.get_stroke_rect(img).encloses(rect), "stroke rect covers the dab")
	p.end_stroke()

	# Big radius through the sphere: the hidden side stays clean with occlusion and backface culling.
	img = white_image(256)
	p.begin_stroke()
	p.paint_dab(img, brush, Vector3.ZERO, 2.0, eye, Vector3.UP)
	p.end_stroke()
	check(pixel_at(img, front_uv).is_equal_approx(Color.RED) and pixel_at(img, back_uv).is_equal_approx(Color.WHITE), "only surfaces facing the eye are painted")
	brush.paint_backfaces = true
	brush.occlusion = false
	img = white_image(256)
	p.begin_stroke()
	p.paint_dab(img, brush, Vector3.ZERO, 2.0, eye, Vector3.UP)
	p.end_stroke()
	check(pixel_at(img, back_uv).is_equal_approx(Color.RED), "with back faces on, the far side is painted too")
	brush.paint_backfaces = false
	brush.occlusion = true

	# A stroke does not build up over itself.
	img = white_image(256)
	brush.strength = 0.5
	p.begin_stroke()
	p.paint_dab(img, brush, hit["position"], 0.15, eye, Vector3.UP)
	p.paint_dab(img, brush, hit["position"], 0.15, eye, Vector3.UP)
	p.end_stroke()
	var g := pixel_at(img, front_uv).g
	check(absf(g - 0.5) < 0.02, "two dabs in one stroke stay at the stroke's strength (g=%.2f)" % g)
	p.begin_stroke()
	p.paint_dab(img, brush, hit["position"], 0.15, eye, Vector3.UP)
	p.end_stroke()
	g = pixel_at(img, front_uv).g
	check(absf(g - 0.25) < 0.02, "a second stroke adds on top (g=%.2f)" % g)
	brush.strength = 1.0

	# Only one slot.
	img = white_image(256)
	p.begin_stroke()
	p.paint_dab(img, brush, Vector3.ZERO, 2.0, eye, Vector3.UP, 0)
	p.end_stroke()
	var top_face := -1
	for f in p.get_slot_faces(1):
		if p.get_face_normal(f).z > 0.5:
			top_face = f
			break
	var top_uv := face_center(p, top_face)
	check(pixel_at(img, top_uv).is_equal_approx(Color.WHITE), "a slot filter leaves the other material alone")

	# Fill an island, and erase.
	img = white_image(256)
	var fill_island := p.get_island_faces(p.get_face_island(hit["face"]))
	brush.color = Color.BLUE
	p.begin_stroke()
	p.fill_faces(img, fill_island, brush)
	p.end_stroke()
	var filled := true
	for f in fill_island:
		if not pixel_at(img, face_center(p, f)).is_equal_approx(Color.BLUE):
			filled = false
	check(filled, "fill covers the island")
	brush.blend_mode = MeshPaintBrush.BLEND_ERASE
	p.begin_stroke()
	p.paint_dab(img, brush, hit["position"], 0.15, eye, Vector3.UP)
	p.end_stroke()
	check(pixel_at(img, front_uv).a < 0.01, "erase clears alpha")
	brush.blend_mode = MeshPaintBrush.BLEND_MIX

	# Painting in UV space.
	img = white_image(64)
	brush.color = Color.GREEN
	p.begin_stroke()
	rect = p.paint_dab_uv(img, brush, Vector2(0.5, 0.5), 0.1)
	p.end_stroke()
	check(rect.has_area() and img.get_pixel(32, 32).is_equal_approx(Color.GREEN) and img.get_pixel(2, 2).is_equal_approx(Color.WHITE), "UV dab paints a disc")

	# A textured brush (stamp).
	var stamp := Image.create_empty(8, 8, false, Image.FORMAT_RGBA8)
	stamp.fill(Color(1, 1, 1, 0))
	stamp.fill_rect(Rect2i(0, 0, 4, 8), Color(1, 1, 1, 1))
	brush.texture = ImageTexture.create_from_image(stamp)
	img = white_image(64)
	brush.color = Color.BLACK
	p.begin_stroke()
	p.paint_dab_uv(img, brush, Vector2(0.5, 0.5), 0.25)
	p.end_stroke()
	check(img.get_pixel(26, 32).is_equal_approx(Color.BLACK) and img.get_pixel(38, 32).is_equal_approx(Color.WHITE), "the stamp's alpha is the brush shape")
	brush.texture = null

	# Material channels.
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.2, 0.4, 0.6)
	mat.roughness = 0.3
	var ch := MeshPainter.make_channel_image(mat, MeshPainter.CHANNEL_ALBEDO, Vector2i(32, 32))
	check(ch.get_pixel(5, 5).is_equal_approx(Color(0.2, 0.4, 0.6)), "a new albedo layer starts as the material colour")
	ch = MeshPainter.make_channel_image(mat, MeshPainter.CHANNEL_ROUGHNESS, Vector2i(32, 32))
	check(absf(ch.get_pixel(5, 5).r - 0.3) < 0.01, "a new roughness layer starts at the roughness")
	var tex_img := Image.create_empty(2, 2, false, Image.FORMAT_RGBA8)
	tex_img.set_pixel(0, 0, Color.RED)
	tex_img.set_pixel(1, 0, Color.GREEN)
	tex_img.set_pixel(0, 1, Color.BLUE)
	tex_img.set_pixel(1, 1, Color.WHITE)
	mat.albedo_texture = ImageTexture.create_from_image(tex_img)
	mat.albedo_color = Color.WHITE
	ch = MeshPainter.make_channel_image(mat, MeshPainter.CHANNEL_ALBEDO, Vector2i(32, 32))
	check(ch.get_size() == Vector2i(2, 2), "an untiled texture is painted at its own size")
	mat.uv1_scale = Vector3(2, 2, 1)
	ch = MeshPainter.make_channel_image(mat, MeshPainter.CHANNEL_ALBEDO, Vector2i(32, 32))
	check(ch.get_size() == Vector2i(32, 32) and ch.get_pixel(1, 1).r > 0.5 and ch.get_pixel(17, 1).r > 0.5, "a tiled texture is baked with its tiling")
	MeshPainter.set_channel_texture(mat, MeshPainter.CHANNEL_ALBEDO, ImageTexture.create_from_image(ch))
	check(mat.uv1_scale == Vector3.ONE, "the material stops tiling once the tiling is baked")
	MeshPainter.set_channel_texture(mat, MeshPainter.CHANNEL_ROUGHNESS, ImageTexture.create_from_image(ch))
	check(mat.roughness == 1.0 and mat.roughness_texture != null, "roughness texture holds the value")
	MeshPainter.set_channel_texture(mat, MeshPainter.CHANNEL_EMISSION, ImageTexture.create_from_image(ch))
	check(mat.emission_enabled and mat.emission_operator == BaseMaterial3D.EMISSION_OP_MULTIPLY, "emission layer turns emission on")

	# Blend shapes, skin and tangents survive.
	var arrays := sphere_mesh.surface_get_arrays(0)
	var vcount: int = arrays[Mesh.ARRAY_VERTEX].size()
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	for i in vcount:
		bones.append_array([i % 3, 0, 0, 0])
		weights.append_array([1.0, 0.0, 0.0, 0.0])
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights
	var shape := []
	shape.resize(Mesh.ARRAY_MAX)
	var moved := PackedVector3Array()
	for v in arrays[Mesh.ARRAY_VERTEX]:
		moved.push_back(v * 1.5)
	shape[Mesh.ARRAY_VERTEX] = moved
	shape[Mesh.ARRAY_NORMAL] = arrays[Mesh.ARRAY_NORMAL]
	shape[Mesh.ARRAY_TANGENT] = arrays[Mesh.ARRAY_TANGENT]
	var rich := ArrayMesh.new()
	rich.add_blend_shape("Grow")
	rich.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [shape])
	p = MeshPainter.new()
	p.load_mesh(rich)
	p.smart_unwrap()
	out = p.commit_mesh()
	check(out.get_blend_shape_count() == 1 and out.get_blend_shape_name(0) == "Grow", "blend shape kept")
	var out_arrays := out.surface_get_arrays(0)
	var out_shape: Array = out.surface_get_blend_shape_arrays(0)[0]
	var shape_ok := true
	for i in out_arrays[Mesh.ARRAY_VERTEX].size():
		if not (out_shape[Mesh.ARRAY_VERTEX][i] as Vector3).is_equal_approx(out_arrays[Mesh.ARRAY_VERTEX][i] * 1.5):
			shape_ok = false
			break
	check(shape_ok, "blend shape targets follow the split vertices")
	check(out_arrays[Mesh.ARRAY_BONES] != null and out_arrays[Mesh.ARRAY_BONES].size() == out_arrays[Mesh.ARRAY_VERTEX].size() * 4, "bones kept")
	var weights_ok := true
	for i in out_arrays[Mesh.ARRAY_VERTEX].size():
		if absf(out_arrays[Mesh.ARRAY_WEIGHTS][i * 4] - 1.0) > 0.001:
			weights_ok = false
			break
	check(weights_ok, "weights kept")
	check(out_arrays[Mesh.ARRAY_TANGENT] != null and out_arrays[Mesh.ARRAY_TANGENT].size() == out_arrays[Mesh.ARRAY_VERTEX].size() * 4, "tangents made again for the new UVs")

	# LODs are made again when the mesh had them (imported meshes do).
	var importer := ImporterMesh.new()
	importer.add_surface(Mesh.PRIMITIVE_TRIANGLES, to_array_mesh(big_sphere()).surface_get_arrays(0))
	importer.generate_lods(60.0, 0.0, [])
	var with_lods := importer.get_mesh()
	var lod_count := func(m: Mesh) -> int:
		var lods = RenderingServer.mesh_get_surface(m.get_rid(), 0).get("lods", [])
		return lods.size()
	check(lod_count.call(with_lods) > 0, "test mesh has LODs (%d)" % lod_count.call(with_lods))
	p = MeshPainter.new()
	p.load_mesh(with_lods)
	p.smart_unwrap()
	var relodded := p.commit_mesh()
	check(lod_count.call(relodded) > 0, "commit makes LODs again (%d)" % lod_count.call(relodded))
	check(lod_count.call(p.commit_mesh(false)) == 0, "previews skip LODs")

	# Unwrap only some faces.
	p = MeshPainter.new()
	p.load_mesh(sphere_mesh)
	p.smart_unwrap()
	var some := PackedInt32Array()
	for f in 40:
		some.push_back(f)
	var islands_before := p.get_island_count()
	check(p.smart_unwrap(some) == OK and uvs_inside(p), "unwrap selected faces only, then pack")
	check(p.get_island_count() >= 1 and p.get_island_count() <= islands_before + 40, "islands after a partial unwrap (%d)" % p.get_island_count())

	# Timing on a mid-poly mesh.
	var big := SphereMesh.new()
	big.radial_segments = 128
	big.rings = 80
	p = MeshPainter.new()
	t0 = Time.get_ticks_msec()
	p.load_mesh(to_array_mesh(big))
	var load_ms := Time.get_ticks_msec() - t0
	t0 = Time.get_ticks_msec()
	p.smart_unwrap()
	var unwrap_ms := Time.get_ticks_msec() - t0
	t0 = Time.get_ticks_msec()
	out = p.commit_mesh()
	var commit_ms := Time.get_ticks_msec() - t0
	img = white_image(2048)
	brush.color = Color.RED
	hit = p.intersect_ray(Vector3(0, 0, 3), Vector3(0, 0, -1))
	t0 = Time.get_ticks_usec()
	p.begin_stroke()
	for i in 20:
		p.paint_dab(img, brush, hit["position"] + Vector3(i * 0.005, 0, 0), 0.1, eye, Vector3.UP)
	p.end_stroke()
	var dab_us := (Time.get_ticks_usec() - t0) / 20
	print("  info %d faces: load %d ms, unwrap %d ms, commit %d ms, dab %d us" % [p.get_face_count(), load_ms, unwrap_ms, commit_ms, dab_us])
	check(dab_us < 30000, "a dab on %d faces at 2048 px is quick (%d us)" % [p.get_face_count(), dab_us])
	check(commit_ms < 2000, "commit is quick (%d ms)" % commit_ms)

	print("%d checks, %d failures" % [checks, failures])
	quit(1 if failures > 0 else 0)
