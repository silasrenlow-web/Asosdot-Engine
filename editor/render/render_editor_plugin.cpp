/**************************************************************************/
/*  render_editor_plugin.cpp                                              */
/**************************************************************************/
/*  Asosdot: the Render workspace.                                        */
/**************************************************************************/

#include "render_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_toaster.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/aspect_ratio_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/slider.h"
#include "scene/gui/split_container.h"
#include "scene/gui/subviewport_container.h"
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
/*                                 RenderShot                                 */
/* -------------------------------------------------------------------------- */

Dictionary RenderShot::get_defaults() {
	Dictionary d;
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
	return d;
}

RenderShot::RenderShot() {
	values = get_defaults();
}

Variant RenderShot::get_value(const StringName &p_name) const {
	return values.get(p_name, Variant());
}

void RenderShot::set_value(const StringName &p_name, const Variant &p_value) {
	set(p_name, p_value);
}

void RenderShot::set_state(const Dictionary &p_state) {
	values = get_defaults();
	for (const KeyValue<Variant, Variant> &E : p_state) {
		if (values.has(E.key) && Variant::can_convert(E.value.get_type(), Variant(values[E.key]).get_type())) {
			values[E.key] = E.value;
		}
	}
	notify_property_list_changed();
	emit_changed();
}

bool RenderShot::_set(const StringName &p_name, const Variant &p_value) {
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
	values[p_name] = value;
	if (p_name == SNAME("studio") || p_name == SNAME("resolution") || p_name == SNAME("camera") || p_name == SNAME("animation_player") || p_name == SNAME("transparent_background")) {
		notify_property_list_changed();
	}
	emit_changed();
	return true;
}

bool RenderShot::_get(const StringName &p_name, Variant &r_ret) const {
	if (!values.has(p_name)) {
		return false;
	}
	r_ret = values[p_name];
	return true;
}

void RenderShot::_get_property_list(List<PropertyInfo> *p_list) const {
	const bool scene_camera = !NodePath(values["camera"]).is_empty();
	const bool studio_lit = int(values["studio"]) != STUDIO_SCENE;
	const bool custom = int(values["resolution"]) == RESOLUTION_CUSTOM;
	const uint32_t hidden = PROPERTY_USAGE_NO_EDITOR;
	const uint32_t shown = PROPERTY_USAGE_DEFAULT;

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

	p_list->push_back(PropertyInfo(Variant::NIL, "Output", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	p_list->push_back(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_ENUM, U"1280 × 720,1920 × 1080,2560 × 1440,3840 × 2160 (4K),1080 × 1080,2048 × 2048,1080 × 1920 (Portrait),512 × 512 (Icon),Custom"));
	p_list->push_back(PropertyInfo(Variant::INT, "width", PROPERTY_HINT_RANGE, "16,16384,1,suffix:px", custom ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::INT, "height", PROPERTY_HINT_RANGE, "16,16384,1,suffix:px", custom ? shown : hidden));
	p_list->push_back(PropertyInfo(Variant::INT, "supersampling", PROPERTY_HINT_ENUM, U"Off:1,2×:2,3×:3,4×:4"));

	p_list->push_back(PropertyInfo(Variant::NIL, "Animation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	p_list->push_back(PropertyInfo(Variant::FLOAT, "duration", PROPERTY_HINT_RANGE, "0.1,600,0.01,or_greater,suffix:s"));
	p_list->push_back(PropertyInfo(Variant::INT, "fps", PROPERTY_HINT_RANGE, "1,240,1,suffix:FPS"));
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
}

void RenderShot::_validate_property(PropertyInfo &p_property) const {
	// A shot is settings, not a resource anyone saves.
	if (String(p_property.name).begins_with("resource_")) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

bool RenderShot::_property_can_revert(const StringName &p_name) const {
	return values.has(p_name);
}

bool RenderShot::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	const Dictionary defaults = get_defaults();
	if (!defaults.has(p_name)) {
		return false;
	}
	r_property = defaults[p_name];
	return true;
}

Size2i RenderShot::get_output_size() const {
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

void RenderShot::_bind_methods() {
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

Node *RenderEditor::_get_scene_root() const {
	if (EditorNode::get_editor_data().get_edited_scene_count() == 0) {
		return nullptr;
	}
	Node *root = EditorNode::get_singleton()->get_edited_scene();
	return root && root->is_inside_tree() ? root : nullptr;
}

AABB RenderEditor::_get_subject_bounds() const {
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

Camera3D *RenderEditor::_get_scene_camera() const {
	const NodePath path = shot->get_value("camera");
	Node *root = _get_scene_root();
	if (path.is_empty() || !root) {
		return nullptr;
	}
	return Object::cast_to<Camera3D>(root->get_node_or_null(path));
}

AnimationPlayer *RenderEditor::_get_animation_player() const {
	const NodePath path = shot->get_value("animation_player");
	Node *root = _get_scene_root();
	if (path.is_empty() || !root) {
		return nullptr;
	}
	return Object::cast_to<AnimationPlayer>(root->get_node_or_null(path));
}

void RenderEditor::_frame_subject() {
	const AABB bounds = _get_subject_bounds();
	subject_bounds = bounds;
	const float radius = MAX(bounds.size.length() * 0.5f, 0.05f);
	const Size2i out = shot->get_output_size();
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

Transform3D RenderEditor::_camera_transform_at(float p_time, float &r_fov) const {
	if (Camera3D *camera = _get_scene_camera()) {
		r_fov = camera->get_fov();
		return camera->get_global_transform();
	}
	float yaw = orbit_yaw;
	float distance = orbit_distance;
	const float duration = MAX(float(shot->get_value("duration")), 0.01f);
	const float u = CLAMP(p_time / duration, 0.0f, 1.0f);
	switch (int(shot->get_value("camera_motion"))) {
		case RenderShot::CAMERA_MOTION_TURNTABLE:
			yaw += Math::TAU * u;
			break;
		case RenderShot::CAMERA_MOTION_HALF_TURN:
			yaw += Math::PI * u;
			break;
		case RenderShot::CAMERA_MOTION_SWING:
			yaw += Math::sin(Math::TAU * u) * Math::deg_to_rad(25.0f);
			break;
		case RenderShot::CAMERA_MOTION_DOLLY_IN:
			distance *= Math::lerp(1.25f, 0.8f, u * u * (3.0f - 2.0f * u));
			break;
		default:
			break;
	}
	r_fov = shot->get_value("fov");
	return orbit_transform(orbit_target, yaw, orbit_pitch, distance);
}

void RenderEditor::_update_cameras() {
	float fov = 35.0;
	const Transform3D xform = _camera_transform_at(time, fov);
	Camera3D *scene_camera = _get_scene_camera();
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
		camera->set_fov(fov);
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

void RenderEditor::_update_studio() {
	const int mode = shot->get_value("studio");
	const bool transparent = shot->get_value("transparent_background");
	const bool lit = active && mode != RenderShot::STUDIO_SCENE;
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
		case RenderShot::STUDIO_STUDIO: {
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
		case RenderShot::STUDIO_SOFT_BOX: {
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
		case RenderShot::STUDIO_OUTDOOR: {
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
		case RenderShot::STUDIO_NIGHT: {
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
		case RenderShot::STUDIO_SPOTLIGHT: {
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
		}
		camera_environment = scene_environment.is_valid() ? Ref<Environment>(scene_environment->duplicate()) : Ref<Environment>(memnew(Environment));
		camera_environment->set_background(Environment::BG_CLEAR_COLOR);
	} else if (Camera3D *scene_camera = _get_scene_camera()) {
		camera_environment = scene_camera->get_environment();
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

void RenderEditor::_place_studio(const Transform3D &p_camera) {
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

void RenderEditor::_build_backdrop(float p_size) {
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

void RenderEditor::_apply_time(float p_time) {
	time = p_time;
	AnimationPlayer *player = _get_animation_player();
	const StringName animation = String(shot->get_value("animation"));
	if (player && animation != StringName() && player->has_animation(animation)) {
		if (!posed || posed_player != player->get_instance_id() || posed_clip != animation) {
			_restore_pose();
			_save_pose(player, animation);
		}
		const Ref<Animation> clip = player->get_animation(animation);
		double at = double(p_time) * double(shot->get_value("animation_speed"));
		if (clip->get_length() > 0.0) {
			at = clip->get_loop_mode() != Animation::LOOP_NONE ? Math::fmod(at, (double)clip->get_length()) : MIN(at, (double)clip->get_length());
		}
		if (player->get_assigned_animation() != animation) {
			player->set_assigned_animation(animation);
		}
		player->seek(at, true);
	}
	_update_cameras();
	_update_timeline();
}

void RenderEditor::_save_pose(AnimationPlayer *p_player, const StringName &p_clip) {
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
		const NodePath path = clip->track_get_path(t);
		Node *node = animation_root->get_node_or_null(NodePath(path.get_concatenated_names()));
		if (!node) {
			continue;
		}
		SavedValue saved;
		saved.object = node->get_instance_id();
		switch (clip->track_get_type(t)) {
			case Animation::TYPE_VALUE:
			case Animation::TYPE_BEZIER: {
				saved.property = path.get_subnames();
				bool valid = false;
				saved.value = node->get_indexed(saved.property, &valid);
				if (!valid || saved.property.is_empty()) {
					continue;
				}
			} break;
			case Animation::TYPE_POSITION_3D:
			case Animation::TYPE_ROTATION_3D:
			case Animation::TYPE_SCALE_3D: {
				Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node);
				if (skeleton && path.get_subname_count() == 1) {
					saved.bone = skeleton->find_bone(path.get_subname(0));
					if (saved.bone < 0) {
						continue;
					}
					Array pose;
					pose.push_back(skeleton->get_bone_pose_position(saved.bone));
					pose.push_back(skeleton->get_bone_pose_rotation(saved.bone));
					pose.push_back(skeleton->get_bone_pose_scale(saved.bone));
					saved.value = pose;
				} else if (Node3D *node_3d = Object::cast_to<Node3D>(node)) {
					saved.value = node_3d->get_transform();
				} else {
					continue;
				}
			} break;
			case Animation::TYPE_BLEND_SHAPE: {
				MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(node);
				if (!mesh_instance || path.get_subname_count() != 1) {
					continue;
				}
				saved.blend_shape = mesh_instance->find_blend_shape_by_name(path.get_subname(0));
				if (saved.blend_shape < 0) {
					continue;
				}
				saved.value = mesh_instance->get_blend_shape_value(saved.blend_shape);
			} break;
			default:
				continue;
		}
		saved_pose.push_back(saved);
	}
}

void RenderEditor::_restore_pose() {
	if (!posed) {
		return;
	}
	posed = false;
	// In reverse, so where two tracks touch the same thing, the value saved
	// first, from before posing, is the one left.
	for (int i = (int)saved_pose.size() - 1; i >= 0; i--) {
		const SavedValue &saved = saved_pose[i];
		Object *object = ObjectDB::get_instance(saved.object);
		if (!object) {
			continue;
		}
		if (saved.blend_shape >= 0) {
			Object::cast_to<MeshInstance3D>(object)->set_blend_shape_value(saved.blend_shape, saved.value);
		} else if (saved.bone >= 0) {
			Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object);
			const Array pose = saved.value;
			skeleton->set_bone_pose_position(saved.bone, pose[0]);
			skeleton->set_bone_pose_rotation(saved.bone, pose[1]);
			skeleton->set_bone_pose_scale(saved.bone, pose[2]);
		} else if (saved.property.is_empty()) {
			Object::cast_to<Node3D>(object)->set_transform(saved.value);
		} else {
			object->set_indexed(saved.property, saved.value);
		}
	}
	saved_pose.clear();
	AnimationPlayer *player = ObjectDB::get_instance<AnimationPlayer>(posed_player);
	if (player && posed_assigned != player->get_assigned_animation() && player->has_animation(posed_assigned)) {
		// The animation that was chosen before; its pose is back already.
		player->set_assigned_animation(posed_assigned);
	}
}

void RenderEditor::_update_timeline() {
	const float duration = MAX(float(shot->get_value("duration")), 0.01f);
	timeline->set_max(duration);
	timeline->set_step(1.0 / MAX(int(shot->get_value("fps")), 1));
	timeline->set_value_no_signal(time);
	time_label->set_text(vformat("%s / %s s", String::num(time, 2), String::num(duration, 2)));
}

void RenderEditor::_timeline_changed(double p_value) {
	playing = false;
	play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
	_apply_time(p_value);
}

void RenderEditor::_play_pressed() {
	playing = !playing;
	play_button->set_button_icon(get_editor_theme_icon(playing ? SNAME("Pause") : SNAME("Play")));
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */

void RenderEditor::_shot_changed() {
	if (job.active) {
		return;
	}
	_update_studio();
	_apply_time(MIN(time, float(shot->get_value("duration"))));
}

void RenderEditor::_update_output_label() {
	const Size2i out = shot->get_output_size();
	aspect->set_ratio(float(out.x) / MAX(out.y, 1));
	size_label->set_text(vformat(U"%d × %d", out.x, out.y));
	hint_label->set_text(_get_scene_camera() ? TTR("Viewing through the scene's camera. Click the subject to select it for editing.") : TTR("Drag to orbit, Shift+drag to pan, scroll to zoom. Click the subject to select it for editing."));
}

/* -------------------------------------------------------------------------- */
/*                                   Input                                    */
/* -------------------------------------------------------------------------- */

void RenderEditor::_preview_input(const Ref<InputEvent> &p_event) {
	const bool studio_camera = _get_scene_camera() == nullptr;
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		if (mb->is_pressed() && (mb->get_button_index() == MouseButton::WHEEL_UP || mb->get_button_index() == MouseButton::WHEEL_DOWN)) {
			if (studio_camera) {
				orbit_distance *= mb->get_button_index() == MouseButton::WHEEL_UP ? 0.9f : 1.0f / 0.9f;
				orbit_distance = CLAMP(orbit_distance, 0.01f, 100000.0f);
				_update_cameras();
			}
			preview_surface->accept_event();
			return;
		}
		if (mb->get_button_index() == MouseButton::LEFT || mb->get_button_index() == MouseButton::MIDDLE || mb->get_button_index() == MouseButton::RIGHT) {
			if (mb->is_pressed()) {
				dragging = true;
				drag_moved = false;
			} else {
				if (dragging && !drag_moved && mb->get_button_index() == MouseButton::LEFT) {
					_pick(mb->get_position());
				}
				dragging = false;
			}
			preview_surface->accept_event();
		}
		return;
	}
	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && dragging) {
		if (mm->get_relative().length() > 1.0f) {
			drag_moved = true;
		}
		if (!studio_camera) {
			return;
		}
		if (mm->is_shift_pressed()) {
			const Basis basis = preview_camera->get_global_transform().basis;
			const float scale = orbit_distance * 0.0015f / EDSCALE;
			orbit_target += (-basis.get_column(0) * mm->get_relative().x + basis.get_column(1) * mm->get_relative().y) * scale;
		} else {
			orbit_yaw -= mm->get_relative().x * 0.008f;
			orbit_pitch = CLAMP(orbit_pitch - mm->get_relative().y * 0.008f, -1.55f, 1.55f);
		}
		_update_cameras();
		preview_surface->accept_event();
	}
}

void RenderEditor::_pick(const Point2 &p_point) {
	Node *root = _get_scene_root();
	if (!root) {
		return;
	}
	const Vector3 from = preview_camera->project_ray_origin(p_point);
	const Vector3 dir = preview_camera->project_ray_normal(p_point);
	Node *best = nullptr;
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
	if (!best) {
		return;
	}
	// Select what the scene can edit: a node of an instanced scene selects
	// the instance, as in the 3D view.
	while (best != root && best->get_owner() != root && best->get_parent()) {
		best = best->get_parent();
	}
	// Selected for the Inspector only, so the Render tab stays open while
	// its materials and colours are changed and the preview follows.
	EditorNode::get_singleton()->get_editor_selection()->clear();
	EditorNode::get_singleton()->get_editor_selection()->add_node(best);
	EditorNode::get_singleton()->push_item(best, String(), true);
}

/* -------------------------------------------------------------------------- */
/*                                 Rendering                                  */
/* -------------------------------------------------------------------------- */

void RenderEditor::_render_image_pressed() {
	Node *root = _get_scene_root();
	const String scene_path = root ? root->get_scene_file_path() : String();
	const String base = scene_path.is_empty() ? String("res://render") : scene_path.get_basename() + "_render";
	image_dialog->set_current_path(base + ".png");
	image_dialog->popup_file_dialog();
}

void RenderEditor::_render_animation_pressed() {
	Node *root = _get_scene_root();
	const String scene_path = root ? root->get_scene_file_path() : String();
	const String base = scene_path.is_empty() ? String("res://render") : scene_path.get_basename() + "_render";
	animation_dialog->set_current_path(base + ".avi");
	animation_dialog->popup_file_dialog();
}

void RenderEditor::_image_path_selected(const String &p_path) {
	_start_job(p_path, false);
}

void RenderEditor::_animation_path_selected(const String &p_path) {
	_start_job(p_path, true);
}

void RenderEditor::_start_job(const String &p_path, bool p_animation) {
	if (job.active) {
		return;
	}
	const String extension = p_path.get_extension().to_lower();
	Size2i out = shot->get_output_size();
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
	int supersampling = MAX(int(shot->get_value("supersampling")), 1);
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
	job.fps = MAX(int(shot->get_value("fps")), 1);
	job.frame_count = p_animation ? MAX(1, (int)Math::round(float(shot->get_value("duration")) * job.fps)) : 1;
	job.warmup = WARMUP_FRAMES;
	job.writer = writer;
	job.time_before = time;

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
	job.low_processor_before = OS::get_singleton()->is_in_low_processor_usage_mode();
	OS::get_singleton()->set_low_processor_usage_mode(false);
	progress_bar->set_max(job.frame_count);
	progress_bar->set_value(0);
	progress_label->set_text(vformat(TTR("Rendering %s at %d × %d..."), p_path.get_file(), out.x, out.y));
	progress_dialog->popup_centered(Size2(420, 0) * EDSCALE);
	set_process(true);
}

Ref<Image> RenderEditor::_grab_frame() {
	Ref<Image> image = render_viewport->get_texture()->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	if (job.supersampling > 1) {
		image->resize(job.size.x, job.size.y, Image::INTERPOLATE_LANCZOS);
	}
	return image;
}

void RenderEditor::_step_job() {
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
		Error err = OK;
		if (!job.animation) {
			const String extension = job.path.get_extension().to_lower();
			if (extension == "jpg" || extension == "jpeg") {
				err = image->save_jpg(job.path, 0.92);
			} else if (extension == "webp") {
				err = image->save_webp(job.path, false, 0.92);
			} else {
				err = image->save_png(job.path);
			}
		} else if (job.writer) {
			err = job.writer->write_frame_image(image);
		} else {
			err = image->save_png(vformat("%s_%s.png", job.path.get_basename(), String::num_int64(job.frame + 1).pad_zeros(4)));
		}
		if (err != OK) {
			_finish_job(vformat(TTR("Could not write \"%s\"."), job.path));
			return;
		}
		job.frame++;
		job.waiting = false;
		progress_bar->set_value(job.frame);
		if (job.frame >= job.frame_count) {
			_finish_job();
			return;
		}
	}
	_apply_time(job.animation ? job.frame / job.fps : time);
	render_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	job.waiting = true;
}

void RenderEditor::_finish_job(const String &p_error) {
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
	const float time_before = job.time_before;
	job = Job();
	progress_dialog->hide();
	render_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	render_viewport->set_size(Size2i(2, 2));
	set_process(active);
	_apply_time(time_before);

	if (path.begins_with("res://")) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	if (!p_error.is_empty()) {
		EditorToaster::get_singleton()->popup_str(p_error, EditorToaster::SEVERITY_WARNING);
	} else if (animation) {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("Rendered %d frames to %s."), frames, path), EditorToaster::SEVERITY_INFO);
	} else {
		EditorToaster::get_singleton()->popup_str(vformat(TTR("Rendered %s."), path), EditorToaster::SEVERITY_INFO);
	}
}

void RenderEditor::_cancel_job() {
	if (job.active) {
		job.cancelled = true;
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                 */
/* -------------------------------------------------------------------------- */

void RenderEditor::set_active(bool p_active) {
	if (active == p_active) {
		return;
	}
	active = p_active;
	preview->set_update_mode(active ? SubViewport::UPDATE_WHEN_VISIBLE : SubViewport::UPDATE_DISABLED);
	if (active) {
		if (!framed) {
			_frame_subject();
		}
		_update_studio();
		_apply_time(time);
	} else {
		playing = false;
		play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
		_restore_pose();
		_update_studio();
	}
	set_process(active || job.active);
}

void RenderEditor::scene_changed() {
	_restore_pose();
	playing = false;
	shot->set_meta(SNAME("__base_node_relative"), _get_scene_root());
	shot->notify_property_list_changed();
	if (active) {
		if (!framed) {
			_frame_subject();
		}
		_update_studio();
		_apply_time(time);
	}
}

Dictionary RenderEditor::get_state() const {
	Dictionary state;
	state["shot"] = shot->get_state();
	state["orbit_target"] = orbit_target;
	state["orbit_yaw"] = orbit_yaw;
	state["orbit_pitch"] = orbit_pitch;
	state["orbit_distance"] = orbit_distance;
	state["framed"] = framed;
	return state;
}

void RenderEditor::set_state(const Dictionary &p_state) {
	shot->set_state(p_state.get("shot", Dictionary()));
	orbit_target = p_state.get("orbit_target", Vector3());
	orbit_yaw = p_state.get("orbit_yaw", 0.6);
	orbit_pitch = p_state.get("orbit_pitch", -0.2);
	orbit_distance = p_state.get("orbit_distance", 4.0);
	framed = p_state.get("framed", false);
	shot->set_meta(SNAME("__base_node_relative"), _get_scene_root());
	if (active) {
		if (!framed) {
			_frame_subject();
		}
		_update_studio();
		_apply_time(0.0);
	}
}

void RenderEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			frame_button->set_button_icon(get_editor_theme_icon(SNAME("ZoomReset")));
			render_image_button->set_button_icon(get_editor_theme_icon(SNAME("Image")));
			render_animation_button->set_button_icon(get_editor_theme_icon(SNAME("Animation")));
			play_button->set_button_icon(get_editor_theme_icon(playing ? SNAME("Pause") : SNAME("Play")));
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// The scene goes too when the editor closes; stop looking into it.
			inspector->edit(nullptr);
			shot->remove_meta(SNAME("__base_node_relative"));
		} break;

		case NOTIFICATION_ENTER_TREE: {
			inspector->edit(shot.ptr());
		} break;

		case NOTIFICATION_PROCESS: {
			if (job.active) {
				_step_job();
				break;
			}
			if (repose_pending && active) {
				repose_pending = false;
				_apply_time(time);
			}
			if (playing && active) {
				const float duration = MAX(float(shot->get_value("duration")), 0.01f);
				float t = time + get_process_delta_time();
				if (t > duration) {
					t = Math::fmod(t, duration);
				}
				_apply_time(t);
			}
		} break;
	}
}

void RenderEditor::_draw_checker() {
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

RenderEditor::RenderEditor() {
	shot.instantiate();
	shot->connect_changed(callable_mp(this, &RenderEditor::_shot_changed));

	set_v_size_flags(SIZE_EXPAND_FILL);

	// Toolbar.
	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);
	frame_button = memnew(Button);
	frame_button->set_text(TTR("Frame Subject"));
	frame_button->set_theme_type_variation(SceneStringName(FlatButton));
	frame_button->set_tooltip_text(TTR("Point the studio camera at everything in the scene, filling the picture."));
	frame_button->connect(SceneStringName(pressed), callable_mp(this, &RenderEditor::_frame_subject));
	toolbar->add_child(frame_button);
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	toolbar->add_child(spacer);
	size_label = memnew(Label);
	size_label->set_modulate(Color(1, 1, 1, 0.7));
	toolbar->add_child(size_label);
	render_image_button = memnew(Button);
	render_image_button->set_text(TTR("Render Image"));
	render_image_button->set_tooltip_text(TTR("Render the current frame to a PNG, JPEG or WebP image."));
	render_image_button->connect(SceneStringName(pressed), callable_mp(this, &RenderEditor::_render_image_pressed));
	toolbar->add_child(render_image_button);
	render_animation_button = memnew(Button);
	render_animation_button->set_text(TTR("Render Animation"));
	render_animation_button->set_tooltip_text(TTR("Render every frame of the animation to a video (.avi or .ogv) or a numbered PNG sequence, which keeps a transparent background."));
	render_animation_button->connect(SceneStringName(pressed), callable_mp(this, &RenderEditor::_render_animation_pressed));
	toolbar->add_child(render_animation_button);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split);

	VBoxContainer *view = memnew(VBoxContainer);
	view->set_h_size_flags(SIZE_EXPAND_FILL);
	split->add_child(view);

	// The picture, at the output's shape.
	aspect = memnew(AspectRatioContainer);
	aspect->set_v_size_flags(SIZE_EXPAND_FILL);
	aspect->set_stretch_mode(AspectRatioContainer::STRETCH_FIT);
	view->add_child(aspect);
	preview_holder = memnew(Control);
	preview_holder->set_clip_contents(true);
	preview_holder->connect(SceneStringName(draw), callable_mp(this, &RenderEditor::_draw_checker));
	aspect->add_child(preview_holder);
	preview_container = memnew(SubViewportContainer);
	preview_container->set_stretch(true);
	preview_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_holder->add_child(preview_container);
	preview = memnew(SubViewport);
	preview->set_disable_input(true);
	preview->set_update_mode(SubViewport::UPDATE_DISABLED);
	preview->set_msaa_3d(Viewport::MSAA_4X);
	preview->set_positional_shadow_atlas_size(4096);
	preview_container->add_child(preview);
	preview_camera = memnew(Camera3D);
	// The scene's layers only: no gizmos, grid or selection outlines.
	preview_camera->set_cull_mask((1 << 20) - 1);
	preview->add_child(preview_camera);
	preview_camera->make_current();
	preview_surface = memnew(Control);
	preview_surface->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_surface->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	preview_surface->connect(SceneStringName(gui_input), callable_mp(this, &RenderEditor::_preview_input));
	preview_holder->add_child(preview_surface);

	HBoxContainer *transport = memnew(HBoxContainer);
	view->add_child(transport);
	play_button = memnew(Button);
	play_button->set_theme_type_variation(SceneStringName(FlatButton));
	play_button->set_tooltip_text(TTR("Play the animation in the preview."));
	play_button->connect(SceneStringName(pressed), callable_mp(this, &RenderEditor::_play_pressed));
	transport->add_child(play_button);
	timeline = memnew(HSlider);
	timeline->set_h_size_flags(SIZE_EXPAND_FILL);
	timeline->set_v_size_flags(SIZE_SHRINK_CENTER);
	timeline->connect(SceneStringName(value_changed), callable_mp(this, &RenderEditor::_timeline_changed));
	transport->add_child(timeline);
	time_label = memnew(Label);
	time_label->set_custom_minimum_size(Size2(110, 0) * EDSCALE);
	time_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	transport->add_child(time_label);
	hint_label = memnew(Label);
	hint_label->set_modulate(Color(1, 1, 1, 0.6));
	hint_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	view->add_child(hint_label);

	// The shot's settings.
	inspector = memnew(EditorInspector);
	inspector->set_custom_minimum_size(Size2(360, 0) * EDSCALE);
	inspector->set_v_size_flags(SIZE_EXPAND_FILL);
	inspector->set_hide_script(true);
	inspector->set_hide_metadata(true);
	inspector->set_mark_unsaved(false);
	inspector->set_use_folding(false);
	split->add_child(inspector);
	inspector->edit(shot.ptr());

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
	image_dialog->connect("file_selected", callable_mp(this, &RenderEditor::_image_path_selected));
	add_child(image_dialog);
	animation_dialog = memnew(EditorFileDialog);
	animation_dialog->set_title(TTR("Render Animation"));
	animation_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	animation_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	animation_dialog->add_filter("*.avi", TTR("AVI Video (Motion JPEG)"));
	animation_dialog->add_filter("*.ogv", TTR("Ogg Theora Video"));
	animation_dialog->add_filter("*.png", TTR("PNG Image Sequence"));
	animation_dialog->connect("file_selected", callable_mp(this, &RenderEditor::_animation_path_selected));
	add_child(animation_dialog);

	progress_dialog = memnew(ConfirmationDialog);
	progress_dialog->set_title(TTR("Rendering"));
	progress_dialog->get_ok_button()->hide();
	progress_dialog->set_cancel_button_text(TTR("Cancel"));
	progress_dialog->connect("canceled", callable_mp(this, &RenderEditor::_cancel_job));
	VBoxContainer *progress_box = memnew(VBoxContainer);
	progress_dialog->add_child(progress_box);
	progress_label = memnew(Label);
	progress_box->add_child(progress_label);
	progress_bar = memnew(ProgressBar);
	progress_box->add_child(progress_bar);
	add_child(progress_dialog);

	_update_timeline();
}

RenderEditor::~RenderEditor() {
	if (job.writer) {
		job.writer->end_frames();
	}
}

/* -------------------------------------------------------------------------- */
/*                                   Plugin                                   */
/* -------------------------------------------------------------------------- */

void RenderEditorPlugin::make_visible(bool p_visible) {
	render_editor->set_visible(p_visible);
	render_editor->set_active(p_visible);
}

Dictionary RenderEditorPlugin::get_state() const {
	return render_editor->get_state();
}

void RenderEditorPlugin::set_state(const Dictionary &p_state) {
	render_editor->set_state(p_state);
}

void RenderEditorPlugin::clear() {
	render_editor->set_state(Dictionary());
}

void RenderEditorPlugin::edited_scene_changed() {
	render_editor->scene_changed();
}

void RenderEditorPlugin::apply_changes() {
	// Called before the scene is saved: a pose from the preview must not be
	// saved with it.
	if (render_editor->posed) {
		render_editor->_restore_pose();
		render_editor->repose_pending = true;
	}
}

RenderEditorPlugin::RenderEditorPlugin() {
	render_editor = memnew(RenderEditor);
	EditorInterface::get_singleton()->get_editor_main_screen()->add_child(render_editor);
	render_editor->hide();
}
