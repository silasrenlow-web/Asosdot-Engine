/**************************************************************************/
/*  render_editor_plugin.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "render_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/io/dir_access.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/aspect_ratio_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/separator.h"
#include "scene/gui/slider.h"
#include "scene/gui/split_container.h"
#include "scene/gui/subviewport_container.h"
#include "scene/gui/tab_bar.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "scene/resources/sky.h"
#include "servers/movie_writer/movie_writer.h"
#include "servers/rendering/rendering_server.h"

namespace {

// The largest texture a GPU is sure to take, which caps supersampling.
constexpr int MAX_RENDER_SIZE = 16384;
// Renders drawn and thrown away before the first frame is kept, so effects
// that build up over frames, such as ambient occlusion, have settled.
constexpr int WARMUP_FRAMES = 4;

// The backdrop's own render layer, so the studio's shadowless fill and rim
// lights can leave it alone while it catches the key light's shadow.
constexpr uint32_t BACKDROP_LAYER = 1 << 19;

// Black where the key light is blocked, clear where it lands: the subject's
// shadow on its own, for a transparent render.
constexpr const char *SHADOW_CATCHER_SHADER = R"(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, specular_disabled;

uniform float opacity = 0.65;

void fragment() {
	ALBEDO = vec3(0.0);
	ALPHA = opacity;
}

void light() {
	ALPHA = min(ALPHA, opacity * (1.0 - ATTENUATION));
}
)";

Transform3D orbit_transform(const Vector3 &p_target, float p_yaw, float p_pitch, float p_distance) {
	const Basis basis = Basis(Vector3(0, 1, 0), p_yaw) * Basis(Vector3(1, 0, 0), p_pitch);
	return Transform3D(basis, p_target + basis.xform(Vector3(0, 0, p_distance)));
}

float yaw_of(const Basis &p_basis) {
	const Vector3 forward = -p_basis.get_column(2);
	return Math::atan2(-forward.x, -forward.z);
}

template <typename F>
void each_geometry(Node *p_node, const F &p_function) {
	GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node);
	if (geometry && geometry->is_visible_in_tree()) {
		p_function(geometry);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		each_geometry(p_node->get_child(i), p_function);
	}
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                 AsosdotRenderShot                                 */
/* -------------------------------------------------------------------------- */

Dictionary AsosdotRenderShot::get_defaults() {
	Dictionary d;
	d["shot_name"] = String("Shot 1");
	d["camera_blend"] = 0.0;
	d["camera"] = NodePath();
	d["fov"] = 35.0;
	d["studio"] = STUDIO_STUDIO;
	d["light_rotation"] = 0.0;
	d["light_energy"] = 1.0;
	d["backdrop"] = true;
	d["backdrop_color"] = Color(0.62, 0.62, 0.64);
	d["transparent_background"] = false;
	d["ground_shadow"] = true;
	d["ambient_occlusion"] = true;
	d["resolution"] = RESOLUTION_1080P;
	d["width"] = 1920;
	d["height"] = 1080;
	d["supersampling"] = 2;
	d["duration"] = 4.0;
	d["fps"] = 30;
	d["camera_motion"] = CAMERA_MOTION_TURNTABLE;
	d["animation_player"] = NodePath();
	d["animation"] = String();
	d["animation_speed"] = 1.0;
	d["animation_start"] = 0.0;
	d["dialogue"] = String(); // codespell:ignore dialogue
	d["dialogue_offset"] = 0.0;
	d["dialogue_volume_db"] = 0.0;
	d["music"] = String();
	d["music_start"] = 0.0;
	d["music_offset"] = 0.0;
	d["music_volume_db"] = 0.0;
	return d;
}

AsosdotRenderShot::AsosdotRenderShot() {
	values = get_defaults();
	editor_set_section_unfold("Shot", true);
	editor_set_section_unfold("Camera", true);
	editor_set_section_unfold("Sequence Output", true);
}

Variant AsosdotRenderShot::get_value(const StringName &p_name) const {
	return values.get(p_name, Variant());
}

void AsosdotRenderShot::set_value(const StringName &p_name, const Variant &p_value) {
	set(p_name, p_value);
}

void AsosdotRenderShot::set_state(const Dictionary &p_state) {
	values = get_defaults();
	for (const KeyValue<Variant, Variant> &E : p_state) {
		if (values.has(E.key) && Variant::can_convert(E.value.get_type(), Variant(values[E.key]).get_type())) {
			_set(E.key, E.value);
		}
	}
	notify_property_list_changed();
	emit_changed();
}

bool AsosdotRenderShot::_set(const StringName &p_name, const Variant &p_value) {
	if (!values.has(p_name)) {
		return false;
	}
	const Variant::Type type = Variant(values[p_name]).get_type();
	Variant value = p_value;
	if (value.get_type() != type) {
		Callable::CallError ce;
		const Variant *args[1] = { &p_value };
		Variant::construct(type, value, args, 1, ce);
		if (ce.error != Callable::CallError::CALL_OK) {
			return false;
		}
	}
	if (type == Variant::FLOAT && !Math::is_finite(double(value))) {
		return false;
	}
	if (p_name == SNAME("duration")) {
		value = CLAMP(double(value), 0.1, 3600.0);
	} else if (p_name == SNAME("fps")) {
		value = CLAMP(int(value), 1, 240);
	} else if (p_name == SNAME("supersampling")) {
		value = CLAMP(int(value), 1, 4);
	} else if (p_name == SNAME("animation_speed")) {
		value = CLAMP(double(value), 0.01, 100.0);
	} else if (p_name == SNAME("camera_blend") || p_name == SNAME("animation_start") || p_name == SNAME("music_start") || p_name == SNAME("music_offset") || p_name == SNAME("dialogue_offset")) {
		value = CLAMP(double(value), 0.0, 86400.0);
	} else if (p_name == SNAME("music_volume_db") || p_name == SNAME("dialogue_volume_db")) {
		value = CLAMP(double(value), -80.0, 24.0);
	}
	values[p_name] = value;
	if (p_name == SNAME("studio") || p_name == SNAME("resolution") || p_name == SNAME("camera") || p_name == SNAME("animation_player") || p_name == SNAME("transparent_background")) {
		notify_property_list_changed();
	}
	emit_changed();
	return true;
}

bool AsosdotRenderShot::_get(const StringName &p_name, Variant &r_ret) const {
	if (!values.has(p_name)) {
		return false;
	}
	r_ret = values[p_name];
	return true;
}

void AsosdotRenderShot::_get_property_list(List<PropertyInfo> *p_list) const {
	const bool scene_camera = !NodePath(values["camera"]).is_empty();
	const bool studio_lit = int(values["studio"]) != STUDIO_SCENE;
	const bool custom = int(values["resolution"]) == RESOLUTION_CUSTOM;
	const uint32_t hidden = PROPERTY_USAGE_NO_EDITOR;
	const uint32_t shown = PROPERTY_USAGE_DEFAULT;
	if (sequence_output) {
		p_list->push_back(PropertyInfo(Variant::NIL, "Sequence Output", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
		p_list->push_back(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_ENUM, U"1280 × 720,1920 × 1080,2560 × 1440,3840 × 2160 (4K),1080 × 1080,2048 × 2048,1080 × 1920 (Portrait),512 × 512 (Icon),Custom"));
		p_list->push_back(PropertyInfo(Variant::INT, "width", PROPERTY_HINT_RANGE, "16,16384,1,suffix:px", custom ? shown : hidden));
		p_list->push_back(PropertyInfo(Variant::INT, "height", PROPERTY_HINT_RANGE, "16,16384,1,suffix:px", custom ? shown : hidden));
		p_list->push_back(PropertyInfo(Variant::INT, "supersampling", PROPERTY_HINT_ENUM, U"Off:1,2×:2,3×:3,4×:4"));
		p_list->push_back(PropertyInfo(Variant::INT, "fps", PROPERTY_HINT_RANGE, "1,240,1,suffix:FPS"));
		p_list->push_back(PropertyInfo(Variant::NIL, "Music", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
		p_list->push_back(PropertyInfo(Variant::STRING, "music", PROPERTY_HINT_FILE, "*.wav,*.ogg,*.mp3"));
		p_list->push_back(PropertyInfo(Variant::FLOAT, "music_start", PROPERTY_HINT_RANGE, "0,3600,0.01,or_greater,suffix:s"));
		p_list->push_back(PropertyInfo(Variant::FLOAT, "music_offset", PROPERTY_HINT_RANGE, "0,3600,0.01,or_greater,suffix:s"));
		p_list->push_back(PropertyInfo(Variant::FLOAT, "music_volume_db", PROPERTY_HINT_RANGE, "-80,24,0.1,suffix:dB"));
		return;
	}
	p_list->push_back(PropertyInfo(Variant::NIL, "Shot", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	p_list->push_back(PropertyInfo(Variant::STRING, "shot_name"));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "duration", PROPERTY_HINT_RANGE, "0.1,3600,0.01,suffix:s"));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "camera_blend", PROPERTY_HINT_RANGE, "0,60,0.01,suffix:s"));

	p_list->push_back(PropertyInfo(Variant::NIL, "Camera", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	p_list->push_back(PropertyInfo(Variant::NODE_PATH, "camera", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Camera3D"));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "fov", PROPERTY_HINT_RANGE, "5,120,0.1,degrees", scene_camera ? hidden : shown));

	p_list->push_back(PropertyInfo(Variant::NIL, "Studio", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	p_list->push_back(PropertyInfo(Variant::INT, "studio", PROPERTY_HINT_ENUM, "Scene Lighting,Studio,Soft Box,Outdoor,Night,Spotlight"));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "light_rotation", PROPERTY_HINT_RANGE, "-180,180,1,degrees", studio_lit ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "light_energy", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater", studio_lit ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::BOOL, "backdrop", PROPERTY_HINT_NONE, "", studio_lit ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::COLOR, "backdrop_color", PROPERTY_HINT_COLOR_NO_ALPHA, "", studio_lit ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::BOOL, "ambient_occlusion", PROPERTY_HINT_NONE, "", studio_lit ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::BOOL, "transparent_background"));
	p_list->push_back(PropertyInfo(Variant::BOOL, "ground_shadow", PROPERTY_HINT_NONE, "", (studio_lit && bool(values["transparent_background"])) ? shown : hidden));

	p_list->push_back(PropertyInfo(Variant::NIL, "Animation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	p_list->push_back(PropertyInfo(Variant::INT, "camera_motion", PROPERTY_HINT_ENUM, "None,Turntable,Half Turn,Swing,Dolly In", scene_camera ? hidden : shown));
	p_list->push_back(PropertyInfo(Variant::NODE_PATH, "animation_player", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AnimationPlayer"));

	// The animations of the chosen player, found through the scene.
	String names;
	const Node *base = Object::cast_to<Node>(get_meta(SNAME("__base_node_relative"), Variant()));
	const NodePath player_path = values["animation_player"];
	const AnimationPlayer *player = (base && base->is_inside_tree() && !player_path.is_empty()) ? Object::cast_to<AnimationPlayer>(base->get_node_or_null(player_path)) : nullptr;
	if (player) {
		LocalVector<StringName> list;
		player->get_animation_list(&list);
		for (const StringName &animation_name : list) {
			if (animation_name == SNAME("RESET")) {
				continue;
			}
			names += (names.is_empty() ? "" : ",") + String(animation_name);
		}
	}
	p_list->push_back(PropertyInfo(Variant::STRING, "animation", PROPERTY_HINT_ENUM_SUGGESTION, names, player ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "animation_speed", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater", player ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "animation_start", PROPERTY_HINT_RANGE, "0,3600,0.01,or_greater,suffix:s", player ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::NIL, "Dialogue", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP)); // codespell:ignore dialogue
	p_list->push_back(PropertyInfo(Variant::STRING, "dialogue", PROPERTY_HINT_FILE, "*.wav,*.ogg,*.mp3")); // codespell:ignore dialogue
	p_list->push_back(PropertyInfo(Variant::FLOAT, "dialogue_offset", PROPERTY_HINT_RANGE, "0,3600,0.01,or_greater,suffix:s"));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "dialogue_volume_db", PROPERTY_HINT_RANGE, "-80,24,0.1,suffix:dB"));
}

void AsosdotRenderShot::_validate_property(PropertyInfo &p_property) const {
	// A shot is settings, not a resource anyone saves.
	if (String(p_property.name).begins_with("resource_")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

bool AsosdotRenderShot::_property_can_revert(const StringName &p_name) const {
	return values.has(p_name);
}

bool AsosdotRenderShot::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	const Dictionary defaults = get_defaults();
	if (!defaults.has(p_name)) {
		return false;
	}
	r_property = defaults[p_name];
	return true;
}

Size2i AsosdotRenderShot::get_output_size() const {
	switch (int(values["resolution"])) {
		case RESOLUTION_720P:
			return Size2i(1280, 720);
		case RESOLUTION_1080P:
			return Size2i(1920, 1080);
		case RESOLUTION_1440P:
			return Size2i(2560, 1440);
		case RESOLUTION_4K:
			return Size2i(3840, 2160);
		case RESOLUTION_SQUARE_1080:
			return Size2i(1080, 1080);
		case RESOLUTION_SQUARE_2048:
			return Size2i(2048, 2048);
		case RESOLUTION_PORTRAIT_1080:
			return Size2i(1080, 1920);
		case RESOLUTION_ICON_512:
			return Size2i(512, 512);
		default:
			return Size2i(CLAMP(int(values["width"]), 16, MAX_RENDER_SIZE), CLAMP(int(values["height"]), 16, MAX_RENDER_SIZE));
	}
}

void AsosdotRenderShot::_bind_methods() {
	BIND_ENUM_CONSTANT(STUDIO_SCENE);
	BIND_ENUM_CONSTANT(STUDIO_STUDIO);
	BIND_ENUM_CONSTANT(STUDIO_SOFT_BOX);
	BIND_ENUM_CONSTANT(STUDIO_OUTDOOR);
	BIND_ENUM_CONSTANT(STUDIO_NIGHT);
	BIND_ENUM_CONSTANT(STUDIO_SPOTLIGHT);
}

/* -------------------------------------------------------------------------- */
/*                                   Scene                                    */
/* -------------------------------------------------------------------------- */

Node *AsosdotRenderEditor::_get_scene_root() const {
	if (EditorNode::get_editor_data().get_edited_scene_count() == 0) {
		return nullptr;
	}
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	return root && root->is_inside_tree() ? root : nullptr;
}

AABB AsosdotRenderEditor::_get_subject_bounds() const {
	Node *root = _get_scene_root();
	AABB bounds;
	bool any = false;
	if (root) {
		each_geometry(root, [&](GeometryInstance3D *p_geometry) {
			const AABB box = p_geometry->get_global_transform().xform(p_geometry->get_aabb());
			if (!box.has_volume() && !box.has_surface()) {
				return;
			}
			bounds = any ? bounds.merge(box) : box;
			any = true;
		});
	}
	if (!any) {
		return AABB(Vector3(-0.5, 0, -0.5), Vector3(1, 1, 1));
	}
	return bounds;
}

Camera3D *AsosdotRenderEditor::_get_scene_camera() const {
	const NodePath path = shot->get_value("camera");
	Node *root = _get_scene_root();
	if (path.is_empty() || !root) {
		return nullptr;
	}
	return Object::cast_to<Camera3D>(root->get_node_or_null(path));
}

AnimationPlayer *AsosdotRenderEditor::_get_animation_player() const {
	const NodePath path = shot->get_value("animation_player");
	Node *root = _get_scene_root();
	if (path.is_empty() || !root) {
		return nullptr;
	}
	return Object::cast_to<AnimationPlayer>(root->get_node_or_null(path));
}

void AsosdotRenderEditor::_frame_subject() {
	const AABB bounds = _get_subject_bounds();
	subject_bounds = bounds;
	const float radius = MAX(bounds.size.length() * 0.5f, 0.05f);
	const Size2i out = sequence_output->get_output_size();
	const float ratio = float(out.x) / MAX(out.y, 1);
	// Godot's field of view is vertical; a tall picture is narrower across.
	float fov = Math::deg_to_rad(float(shot->get_value("fov")));
	if (ratio < 1.0f) {
		fov = 2.0f * Math::atan(Math::tan(fov * 0.5f) * ratio);
	}
	orbit_target = bounds.get_center();
	orbit_distance = radius / Math::sin(fov * 0.5f) * 1.1f;
	framed = true;
	_update_studio();
}

/* -------------------------------------------------------------------------- */
/*                                   Camera                                   */
/* -------------------------------------------------------------------------- */

Transform3D AsosdotRenderEditor::_camera_transform_at(float p_time, float &r_fov) const {
	if (Camera3D *camera = _get_scene_camera()) {
		r_fov = camera->get_fov();
		return camera->get_global_transform();
	}
	float yaw = orbit_yaw;
	float distance = orbit_distance;
	const float duration = MAX(float(shot->get_value("duration")), 0.01f);
	const float u = CLAMP(p_time / duration, 0.0f, 1.0f);
	switch (int(shot->get_value("camera_motion"))) {
		case AsosdotRenderShot::CAMERA_MOTION_TURNTABLE:
			yaw += Math::TAU * u;
			break;
		case AsosdotRenderShot::CAMERA_MOTION_HALF_TURN:
			yaw += Math::PI * u;
			break;
		case AsosdotRenderShot::CAMERA_MOTION_SWING:
			yaw += Math::sin(Math::TAU * u) * Math::deg_to_rad(25.0f);
			break;
		case AsosdotRenderShot::CAMERA_MOTION_DOLLY_IN:
			distance *= Math::lerp(1.25f, 0.8f, u * u * (3.0f - 2.0f * u));
			break;
		default:
			break;
	}
	r_fov = shot->get_value("fov");
	return orbit_transform(orbit_target, yaw, orbit_pitch, distance);
}

void AsosdotRenderEditor::_update_cameras() {
	float fov = 35.0;
	Transform3D xform = _camera_transform_at(time, fov);
	Camera3D *scene_camera = _get_scene_camera();
	const int projection = scene_camera ? int(scene_camera->get_projection()) : int(Camera3D::PROJECTION_PERSPECTIVE);
	const int keep_aspect = scene_camera ? int(scene_camera->get_keep_aspect_mode()) : int(Camera3D::KEEP_HEIGHT);
	if (camera_blend_active && projection == camera_blend_projection && keep_aspect == camera_blend_aspect) {
		xform = camera_blend_from.interpolate_with(xform, camera_blend_weight);
	}
	for (Camera3D *camera : { preview_camera, render_camera }) {
		if (scene_camera) {
			camera->set_projection(scene_camera->get_projection());
			camera->set_size(scene_camera->get_size());
			camera->set_near(scene_camera->get_near());
			camera->set_far(scene_camera->get_far());
			camera->set_keep_aspect_mode(scene_camera->get_keep_aspect_mode());
		} else {
			camera->set_projection(Camera3D::PROJECTION_PERSPECTIVE);
			camera->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
			const float radius = MAX(subject_bounds.size.length() * 0.5f, 0.05f);
			camera->set_near(MAX(radius * 0.005f, 0.01f));
			camera->set_far(MAX(radius * 200.0f, orbit_distance * 20.0f));
		}
		const bool blend = camera_blend_active && int(camera->get_projection()) == camera_blend_projection && int(camera->get_keep_aspect_mode()) == camera_blend_aspect;
		camera->set_fov(blend ? Math::lerp(camera_blend_fov, fov, camera_blend_weight) : fov);
		if (blend) {
			camera->set_size(Math::lerp(camera_blend_size, camera->get_size(), camera_blend_weight));
		}
		if (camera->is_inside_tree()) {
			camera->set_global_transform(xform);
		} else {
			camera->set_transform(xform);
		}
	}
	_place_studio(xform);
}

/* -------------------------------------------------------------------------- */
/*                                   Studio                                   */
/* -------------------------------------------------------------------------- */

void AsosdotRenderEditor::_update_studio() {
	const int mode = shot->get_value("studio");
	const bool transparent = shot->get_value("transparent_background");
	const bool lit = active && mode != AsosdotRenderShot::STUDIO_SCENE;
	const float energy = shot->get_value("light_energy");

	// The 3D view's preview sun and sky would add to the studio's lights.
	if (Node3DEditor::get_singleton()) {
		Node3DEditor::get_singleton()->set_preview_suspended(lit);
	}

	studio->set_visible(active);
	key_light->set_visible(lit);
	fill_light->set_visible(false);
	rim_light->set_visible(false);
	spot_light->set_visible(false);
	// A transparent render keeps the subject's shadow on the ground, with
	// the ground itself left out, so a cut-out still stands on something.
	const bool catch_shadow = transparent && bool(shot->get_value("ground_shadow"));
	backdrop->set_visible(lit && bool(shot->get_value("backdrop")) && (!transparent || catch_shadow));
	backdrop->set_material_override(catch_shadow ? Ref<Material>(shadow_catcher_material) : Ref<Material>());
	for (Light3D *light : { (Light3D *)fill_light, (Light3D *)rim_light }) {
		light->set_cull_mask(catch_shadow ? ~BACKDROP_LAYER : 0xFFFFFFFF);
	}
	backdrop_material->set_albedo(shot->get_value("backdrop_color"));

	for (DirectionalLight3D *light : { key_light, fill_light, rim_light }) {
		light->set_shadow(light == key_light);
		light->set_param(Light3D::PARAM_SIZE, 1.0);
		light->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
	}

	environment->set_tonemapper(Environment::TONE_MAPPER_AGX);
	environment->set_ssao_enabled(bool(shot->get_value("ambient_occlusion")));
	environment->set_glow_enabled(true);
	environment->set_glow_intensity(0.3);
	environment->set_reflection_source(Environment::REFLECTION_SOURCE_BG);
	environment->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
	environment->set_background(Environment::BG_COLOR);
	environment->set_sky(Ref<Sky>());

	switch (mode) {
		case AsosdotRenderShot::STUDIO_STUDIO: {
			// Key, fill and rim: the classic three.
			environment->set_bg_color(Color(0.16, 0.16, 0.18));
			environment->set_ambient_light_color(Color(0.55, 0.57, 0.62));
			environment->set_ambient_light_energy(0.35);
			key_light->set_color(Color(1.0, 0.95, 0.88));
			key_light->set_param(Light3D::PARAM_ENERGY, 2.2 * energy);
			key_light->set_param(Light3D::PARAM_SIZE, 3.0);
			fill_light->set_visible(lit);
			fill_light->set_color(Color(0.72, 0.82, 1.0));
			fill_light->set_param(Light3D::PARAM_ENERGY, 0.55 * energy);
			rim_light->set_visible(lit);
			rim_light->set_color(Color(1, 1, 1));
			rim_light->set_param(Light3D::PARAM_ENERGY, 1.8 * energy);
		} break;
		case AsosdotRenderShot::STUDIO_SOFT_BOX: {
			// Big soft sources and a bright seamless backdrop: product shots.
			environment->set_bg_color(Color(0.92, 0.92, 0.94));
			environment->set_ambient_light_color(Color(0.92, 0.93, 0.97));
			environment->set_ambient_light_energy(0.8);
			key_light->set_color(Color(1, 1, 1));
			key_light->set_param(Light3D::PARAM_ENERGY, 1.5 * energy);
			key_light->set_param(Light3D::PARAM_SIZE, 14.0);
			fill_light->set_visible(lit);
			fill_light->set_color(Color(1, 1, 1));
			fill_light->set_param(Light3D::PARAM_ENERGY, 0.8 * energy);
			rim_light->set_visible(lit);
			rim_light->set_color(Color(1, 1, 1));
			rim_light->set_param(Light3D::PARAM_ENERGY, 0.6 * energy);
		} break;
		case AsosdotRenderShot::STUDIO_OUTDOOR: {
			// Sun and sky.
			environment->set_background(Environment::BG_SKY);
			Ref<Sky> sky;
			sky.instantiate();
			sky->set_material(sky_material);
			environment->set_sky(sky);
			environment->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
			environment->set_ambient_light_energy(1.0);
			environment->set_reflection_source(Environment::REFLECTION_SOURCE_SKY);
			key_light->set_color(Color(1.0, 0.94, 0.84));
			key_light->set_param(Light3D::PARAM_ENERGY, 2.0 * energy);
			key_light->set_param(Light3D::PARAM_SIZE, 0.6);
			key_light->set_sky_mode(DirectionalLight3D::SKY_MODE_LIGHT_AND_SKY);
		} break;
		case AsosdotRenderShot::STUDIO_NIGHT: {
			// Moonlight and a cold rim, for the dark.
			environment->set_bg_color(Color(0.01, 0.012, 0.025));
			environment->set_ambient_light_color(Color(0.12, 0.15, 0.25));
			environment->set_ambient_light_energy(0.3);
			key_light->set_color(Color(0.55, 0.65, 1.0));
			key_light->set_param(Light3D::PARAM_ENERGY, 0.6 * energy);
			rim_light->set_visible(lit);
			rim_light->set_color(Color(0.45, 0.75, 1.0));
			rim_light->set_param(Light3D::PARAM_ENERGY, 1.6 * energy);
		} break;
		case AsosdotRenderShot::STUDIO_SPOTLIGHT: {
			// One hard light from above in the dark: horror posters.
			environment->set_bg_color(Color(0, 0, 0));
			environment->set_ambient_light_color(Color(0.06, 0.06, 0.07));
			environment->set_ambient_light_energy(0.15);
			key_light->set_visible(false);
			spot_light->set_visible(lit);
			spot_light->set_color(Color(1.0, 0.93, 0.82));
			spot_light->set_param(Light3D::PARAM_ENERGY, 12.0 * energy);
			rim_light->set_visible(lit);
			rim_light->set_color(Color(0.8, 0.85, 1.0));
			rim_light->set_param(Light3D::PARAM_ENERGY, 0.35 * energy);
		} break;
		default:
			break;
	}

	// The scene's own environment when it lights itself, cleared behind the
	// subject when the background is to be transparent.
	// Scene Lighting looks at the scene exactly as the 3D view does: its View Environment choice
	// decides between the scene's own environment, with the preview sun and sky when it has none,
	// and the plain editor environment. The 3D view is the reference, nothing is added to it.
	Ref<Environment> view_environment;
	if (Node3DEditor *editor_3d = Node3DEditor::get_singleton()) {
		if (Node3DEditorViewport *view = editor_3d->get_last_used_viewport()) {
			view_environment = view->get_view_environment();
		}
	}
	Ref<Environment> camera_environment;
	if (lit) {
		camera_environment = environment;
		if (transparent) {
			camera_environment = environment->duplicate();
			camera_environment->set_background(Environment::BG_CLEAR_COLOR);
		}
	} else if (transparent) {
		Ref<Environment> scene_environment = get_viewport() && get_viewport()->find_world_3d().is_valid() ? get_viewport()->find_world_3d()->get_environment() : Ref<Environment>();
		if (Camera3D *scene_camera = _get_scene_camera(); scene_camera && scene_camera->get_environment().is_valid()) {
			scene_environment = scene_camera->get_environment();
		} else if (!scene_camera && view_environment.is_valid()) {
			scene_environment = view_environment;
		}
		camera_environment = scene_environment.is_valid() ? Ref<Environment>(scene_environment->duplicate()) : Ref<Environment>(memnew(Environment));
		camera_environment->set_background(Environment::BG_CLEAR_COLOR);
	} else if (Camera3D *scene_camera = _get_scene_camera()) {
		camera_environment = scene_camera->get_environment();
	} else {
		camera_environment = view_environment;
	}
	preview_camera->set_environment(camera_environment);
	render_camera->set_environment(camera_environment);
	preview->set_transparent_background(transparent);
	render_viewport->set_transparent_background(transparent);
	preview_holder->queue_redraw();

	subject_bounds = _get_subject_bounds();
	_update_cameras();
	_update_output_label();
}

void AsosdotRenderEditor::_place_studio(const Transform3D &p_camera) {
	const Vector3 center = subject_bounds.get_center();
	const float radius = MAX(subject_bounds.size.length() * 0.5f, 0.05f);
	const float camera_yaw = yaw_of(p_camera.basis);
	// The rig turns with the camera, so a turntable looks like the subject
	// turning under fixed lights. light_rotation swings it round from there.
	const float rig = camera_yaw + Math::deg_to_rad(float(shot->get_value("light_rotation")));
	const float reach = p_camera.origin.distance_to(center) + radius * 4.0f;

	key_light->set_transform(Transform3D(Basis(Vector3(0, 1, 0), rig + Math::deg_to_rad(35.0f)) * Basis(Vector3(1, 0, 0), Math::deg_to_rad(-40.0f)), center));
	fill_light->set_transform(Transform3D(Basis(Vector3(0, 1, 0), rig - Math::deg_to_rad(55.0f)) * Basis(Vector3(1, 0, 0), Math::deg_to_rad(-12.0f)), center));
	rim_light->set_transform(Transform3D(Basis(Vector3(0, 1, 0), rig + Math::deg_to_rad(165.0f)) * Basis(Vector3(1, 0, 0), Math::deg_to_rad(-35.0f)), center));
	key_light->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, reach);

	const Basis spot_basis(Vector3(0, 1, 0), rig + Math::deg_to_rad(20.0f));
	const Vector3 spot_at = center + spot_basis.xform(Vector3(0, radius * 3.0f, radius * 2.0f));
	spot_light->set_transform(Transform3D().looking_at(center - spot_at).translated(spot_at));
	spot_light->set_param(Light3D::PARAM_RANGE, radius * 8.0f);
	spot_light->set_param(Light3D::PARAM_SPOT_ANGLE, 26.0);

	// The backdrop stands behind the subject as the camera sees it, its floor
	// at the subject's feet, big enough to fill the picture.
	const float size = MAX(radius * 2.5f, p_camera.origin.distance_to(center));
	if (backdrop_built_size <= 0.0f || size > backdrop_built_size * 1.25f || size < backdrop_built_size * 0.6f) {
		_build_backdrop(size);
	}
	backdrop->set_transform(Transform3D(Basis(Vector3(0, 1, 0), camera_yaw), Vector3(center.x, subject_bounds.position.y, center.z)));
}

void AsosdotRenderEditor::_build_backdrop(float p_size) {
	backdrop_built_size = p_size;
	// A cyclorama: a floor that curves up into a wall, so there is no corner
	// behind the subject.
	const float front = p_size * 3.0f;
	const float back = p_size * 0.9f;
	const float bend = p_size * 0.8f;
	const float height = p_size * 4.0f;
	const float half_width = p_size * 5.0f;

	LocalVector<Vector2> profile; // (z, y), from the front edge up the wall.
	profile.push_back(Vector2(front, 0));
	profile.push_back(Vector2(-back, 0));
	constexpr int BEND_STEPS = 12;
	for (int i = 1; i <= BEND_STEPS; i++) {
		const float a = Math::PI * 0.5f * i / BEND_STEPS;
		profile.push_back(Vector2(-back - bend * Math::sin(a), bend - bend * Math::cos(a)));
	}
	profile.push_back(Vector2(-back - bend, height));

	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedInt32Array indices;
	for (uint32_t i = 0; i < profile.size(); i++) {
		const Vector2 prev = profile[i > 0 ? i - 1 : i];
		const Vector2 next = profile[i + 1 < profile.size() ? i + 1 : i];
		const Vector2 tangent = (next - prev).normalized();
		const Vector3 normal = Vector3(0, -tangent.x, tangent.y).normalized();
		for (float x : { -half_width, half_width }) {
			vertices.push_back(Vector3(x, profile[i].y, profile[i].x));
			normals.push_back(normal);
		}
	}
	for (uint32_t i = 0; i + 1 < profile.size(); i++) {
		const int l0 = i * 2;
		const int r0 = l0 + 1;
		const int l1 = l0 + 2;
		const int r1 = l0 + 3;
		indices.push_back(l0);
		indices.push_back(l1);
		indices.push_back(r0);
		indices.push_back(r0);
		indices.push_back(l1);
		indices.push_back(r1);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, backdrop_material);
	backdrop->set_mesh(mesh);
}

/* -------------------------------------------------------------------------- */
/*                                    Time                                    */
/* -------------------------------------------------------------------------- */

void AsosdotRenderEditor::_apply_time(float p_time) {
	time = p_time;
	if (!evaluating_transition) {
		sequence_time = _shot_start(selected_shot) + p_time;
	}
	clip_time = double(shot->get_value("animation_start")) + double(p_time) * double(shot->get_value("animation_speed"));
	applied_animation = vformat("%s|%s|%s|%s", shot->get_value("animation_player"), shot->get_value("animation"), shot->get_value("animation_speed"), shot->get_value("animation_start"));
	AnimationPlayer *player = _get_animation_player();
	const StringName animation = String(shot->get_value("animation"));
	if (player && animation != StringName() && player->has_animation(animation)) {
		if (!posed || posed_player != player->get_instance_id() || posed_clip != animation) {
			_restore_pose();
			_save_pose(player, animation);
		}
		const Ref<Animation> clip = player->get_animation(animation);
		if (clip->get_length() > 0.0) {
			clip_time = clip->get_loop_mode() != Animation::LOOP_NONE ? Math::fmod(clip_time, (double)clip->get_length()) : MIN(clip_time, (double)clip->get_length());
		}
		if (player->get_assigned_animation() != animation) {
			player->set_assigned_animation(animation);
		}
		player->seek(clip_time, true);
	} else {
		// The clip is gone, or another was chosen with nothing to play.
		_restore_pose();
	}
	_update_cameras();
	_update_timeline();
}

void AsosdotRenderEditor::_save_pose(AnimationPlayer *p_player, const StringName &p_clip) {
	saved_pose.clear();
	posed = true;
	posed_player = p_player->get_instance_id();
	posed_clip = p_clip;
	posed_assigned = p_player->get_assigned_animation();
	const Ref<Animation> clip = p_player->get_animation(p_clip);
	Node *animation_root = p_player->get_node_or_null(p_player->get_root_node());
	if (clip.is_null() || !animation_root) {
		return;
	}
	for (int t = 0; t < clip->get_track_count(); t++) {
		SavedValue saved;
		if (_saved_value_for_track(clip, t, animation_root, saved)) {
			saved_pose.push_back(saved);
		}
	}
}

bool AsosdotRenderEditor::_saved_value_for_track(const Ref<Animation> &p_clip, int p_track, Node *p_animation_root, SavedValue &r_saved) const {
	const NodePath path = p_clip->track_get_path(p_track);
	Node *node = p_animation_root->get_node_or_null(NodePath(path.get_concatenated_names()));
	if (!node) {
		return false;
	}
	r_saved.object = node->get_instance_id();
	switch (p_clip->track_get_type(p_track)) {
		case Animation::TYPE_VALUE:
		case Animation::TYPE_BEZIER: {
			r_saved.property = path.get_subnames();
			bool valid = false;
			r_saved.value = node->get_indexed(r_saved.property, &valid);
			return valid && !r_saved.property.is_empty();
		}
		case Animation::TYPE_POSITION_3D:
		case Animation::TYPE_ROTATION_3D:
		case Animation::TYPE_SCALE_3D: {
			Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node);
			if (skeleton && path.get_subname_count() == 1) {
				r_saved.bone = skeleton->find_bone(path.get_subname(0));
				if (r_saved.bone < 0) {
					return false;
				}
				Array pose;
				pose.push_back(skeleton->get_bone_pose_position(r_saved.bone));
				pose.push_back(skeleton->get_bone_pose_rotation(r_saved.bone));
				pose.push_back(skeleton->get_bone_pose_scale(r_saved.bone));
				r_saved.value = pose;
				return true;
			}
			if (Node3D *node_3d = Object::cast_to<Node3D>(node)) {
				r_saved.value = node_3d->get_transform();
				return true;
			}
			return false;
		}
		case Animation::TYPE_BLEND_SHAPE: {
			MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(node);
			if (!mesh_instance || path.get_subname_count() != 1) {
				return false;
			}
			r_saved.blend_shape = mesh_instance->find_blend_shape_by_name(path.get_subname(0));
			if (r_saved.blend_shape < 0) {
				return false;
			}
			r_saved.value = mesh_instance->get_blend_shape_value(r_saved.blend_shape);
			return true;
		}
		default:
			return false;
	}
}

String AsosdotRenderEditor::_saved_value_key(const SavedValue &p_saved) {
	String property;
	for (const StringName &name : p_saved.property) {
		property += ":" + String(name);
	}
	return vformat("%d/%d/%d%s", (uint64_t)p_saved.object, p_saved.bone, p_saved.blend_shape, property);
}

void AsosdotRenderEditor::_put_back(const SavedValue &p_saved) {
	Object *object = ObjectDB::get_instance(p_saved.object);
	if (!object) {
		return;
	}
	if (p_saved.blend_shape >= 0) {
		Object::cast_to<MeshInstance3D>(object)->set_blend_shape_value(p_saved.blend_shape, p_saved.value);
	} else if (p_saved.bone >= 0) {
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object);
		const Array pose = p_saved.value;
		skeleton->set_bone_pose_position(p_saved.bone, pose[0]);
		skeleton->set_bone_pose_rotation(p_saved.bone, pose[1]);
		skeleton->set_bone_pose_scale(p_saved.bone, pose[2]);
	} else if (p_saved.property.is_empty()) {
		Object::cast_to<Node3D>(object)->set_transform(p_saved.value);
	} else {
		object->set_indexed(p_saved.property, p_saved.value);
	}
}

void AsosdotRenderEditor::_sync_saved_pose() {
	// Keys can give the clip tracks it didn't have, and undoing them take
	// tracks away. What a new track animates is saved as it is now, which is
	// the pose it was given by hand; what a track no longer animates goes
	// back to what was saved for it and is forgotten.
	AnimationPlayer *player = ObjectDB::get_instance<AnimationPlayer>(posed_player);
	if (!posed || !player || !player->has_animation(posed_clip)) {
		return;
	}
	const Ref<Animation> clip = player->get_animation(posed_clip);
	Node *animation_root = player->get_node_or_null(player->get_root_node());
	if (clip.is_null() || !animation_root) {
		return;
	}
	HashSet<String> have;
	for (const SavedValue &saved : saved_pose) {
		have.insert(_saved_value_key(saved));
	}
	HashSet<String> wanted;
	LocalVector<SavedValue> fresh;
	for (int t = 0; t < clip->get_track_count(); t++) {
		SavedValue saved;
		if (!_saved_value_for_track(clip, t, animation_root, saved)) {
			continue;
		}
		const String key = _saved_value_key(saved);
		wanted.insert(key);
		if (!have.has(key)) {
			have.insert(key);
			fresh.push_back(saved);
		}
	}
	for (int i = (int)saved_pose.size() - 1; i >= 0; i--) {
		if (!wanted.has(_saved_value_key(saved_pose[i]))) {
			_put_back(saved_pose[i]);
			saved_pose.remove_at(i);
		}
	}
	for (const SavedValue &saved : fresh) {
		saved_pose.push_back(saved);
	}
}

void AsosdotRenderEditor::_restore_pose() {
	if (!posed) {
		return;
	}
	posed = false;
	// In reverse, so where two tracks touch the same thing, the value saved
	// first, from before posing, is the one left.
	for (int i = (int)saved_pose.size() - 1; i >= 0; i--) {
		_put_back(saved_pose[i]);
	}
	saved_pose.clear();
	AnimationPlayer *player = ObjectDB::get_instance<AnimationPlayer>(posed_player);
	if (player && posed_assigned != player->get_assigned_animation() && player->has_animation(posed_assigned)) {
		// The animation that was chosen before; its pose is back already.
		player->set_assigned_animation(posed_assigned);
	}
}

void AsosdotRenderEditor::_update_timeline() {
	// The sequence strip draws the playhead and the keys, so only the readout is kept here.
	const double duration = _sequence_duration();
	sequence_strip->queue_redraw();
	time_label->set_text(_format_timecode(sequence_time, sequence_output->get_value("fps")));
	time_label->set_tooltip_text(vformat(TTR("%s / %s seconds"), String::num(sequence_time, 3), String::num(duration, 3)));
}

void AsosdotRenderEditor::_seek_sequence(double p_time) {
	playing = false;
	play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
	_stop_audio();
	_apply_sequence_time(p_time);
}

void AsosdotRenderEditor::_play_pressed() {
	if (job.active) {
		return;
	}
	playing = !playing;
	_update_preview_audio(true);
	play_button->set_button_icon(get_editor_theme_icon(playing ? SNAME("Pause") : SNAME("Play")));
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */

void AsosdotRenderEditor::_shot_changed() {
	if (updating_sequence) {
		return;
	}
	_update_sequence_ui();
	sequence_time = _shot_start(selected_shot) + MIN(time, float(shot->get_value("duration")));
	if (job.active) {
		return;
	}
	if (!active) {
		// Undoing keys from another tab can change the shot; the scene is
		// only posed while the tab is open.
		_update_output_label();
		_update_timeline();
		return;
	}
	_update_studio();
	const float t = MIN(time, float(shot->get_value("duration")));
	// Lighting, size and the like leave a pose that isn't keyed yet alone.
	if (t != time || camera_blend_active || double(shot->get_value("camera_blend")) > 0.0 || applied_animation != vformat("%s|%s|%s|%s", shot->get_value("animation_player"), shot->get_value("animation"), shot->get_value("animation_speed"), shot->get_value("animation_start"))) {
		_apply_sequence_time(sequence_time);
	} else {
		_update_cameras();
		_update_timeline();
	}
	_update_preview_audio(true);
	_update_pose_bar();
}

void AsosdotRenderEditor::_update_output_label() {
	const Size2i out = sequence_output->get_output_size();
	aspect->set_ratio(float(out.x) / MAX(out.y, 1));
	size_label->set_text(vformat(U"%d × %d · %d FPS", out.x, out.y, sequence_output->get_value("fps")));
	_update_workspace_labels();
	if (pose_mode) {
		hint_label->set_text(TTR("R: Rotate   G: Move   E: Reach   I: Key Pose"));
		hint_label->set_tooltip_text(TTR("Click a bone or part to pose it. Shift+drag rotates freely; Alt+drag twists. Shift+I keys the whole skeleton; Alt+I deletes keys. Drag elsewhere to orbit."));
	} else {
		hint_label->set_text(_get_scene_camera() ? TTR(U"Scene camera · Click a subject to select it") : TTR("Drag: Orbit   Shift+drag: Pan   Scroll: Zoom"));
		hint_label->set_tooltip_text(TTR("The preview shows the render camera. Click a subject to select it for editing. Studio cameras can orbit, pan and zoom."));
	}
}

/* -------------------------------------------------------------------------- */
/*                                   Input                                    */
/* -------------------------------------------------------------------------- */

// The navigator speaks the 3D view's language: the camera sits `distance` behind a focus point,
// turned by y_rot and x_rot, which are the negatives of yaw and pitch.
void AsosdotRenderEditor::_sync_navigator_from_orbit() {
	View3DController::Cursor &cursor = navigator->cursor;
	cursor.pos = orbit_target;
	cursor.y_rot = -orbit_yaw;
	cursor.x_rot = -orbit_pitch;
	cursor.unsnapped_y_rot = cursor.y_rot;
	cursor.unsnapped_x_rot = cursor.x_rot;
	cursor.distance = orbit_distance;
	cursor.fov_scale = 1.0;
	const Vector3 forward = navigator->to_camera_transform().basis.xform(Vector3(0, 0, -1));
	cursor.eye_pos = cursor.pos - cursor.distance * forward;
	// Without a delta the smoothed cursor is the cursor itself, which is what freelook starts from.
	navigator->update_camera();
}

bool AsosdotRenderEditor::_sync_orbit_from_navigator() {
	const View3DController::Cursor &cursor = navigator->cursor;
	const Vector3 target = cursor.pos;
	const float yaw = -cursor.y_rot;
	const float pitch = -cursor.x_rot;
	const float distance = cursor.distance;
	if (target == orbit_target && yaw == orbit_yaw && pitch == orbit_pitch && distance == orbit_distance) {
		return false;
	}
	orbit_target = target;
	orbit_yaw = yaw;
	orbit_pitch = pitch;
	orbit_distance = distance;
	return true;
}

void AsosdotRenderEditor::_set_freelook(bool p_enabled) {
	if (navigator->is_freelook_enabled() == p_enabled) {
		return;
	}
	if (p_enabled) {
		_sync_navigator_from_orbit();
	}
	// Hides and captures the mouse while on, and puts it back where it was when off.
	navigator->set_freelook_enabled(p_enabled);
	if (p_enabled && !preview_surface->has_focus()) {
		preview_surface->grab_focus();
	}
	if (!p_enabled && _sync_orbit_from_navigator()) {
		_update_cameras();
	}
}

void AsosdotRenderEditor::_preview_input(const Ref<InputEvent> &p_event) {
	if (job.active) {
		return;
	}
	const bool studio_camera = _get_scene_camera() == nullptr;
	if (!studio_camera) {
		_set_freelook(false);
	}
	const Ref<InputEventMouseButton> mb = p_event;
	bool navigated = false;
	if (studio_camera && pose_drag.kind == DRAG_NONE) {
		// Holding the right mouse button flies the camera like in the 3D view, with the same keys.
		if (mb.is_valid() && mb->get_button_index() == MouseButton::RIGHT) {
			if (!mb->is_pressed()) {
				_set_freelook(false);
			} else if (Node3DEditorViewport::starts_freelook(mb)) {
				_set_freelook(true);
			}
		}
		// Everything else the 3D view does with the mouse: orbit, pan, zoom, the modifiers
		// and buttons the editor settings choose.
		_sync_navigator_from_orbit();
		navigated = navigator->gui_input(p_event, preview_surface->get_global_rect());
		if (_sync_orbit_from_navigator()) {
			_update_cameras();
		}
	}
	const Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && !k->is_echo()) {
		if (navigator->is_freelook_enabled()) {
			// The keys move the camera now, they are not the pose tools.
			if (k->get_keycode() == Key::ESCAPE) {
				_set_freelook(false);
				preview_surface->accept_event();
			}
			return;
		}
		if (pose_drag.kind != DRAG_NONE && k->get_keycode() == Key::ESCAPE) {
			_cancel_pose_drag();
			preview_surface->accept_event();
			return;
		}
		if (!pose_mode || pose_drag.kind != DRAG_NONE) {
			return;
		}
		bool handled = true;
		// An int, since the shifted and alted cases are not keys of the enum, which clang
		// refuses in a switch over it.
		switch ((int)k->get_keycode_with_modifiers()) {
			case (int)Key::R:
				_set_pose_tool(POSE_TOOL_ROTATE);
				break;
			case (int)Key::G:
				_set_pose_tool(POSE_TOOL_MOVE);
				break;
			case (int)Key::E:
				_set_pose_tool(POSE_TOOL_REACH);
				break;
			case (int)Key::I:
				_key(KEY_POSED);
				break;
			case (int)(KeyModifierMask::SHIFT | Key::I):
				_key(KEY_SKELETON);
				break;
			case (int)(KeyModifierMask::ALT | Key::I):
				_key(KEY_DELETE);
				break;
			case (int)Key::UP:
				_select_parent();
				break;
			default:
				handled = false;
		}
		if (handled) {
			preview_surface->accept_event();
		}
		return;
	}
	if (mb.is_valid() && pose_drag.kind != DRAG_NONE) {
		// Another button while posing puts the pose back as it was.
		if (mb->is_pressed() && mb->get_button_index() == MouseButton::RIGHT) {
			_cancel_pose_drag();
		} else if (!mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
			_end_pose_drag();
		}
		preview_surface->accept_event();
		return;
	}
	if (mb.is_valid() && pose_mode && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
		if (_begin_pose_drag(mb->get_position(), mb->is_shift_pressed(), mb->is_alt_pressed())) {
			playing = false;
			play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
			preview_surface->accept_event();
			return;
		}
	}
	if (mb.is_valid()) {
		if (mb->get_button_index() == MouseButton::WHEEL_UP || mb->get_button_index() == MouseButton::WHEEL_DOWN) {
			// Zoomed by the navigator above, with the studio camera; a scene camera stays where it is.
			preview_surface->accept_event();
			return;
		}
		if (mb->get_button_index() == MouseButton::LEFT || mb->get_button_index() == MouseButton::MIDDLE || mb->get_button_index() == MouseButton::RIGHT) {
			if (mb->is_pressed()) {
				dragging = true;
				drag_moved = false;
			} else {
				if (dragging && !drag_moved && mb->get_button_index() == MouseButton::LEFT) {
					if (pose_mode) {
						_pose_pick(mb->get_position());
					} else {
						_pick(mb->get_position());
					}
				}
				dragging = false;
			}
			preview_surface->accept_event();
		}
		return;
	}
	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && pose_drag.kind != DRAG_NONE) {
		_update_pose_drag(mm->get_position());
		preview_surface->accept_event();
		return;
	}
	if (mm.is_valid() && pose_mode && !dragging) {
		const PoseTarget hovered = _pick_bone(mm->get_position());
		if (!(hovered == pose_hovered)) {
			pose_hovered = hovered;
			preview_surface->queue_redraw();
		}
	}
	if (mm.is_valid() && dragging) {
		if (mm->get_relative().length() > 1.0f) {
			drag_moved = true;
		}
		if (navigated) {
			preview_surface->accept_event();
		}
	}
}

GeometryInstance3D *AsosdotRenderEditor::_raycast_geometry(const Point2 &p_point) const {
	Node *root = _get_scene_root();
	if (!root) {
		return nullptr;
	}
	const Vector3 from = preview_camera->project_ray_origin(p_point);
	const Vector3 dir = preview_camera->project_ray_normal(p_point);
	GeometryInstance3D *best = nullptr;
	float best_distance = Math::INF;
	each_geometry(root, [&](GeometryInstance3D *p_geometry) {
		const Transform3D xform = p_geometry->get_global_transform();
		const AABB box = xform.xform(p_geometry->get_aabb());
		Vector3 hit;
		bool inside = false;
		if (!box.find_intersects_ray(from, dir, inside, &hit) && !inside) {
			return;
		}
		float distance = from.distance_to(hit);
		// Meshes are tested against their triangles, so clicking past a
		// character's arm reaches what is behind it.
		const MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_geometry);
		if (mesh_instance && mesh_instance->get_mesh().is_valid()) {
			const Ref<TriangleMesh> triangles = mesh_instance->get_mesh()->generate_triangle_mesh();
			if (triangles.is_null()) {
				return;
			}
			const Transform3D inverse = xform.affine_inverse();
			Vector3 point;
			Vector3 normal;
			if (!triangles->intersect_ray(inverse.xform(from), inverse.basis.xform(dir).normalized(), point, normal)) {
				return;
			}
			distance = from.distance_to(xform.xform(point));
		}
		if (distance < best_distance) {
			best_distance = distance;
			best = p_geometry;
		}
	});
	return best;
}

void AsosdotRenderEditor::_pick(const Point2 &p_point) {
	Node *root = _get_scene_root();
	Node *best = _raycast_geometry(p_point);
	if (!root || !best) {
		return;
	}
	// Select what the scene can edit: a node of an instanced scene selects
	// the instance, as in the 3D view.
	while (best != root && best->get_owner() != root && best->get_parent()) {
		best = best->get_parent();
	}
	// Selected for the Inspector only, so the Render tab stays open while
	// its materials and colors are changed and the preview follows.
	EditorNode::get_singleton()->get_editor_selection()->clear();
	EditorNode::get_singleton()->get_editor_selection()->add_node(best);
	EditorNode::get_singleton()->push_item(best, String(), true);
}

/* -------------------------------------------------------------------------- */
/*                                 Rendering                                  */
/* -------------------------------------------------------------------------- */

String AsosdotRenderEditor::_default_output_path(const String &p_extension) const {
	// Where the last render went, or beside the scene, in the project.
	Node *root = _get_scene_root();
	const String scene_path = root ? root->get_scene_file_path() : String();
	String dir = last_output_dir;
	if (dir.is_empty() || !DirAccess::dir_exists_absolute(dir)) {
		dir = scene_path.is_empty() ? String("res://") : scene_path.get_base_dir();
	}
	const String name = (scene_path.is_empty() ? String("render") : scene_path.get_file().get_basename() + "_render") + "." + p_extension;
	// The dialogs browse the whole disk, which knows nothing of res://.
	return ProjectSettings::get_singleton()->globalize_path(dir.path_join(name));
}

String AsosdotRenderEditor::_chosen_output_path(const String &p_path) {
	// A file in the project is written through res://, so the FileSystem
	// dock finds and imports it.
	const String path = ProjectSettings::get_singleton()->localize_path(p_path);
	last_output_dir = path.get_base_dir();
	return path;
}

void AsosdotRenderEditor::_reveal_output() {
	const String path = reveal_path;
	reveal_path = String();
	if (!path.is_empty() && FileSystemDock::get_singleton() && FileAccess::exists(path)) {
		FileSystemDock::get_singleton()->navigate_to_path(path);
	}
}

void AsosdotRenderEditor::_render_image_pressed() {
	image_dialog->set_current_path(_default_output_path("png"));
	image_dialog->popup_file_dialog();
}

void AsosdotRenderEditor::_render_animation_pressed() {
	animation_dialog->set_current_path(_default_output_path("ogv"));
	animation_dialog->popup_file_dialog();
}

void AsosdotRenderEditor::_image_path_selected(const String &p_path) {
	_start_job(_chosen_output_path(p_path), false);
}

void AsosdotRenderEditor::_animation_path_selected(const String &p_path) {
	_start_job(_chosen_output_path(p_path), true);
}

void AsosdotRenderEditor::_start_job(const String &p_path, bool p_animation) {
	if (job.active) {
		return;
	}
	const String extension = p_path.get_extension().to_lower();
	Size2i out = sequence_output->get_output_size();
	MovieWriter *writer = nullptr;
	if (p_animation && extension != "png") {
		writer = MovieWriter::find_writer_for_file(p_path);
		if (!writer) {
			EditorToaster::get_singleton()->popup_str(vformat(TTR("Animations can't be written as .%s. Use .avi, .ogv or .png."), extension), EditorToaster::SEVERITY_ERROR);
			return;
		}
		// Video encoders want even dimensions.
		out -= Size2i(out.x & 1, out.y & 1);
	}
	int supersampling = MAX(int(sequence_output->get_value("supersampling")), 1);
	while (supersampling > 1 && MAX(out.x, out.y) * supersampling > MAX_RENDER_SIZE) {
		supersampling--;
	}

	playing = false;
	job = Job();
	job.active = true;
	job.animation = p_animation;
	job.path = p_path;
	job.size = out;
	job.supersampling = supersampling;
	job.fps = MAX(int(sequence_output->get_value("fps")), 1);
	job.frame_count = p_animation ? MAX(1, (int)Math::ceil(_sequence_duration() * job.fps - 0.00001)) : 1;
	job.warmup = WARMUP_FRAMES;
	job.writer = writer;
	job.time_before = sequence_time;
	job.shot_before = selected_shot;
	job.low_processor_before = OS::get_singleton()->is_in_low_processor_usage_mode();
	_stop_audio();
	_set_sequence_editing(false);
	play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));

	if (writer) {
		const Error err = writer->begin_frames(out, (uint32_t)job.fps, p_path);
		if (err != OK) {
			job.writer = nullptr;
			_finish_job(vformat(TTR("Could not start writing \"%s\"."), p_path));
			return;
		}
	}

	render_viewport->set_size(out * supersampling);
	// Soft shadows are dithered at the project's quality, which shows in a
	// still; a render has time for the smoothest filter.
	RenderingServer::get_singleton()->directional_soft_shadow_filter_set_quality(RSE::SHADOW_QUALITY_SOFT_ULTRA);
	RenderingServer::get_singleton()->positional_soft_shadow_filter_set_quality(RSE::SHADOW_QUALITY_SOFT_ULTRA);
	// Continuous redraws while rendering, one frame drawn per editor frame.
	OS::get_singleton()->set_low_processor_usage_mode(false);
	progress_bar->set_max(job.frame_count);
	progress_bar->set_value(0);
	progress_label->set_text(vformat(TTR(U"Rendering %s at %d × %d..."), p_path.get_file(), out.x, out.y));
	progress_dialog->popup_centered(Size2(420, 0) * EDSCALE);

	// Enough frames in flight to keep the cores busy, within about 512 MiB of image data.
	const int64_t frame_bytes = MAX(int64_t(out.x) * out.y * 4, int64_t(1));
	max_frames_in_flight = CLAMP(int((int64_t(512) << 20) / frame_bytes), 2, MAX(2, WorkerThreadPool::get_singleton()->get_thread_count()));
	frame_write_failed.clear();
	if (job.writer) {
		_prepare_render_audio();
		frame_writer_movie = job.writer;
		frame_slots.post(max_frames_in_flight);
		frame_writer_thread.start(&AsosdotRenderEditor::_frame_writer_thread_func, this);
	}
	set_process(true);
}

namespace {

struct SupersampleResolve {
	const uint8_t *src = nullptr;
	uint8_t *dst = nullptr;
	int src_width = 0;
	int dst_width = 0;
	int factor = 1;
};

// One output row: each pixel is the average of its factor x factor block. Colour is weighted by
// alpha, so the edges of a transparent render don't take on the background's colour.
void _resolve_supersampled_row(void *p_userdata, uint32_t p_row) {
	const SupersampleResolve &r = *static_cast<const SupersampleResolve *>(p_userdata);
	const uint32_t block = r.factor * r.factor;
	uint8_t *out = r.dst + size_t(p_row) * r.dst_width * 4;
	for (int x = 0; x < r.dst_width; x++) {
		uint32_t sum_r = 0;
		uint32_t sum_g = 0;
		uint32_t sum_b = 0;
		uint32_t sum_a = 0;
		for (int sy = 0; sy < r.factor; sy++) {
			const uint8_t *in = r.src + (size_t(p_row) * r.factor + sy) * r.src_width * 4 + size_t(x) * r.factor * 4;
			for (int sx = 0; sx < r.factor; sx++, in += 4) {
				const uint32_t a = in[3];
				sum_r += in[0] * a;
				sum_g += in[1] * a;
				sum_b += in[2] * a;
				sum_a += a;
			}
		}
		if (sum_a > 0) {
			out[0] = uint8_t((sum_r + sum_a / 2) / sum_a);
			out[1] = uint8_t((sum_g + sum_a / 2) / sum_a);
			out[2] = uint8_t((sum_b + sum_a / 2) / sum_a);
		} else {
			out[0] = 0;
			out[1] = 0;
			out[2] = 0;
		}
		out[3] = uint8_t((sum_a + block / 2) / block);
		out += 4;
	}
}

// Supersampling's resolve, done as a box filter across all cores. A Lanczos resize of the same
// frame took 150 ms at 1080p and 650 ms at 4K on one thread.
Ref<Image> _resolve_supersampled(const Ref<Image> &p_image, int p_factor, const Size2i &p_size) {
	if (p_image->get_width() != p_size.x * p_factor || p_image->get_height() != p_size.y * p_factor) {
		Ref<Image> resized = p_image->duplicate();
		resized->resize(p_size.x, p_size.y, Image::INTERPOLATE_LANCZOS);
		return resized;
	}
	const Vector<uint8_t> src = p_image->get_data();
	Vector<uint8_t> dst;
	dst.resize(p_size.x * p_size.y * 4);

	SupersampleResolve resolve;
	resolve.src = src.ptr();
	resolve.dst = dst.ptrw();
	resolve.src_width = p_image->get_width();
	resolve.dst_width = p_size.x;
	resolve.factor = p_factor;

	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const WorkerThreadPool::GroupID group = pool->add_native_group_task(&_resolve_supersampled_row, &resolve, p_size.y, -1, true, "Render supersampling resolve");
	pool->wait_for_group_task_completion(group);

	return Image::create_from_data(p_size.x, p_size.y, false, Image::FORMAT_RGBA8, dst);
}

struct ImageFileWrite {
	Ref<Image> image;
	String path;
	SafeFlag *failed = nullptr;
};

void _write_image_file(void *p_userdata) {
	ImageFileWrite *write = static_cast<ImageFileWrite *>(p_userdata);
	const String extension = write->path.get_extension().to_lower();
	Error err;
	if (extension == "jpg" || extension == "jpeg") {
		err = write->image->save_jpg(write->path, 0.92);
	} else if (extension == "webp") {
		err = write->image->save_webp(write->path, false, 0.92);
	} else {
		err = write->image->save_png(write->path);
	}
	if (err != OK) {
		write->failed->set();
	}
	memdelete(write);
}

} // namespace

void AsosdotRenderEditor::_queue_image_file(const Ref<Image> &p_image, const String &p_path) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (frame_write_tasks.size() >= uint32_t(max_frames_in_flight)) {
		pool->wait_for_task_completion(frame_write_tasks[0]);
		frame_write_tasks.remove_at(0);
	}
	ImageFileWrite *write = memnew(ImageFileWrite);
	write->image = p_image;
	write->path = p_path;
	write->failed = &frame_write_failed;
	frame_write_tasks.push_back(pool->add_native_task(&_write_image_file, write, false, "Render frame write"));
}

void AsosdotRenderEditor::_frame_writer_thread_func(void *p_userdata) {
	AsosdotRenderEditor *self = static_cast<AsosdotRenderEditor *>(p_userdata);
	while (true) {
		self->frame_queue_semaphore.wait();
		MovieFrame frame;
		{
			MutexLock lock(self->frame_queue_mutex);
			if (self->frame_queue.is_empty()) {
				if (self->frame_writer_exit) {
					return;
				}
				continue;
			}
			frame = self->frame_queue.front()->get();
			self->frame_queue.pop_front();
		}
		if (!self->frame_write_failed.is_set() && self->frame_writer_movie->write_frame_image(frame.image, frame.audio) != OK) {
			self->frame_write_failed.set();
		}
		self->frame_slots.post();
	}
}

void AsosdotRenderEditor::_queue_movie_frame(const Ref<Image> &p_image) {
	frame_slots.wait(); // Holds rendering back while the encoder catches up.
	{
		MutexLock lock(frame_queue_mutex);
		frame_queue.push_back({ p_image, _mix_render_audio() });
	}
	frame_queue_semaphore.post();
}

// Waits for every queued frame to be written. Safe to call when nothing is queued.
void AsosdotRenderEditor::_flush_frame_writes() {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	for (const WorkerThreadPool::TaskID &task : frame_write_tasks) {
		pool->wait_for_task_completion(task);
	}
	frame_write_tasks.clear();

	if (frame_writer_thread.is_started()) {
		{
			MutexLock lock(frame_queue_mutex);
			frame_writer_exit = true;
		}
		frame_queue_semaphore.post();
		frame_writer_thread.wait_to_finish();
		frame_writer_exit = false;
		while (frame_slots.try_wait()) {
		}
	}
	frame_writer_movie = nullptr;
}

Ref<Image> AsosdotRenderEditor::_grab_frame() {
	Ref<Image> image = render_viewport->get_texture()->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	if (job.supersampling > 1) {
		image = _resolve_supersampled(image, job.supersampling, job.size);
	}
	return image;
}

void AsosdotRenderEditor::_step_job() {
	if (job.cancelled) {
		_finish_job(TTR("Rendering was cancelled."));
		return;
	}
	if (job.waiting) {
		// What was asked for last frame has been drawn.
		if (job.warmup > 0) {
			job.warmup--;
			render_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
			return;
		}
		const Ref<Image> image = _grab_frame();
		if (image.is_null()) {
			_finish_job(TTR("The render came back empty."));
			return;
		}
		if (job.writer) {
			_queue_movie_frame(image);
		} else if (job.animation) {
			_queue_image_file(image, vformat("%s_%s.png", job.path.get_basename(), String::num_int64(job.frame + 1).pad_zeros(4)));
		} else {
			_queue_image_file(image, job.path);
		}
		if (frame_write_failed.is_set()) {
			_finish_job(vformat(TTR("Could not write \"%s\"."), job.path));
			return;
		}
		job.frame++;
		job.waiting = false;
		progress_bar->set_value(job.frame);
		if (job.frame >= job.frame_count) {
			// The last frames may still be writing, and a failure there should still be reported.
			_flush_frame_writes();
			_finish_job(frame_write_failed.is_set() ? vformat(TTR("Could not write \"%s\"."), job.path) : String());
			return;
		}
	}
	if (job.animation) {
		_apply_sequence_time(job.frame / job.fps);
		if (job.rendered_shot != selected_shot) {
			job.warmup = WARMUP_FRAMES;
			job.rendered_shot = selected_shot;
		}
		progress_label->set_text(vformat(TTR("Rendering shot %d of %d: %s"), selected_shot + 1, shots.size(), shot->get_value("shot_name")));
	} else {
		// A still is taken as the preview shows it, with any pose that isn't
		// keyed yet.
		_update_cameras();
	}
	render_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	job.waiting = true;
}

void AsosdotRenderEditor::_finish_job(const String &p_error) {
	_flush_frame_writes();
	if (job.writer) {
		job.writer->end_frames();
		job.writer = nullptr;
	}
	OS::get_singleton()->set_low_processor_usage_mode(job.low_processor_before);
	RenderingServer::get_singleton()->directional_soft_shadow_filter_set_quality(RSE::ShadowQuality(int(GLOBAL_GET("rendering/lights_and_shadows/directional_shadow/soft_shadow_filter_quality"))));
	RenderingServer::get_singleton()->positional_soft_shadow_filter_set_quality(RSE::ShadowQuality(int(GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/soft_shadow_filter_quality"))));
	const String path = job.path;
	const bool animation = job.animation;
	const int frames = job.frame;
	const double time_before = job.time_before;
	const int shot_before = job.shot_before;
	render_audio.clear();
	job = Job();
	_set_sequence_editing(true);
	progress_dialog->hide();
	render_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	render_viewport->set_size(Size2i(2, 2));
	set_process(active);
	if (animation) {
		if (active) {
			_apply_sequence_time(time_before);
		} else {
			_restore_pose();
			_activate_shot(shot_before);
			sequence_time = time_before;
			time = MAX(0.0, time_before - _shot_start(selected_shot));
			_update_timeline();
		}
	} else {
		_update_cameras();
	}

	if (path.begins_with("res://") && frames > 0) {
		// Shown in the FileSystem dock once the scan has found it.
		reveal_path = animation && path.get_extension().to_lower() == "png" ? vformat("%s_%s.png", path.get_basename(), String::num_int64(1).pad_zeros(4)) : path;
		if (!EditorFileSystem::get_singleton()->is_connected("filesystem_changed", callable_mp(this, &AsosdotRenderEditor::_reveal_output))) {
			EditorFileSystem::get_singleton()->connect("filesystem_changed", callable_mp(this, &AsosdotRenderEditor::_reveal_output), CONNECT_ONE_SHOT);
		}
		EditorFileSystem::get_singleton()->scan_changes();
	}
	const String where = path.begins_with("res://") ? path : vformat(TTR("%s, outside the project"), path);
	if (!p_error.is_empty()) {
		EditorToaster::get_singleton()->popup_str(p_error, EditorToaster::SEVERITY_WARNING);
	} else if (animation) {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("Rendered %d frames to %s."), frames, where), EditorToaster::SEVERITY_INFO);
	} else {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("Rendered %s."), where), EditorToaster::SEVERITY_INFO);
	}
}

void AsosdotRenderEditor::_cancel_job() {
	if (job.active) {
		job.cancelled = true;
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                 */
/* -------------------------------------------------------------------------- */

void AsosdotRenderEditor::set_active(bool p_active) {
	if (active == p_active) {
		return;
	}
	active = p_active;
	preview->set_update_mode(active ? SubViewport::UPDATE_WHEN_VISIBLE : SubViewport::UPDATE_DISABLED);
	set_process_input(active);
	if (active) {
		_set_workspace_focus(workspace_focus);
		if (!framed) {
			_frame_subject();
		}
		_update_studio();
		_apply_sequence_time(sequence_time);
	} else {
		_set_freelook(false);
		if (owns_editor_focus) {
			EditorInterface::get_singleton()->set_distraction_free_mode(editor_focus_before);
			owns_editor_focus = false;
		}
		_cancel_job();
		_stop_audio();
		_cancel_pose_drag();
		playing = false;
		play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
		_restore_pose();
		_update_studio();
	}
	_update_pose_bar();
	set_process(active || job.active);
}

void AsosdotRenderEditor::scene_changed() {
	if (job.active) {
		_finish_job(TTR("Rendering stopped because the edited scene changed."));
	}
	_stop_audio();
	_set_freelook(false);
	_cancel_pose_drag();
	_restore_pose();
	playing = false;
	pose_selected = PoseTarget();
	pose_hovered = PoseTarget();
	pose_touched.clear();
	for (const SequenceShot &entry : shots) {
		entry.settings->set_meta(SNAME("__base_node_relative"), _get_scene_root());
		entry.settings->notify_property_list_changed();
	}
	if (active) {
		if (!framed) {
			_frame_subject();
		}
		_update_studio();
		_apply_sequence_time(sequence_time);
	}
	_update_pose_bar();
}

void AsosdotRenderEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PREDELETE: {
			_flush_frame_writes();
		} break;

		case NOTIFICATION_RESIZED: {
			_update_workspace_layout();
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			_update_workspace_theme();
			frame_button->set_button_icon(get_editor_theme_icon(SNAME("ZoomReset")));
			render_image_button->set_button_icon(get_editor_theme_icon(SNAME("Image")));
			render_animation_button->set_button_icon(get_editor_theme_icon(SNAME("Animation")));
			play_button->set_button_icon(get_editor_theme_icon(playing ? SNAME("Pause") : SNAME("Play")));
			pose_button->set_button_icon(get_editor_theme_icon(SNAME("Skeleton3D")));
			pose_tool_buttons[POSE_TOOL_ROTATE]->set_button_icon(get_editor_theme_icon(SNAME("ToolRotate")));
			pose_tool_buttons[POSE_TOOL_MOVE]->set_button_icon(get_editor_theme_icon(SNAME("ToolMove")));
			pose_tool_buttons[POSE_TOOL_REACH]->set_button_icon(get_editor_theme_icon(SNAME("SkeletonIK3D")));
			parent_button->set_button_icon(get_editor_theme_icon(SNAME("ArrowUp")));
			reset_pose_button->set_button_icon(get_editor_theme_icon(SNAME("Reload")));
			new_clip_button->set_button_icon(get_editor_theme_icon(SNAME("New")));
			auto_key_button->set_button_icon(get_editor_theme_icon(SNAME("AutoKey")));
			key_button->set_button_icon(get_editor_theme_icon(SNAME("Key")));
			key_menu->set_button_icon(get_editor_theme_icon(SNAME("GuiDropdown")));
			previous_key_button->set_button_icon(get_editor_theme_icon(SNAME("PagePrevious")));
			next_key_button->set_button_icon(get_editor_theme_icon(SNAME("PageNext")));
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_set_freelook(false);
			// The scene goes too when the editor closes; stop looking into it.
			inspector->edit(nullptr);
			_stop_audio();
			for (const SequenceShot &entry : shots) {
				entry.settings->remove_meta(SNAME("__base_node_relative"));
			}
		} break;

		case NOTIFICATION_ENTER_TREE: {
			_settings_selected(settings_selector->get_current_tab());
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			Node3DEditorViewport::configure_view_3d_controller(navigator.ptr(), false);
		} break;

		case NOTIFICATION_PROCESS: {
			if (job.active) {
				_set_freelook(false);
				_step_job();
				break;
			}
			if (navigator->is_freelook_enabled()) {
				if (!active || _get_scene_camera() || !Input::get_singleton()->is_mouse_button_pressed(MouseButton::RIGHT)) {
					// The button was let go somewhere we did not see it.
					_set_freelook(false);
				} else {
					// W, A, S, D, Q and E fly the camera, as they do in the 3D view.
					_sync_navigator_from_orbit();
					navigator->update_freelook(get_process_delta_time());
					if (_sync_orbit_from_navigator()) {
						_update_cameras();
					}
				}
			}
			if (repose_pending && active) {
				repose_pending = false;
				_apply_sequence_time(sequence_time);
			}
			if (playing && active) {
				const double duration = _sequence_duration();
				double t = sequence_time + get_process_delta_time();
				const bool looped = t >= duration;
				if (looped) {
					t = Math::fmod(t, duration);
				}
				_apply_sequence_time(t);
				_update_preview_audio(looped);
			}
			if (pose_mode && active) {
				// Bones follow the animation and undo.
				preview_surface->queue_redraw();
			}
		} break;
	}
}

void AsosdotRenderEditor::_draw_checker() {
	if (!bool(shot->get_value("transparent_background"))) {
		return;
	}
	// A checkerboard behind a transparent render, so the cut-out shows.
	const float cell = 12 * EDSCALE;
	const Size2 size = preview_holder->get_size();
	preview_holder->draw_rect(Rect2(Point2(), size), Color(0.42, 0.42, 0.42));
	for (int y = 0; y * cell < size.y; y++) {
		for (int x = (y & 1); x * cell < size.x; x += 2) {
			preview_holder->draw_rect(Rect2(Point2(x, y) * cell, Size2(cell, cell)).intersection(Rect2(Point2(), size)), Color(0.58, 0.58, 0.58));
		}
	}
}

AsosdotRenderEditor::AsosdotRenderEditor() {
	shot.instantiate();
	SequenceShot first;
	first.settings = shot;
	first.id = String::num_uint64(shot->get_instance_id());
	shots.push_back(first);
	sequence_output.instantiate();
	sequence_output->set_sequence_output(true);
	sequence_output->connect_changed(callable_mp(this, &AsosdotRenderEditor::_sequence_output_changed));
	shot->connect_changed(callable_mp(this, &AsosdotRenderEditor::_shot_changed));

	set_v_size_flags(SIZE_EXPAND_FILL);

	_build_workspace();

	navigator.instantiate();
	Node3DEditorViewport::configure_view_3d_controller(navigator.ptr());
	// The same limits as the old wheel zoom: from a centimetre to 10 km away (a quarter of the
	// far plane, a quarter of the near one).
	navigator->set_z_near(0.0025);
	navigator->set_z_far(40000);

	// The studio, in the scene's world but not in the scene.
	studio = memnew(Node3D);
	studio->set_visible(false);
	preview->add_child(studio);
	key_light = memnew(DirectionalLight3D);
	key_light->set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_4_SPLITS);
	studio->add_child(key_light);
	fill_light = memnew(DirectionalLight3D);
	studio->add_child(fill_light);
	rim_light = memnew(DirectionalLight3D);
	studio->add_child(rim_light);
	spot_light = memnew(SpotLight3D);
	spot_light->set_shadow(true);
	studio->add_child(spot_light);
	backdrop_material.instantiate();
	backdrop_material->set_roughness(0.9);
	Ref<Shader> catcher_shader;
	catcher_shader.instantiate();
	catcher_shader->set_code(SHADOW_CATCHER_SHADER);
	shadow_catcher_material.instantiate();
	shadow_catcher_material->set_shader(catcher_shader);
	backdrop = memnew(MeshInstance3D);
	backdrop->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	backdrop->set_layer_mask(BACKDROP_LAYER);
	studio->add_child(backdrop);
	environment.instantiate();
	sky_material.instantiate();

	// Offline rendering.
	render_viewport = memnew(SubViewport);
	render_viewport->set_size(Size2i(2, 2));
	render_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	render_viewport->set_msaa_3d(Viewport::MSAA_4X);
	render_viewport->set_positional_shadow_atlas_size(4096);
	add_child(render_viewport);
	render_camera = memnew(Camera3D);
	render_camera->set_cull_mask((1 << 20) - 1);
	render_viewport->add_child(render_camera);
	render_camera->make_current();

	image_dialog = memnew(EditorFileDialog);
	image_dialog->set_title(TTR("Render Image"));
	image_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	image_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	image_dialog->add_filter("*.png", TTR("PNG Image"));
	image_dialog->add_filter("*.jpg,*.jpeg", TTR("JPEG Image"));
	image_dialog->add_filter("*.webp", TTR("WebP Image"));
	image_dialog->connect("file_selected", callable_mp(this, &AsosdotRenderEditor::_image_path_selected));
	add_child(image_dialog);
	animation_dialog = memnew(EditorFileDialog);
	animation_dialog->set_title(TTR("Render Sequence"));
	animation_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	animation_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	animation_dialog->add_filter("*.ogv", TTR("Ogg Theora Video (plays in Godot)"));
	animation_dialog->add_filter("*.avi", TTR("AVI Video (Motion JPEG, for video editors)"));
	animation_dialog->add_filter("*.png", TTR("PNG Image Sequence"));
	animation_dialog->connect("file_selected", callable_mp(this, &AsosdotRenderEditor::_animation_path_selected));
	add_child(animation_dialog);

	progress_dialog = memnew(ConfirmationDialog);
	progress_dialog->set_title(TTR("Rendering"));
	progress_dialog->get_ok_button()->hide();
	progress_dialog->set_cancel_button_text(TTR("Cancel"));
	progress_dialog->connect("canceled", callable_mp(this, &AsosdotRenderEditor::_cancel_job));
	VBoxContainer *progress_box = memnew(VBoxContainer);
	progress_dialog->add_child(progress_box);
	progress_label = memnew(Label);
	progress_box->add_child(progress_label);
	progress_bar = memnew(ProgressBar);
	progress_box->add_child(progress_bar);
	add_child(progress_dialog);

	music_player = memnew(AudioStreamPlayer);
	music_player->set_playback_type(AudioServer::PLAYBACK_TYPE_STREAM);
	add_child(music_player);
	dialogue_player = memnew(AudioStreamPlayer);
	dialogue_player->set_playback_type(AudioServer::PLAYBACK_TYPE_STREAM);
	add_child(dialogue_player);
	_update_sequence_ui();
	_update_timeline();
}

void AsosdotRenderEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_state"), &AsosdotRenderEditor::get_state);
	ClassDB::bind_method(D_METHOD("set_state", "state"), &AsosdotRenderEditor::set_state);
	ClassDB::bind_method(D_METHOD("seek_sequence", "time"), &AsosdotRenderEditor::_apply_sequence_time);
	ClassDB::bind_method(D_METHOD("render_sequence", "path"), &AsosdotRenderEditor::_render_sequence);
	ClassDB::bind_method(D_METHOD("is_rendering"), &AsosdotRenderEditor::is_rendering);
	ClassDB::bind_method(D_METHOD("cancel_render"), &AsosdotRenderEditor::_cancel_job);
	// Called by undo and redo.
	ClassDB::bind_method(D_METHOD("_set_clip_state", "clip", "state"), &AsosdotRenderEditor::_set_clip_state);
	ClassDB::bind_method(D_METHOD("_clip_changed"), &AsosdotRenderEditor::_clip_changed);
}

AsosdotRenderEditor::~AsosdotRenderEditor() {
	if (job.writer) {
		job.writer->end_frames();
	}
}

/* -------------------------------------------------------------------------- */
/*                                   Plugin                                   */
/* -------------------------------------------------------------------------- */

void AsosdotRenderEditorPlugin::make_visible(bool p_visible) {
	render_editor->set_visible(p_visible);
	render_editor->set_active(p_visible);
}

Dictionary AsosdotRenderEditorPlugin::get_state() const {
	return render_editor->get_state();
}

void AsosdotRenderEditorPlugin::set_state(const Dictionary &p_state) {
	render_editor->set_state(p_state);
}

void AsosdotRenderEditorPlugin::clear() {
	render_editor->set_state(Dictionary());
}

void AsosdotRenderEditorPlugin::edited_scene_changed() {
	render_editor->scene_changed();
}

void AsosdotRenderEditorPlugin::apply_changes() {
	// Called before the scene is saved: a pose from the preview must not be
	// saved with it.
	render_editor->_cancel_pose_drag();
	if (render_editor->posed) {
		render_editor->_restore_pose();
		render_editor->repose_pending = true;
	}
}

AsosdotRenderEditorPlugin::AsosdotRenderEditorPlugin() {
	render_editor = memnew(AsosdotRenderEditor);
	EditorInterface::get_singleton()->get_editor_main_screen()->add_child(render_editor);
	render_editor->hide();
}
