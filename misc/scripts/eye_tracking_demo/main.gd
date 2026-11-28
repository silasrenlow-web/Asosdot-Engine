# Eye tracking demo: a figure in a dark corridor that only moves while you
# are not looking at it (or while you blink), with gaze focus on the camera.
#
# Open this folder as a project in the Asosdot editor and run it. The webcam is
# allowed in project.godot (eye_tracking/webcam/enabled). Nothing is recorded:
# frames are measured in memory and dropped.
#
#   C  calibrate (look at the dots)      F  gaze focus on/off
#   D  debug overlay on/off              M  webcam / mouse
#   R  put the figure back
extends Node3D

var camera: Camera3D
var attributes: CameraAttributesPractical
var figure: Node3D
var target: GazeTarget3D
var label: Label
var start_position := Vector3(0, 0, -14)
var using_webcam := true


func _ready() -> void:
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.03)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.25, 0.25, 0.3)
	env.fog_enabled = true
	env.fog_density = 0.04
	env.fog_light_color = Color(0.05, 0.05, 0.07)
	var world_env := WorldEnvironment.new()
	world_env.environment = env
	add_child(world_env)

	attributes = CameraAttributesPractical.new()
	attributes.gaze_focus_enabled = true
	attributes.gaze_focus_radius = 0.12
	attributes.gaze_focus_softness = 0.35
	attributes.gaze_focus_amount = 0.85
	attributes.gaze_focus_depth_range = 6.0
	camera = Camera3D.new()
	camera.attributes = attributes
	camera.position = Vector3(0, 1.6, 0)
	add_child(camera)
	camera.make_current()

	var light := OmniLight3D.new()
	light.position = Vector3(0, 2.6, -1)
	light.omni_range = 30
	light.light_energy = 1.5
	add_child(light)

	# A corridor: floor, walls and pillars at many depths, so the blur shows.
	add_box(Vector3(0, -0.05, -15), Vector3(6, 0.1, 34), Color(0.3, 0.3, 0.32))
	add_box(Vector3(-3, 1.5, -15), Vector3(0.1, 3, 34), Color(0.35, 0.33, 0.3))
	add_box(Vector3(3, 1.5, -15), Vector3(0.1, 3, 34), Color(0.35, 0.33, 0.3))
	for i in 8:
		add_box(Vector3(-2.3 if i % 2 == 0 else 2.3, 1.5, -3.0 - i * 3.5), Vector3(0.5, 3, 0.5), Color(0.5, 0.48, 0.45))

	figure = Node3D.new()
	figure.position = start_position
	add_child(figure)
	var body := MeshInstance3D.new()
	var capsule := CapsuleMesh.new()
	capsule.radius = 0.3
	capsule.height = 1.8
	var skin := StandardMaterial3D.new()
	skin.albedo_color = Color(0.85, 0.82, 0.78)
	capsule.material = skin
	body.mesh = capsule
	body.position = Vector3(0, 0.9, 0)
	figure.add_child(body)

	target = GazeTarget3D.new()
	target.position = Vector3(0, 1.0, 0)
	target.radius = 0.6
	figure.add_child(target)

	var layer := CanvasLayer.new()
	add_child(layer)
	label = Label.new()
	# Bottom left: the tracker's debug overlay is at the top left.
	label.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	label.grow_vertical = Control.GROW_DIRECTION_BEGIN
	label.position += Vector2(16, -16)
	label.add_theme_font_size_override("font_size", 18)
	layer.add_child(label)

	EyeTracker.debug_overlay = true
	_start_tracker()


func add_box(p_position: Vector3, p_size: Vector3, p_color: Color) -> void:
	var box := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = p_size
	var material := StandardMaterial3D.new()
	material.albedo_color = p_color
	mesh.material = material
	box.mesh = mesh
	box.position = p_position
	add_child(box)


func _start_tracker() -> void:
	if using_webcam:
		var err := EyeTracker.start(EyeTracker.SOURCE_WEBCAM)
		if err != OK:
			print("Webcam not available (error %d); following the mouse." % err)
	else:
		EyeTracker.start(EyeTracker.SOURCE_MOUSE)


func _process(delta: float) -> void:
	# It comes closer only while nobody watches it.
	if not target.is_watched():
		var to_camera := camera.global_position - figure.global_position
		to_camera.y = 0
		if to_camera.length() > 1.2:
			figure.global_position += to_camera.normalized() * 2.5 * delta
	label.text = "Figure: %s, %.1f m away\nC calibrate  F gaze focus (%s)  D debug  M %s  R reset" % [
		"WATCHED" if target.is_watched() else "moving",
		camera.global_position.distance_to(figure.global_position),
		"on" if attributes.gaze_focus_enabled else "off",
		"use mouse" if using_webcam else "use webcam",
	]


func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey and event.pressed and not event.echo):
		return
	match event.keycode:
		KEY_C:
			EyeTracker.start_calibration()
		KEY_F:
			attributes.gaze_focus_enabled = not attributes.gaze_focus_enabled
		KEY_D:
			EyeTracker.debug_overlay = not EyeTracker.debug_overlay
		KEY_M:
			using_webcam = not using_webcam
			_start_tracker()
		KEY_R:
			figure.position = start_position
