extends Node

## The Asosdot trailer, made entirely in code: every set is built from
## primitives here, the cameras move on keyframes here, and the music is
## synthesised by score.gd. Render it with Movie Maker:
##
##   asosdot --path misc/trailer --write-movie trailer.avi --fixed-fps 60
##
## Shots start on bar lines of the score (2.5 seconds at 96 BPM).

const Score := preload("res://score.gd")

const BAR := 2.5
const BEAT := BAR / 4.0
const LENGTH := 27 * BAR # 67.5 seconds

# Sets sit far apart so their lights never reach each other.
const GALLERY_AT := Vector3(0, 0, 0)
const COURTYARD_AT := Vector3(600, 0, 0)
const LANTERNS_AT := Vector3(1200, 0, 0)
const TITLE_AT := Vector3(1800, 0, 0)
const TERRAIN_AT := Vector3(4000, 0, 0)

var time := 0.0
var stills_dir := ""
var stills: Array[float] = [] # times to save a frame at, with --stills=<folder>
var camera: Camera3D
var world_environment: WorldEnvironment
var music: AudioStreamPlayer

var sets := {}
var environments := {}
var shots: Array[Dictionary] = []
var current_shot := -1

# Gallery
var gallery_spot: SpotLight3D
var gallery_sculpture: MeshInstance3D
# Courtyard
var courtyard_sun: DirectionalLight3D
# Lanterns
var lanterns: Array[Dictionary] = []
# Title
var title_rope: Rope3D
var title_bulb: Node3D
# Terrain
var terrain: Terrain3D
var terrain_sun: DirectionalLight3D

# Overlay
var fade: ColorRect
var title_label: Label
var subtitle_label: Label
var kicker_label: Label
var caption_label: Label
var end_lines: Array[Label] = []


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--stills="):
			stills_dir = arg.substr(9)
			for t in [1.5, 4.5, 9.0, 13.0, 18.5, 22.0, 27.0, 29.5, 33.0, 36.0, 40.0, 45.0, 49.0, 51.8, 53.0, 54.5, 56.5, 59.5, 63.0]:
				stills.append(t)
	var started := Time.get_ticks_msec()
	var score := Score.new(LENGTH)
	score.compose()
	music = AudioStreamPlayer.new()
	music.stream = score.to_stream()
	add_child(music)
	_setup_audio_bus()
	print("Score: %d ms" % (Time.get_ticks_msec() - started))

	world_environment = WorldEnvironment.new()
	add_child(world_environment)
	camera = Camera3D.new()
	camera.fov = 52.0
	camera.far = 3000.0
	add_child(camera)
	camera.make_current()

	started = Time.get_ticks_msec()
	_build_gallery()
	_build_courtyard()
	_build_lanterns()
	_build_title()
	_build_terrain()
	print("Sets: %d ms" % (Time.get_ticks_msec() - started))

	_build_overlay()
	_build_shots()
	for key in sets:
		sets[key].visible = false
	music.play()


func _setup_audio_bus() -> void:
	var reverb := AudioEffectReverb.new()
	reverb.room_size = 0.75
	reverb.damping = 0.45
	reverb.spread = 1.0
	reverb.wet = 0.22
	reverb.dry = 0.95
	AudioServer.add_bus_effect(0, reverb)
	var limiter := AudioEffectHardLimiter.new()
	limiter.ceiling_db = -0.8
	AudioServer.add_bus_effect(0, limiter)


# --- Helpers ---------------------------------------------------------------

func material(p_color: Color, p_roughness := 0.8, p_metallic := 0.0) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = p_color
	m.roughness = p_roughness
	m.metallic = p_metallic
	return m


func emissive(p_color: Color, p_energy: float) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = p_color
	m.emission_enabled = true
	m.emission = p_color
	m.emission_energy_multiplier = p_energy
	return m


func box(p_parent: Node3D, p_size: Vector3, p_position: Vector3, p_material: Material, p_static := true) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	mesh.add_uv2 = true
	mesh.material = p_material
	var mi := MeshInstance3D.new()
	mi.mesh = mesh
	mi.position = p_position
	mi.gi_mode = GeometryInstance3D.GI_MODE_STATIC if p_static else GeometryInstance3D.GI_MODE_DYNAMIC
	p_parent.add_child(mi)
	return mi


func new_set(p_name: String, p_at: Vector3) -> Node3D:
	var root := Node3D.new()
	root.name = p_name
	root.position = p_at
	add_child(root)
	sets[p_name] = root
	return root


func bake(p_set: Node3D, p_gi: RealtimeGI) -> void:
	p_set.add_child(p_gi)
	var started := Time.get_ticks_msec()
	var err := p_gi.bake(p_set, "user://trailer_%s.rgi" % p_set.name.to_lower())
	if err != RealtimeGI.BAKE_ERROR_OK:
		push_error("Baking %s failed: %d" % [p_set.name, err])
	else:
		print("%s: %d patches, %d probes in %d ms" % [p_set.name, p_gi.data.get_patch_count(), p_gi.data.get_probe_count(), Time.get_ticks_msec() - started])


func base_environment() -> Environment:
	var env := Environment.new()
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.tonemap_exposure = 1.0
	env.glow_enabled = true
	env.glow_intensity = 0.5
	env.glow_bloom = 0.05
	env.glow_hdr_threshold = 1.2
	env.ssao_enabled = true
	env.ssao_intensity = 1.4
	env.post_vignette_intensity = 0.35
	env.post_vignette_softness = 0.6
	env.post_film_grain = 0.035
	env.post_chromatic_aberration = 0.12
	return env


## Smooth camera moves through keyframes: [time, position, look_at].
func camera_at(p_t: float, p_keys: Array) -> Transform3D:
	var n := p_keys.size()
	var i := 0
	while i < n - 2 and p_t > p_keys[i + 1][0]:
		i += 1
	var a: Array = p_keys[i]
	var b: Array = p_keys[mini(i + 1, n - 1)]
	var before: Array = p_keys[maxi(i - 1, 0)]
	var after: Array = p_keys[mini(i + 2, n - 1)]
	var w := 0.0 if b[0] == a[0] else clampf((p_t - a[0]) / (b[0] - a[0]), 0.0, 1.0)
	w = w * w * (3.0 - 2.0 * w) if n == 2 else w
	var pos: Vector3 = a[1].cubic_interpolate(b[1], before[1], after[1], w)
	var target: Vector3 = a[2].cubic_interpolate(b[2], before[2], after[2], w)
	return Transform3D.IDENTITY.translated(pos).looking_at(target, Vector3.UP)


# --- Sets ------------------------------------------------------------------

func _build_gallery() -> void:
	var root := new_set("Gallery", GALLERY_AT)
	var concrete := material(Color(0.5, 0.48, 0.45), 0.55)
	var plaster := material(Color(0.82, 0.8, 0.76), 0.9)
	box(root, Vector3(14, 0.2, 28), Vector3(0, -0.1, 0), concrete)
	box(root, Vector3(14, 0.2, 28), Vector3(0, 6.1, 0), plaster)
	box(root, Vector3(0.2, 6, 28), Vector3(-7.1, 3, 0), material(Color(0.72, 0.09, 0.07), 0.85))
	box(root, Vector3(0.2, 6, 28), Vector3(7.1, 3, 0), material(Color(0.05, 0.42, 0.48), 0.85))
	box(root, Vector3(14, 6, 0.2), Vector3(0, 3, -14.1), plaster)
	box(root, Vector3(14, 6, 0.2), Vector3(0, 3, 14.1), plaster)
	for z in [-10.0, -5.0, 0.0, 5.0, 10.0]:
		box(root, Vector3(0.9, 6, 0.9), Vector3(-4.6, 3, z), plaster)
		box(root, Vector3(0.9, 6, 0.9), Vector3(4.6, 3, z), plaster)
		box(root, Vector3(14, 0.55, 0.6), Vector3(0, 5.72, z), plaster)
	box(root, Vector3(1.6, 1.0, 1.6), Vector3(0, 0.5, -3), plaster)

	# Moves, so it takes its light from the probes rather than the lightmap.
	var torus := TorusMesh.new()
	torus.inner_radius = 0.42
	torus.outer_radius = 0.95
	torus.rings = 64
	torus.ring_segments = 32
	torus.material = material(Color(0.9, 0.88, 0.85), 0.35)
	gallery_sculpture = MeshInstance3D.new()
	gallery_sculpture.mesh = torus
	gallery_sculpture.position = Vector3(0, 2.2, -3)
	gallery_sculpture.gi_mode = GeometryInstance3D.GI_MODE_DYNAMIC
	root.add_child(gallery_sculpture)

	gallery_spot = SpotLight3D.new()
	gallery_spot.spot_angle = 32.0
	gallery_spot.spot_range = 18.0
	gallery_spot.light_energy = 22.0
	gallery_spot.shadow_enabled = true
	gallery_spot.light_color = Color(1.0, 0.93, 0.85)
	root.add_child(gallery_spot)

	var gi := RealtimeGI.new()
	gi.patch_size = 0.35
	gi.probe_spacing = 1.5
	gi.energy = 1.25
	gi.bounces_per_frame = 2
	gi.shadow_samples = 4
	bake(root, gi)

	var env := base_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.0, 0.0, 0.0)
	env.ambient_light_energy = 0.0
	env.tonemap_exposure = 1.25
	environments["Gallery"] = env


func _gallery_light(p_t: float, p_length: float) -> void:
	# The spot travels down the room and swings from the red wall to the teal
	# one; its colour drifts from white to amber. All of it bounces.
	var u := p_t / p_length
	gallery_spot.position = Vector3(0, 5.3, lerpf(9.0, -9.0, u))
	var yaw := sin(p_t * 0.9) * 55.0
	gallery_spot.rotation_degrees = Vector3(-62.0, yaw, 0.0)
	gallery_spot.light_color = Color(1.0, 0.93, 0.85).lerp(Color(1.0, 0.55, 0.25), smoothstep(0.55, 0.9, u))
	gallery_sculpture.rotation = Vector3(p_t * 0.35, p_t * 0.6, 0.0)


func _build_courtyard() -> void:
	var root := new_set("Courtyard", COURTYARD_AT)
	var stone := material(Color(0.78, 0.7, 0.58), 0.85)
	var plaster := material(Color(0.88, 0.82, 0.72), 0.9)
	box(root, Vector3(70, 0.2, 70), Vector3(0, -0.1, 0), stone)
	box(root, Vector3(44, 9, 1), Vector3(0, 4.5, -12.5), plaster)
	box(root, Vector3(1, 9, 30), Vector3(-22, 4.5, 2), plaster)
	for i in 10:
		var x := -18.0 + i * 4.0
		box(root, Vector3(0.9, 5, 0.9), Vector3(x, 2.5, -6), plaster)
	box(root, Vector3(40, 0.6, 7.2), Vector3(0, 5.3, -9.2), plaster)
	# A pool and some planters to catch light.
	box(root, Vector3(12, 0.12, 7), Vector3(0, 0.02, 5), material(Color(0.04, 0.16, 0.22), 0.04))
	for p in [Vector3(-9, 0.5, 2), Vector3(9, 0.5, 2), Vector3(-9, 0.5, 9), Vector3(9, 0.5, 9)]:
		box(root, Vector3(1.6, 1.0, 1.6), p, stone)
		box(root, Vector3(1.3, 0.9, 1.3), p + Vector3(0, 0.9, 0), material(Color(0.16, 0.3, 0.09), 0.9))

	courtyard_sun = DirectionalLight3D.new()
	courtyard_sun.shadow_enabled = true
	courtyard_sun.directional_shadow_max_distance = 80.0
	root.add_child(courtyard_sun)

	var gi := RealtimeGI.new()
	gi.patch_size = 0.5
	gi.probe_spacing = 3.0
	gi.energy = 1.2
	gi.bounces_per_frame = 2
	bake(root, gi)

	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.2, 0.36, 0.62)
	sky_material.sky_horizon_color = Color(0.62, 0.66, 0.72)
	sky_material.ground_horizon_color = Color(0.5, 0.45, 0.4)
	var sky := Sky.new()
	sky.sky_material = sky_material
	var env := base_environment()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ssr_enabled = true
	env.fog_enabled = true
	env.fog_density = 0.002
	env.fog_light_color = Color(0.75, 0.62, 0.5)
	env.fog_sun_scatter = 0.3
	environments["Courtyard"] = env


func _courtyard_light(p_t: float, p_length: float) -> void:
	# Mid afternoon to the last minutes of the day.
	var u := smoothstep(0.0, 1.0, p_t / p_length)
	courtyard_sun.rotation_degrees = Vector3(lerpf(-48.0, -3.5, u), lerpf(-25.0, -70.0, u), 0.0)
	courtyard_sun.light_color = Color(1.0, 0.97, 0.92).lerp(Color(1.0, 0.42, 0.16), u)
	courtyard_sun.light_energy = lerpf(2.4, 1.6, u)


func _build_lanterns() -> void:
	var root := new_set("Lanterns", LANTERNS_AT)
	var brick := material(Color(0.5, 0.22, 0.15), 0.9)
	var cobbles := material(Color(0.3, 0.29, 0.27), 0.7)
	var wood := material(Color(0.22, 0.14, 0.08), 0.8)
	box(root, Vector3(12, 0.2, 44), Vector3(0, -0.1, -4), cobbles)
	box(root, Vector3(1, 10, 44), Vector3(-5.5, 5, -4), brick)
	box(root, Vector3(1, 10, 44), Vector3(5.5, 5, -4), brick)
	box(root, Vector3(12, 10, 1), Vector3(0, 5, -26), brick)
	var rope_material := material(Color(0.12, 0.09, 0.06), 0.9)
	var flare := LensFlare.create_procedural()
	for z in [-14.0, -7.0, 0.0]:
		box(root, Vector3(10, 0.35, 0.35), Vector3(0, 6.5, z), wood)
		for x in [-2.0, 2.0]:
			var anchor := Node3D.new()
			anchor.position = Vector3(x, 6.3, z)
			root.add_child(anchor)
			var rope := Rope3D.new()
			rope.segment_count = 14
			rope.rope_length = 2.4
			rope.thickness = 0.02
			rope.material = rope_material
			rope.damping = 0.015
			root.add_child(rope)
			rope.attach_start_enabled = true
			rope.attach_start_node = rope.get_path_to(anchor)
			var lantern := Node3D.new()
			root.add_child(lantern)
			var body := MeshInstance3D.new()
			var lantern_mesh := BoxMesh.new()
			lantern_mesh.size = Vector3(0.26, 0.38, 0.26)
			lantern_mesh.material = emissive(Color(1.0, 0.6, 0.26), 5.0)
			body.mesh = lantern_mesh
			body.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
			body.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			lantern.add_child(body)
			var light := OmniLight3D.new()
			light.light_color = Color(1.0, 0.62, 0.3)
			light.light_energy = 2.0
			light.omni_range = 8.0
			light.shadow_enabled = true
			light.position = Vector3(0, -0.05, 0)
			lantern.add_child(light)
			if z == 0.0 and x < 0.0:
				var lens := LensFlare3D.new()
				lens.flare = flare
				lens.intensity = 0.35
				lantern.add_child(lens)
			lanterns.append({ "rope": rope, "lantern": lantern, "phase": x * 1.3 + z * 0.4 })

	var gi := RealtimeGI.new()
	gi.patch_size = 0.4
	gi.probe_spacing = 1.5
	gi.energy = 1.3
	gi.environment_energy = 1.0
	bake(root, gi)

	var env := base_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.03, 0.06)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_BG
	env.background_energy_multiplier = 1.0
	env.volumetric_fog_enabled = true
	env.volumetric_fog_density = 0.02
	env.volumetric_fog_albedo = Color(0.9, 0.85, 0.8)
	env.tonemap_exposure = 1.2
	env.glow_intensity = 0.8
	environments["Lanterns"] = env


func _lantern_wind(p_t: float) -> void:
	for l in lanterns:
		var rope: Rope3D = l["rope"]
		var gust := sin(p_t * 0.7 + l["phase"]) * 0.5 + 0.5
		rope.wind = Vector3(1.6 + gust * 2.4, 0.0, sin(p_t * 0.45 + l["phase"]) * 1.2)
		var last := rope.get_point_count() - 1
		if last > 0:
			var lantern: Node3D = l["lantern"]
			var end := rope.get_point_position(last)
			var before := rope.get_point_position(last - 1)
			lantern.global_position = end + (end - before).normalized() * 0.19
			var up := (before - end).normalized()
			if up.length_squared() > 0.5:
				lantern.global_basis = Basis(Quaternion(Vector3.UP, up))


func _build_title() -> void:
	var root := new_set("Title", TITLE_AT)
	box(root, Vector3(60, 0.2, 60), Vector3(0, -0.1, 0), material(Color(0.12, 0.11, 0.1), 0.6))
	var anchor := Node3D.new()
	anchor.position = Vector3(0, 9.0, 0)
	root.add_child(anchor)
	title_rope = Rope3D.new()
	title_rope.segment_count = 20
	title_rope.rope_length = 6.2
	title_rope.thickness = 0.008
	title_rope.material = material(Color(0.05, 0.05, 0.05), 0.6)
	title_rope.damping = 0.004
	root.add_child(title_rope)
	title_rope.attach_start_enabled = true
	title_rope.attach_start_node = title_rope.get_path_to(anchor)

	title_bulb = Node3D.new()
	root.add_child(title_bulb)
	var sphere := SphereMesh.new()
	sphere.radius = 0.07
	sphere.height = 0.14
	sphere.material = emissive(Color(1.0, 0.86, 0.66), 18.0)
	var bulb := MeshInstance3D.new()
	bulb.mesh = sphere
	bulb.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	title_bulb.add_child(bulb)
	var light := OmniLight3D.new()
	light.light_color = Color(1.0, 0.82, 0.6)
	light.light_energy = 3.0
	light.omni_range = 14.0
	light.shadow_enabled = true
	title_bulb.add_child(light)
	var lens := LensFlare3D.new()
	lens.flare = LensFlare.create_default()
	lens.intensity = 0.9
	title_bulb.add_child(lens)

	var env := base_environment()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0, 0, 0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	env.volumetric_fog_enabled = true
	env.volumetric_fog_density = 0.035
	env.volumetric_fog_albedo = Color(0.85, 0.8, 0.75)
	env.glow_intensity = 0.9
	environments["Title"] = env


func _title_bulb() -> void:
	var last := title_rope.get_point_count() - 1
	if last > 0:
		title_bulb.global_position = title_rope.get_point_position(last)


func _build_terrain() -> void:
	var root := new_set("Terrain", TERRAIN_AT)
	terrain = Terrain3D.new()
	var data := TerrainData.new()
	var grass_layer := TerrainLayer.new()
	grass_layer.albedo_color = Color(0.22, 0.34, 0.1)
	grass_layer.roughness = 0.95
	var rock_layer := TerrainLayer.new()
	rock_layer.albedo_color = Color(0.42, 0.39, 0.35)
	rock_layer.roughness = 0.85
	data.add_layer(grass_layer)
	data.add_layer(rock_layer)

	# Rolling hills either side of a valley the camera flies along.
	var noise := FastNoiseLite.new()
	noise.seed = 7
	noise.fractal_octaves = 5
	noise.frequency = 0.0045
	var size := data.region_size
	for ry: int in [-1, 0]:
		for rx: int in [-1, 0]:
			var image := Image.create_empty(size, size, false, Image.FORMAT_RF)
			for y in size:
				for x in size:
					var vx := rx * size + x
					var vz := ry * size + y
					var valley := vx - 35.0 * sin(vz * 0.012)
					var h := noise.get_noise_2d(vx, vz) * 26.0 + pow(absf(valley) / 70.0, 2.0) * 14.0
					image.set_pixel(x, y, Color(h, 0, 0))
			data.set_height_map(Vector2i(rx, ry), image)
	terrain.data = data
	root.add_child(terrain)

	# Rock where it's steep.
	for vz in range(-size, size, 1):
		for vx in range(-size, size, 1):
			var n := terrain.get_normal(terrain.vertex_to_global(Vector2i(vx, vz)))
			var steep := smoothstep(0.82, 0.7, n.y)
			if steep > 0.0:
				data.set_layer_weights(Vector2i(vx, vz), PackedFloat32Array([1.0 - steep, steep]))

	var foliage := Foliage3D.new()
	var foliage_data := FoliageData.new()
	var grass := FoliageType.new()
	grass.kind = FoliageType.KIND_GRASS
	grass.grass_height = 0.55
	grass.grass_width = 0.05
	grass.grass_base_color = Color(0.1, 0.17, 0.04)
	grass.grass_tip_color = Color(0.62, 0.6, 0.28)
	grass.color_variation = 0.25
	grass.wind_strength = 0.6
	grass.slope_max = 35.0
	grass.visibility_range = 90.0
	foliage_data.add_type(grass)
	for z in range(-240, 240, 1):
		var cx := 35.0 * sin(z * 0.012)
		for x in range(int(cx) - 70, int(cx) + 70, 1):
			foliage_data.set_density(0, Vector2(x, z), 1.0)
	foliage.data = foliage_data
	terrain.add_child(foliage)

	terrain_sun = DirectionalLight3D.new()
	terrain_sun.shadow_enabled = true
	terrain_sun.directional_shadow_max_distance = 250.0
	terrain_sun.light_color = Color(1.0, 0.72, 0.46)
	terrain_sun.light_energy = 2.2
	terrain_sun.rotation_degrees = Vector3(-9.0, 160.0, 0.0)
	root.add_child(terrain_sun)
	var sun_flare := LensFlare3D.new()
	sun_flare.flare = LensFlare.create_default()
	sun_flare.directional = true
	sun_flare.intensity = 1.0
	terrain_sun.add_child(sun_flare)

	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.22, 0.34, 0.55)
	sky_material.sky_horizon_color = Color(0.85, 0.66, 0.5)
	sky_material.ground_horizon_color = Color(0.5, 0.42, 0.34)
	var sky := Sky.new()
	sky.sky_material = sky_material
	var env := base_environment()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.fog_enabled = true
	env.fog_density = 0.004
	env.fog_aerial_perspective = 0.6
	env.fog_light_color = Color(0.8, 0.66, 0.52)
	env.fog_sun_scatter = 0.5
	environments["Terrain"] = env


func terrain_ground(p_x: float, p_z: float) -> float:
	var h := terrain.get_height(TERRAIN_AT + Vector3(p_x, 0, p_z))
	return 0.0 if is_nan(h) else h


# --- Overlay ---------------------------------------------------------------

func font(p_weight: int) -> SystemFont:
	var f := SystemFont.new()
	f.font_names = PackedStringArray(["Bahnschrift", "Segoe UI", "Helvetica Neue", "Arial"])
	f.font_weight = p_weight
	f.antialiasing = TextServer.FONT_ANTIALIASING_GRAY
	return f


func label(p_parent: Control, p_size: int, p_weight: int, p_spacing: int, p_color: Color) -> Label:
	var l := Label.new()
	var variation := FontVariation.new()
	variation.base_font = font(p_weight)
	variation.spacing_glyph = p_spacing
	var settings := LabelSettings.new()
	settings.font = variation
	settings.font_size = p_size
	settings.font_color = p_color
	settings.shadow_color = Color(0, 0, 0, 0.55)
	settings.shadow_size = 6
	settings.shadow_offset = Vector2(0, 2)
	l.label_settings = settings
	l.modulate.a = 0.0
	p_parent.add_child(l)
	return l


func _build_overlay() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	var screen := Control.new()
	screen.set_anchors_preset(Control.PRESET_FULL_RECT)
	screen.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(screen)

	# 2.39:1 letterbox.
	for top in [true, false]:
		var bar := ColorRect.new()
		bar.color = Color.BLACK
		bar.anchor_left = 0.0
		bar.anchor_right = 1.0
		bar.anchor_top = 0.0 if top else 0.8717
		bar.anchor_bottom = 0.1283 if top else 1.0
		screen.add_child(bar)

	title_label = label(screen, 150, 700, 46, Color(1, 0.97, 0.92))
	title_label.text = "ASOSDOT"
	title_label.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	title_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title_label.grow_horizontal = Control.GROW_DIRECTION_BOTH
	title_label.grow_vertical = Control.GROW_DIRECTION_BOTH
	title_label.position.y -= 40
	subtitle_label = label(screen, 34, 400, 30, Color(1, 0.86, 0.7))
	subtitle_label.text = "ENGINE"
	subtitle_label.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	subtitle_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle_label.grow_horizontal = Control.GROW_DIRECTION_BOTH
	subtitle_label.grow_vertical = Control.GROW_DIRECTION_BOTH
	subtitle_label.position.y += 70

	kicker_label = label(screen, 26, 600, 9, Color(1.0, 0.72, 0.42))
	kicker_label.position = Vector2(150, 760)
	caption_label = label(screen, 44, 300, 1, Color(0.97, 0.95, 0.92))
	caption_label.position = Vector2(150, 800)

	var lines := [
		["Realtime GI  ·  Lens flares  ·  Ropes  ·  Terrain  ·  Animation workspace", 30, 400, 3, Color(0.92, 0.9, 0.86)],
		["Free and open source  ·  MIT licence  ·  Built on Godot", 30, 400, 3, Color(1.0, 0.75, 0.5)],
		["github.com/silasrenlow-web/Asosdot-Engine", 32, 600, 2, Color(0.97, 0.95, 0.92)],
	]
	for i in lines.size():
		var l := label(screen, lines[i][1], lines[i][2], lines[i][3], lines[i][4])
		l.text = lines[i][0]
		l.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
		l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		l.grow_horizontal = Control.GROW_DIRECTION_BOTH
		l.grow_vertical = Control.GROW_DIRECTION_BOTH
		l.position.y += 90 + i * 56
		end_lines.append(l)

	fade = ColorRect.new()
	fade.color = Color.BLACK
	fade.set_anchors_preset(Control.PRESET_FULL_RECT)
	fade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	screen.add_child(fade)


func ramp(p_t: float, p_in: float, p_out: float, p_fade := 0.6) -> float:
	return clampf((p_t - p_in) / p_fade, 0.0, 1.0) * clampf((p_out - p_t) / p_fade, 0.0, 1.0)


# --- Shots -----------------------------------------------------------------

func shot(p_bar_from: float, p_bar_to: float, p_set: String, p_update: Callable, p_kicker := "", p_caption := "") -> void:
	shots.append({ "from": p_bar_from * BAR, "to": p_bar_to * BAR, "set": p_set, "update": p_update, "kicker": p_kicker, "caption": p_caption })


func _build_shots() -> void:
	shot(0, 3, "Title", func(t: float, length: float) -> void:
		_title_bulb()
		camera.global_transform = camera_at(t, [[0.0, TITLE_AT + Vector3(0.6, 2.4, 6.5), TITLE_AT + Vector3(0, 2.7, 0)], [length, TITLE_AT + Vector3(-0.3, 2.6, 4.6), TITLE_AT + Vector3(0, 2.8, 0)]])
		title_label.modulate.a = ramp(t, BAR, length - 0.3, 0.5)
		subtitle_label.modulate.a = ramp(t, BAR + 0.7, length - 0.3, 0.6)
	)
	shot(3, 8, "Gallery", func(t: float, length: float) -> void:
		_gallery_light(t, length)
		camera.global_transform = camera_at(t, [[0.0, Vector3(3.2, 1.6, 12.0), Vector3(-0.5, 2.4, -6.0)], [length, Vector3(-1.8, 2.1, 4.2), Vector3(0.0, 2.6, -8.0)]])
	, "REALTIME GLOBAL ILLUMINATION", "Every light can move. The bounce follows.")
	shot(8, 12, "Courtyard", func(t: float, length: float) -> void:
		_courtyard_light(t, length)
		camera.global_transform = camera_at(t, [[0.0, COURTYARD_AT + Vector3(-12.0, 1.7, -9.0), COURTYARD_AT + Vector3(4.0, 2.2, -3.0)], [length, COURTYARD_AT + Vector3(5.0, 1.9, -9.5), COURTYARD_AT + Vector3(14.0, 2.4, 2.0)]])
	, "A SUN THAT SETS", "Bounced light keeps up, every frame.")
	shot(12, 15, "Lanterns", func(t: float, length: float) -> void:
		_lantern_wind(t + 4.0)
		camera.global_transform = camera_at(t, [[0.0, LANTERNS_AT + Vector3(1.4, 0.7, 9.0), LANTERNS_AT + Vector3(-0.5, 4.2, -8.0)], [length, LANTERNS_AT + Vector3(-0.8, 1.2, 4.5), LANTERNS_AT + Vector3(0.0, 4.6, -10.0)]])
	, "ROPE PHYSICS", "Lanterns swing. Their light swings with them.")
	shot(15, 19, "Terrain", func(t: float, length: float) -> void:
		var z := lerpf(150.0, 40.0, t / length)
		var x := 35.0 * sin(z * 0.012)
		var ahead_z := z - 30.0
		var ahead_x := 35.0 * sin(ahead_z * 0.012)
		var pos := TERRAIN_AT + Vector3(x, terrain_ground(x, z) + 2.4, z)
		camera.global_transform = Transform3D.IDENTITY.translated(pos).looking_at(TERRAIN_AT + Vector3(ahead_x, terrain_ground(ahead_x, ahead_z) + 3.0, ahead_z), Vector3.UP)
	, "TERRAIN & FOLIAGE", "Sculpt it, paint it, grow it.")
	shot(19, 21, "Terrain", func(t: float, length: float) -> void:
		# Turn into the low sun for the flare.
		var u := smoothstep(0.0, 1.0, t / length)
		var pos := TERRAIN_AT + Vector3(30.0, terrain_ground(30.0, 20.0) + 3.5, 20.0)
		var sun_dir := terrain_sun.global_basis.z
		var sun_yaw := atan2(sun_dir.z, sun_dir.x)
		var yaw := lerpf(sun_yaw + 1.9, sun_yaw + 0.06, u)
		var look := Vector3(cos(yaw), lerpf(0.02, sun_dir.y, u), sin(yaw))
		camera.global_transform = Transform3D.IDENTITY.translated(pos).looking_at(pos + look * 10.0, Vector3.UP)
		var env: Environment = environments["Terrain"]
		env.post_chromatic_aberration = lerpf(0.12, 0.45, u)
		env.post_film_grain = lerpf(0.035, 0.08, u)
	, "CINEMATIC BY DEFAULT", "Lens flares, film grain, chromatic aberration.")

	# The montage: a cut on every beat.
	var cuts := [
		["Gallery", func(t: float) -> void:
			gallery_spot.light_color = Color(1.0, 0.25, 0.65)
			gallery_spot.position = Vector3(0, 5.3, -1)
			gallery_spot.rotation_degrees = Vector3(-60, -45, 0)
			camera.global_transform = camera_at(t, [[0.0, Vector3(4.5, 1.2, 8.0), Vector3(-2, 2.8, -4)], [BEAT, Vector3(4.0, 1.3, 7.2), Vector3(-2, 2.8, -4)]])],
		["Courtyard", func(t: float) -> void:
			_courtyard_light(9.0, 10.0)
			camera.global_transform = camera_at(t, [[0.0, COURTYARD_AT + Vector3(-3, 1.2, 12), COURTYARD_AT + Vector3(0, 3, -10)], [BEAT, COURTYARD_AT + Vector3(-2.6, 1.3, 11), COURTYARD_AT + Vector3(0, 3, -10)]])],
		["Lanterns", func(t: float) -> void:
			_lantern_wind(30.0 + t)
			camera.global_transform = camera_at(t, [[0.0, LANTERNS_AT + Vector3(-3.5, 3.0, 2.0), LANTERNS_AT + Vector3(2.0, 4.0, -7.0)], [BEAT, LANTERNS_AT + Vector3(-3.2, 3.1, 1.4), LANTERNS_AT + Vector3(2.0, 4.0, -7.0)]])],
		["Terrain", func(t: float) -> void:
			var pos := TERRAIN_AT + Vector3(0.0, terrain_ground(0.0, -60.0) + 14.0, -60.0)
			camera.global_transform = Transform3D.IDENTITY.translated(pos + Vector3(t * 2.0, 0, 0)).looking_at(TERRAIN_AT + Vector3(-20, terrain_ground(-20, -140), -140), Vector3.UP)],
		["Gallery", func(t: float) -> void:
			gallery_spot.light_color = Color(0.2, 0.75, 1.0)
			gallery_spot.position = Vector3(0, 5.3, -6)
			gallery_spot.rotation_degrees = Vector3(-55, 50, 0)
			gallery_sculpture.rotation = Vector3(1.0 + t, 2.0 + t * 1.5, 0)
			camera.global_transform = camera_at(t, [[0.0, Vector3(-1.5, 2.6, 0.5), Vector3(0, 2.2, -3)], [BEAT, Vector3(-1.3, 2.5, 0.0), Vector3(0, 2.2, -3)]])],
		["Lanterns", func(t: float) -> void:
			_lantern_wind(40.0 + t)
			camera.global_transform = camera_at(t, [[0.0, LANTERNS_AT + Vector3(0, 1.6, 12), LANTERNS_AT + Vector3(0, 4.5, -14)], [BEAT, LANTERNS_AT + Vector3(0, 1.7, 11), LANTERNS_AT + Vector3(0, 4.5, -14)]])],
		["Courtyard", func(t: float) -> void:
			_courtyard_light(10.0, 10.0)
			camera.global_transform = camera_at(t, [[0.0, COURTYARD_AT + Vector3(-17, 1.6, -9), COURTYARD_AT + Vector3(-12, 2.2, -6)], [BEAT, COURTYARD_AT + Vector3(-16.6, 1.6, -9.2), COURTYARD_AT + Vector3(-12, 2.2, -6)]])],
		["Title", func(t: float) -> void:
			_title_bulb()
			camera.global_transform = camera_at(t, [[0.0, TITLE_AT + Vector3(0, 2.6, 2.0), TITLE_AT + Vector3(0, 2.8, 0)], [BEAT, TITLE_AT + Vector3(0, 2.6, 1.8), TITLE_AT + Vector3(0, 2.8, 0)]])],
	]
	for i in cuts.size():
		var cut: Array = cuts[i]
		var update: Callable = cut[1]
		shot(21 + i * 0.25, 21 + (i + 1) * 0.25, cut[0], func(t: float, _length: float) -> void:
			update.call(t)
		)

	shot(23, 27, "Title", func(t: float, length: float) -> void:
		_title_bulb()
		camera.global_transform = camera_at(t, [[0.0, TITLE_AT + Vector3(0, 2.2, 9.0), TITLE_AT + Vector3(0, 3.6, 0)], [length, TITLE_AT + Vector3(0, 2.3, 8.2), TITLE_AT + Vector3(0, 3.6, 0)]])
		title_label.modulate.a = ramp(t, 0.3, length - 0.4, 0.8)
		subtitle_label.modulate.a = ramp(t, 0.8, length - 0.4, 0.8)
		for i in end_lines.size():
			end_lines[i].modulate.a = ramp(t, 1.8 + i * 0.6, length - 0.4, 0.8)
		fade.color.a = clampf((t - (length - 1.4)) / 1.2, 0.0, 1.0)
	)


func _process(p_delta: float) -> void:
	var index := -1
	for i in shots.size():
		if time >= shots[i]["from"] and time < shots[i]["to"]:
			index = i
			break
	if index == -1:
		get_tree().quit()
		return

	var s: Dictionary = shots[index]
	if index != current_shot:
		current_shot = index
		for key in sets:
			sets[key].visible = key == s["set"]
		world_environment.environment = environments[s["set"]]
		title_label.modulate.a = 0.0
		subtitle_label.modulate.a = 0.0
		for l in end_lines:
			l.modulate.a = 0.0
		kicker_label.text = s["kicker"]
		caption_label.text = s["caption"]

	var t: float = time - s["from"]
	var length: float = s["to"] - s["from"]
	s["update"].call(t, length)

	# Sets keep simulating while hidden, so ropes are already swinging.
	_lantern_wind(time)
	_title_bulb()

	kicker_label.modulate.a = ramp(t, 0.9, length - 0.5)
	caption_label.modulate.a = ramp(t, 1.15, length - 0.5)
	# The title fades up from black; long shots dip briefly at the cut so the
	# realtime GI has a few frames to settle on the new set.
	if index == 0:
		fade.color.a = 1.0 - clampf(t / 2.0, 0.0, 1.0)
	elif index < shots.size() - 1:
		fade.color.a = 1.0 - clampf(t / 0.12, 0.0, 1.0) if length > BAR else 0.0

	if not stills.is_empty() and time >= stills[0]:
		_save_still.call_deferred(stills.pop_front())

	time += p_delta


func _save_still(p_time: float) -> void:
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.save_png(stills_dir.path_join("trailer_%05.1f.png" % p_time))
