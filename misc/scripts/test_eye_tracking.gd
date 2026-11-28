# Headless checks for eye tracking: the EyeTracker API without a webcam, the
# privacy switch, GazeTarget3D, no face in empty pictures and, when a photo is
# provided, the models on it.
#
# Run from an empty project folder holding this file:
#   bin/asosdot.windows.editor.x86_64.console.exe --headless --path <folder> --script res://test_eye_tracking.gd
#
# The photo checks (face box, eye points, irises, eyelids) are skipped unless the
# folder also holds portrait.jpg, MediaPipe's test picture:
#   https://storage.googleapis.com/mediapipe-assets/portrait.jpg
# It shows a real person, so it is not kept in this repository. Don't commit it.
# The expected positions below are for that picture only.
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

func near(a: Vector2, b: Vector2, tolerance: float) -> bool:
	return a.distance_to(b) <= tolerance

func photo_checks(tracker: Object) -> void:
	# --- The models on a still photo ---------------------------------------
	print("Photo")
	var photo := Image.load_from_file(ProjectSettings.globalize_path("res://portrait.jpg"))
	check(photo != null and not photo.is_empty(), "portrait.jpg loaded")
	var r: Dictionary = tracker.analyze_image(photo)
	check(r.found, "face found")
	if r.found:
		print("    %.1f ms, score %.2f, box %s" % [r.time_ms, r.score, r.box])
		check(r.score > 0.8, "face score above 0.8")
		var box: Rect2 = r.box
		check(box.has_point(Vector2(400, 230)) and box.size.x > 180 and box.size.x < 300, "face box around the face")
		var kp: PackedVector2Array = r.keypoints
		# From the Python reference run of the same models.
		check(near(kp[0], Vector2(357, 181), 8.0), "left eye point (%s)" % kp[0])
		check(near(kp[1], Vector2(454, 182), 8.0), "right eye point (%s)" % kp[1])
		check(near(kp[2], Vector2(407, 236), 10.0), "nose point (%s)" % kp[2])
		var eyes: Array = r.eyes
		check(eyes[0].valid and eyes[1].valid, "both eyes measured")
		check(near(eyes[0].iris, Vector2(363, 175), 5.0), "left iris centre (%s)" % eyes[0].iris)
		check(near(eyes[1].iris, Vector2(451, 173.4), 5.0), "right iris centre (%s)" % eyes[1].iris)
		for e in 2:
			check(eyes[e].iris_radius > 5.0 and eyes[e].iris_radius < 11.0, "iris %d radius %.1f px" % [e, eyes[e].iris_radius])
			check(eyes[e].openness > 0.15 and eyes[e].openness < 0.45, "eye %d open (%.2f)" % [e, eyes[e].openness])
			check(absf(eyes[e].iris_offset.x) < 0.15 and absf(eyes[e].iris_offset.y) < 0.15, "eye %d looks ahead (%s)" % [e, eyes[e].iris_offset])
		check(absf(r.yaw) < 0.1, "head turned little (yaw %.3f)" % r.yaw)
		check(absf(r.roll) < 0.1, "head upright (roll %.3f)" % r.roll)
		check(r.time_ms < 400.0, "one frame measured in %.1f ms" % r.time_ms)

		# --- The same face with the eyes painted over: eyelids shut ------------
		print("Eyes painted shut")
		var shut := photo.duplicate() as Image
		var skin := photo.get_pixel(380, 205)
		for e in 2:
			var c: Vector2 = eyes[e].iris
			shut.fill_rect(Rect2i(int(c.x) - 26, int(c.y) - 9, 52, 18), skin)
		var rs: Dictionary = tracker.analyze_image(shut)
		check(rs.found, "face still found")
		if rs.found:
			for e in 2:
				check(rs.eyes[e].openness < eyes[e].openness * 0.6, "eye %d reads as closed (%.2f, open was %.2f)" % [e, rs.eyes[e].openness, eyes[e].openness])

		# --- Mirrored: the eyes swap sides --------------------------------------
		print("Mirrored photo")
		var mirrored := photo.duplicate() as Image
		mirrored.flip_x()
		var rm: Dictionary = tracker.analyze_image(mirrored)
		check(rm.found, "face found mirrored")
		if rm.found:
			var w := photo.get_width()
			check(near(rm.eyes[0].iris, Vector2(w - eyes[1].iris.x, eyes[1].iris.y), 5.0), "mirrored left iris matches the right one")
			check(near(rm.eyes[1].iris, Vector2(w - eyes[0].iris.x, eyes[0].iris.y), 5.0), "mirrored right iris matches the left one")

		# --- Looking aside: the iris moves within the eye -----------------------
		# Shift the eye picture under the eyelids: move a patch around each iris
		# 5 px to the right, which is what an eye turning right looks like.
		print("Iris moved")
		var aside := photo.duplicate() as Image
		for e in 2:
			var c2: Vector2 = eyes[e].iris
			var patch := photo.get_region(Rect2i(int(c2.x) - 9, int(c2.y) - 9, 18, 18))
			aside.blit_rect(patch, Rect2i(0, 0, 18, 18), Vector2i(int(c2.x) - 9 + 5, int(c2.y) - 9))
		var ra: Dictionary = tracker.analyze_image(aside)
		check(ra.found, "face found with the irises moved")
		if ra.found:
			# The pasted patch drags part of the lids along, so the eye's corners
			# move a little too; the iris has to move more than they do.
			for e in 2:
				check(ra.eyes[e].iris.x > eyes[e].iris.x + 1.5, "eye %d iris moved right in the frame (%.1f -> %.1f)" % [e, eyes[e].iris.x, ra.eyes[e].iris.x])
				check(ra.eyes[e].iris_offset.x > eyes[e].iris_offset.x + 0.01, "eye %d iris offset moved right (%.3f -> %.3f)" % [e, eyes[e].iris_offset.x, ra.eyes[e].iris_offset.x])

	print("Quarter size")
	var small := photo.duplicate() as Image
	small.resize(205, 256)
	var rsm: Dictionary = tracker.analyze_image(small)
	check(rsm.found, "face found at a quarter size")

func _init() -> void:
	await process_frame
	var tracker := EyeTracker
	check(tracker != null, "EyeTracker singleton exists")

	# --- The models on a still photo, when there is one ------------------
	if FileAccess.file_exists("res://portrait.jpg"):
		photo_checks(tracker)
	else:
		print("Photo checks skipped (no portrait.jpg; see the top of this file)")

	# --- No face ---------------------------------------------------------------
	print("No face")
	var blank := Image.create(640, 480, false, Image.FORMAT_RGB8)
	blank.fill(Color(0.4, 0.45, 0.5))
	check(not tracker.analyze_image(blank).found, "no face in a blank picture")
	var noise := Image.create(320, 240, false, Image.FORMAT_RGB8)
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	for y in 240:
		for x in 320:
			noise.set_pixel(x, y, Color(rng.randf(), rng.randf(), rng.randf()))
	check(not tracker.analyze_image(noise).found, "no face in noise")

	# --- Privacy: the webcam stays off unless the project allows it ----------
	print("Privacy")
	check(ProjectSettings.get_setting("eye_tracking/webcam/enabled") == false, "webcam off by default in Project Settings")
	var err: int = tracker.start(EyeTracker.SOURCE_WEBCAM)
	check(err != OK, "start(SOURCE_WEBCAM) refused while the project setting is off")
	check(tracker.get_status() == EyeTracker.STATUS_NO_CAMERA, "status says no camera")
	check(tracker.get_camera_feed_id() == -1, "no camera feed opened")
	tracker.stop()
	check(tracker.get_status() == EyeTracker.STATUS_STOPPED, "stopped")

	# --- Without a webcam -----------------------------------------------------
	print("Screen centre source")
	check(tracker.is_looking_at_screen() and not tracker.are_eyes_closed(), "not running: looking, eyes open")
	check(tracker.start(EyeTracker.SOURCE_SCREEN_CENTER) == OK, "start(SOURCE_SCREEN_CENTER)")
	await process_frame
	check(tracker.get_status() == EyeTracker.STATUS_SIMULATED, "status simulated")
	check(tracker.get_gaze_uv() == Vector2(0.5, 0.5), "gaze at the centre")
	check(tracker.is_looking_at_screen(), "looking at the screen")

	var signals := []
	# Lambdas on an engine singleton must be disconnected before quitting: the
	# engine tears singletons down after the script language, and a lambda
	# still connected then crashes the exit (an engine bug, Input does it too).
	var listeners := {
		"eyes_closed": func(): signals.append("closed"),
		"eyes_opened": func(_t): signals.append("opened"),
		"blinked": func(): signals.append("blinked"),
		"looked_away": func(): signals.append("away"),
		"looked_back": func(): signals.append("back"),
	}
	for signal_name: String in listeners:
		tracker.connect(signal_name, listeners[signal_name])
	InputMap.add_action("test_blink")
	tracker.simulated_blink_action = "test_blink"
	Input.action_press("test_blink")
	await process_frame
	await process_frame
	check(tracker.are_eyes_closed(), "blink action closes the eyes")
	check(not tracker.is_looking_at_screen(), "closed eyes are not looking")
	Input.action_release("test_blink")
	await process_frame
	await process_frame
	check(not tracker.are_eyes_closed(), "released: eyes open")
	check(signals == ["closed", "away", "opened", "blinked", "back"], "signals in order %s" % [signals])
	for signal_name: String in listeners:
		tracker.disconnect(signal_name, listeners[signal_name])

	# --- Calibration data ------------------------------------------------------
	print("Calibration data")
	check(tracker.get_calibration_data().is_empty(), "nothing calibrated yet")
	# The format get_calibration_data() gives: version 3, 12 features, then six blocks of 12.
	var data := PackedFloat32Array([3.0, 12.0])
	for i in 72:
		data.append(0.1 * i + 0.05)
	check(tracker.set_calibration_data(data), "calibration data accepted")
	check(tracker.is_calibrated(), "calibrated")
	check(tracker.get_calibration_data() == data, "calibration data round trip")
	check(not tracker.set_calibration_data(PackedFloat32Array([1.0, 2.0])), "bad calibration data refused")
	var old := PackedFloat32Array([1.0, 7.0])
	old.resize(30)
	check(not tracker.set_calibration_data(old), "calibration from the older tracker refused")
	tracker.clear_calibration()
	check(not tracker.is_calibrated(), "calibration cleared")

	# --- GazeTarget3D ----------------------------------------------------------
	print("GazeTarget3D")
	var world := Node3D.new()
	root.add_child(world)
	var camera := Camera3D.new()
	world.add_child(camera)
	camera.current = true
	var ahead := GazeTarget3D.new()
	ahead.position = Vector3(0, 0, -5)
	world.add_child(ahead)
	var aside_target := GazeTarget3D.new()
	aside_target.position = Vector3(3, 0, -5)
	world.add_child(aside_target)
	var behind := GazeTarget3D.new()
	behind.position = Vector3(0, 0, 5)
	world.add_child(behind)
	var watched_events := []
	ahead.watched.connect(func(): watched_events.append("watched"))
	ahead.unwatched.connect(func(): watched_events.append("unwatched"))
	await process_frame
	await process_frame
	check(ahead.is_watched(), "target straight ahead is watched")
	check(not aside_target.is_watched() and aside_target.is_on_screen(), "target to the side is on screen but not watched")
	check(not behind.is_watched() and not behind.is_on_screen(), "target behind the camera is not seen")
	aside_target.detection_mode = GazeTarget3D.DETECTION_ON_SCREEN
	await process_frame
	check(aside_target.is_watched(), "ON_SCREEN mode: the side target counts")

	ahead.unwatch_delay = 0.0
	Input.action_press("test_blink")
	await process_frame
	await process_frame
	check(not ahead.is_watched(), "eyes closed: not watched")
	Input.action_release("test_blink")
	await process_frame
	await process_frame
	check(ahead.is_watched(), "eyes open again: watched")
	check(watched_events == ["watched", "unwatched", "watched"], "watched signals %s" % [watched_events])

	# Line of sight: a wall between camera and target.
	var wall := StaticBody3D.new()
	var shape := CollisionShape3D.new()
	var box_shape := BoxShape3D.new()
	box_shape.size = Vector3(4, 4, 0.2)
	shape.shape = box_shape
	wall.add_child(shape)
	wall.position = Vector3(0, 0, -2.5)
	world.add_child(wall)
	ahead.require_line_of_sight = true
	await physics_frame
	await physics_frame
	await process_frame
	check(not ahead.is_watched(), "hidden behind a wall: not watched")
	wall.position = Vector3(10, 0, -2.5)
	await physics_frame
	await physics_frame
	await process_frame
	check(ahead.is_watched(), "wall moved away: watched")

	# Stopped tracker: GazeTarget3D falls back to the centre, eyes open.
	tracker.stop()
	await process_frame
	check(ahead.is_watched(), "tracker stopped: the centre counts as looked at")

	print("%d checks, %d failed" % [checks, failures])
	quit(1 if failures > 0 else 0)
