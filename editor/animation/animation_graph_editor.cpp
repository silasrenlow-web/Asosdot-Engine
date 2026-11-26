/**************************************************************************/
/*  animation_graph_editor.cpp                                            */
/**************************************************************************/
/*  Asosdot: the Graph Editor, a view of the timeline that draws every    */
/*  animated number as a curve, as Blender's does: positions, rotations,  */
/*  scales, blend shapes, property values and Bezier tracks, edited with  */
/*  Blender's keys and menus.                                             */
/**************************************************************************/

#include "animation_graph_editor.h"

#include "core/input/input.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/animation/animation_track_editor.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/gui/button.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/image_texture.h"

// Blender's axis colours, and the orange of selected keys.
static const Color AXIS_COLORS[4] = {
	Color(0.96, 0.36, 0.38),
	Color(0.55, 0.85, 0.27),
	Color(0.34, 0.58, 0.98),
	Color(0.86, 0.76, 0.40),
};
static const Color SELECTED_KEY_COLOR = Color(1.0, 0.62, 0.2);
static const Color PLAYHEAD_COLOR = Color(0.31, 0.56, 1.0);
static const Color HANDLE_COLORS[4] = {
	Color(0.75, 0.75, 0.75), // Free.
	Color(0.45, 0.85, 0.45), // Vector (linear).
	Color(0.95, 0.5, 0.75), // Aligned (balanced).
	Color(0.7, 0.5, 0.95), // Mirrored.
};
static constexpr float KEY_RADIUS = 3.5f;
static constexpr float PICK_DISTANCE = 7.0f;
static constexpr float SAMPLE_STEP = 2.0f;

// The Godot transition each easing sets on a key, which shapes the stretch to the next key.
static real_t _easing_transition(int p_option) {
	switch (p_option) {
		case AsosdotGraphEditor::EASING_IN:
			return 2.0;
		case AsosdotGraphEditor::EASING_OUT:
			return 0.5;
		case AsosdotGraphEditor::EASING_IN_OUT:
			return -2.0;
		case AsosdotGraphEditor::EASING_OUT_IN:
			return -0.5;
		default:
			return 1.0;
	}
}

static int _easing_of_transition(real_t p_transition) {
	if (Math::is_equal_approx(p_transition, real_t(1.0))) {
		return 0;
	}
	if (Math::is_equal_approx(p_transition, real_t(2.0))) {
		return 1;
	}
	if (Math::is_equal_approx(p_transition, real_t(0.5))) {
		return 2;
	}
	if (Math::is_equal_approx(p_transition, real_t(-2.0))) {
		return 3;
	}
	if (Math::is_equal_approx(p_transition, real_t(-0.5))) {
		return 4;
	}
	return 5; // Custom.
}

// Drops the points of a polyline that lie within a tolerance of the line through those kept
// (Douglas and Peucker): a nearly flat stretch, most of an idle pose, is then a few points.
static void _simplify_polyline(PackedVector2Array &r_points, float p_tolerance) {
	const int count = r_points.size();
	if (count < 3) {
		return;
	}
	const Vector2 *points = r_points.ptr();
	LocalVector<uint8_t> keep;
	keep.resize(count);
	memset(keep.ptr(), 0, count);
	keep[0] = 1;
	keep[count - 1] = 1;
	LocalVector<Vector2i> ranges;
	ranges.push_back(Vector2i(0, count - 1));
	while (!ranges.is_empty()) {
		const Vector2i range = ranges[ranges.size() - 1];
		ranges.resize(ranges.size() - 1);
		const Vector2 from = points[range.x];
		const Vector2 along = points[range.y] - from;
		const float length_squared = along.length_squared();
		float worst = 0.0f;
		int worst_index = -1;
		for (int i = range.x + 1; i < range.y; i++) {
			const float t = length_squared > 0.0f ? CLAMP((points[i] - from).dot(along) / length_squared, 0.0f, 1.0f) : 0.0f;
			const float distance = (from + along * t).distance_to(points[i]);
			if (distance > worst) {
				worst = distance;
				worst_index = i;
			}
		}
		if (worst > p_tolerance) {
			keep[worst_index] = 1;
			ranges.push_back(Vector2i(range.x, worst_index));
			ranges.push_back(Vector2i(worst_index, range.y));
		}
	}
	PackedVector2Array simplified;
	for (int i = 0; i < count; i++) {
		if (keep[i]) {
			simplified.push_back(points[i]);
		}
	}
	r_points = simplified;
}

static double _nice_step(double p_raw) {
	if (p_raw <= 0.0) {
		return 1.0;
	}
	const double magnitude = Math::pow(10.0, Math::floor(Math::log(p_raw) / Math::log(10.0)));
	const double residual = p_raw / magnitude;
	if (residual <= 1.0) {
		return magnitude;
	}
	if (residual <= 2.0) {
		return magnitude * 2.0;
	}
	if (residual <= 5.0) {
		return magnitude * 5.0;
	}
	return magnitude * 10.0;
}

static bool _is_graphable(Animation::TrackType p_type) {
	return p_type == Animation::TYPE_POSITION_3D || p_type == Animation::TYPE_ROTATION_3D || p_type == Animation::TYPE_SCALE_3D || p_type == Animation::TYPE_BLEND_SHAPE || p_type == Animation::TYPE_VALUE || p_type == Animation::TYPE_BEZIER;
}

/// Layout.

float AsosdotGraphEditor::_row_height() const {
	return Math::round(22 * EDSCALE);
}

float AsosdotGraphEditor::_list_width() const {
	return timeline ? timeline->get_name_limit() : 200 * EDSCALE;
}

float AsosdotGraphEditor::_time_to_x(double p_time) const {
	return (p_time - timeline->get_value()) * timeline->get_zoom_scale() + _list_width();
}

double AsosdotGraphEditor::_x_to_time(float p_x) const {
	return (p_x - _list_width()) / timeline->get_zoom_scale() + timeline->get_value();
}

// With Normalize on, each curve is drawn from -1 to 1 over its own range, as in Blender.
float AsosdotGraphEditor::_value_to_y(int p_channel, double p_value) const {
	double shown = p_value;
	if (normalize_button->is_pressed() && p_channel >= 0 && p_channel < int(channel_ranges.size())) {
		const Vector2 range = channel_ranges[p_channel];
		const double half = MAX((range.y - range.x) * 0.5, 1e-6);
		shown = (p_value - (range.x + range.y) * 0.5) / half;
	}
	return view->get_size().y * 0.5 - (shown - value_center) / value_per_pixel;
}

double AsosdotGraphEditor::_y_to_value(int p_channel, float p_y) const {
	const double shown = value_center + (view->get_size().y * 0.5 - p_y) * value_per_pixel;
	if (normalize_button->is_pressed() && p_channel >= 0 && p_channel < int(channel_ranges.size())) {
		const Vector2 range = channel_ranges[p_channel];
		const double half = MAX((range.y - range.x) * 0.5, 1e-6);
		return shown * half + (range.x + range.y) * 0.5;
	}
	return shown;
}

double AsosdotGraphEditor::_value_per_pixel_for(int p_channel) const {
	if (normalize_button->is_pressed() && p_channel >= 0 && p_channel < int(channel_ranges.size())) {
		const Vector2 range = channel_ranges[p_channel];
		return value_per_pixel * MAX((range.y - range.x) * 0.5, 1e-6);
	}
	return value_per_pixel;
}

String AsosdotGraphEditor::_channel_id(int p_channel) const {
	return channels[p_channel].id;
}

bool AsosdotGraphEditor::_is_channel_shown(int p_channel) const {
	return !hidden_channels.has(_channel_id(p_channel));
}

bool AsosdotGraphEditor::_is_channel_locked(int p_channel) const {
	return read_only || locked_channels.has(_channel_id(p_channel)) || animation->track_is_compressed(channels[p_channel].track);
}

int AsosdotGraphEditor::_channel_of(int p_track, int p_component) const {
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (channels[i].track == p_track && channels[i].component == p_component) {
			return i;
		}
	}
	return -1;
}

/// Channels.

int AsosdotGraphEditor::_component_count(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
			return 1;
		case Variant::VECTOR2:
		case Variant::VECTOR2I:
			return 2;
		case Variant::VECTOR3:
		case Variant::VECTOR3I:
			return 3;
		case Variant::VECTOR4:
		case Variant::VECTOR4I:
		case Variant::QUATERNION:
		case Variant::COLOR:
			return 4;
		default:
			return 0;
	}
}

double AsosdotGraphEditor::_get_component(const Variant &p_value, int p_component) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
			return bool(p_value) ? 1.0 : 0.0;
		case Variant::INT:
		case Variant::FLOAT:
			return double(p_value);
		case Variant::VECTOR2:
			return Vector2(p_value)[p_component];
		case Variant::VECTOR2I:
			return Vector2i(p_value)[p_component];
		case Variant::VECTOR3:
			return Vector3(p_value)[p_component];
		case Variant::VECTOR3I:
			return Vector3i(p_value)[p_component];
		case Variant::VECTOR4:
			return Vector4(p_value)[p_component];
		case Variant::VECTOR4I:
			return Vector4i(p_value)[p_component];
		case Variant::QUATERNION:
			return Quaternion(p_value)[p_component];
		case Variant::COLOR:
			return Color(p_value)[p_component];
		default:
			return 0.0;
	}
}

Variant AsosdotGraphEditor::_set_component(const Variant &p_value, int p_component, double p_component_value) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
			return p_component_value >= 0.5;
		case Variant::INT:
			return int64_t(Math::round(p_component_value));
		case Variant::FLOAT:
			return p_component_value;
		case Variant::VECTOR2: {
			Vector2 v = p_value;
			v[p_component] = p_component_value;
			return v;
		}
		case Variant::VECTOR2I: {
			Vector2i v = p_value;
			v[p_component] = int(Math::round(p_component_value));
			return v;
		}
		case Variant::VECTOR3: {
			Vector3 v = p_value;
			v[p_component] = p_component_value;
			return v;
		}
		case Variant::VECTOR3I: {
			Vector3i v = p_value;
			v[p_component] = int(Math::round(p_component_value));
			return v;
		}
		case Variant::VECTOR4: {
			Vector4 v = p_value;
			v[p_component] = p_component_value;
			return v;
		}
		case Variant::VECTOR4I: {
			Vector4i v = p_value;
			v[p_component] = int(Math::round(p_component_value));
			return v;
		}
		case Variant::QUATERNION: {
			Quaternion v = p_value;
			v[p_component] = p_component_value;
			return v;
		}
		case Variant::COLOR: {
			Color v = p_value;
			v[p_component] = p_component_value;
			return v;
		}
		default:
			return p_value;
	}
}

EulerOrder AsosdotGraphEditor::_get_euler_order(int p_track) const {
	const NodePath path = animation->track_get_path(p_track);
	if (root && path.get_subname_count() == 0) {
		const Node3D *node = Object::cast_to<Node3D>(root->get_node_or_null(NodePath(path.get_concatenated_names())));
		if (node) {
			return node->get_rotation_order();
		}
	}
	return EulerOrder::YXZ;
}

void AsosdotGraphEditor::_update_channels(bool p_force) {
	uint32_t hash = hash_murmur3_one_64(animation.is_valid() ? uint64_t(animation->get_instance_id()) : 0);
	hash = hash_murmur3_one_64(root ? uint64_t(root->get_instance_id()) : 0, hash);
	if (animation.is_valid()) {
		hash = hash_murmur3_one_32(animation->get_track_count(), hash);
		for (int i = 0; i < animation->get_track_count(); i++) {
			hash = hash_murmur3_one_32(String(animation->track_get_path(i)).hash(), hash);
			hash = hash_murmur3_one_32(animation->track_get_type(i), hash);
			if (animation->track_get_type(i) == Animation::TYPE_VALUE && animation->track_get_key_count(i) > 0) {
				hash = hash_murmur3_one_32(animation->track_get_key_value(i, 0).get_type(), hash);
			}
		}
	}
	const bool only_selected = only_selected_button->is_pressed();
	HashSet<Node *> selected_nodes;
	if (only_selected) {
		for (Node *node : EditorNode::get_singleton()->get_editor_selection()->get_full_selected_node_list()) {
			selected_nodes.insert(node);
			hash = hash_murmur3_one_64(uint64_t(node->get_instance_id()), hash);
		}
	}
	hash = hash_murmur3_one_32(only_selected, hash);
	if (!p_force && hash == channels_hash) {
		return;
	}
	channels_hash = hash;
	channels.clear();
	groups.clear();
	selected_channels.clear();
	track_info.clear();
	track_samples.clear();
	keys_dirty = true;
	if (animation.is_null()) {
		return;
	}

	static const char *axes[4] = { "X", "Y", "Z", "W" };
	static const char *color_components[4] = { "R", "G", "B", "A" };
	HashMap<String, int> group_index;
	int single = 0;
	for (int track = 0; track < animation->get_track_count(); track++) {
		const Animation::TrackType type = animation->track_get_type(track);
		if (!_is_graphable(type)) {
			continue;
		}
		const NodePath path = animation->track_get_path(track);
		const String node_path = path.get_concatenated_names();
		const String subnames = path.get_concatenated_subnames();
		Node *node = root ? root->get_node_or_null(NodePath(node_path)) : nullptr;
		if (only_selected && (!node || !selected_nodes.has(node))) {
			continue;
		}

		// Transform tracks of a bone get a group of their own, as an object's channels would.
		const bool transform = type == Animation::TYPE_POSITION_3D || type == Animation::TYPE_ROTATION_3D || type == Animation::TYPE_SCALE_3D;
		String key = node_path;
		String label = node ? String(node->get_name()) : node_path.get_file();
		if (transform && !subnames.is_empty()) {
			key += ":" + subnames;
			label += String::utf8(" › ") + subnames;
		}
		if (!group_index.has(key)) {
			Group group;
			group.key = key;
			group.label = label;
			group.icon = node ? EditorNode::get_singleton()->get_object_icon(node, "Node") : get_editor_theme_icon(SNAME("Node"));
			group_index[key] = groups.size();
			groups.push_back(group);
		}
		const int group = group_index[key];

		int count = 1;
		String base;
		bool axis_colors = true;
		bool color_value = false;
		switch (type) {
			case Animation::TYPE_POSITION_3D:
				count = 3;
				base = TTR("Position");
				break;
			case Animation::TYPE_ROTATION_3D:
				count = 3;
				base = TTR("Rotation");
				break;
			case Animation::TYPE_SCALE_3D:
				count = 3;
				base = TTR("Scale");
				break;
			case Animation::TYPE_BLEND_SHAPE:
				base = subnames;
				axis_colors = false;
				break;
			case Animation::TYPE_BEZIER:
				base = subnames.replace(":", " ");
				axis_colors = false;
				break;
			case Animation::TYPE_VALUE: {
				if (animation->track_get_key_count(track) == 0) {
					continue;
				}
				const Variant first = animation->track_get_key_value(track, 0);
				count = _component_count(first);
				if (count == 0) {
					continue;
				}
				base = subnames;
				axis_colors = count > 1;
				color_value = first.get_type() == Variant::COLOR;
			} break;
			default:
				break;
		}

		for (int component = 0; component < count; component++) {
			Channel channel;
			channel.track = track;
			channel.component = component;
			channel.group = group;
			if (count == 1) {
				channel.label = base;
			} else if (type == Animation::TYPE_VALUE) {
				channel.label = base + " " + (color_value ? color_components[component] : axes[component]);
			} else {
				channel.label = String(axes[component]) + " " + base;
			}
			if (axis_colors) {
				channel.color = color_value && component == 3 ? Color(0.8, 0.8, 0.8) : AXIS_COLORS[component];
			} else {
				// Single curves step round the colour wheel, so neighbours never look alike.
				channel.color = Color::from_hsv(Math::fmod(0.12f + single * 0.618034f, 1.0f), 0.6, 0.95);
				single++;
			}
			channel.rotation = type == Animation::TYPE_ROTATION_3D;
			if (channel.rotation) {
				channel.euler_order = _get_euler_order(track);
			}
			channel.id = String(path) + "/" + itos(type) + "/" + itos(component);
			groups[group].channels.push_back(channels.size());
			channels.push_back(channel);
		}
		TrackInfo info;
		info.components = count;
		info.rotation = type == Animation::TYPE_ROTATION_3D;
		info.euler_order = info.rotation ? _get_euler_order(track) : EulerOrder::YXZ;
		track_info[track] = info;
	}
	_prune_selection();
}

/// Values.

bool AsosdotGraphEditor::_evaluate(int p_channel, double p_time, double &r_value) const {
	const Channel &channel = channels[p_channel];
	if (animation->track_get_key_count(channel.track) == 0) {
		return false;
	}
	switch (animation->track_get_type(channel.track)) {
		case Animation::TYPE_POSITION_3D: {
			Vector3 v;
			if (animation->try_position_track_interpolate(channel.track, p_time, &v) != OK) {
				return false;
			}
			r_value = v[channel.component];
		} break;
		case Animation::TYPE_ROTATION_3D: {
			Quaternion q;
			if (animation->try_rotation_track_interpolate(channel.track, p_time, &q) != OK) {
				return false;
			}
			r_value = Math::rad_to_deg(Basis(q.normalized()).get_euler(channel.euler_order)[channel.component]);
		} break;
		case Animation::TYPE_SCALE_3D: {
			Vector3 v;
			if (animation->try_scale_track_interpolate(channel.track, p_time, &v) != OK) {
				return false;
			}
			r_value = v[channel.component];
		} break;
		case Animation::TYPE_BLEND_SHAPE: {
			float v = 0.0f;
			if (animation->try_blend_shape_track_interpolate(channel.track, p_time, &v) != OK) {
				return false;
			}
			r_value = v;
		} break;
		case Animation::TYPE_VALUE: {
			r_value = _get_component(animation->value_track_interpolate(channel.track, p_time), channel.component);
		} break;
		case Animation::TYPE_BEZIER: {
			r_value = animation->bezier_track_interpolate(channel.track, p_time);
		} break;
		default:
			return false;
	}
	return true;
}

double AsosdotGraphEditor::_key_raw_value(int p_channel, int p_key) const {
	const Channel &channel = channels[p_channel];
	switch (animation->track_get_type(channel.track)) {
		case Animation::TYPE_ROTATION_3D: {
			const Quaternion q = animation->track_get_key_value(channel.track, p_key);
			return Math::rad_to_deg(Basis(q.normalized()).get_euler(channel.euler_order)[channel.component]);
		}
		case Animation::TYPE_BEZIER:
			return animation->bezier_track_get_key_value(channel.track, p_key);
		default:
			return _get_component(animation->track_get_key_value(channel.track, p_key), channel.component);
	}
}

// A key's value for the track, with one component changed by a shown amount; a rotation's Euler
// angle turns back into the quaternion the track keeps.
Variant AsosdotGraphEditor::_key_with_value(int p_channel, const Variant &p_key_value, double p_delta) const {
	const Channel &channel = channels[p_channel];
	switch (animation->track_get_type(channel.track)) {
		case Animation::TYPE_ROTATION_3D: {
			const Quaternion q = p_key_value;
			Vector3 euler = Basis(q.normalized()).get_euler(channel.euler_order);
			euler[channel.component] += Math::deg_to_rad(p_delta);
			return Basis::from_euler(euler, channel.euler_order).get_rotation_quaternion();
		}
		case Animation::TYPE_BEZIER:
			return double(p_key_value) + p_delta;
		default:
			return _set_component(p_key_value, channel.component, _get_component(p_key_value, channel.component) + p_delta);
	}
}

void AsosdotGraphEditor::_evaluate_track(int p_track, double p_time, double r_values[4]) const {
	const TrackInfo *info = track_info.getptr(p_track);
	switch (animation->track_get_type(p_track)) {
		case Animation::TYPE_POSITION_3D: {
			const Vector3 v = animation->position_track_interpolate(p_track, p_time);
			r_values[0] = v.x;
			r_values[1] = v.y;
			r_values[2] = v.z;
		} break;
		case Animation::TYPE_ROTATION_3D: {
			const Quaternion q = animation->rotation_track_interpolate(p_track, p_time);
			const Vector3 euler = Basis(q.normalized()).get_euler(info ? info->euler_order : EulerOrder::YXZ);
			r_values[0] = Math::rad_to_deg(euler.x);
			r_values[1] = Math::rad_to_deg(euler.y);
			r_values[2] = Math::rad_to_deg(euler.z);
		} break;
		case Animation::TYPE_SCALE_3D: {
			const Vector3 v = animation->scale_track_interpolate(p_track, p_time);
			r_values[0] = v.x;
			r_values[1] = v.y;
			r_values[2] = v.z;
		} break;
		case Animation::TYPE_BLEND_SHAPE:
			r_values[0] = animation->blend_shape_track_interpolate(p_track, p_time);
			break;
		case Animation::TYPE_VALUE: {
			const Variant v = animation->value_track_interpolate(p_track, p_time);
			for (int c = 0; c < (info ? info->components : 1); c++) {
				r_values[c] = _get_component(v, c);
			}
		} break;
		case Animation::TYPE_BEZIER:
			r_values[0] = animation->bezier_track_interpolate(p_track, p_time);
			break;
		default:
			break;
	}
}

// Samples a track where its curves bend, in view: a straight stretch is its two keys, a held one a
// step, and a bend a sample every few pixels. All of the track's numbers come from one evaluation.
void AsosdotGraphEditor::_sample_track(int p_track, TrackSamples &r_samples) {
	r_samples.times.clear();
	for (int c = 0; c < 4; c++) {
		r_samples.values[c].clear();
	}
	r_samples.valid = true;
	const int count = animation->track_get_key_count(p_track);
	const TrackInfo *info = track_info.getptr(p_track);
	if (count == 0 || !info) {
		return;
	}
	const int components = info->components;
	const double length = animation->get_length();
	const double zoom = timeline->get_zoom_scale();
	const double view_from = _x_to_time(_list_width() - SAMPLE_STEP);
	const double view_to = _x_to_time(view->get_size().x + SAMPLE_STEP);
	const Animation::TrackType type = animation->track_get_type(p_track);
	const Animation::InterpolationType interpolation = type == Animation::TYPE_BEZIER ? Animation::INTERPOLATION_CUBIC : animation->track_get_interpolation_type(p_track);
	double values[4] = {};
	auto add = [&](double p_time) {
		_evaluate_track(p_track, CLAMP(p_time, 0.0, length), values);
		r_samples.times.push_back(p_time);
		for (int c = 0; c < components; c++) {
			r_samples.values[c].push_back(values[c]);
		}
	};

	const double first = animation->track_get_key_time(p_track, 0);
	const double last = animation->track_get_key_time(p_track, count - 1);
	if (view_from < first) {
		add(view_from); // Held before the first key.
	}
	for (int k = MAX(0, animation->track_find_key(p_track, view_from)); k < count; k++) {
		const double start = animation->track_get_key_time(p_track, k);
		if (start > view_to) {
			break;
		}
		add(start);
		if (k + 1 >= count) {
			break;
		}
		const double end = animation->track_get_key_time(p_track, k + 1);
		if (interpolation == Animation::INTERPOLATION_NEAREST) {
			add(end - MIN(1e-4, (end - start) * 0.01)); // Held until the next key.
			continue;
		}
		const bool straight = interpolation == Animation::INTERPOLATION_LINEAR && type != Animation::TYPE_ROTATION_3D && Math::is_equal_approx(animation->track_get_key_transition(p_track, k), real_t(1.0));
		if (straight) {
			continue;
		}
		// Only the part in view needs its shape.
		const double from = MAX(start, view_from);
		const double to = MIN(end, view_to);
		if (to <= from) {
			continue;
		}
		const int steps = CLAMP(int((to - from) * zoom / 3.0), 1, 512);
		for (int j = from > start ? 0 : 1; j < steps; j++) {
			add(from + (to - from) * j / steps);
		}
		if (to < end) {
			add(to);
		}
	}
	if (view_to > last) {
		add(view_to); // Held after the last key.
	}

	if (info->rotation) {
		// Rotations go on without jumps of a whole turn, from where the key before stands as drawn.
		for (int c = 0; c < 3; c++) {
			const int channel = _channel_of(p_track, c);
			LocalVector<double> &component = r_samples.values[c];
			if (channel < 0 || component.is_empty() || channel >= int(curve_keys.size()) || curve_keys[channel].is_empty()) {
				continue;
			}
			double previous = curve_keys[channel][0].value;
			for (const CurveKey &key : curve_keys[channel]) {
				if (key.time <= r_samples.times[0]) {
					previous = key.value;
				}
			}
			for (double &value : component) {
				value -= Math::round((value - previous) / 360.0) * 360.0;
				previous = value;
			}
		}
	}
}

const AsosdotGraphEditor::TrackSamples &AsosdotGraphEditor::_get_samples(int p_track) {
	TrackSamples &samples = track_samples[p_track];
	if (!samples.valid) {
		_sample_track(p_track, samples);
	}
	return samples;
}

void AsosdotGraphEditor::_on_animation_changed() {
	if (writing_track >= 0) {
		return; // _write_track marks what it changes itself.
	}
	keys_dirty = true;
	track_samples.clear();
	update_view();
}

void AsosdotGraphEditor::_cache_curves() {
	if (!keys_dirty && curve_keys.size() == channels.size()) {
		return;
	}
	keys_dirty = false;
	// While keys move, Normalize keeps the ranges they began with, so they don't swim under the mouse.
	const bool keep_ranges = mode != MODE_NONE && channel_ranges.size() == channels.size();
	curve_keys.resize(channels.size());
	channel_ranges.resize(channels.size());
	for (uint32_t i = 0; i < channels.size(); i++) {
		LocalVector<CurveKey> &keys = curve_keys[i];
		keys.clear();
		const Channel &channel = channels[i];
		const bool bezier = animation->track_get_type(channel.track) == Animation::TYPE_BEZIER;
		Vector2 range(1e20, -1e20);
		const int count = animation->track_get_key_count(channel.track);
		for (int k = 0; k < count; k++) {
			CurveKey key;
			key.index = k;
			key.time = animation->track_get_key_time(channel.track, k);
			key.value = _key_raw_value(i, k);
			if (channel.rotation && k > 0) {
				// Rotations are drawn without jumps of a full turn between keys.
				const double previous = keys[k - 1].value;
				key.value -= Math::round((key.value - previous) / 360.0) * 360.0;
			}
			if (bezier) {
				key.in_handle = animation->bezier_track_get_key_in_handle(channel.track, k);
				key.out_handle = animation->bezier_track_get_key_out_handle(channel.track, k);
				range.x = MIN(range.x, MIN(key.value + key.in_handle.y, key.value + key.out_handle.y));
				range.y = MAX(range.y, MAX(key.value + key.in_handle.y, key.value + key.out_handle.y));
			}
			range.x = MIN(range.x, key.value);
			range.y = MAX(range.y, key.value);
			keys.push_back(key);
		}
		if (count == 0) {
			range = Vector2(-1, 1);
		}
		if (!keep_ranges) {
			channel_ranges[i] = range;
		}
	}
}

int AsosdotGraphEditor::_find_key(const Selected &p_selected) const {
	if (p_selected.track < 0 || p_selected.track >= animation->get_track_count()) {
		return -1;
	}
	return animation->track_find_key(p_selected.track, p_selected.time, Animation::FIND_MODE_APPROX);
}

/// Editing.

LocalVector<AsosdotGraphEditor::TrackKey> AsosdotGraphEditor::_read_track(int p_track) const {
	LocalVector<TrackKey> keys;
	const bool bezier = animation->track_get_type(p_track) == Animation::TYPE_BEZIER;
	for (int k = 0; k < animation->track_get_key_count(p_track); k++) {
		TrackKey key;
		key.time = animation->track_get_key_time(p_track, k);
		key.transition = animation->track_get_key_transition(p_track, k);
		if (bezier) {
			key.value = animation->bezier_track_get_key_value(p_track, k);
			key.in_handle = animation->bezier_track_get_key_in_handle(p_track, k);
			key.out_handle = animation->bezier_track_get_key_out_handle(p_track, k);
			key.handle_mode = animation->bezier_track_get_key_handle_mode(p_track, k);
		} else {
			key.value = animation->track_get_key_value(p_track, k);
		}
		keys.push_back(key);
	}
	return keys;
}

// Puts a track's keys as given, which must be in time order; no undo of its own.
void AsosdotGraphEditor::_write_track(int p_track, const LocalVector<TrackKey> &p_keys) {
	ERR_FAIL_INDEX(p_track, animation->get_track_count());
	// Only this track's curves need sampling again; the change signal each key sends is left alone.
	writing_track = p_track;
	keys_dirty = true;
	track_samples.erase(p_track);
	const bool bezier = animation->track_get_type(p_track) == Animation::TYPE_BEZIER;
	while (animation->track_get_key_count(p_track) > 0) {
		animation->track_remove_key(p_track, animation->track_get_key_count(p_track) - 1);
	}
	for (const TrackKey &key : p_keys) {
		int index = -1;
		if (bezier) {
			index = animation->bezier_track_insert_key(p_track, key.time, real_t(double(key.value)), key.in_handle, key.out_handle);
			if (index >= 0) {
				animation->bezier_track_set_key_handle_mode(p_track, index, Animation::HandleMode(key.handle_mode));
			}
		} else {
			index = animation->track_insert_key(p_track, key.time, key.value, key.transition);
		}
		if (bezier && index >= 0) {
			animation->track_set_key_transition(p_track, index, key.transition);
		}
	}
	writing_track = -1;
	if (view) {
		view->queue_redraw();
	}
}

Array AsosdotGraphEditor::_track_keys_to_array(const LocalVector<TrackKey> &p_keys) const {
	Array keys;
	for (const TrackKey &key : p_keys) {
		Array entry;
		entry.push_back(key.time);
		entry.push_back(key.value);
		entry.push_back(key.transition);
		entry.push_back(key.in_handle);
		entry.push_back(key.out_handle);
		entry.push_back(key.handle_mode);
		keys.push_back(entry);
	}
	return keys;
}

void AsosdotGraphEditor::_set_track_keys(const Ref<Animation> &p_animation, int p_track, const Array &p_keys) {
	ERR_FAIL_COND(p_animation.is_null());
	ERR_FAIL_INDEX(p_track, p_animation->get_track_count());
	LocalVector<TrackKey> keys;
	for (int i = 0; i < p_keys.size(); i++) {
		const Array entry = p_keys[i];
		ERR_CONTINUE(entry.size() < 6);
		TrackKey key;
		key.time = entry[0];
		key.value = entry[1];
		key.transition = entry[2];
		key.in_handle = entry[3];
		key.out_handle = entry[4];
		key.handle_mode = entry[5];
		keys.push_back(key);
	}
	const Ref<Animation> current = animation;
	animation = p_animation;
	_write_track(p_track, keys);
	animation = current;
	if (view) {
		view->queue_redraw();
	}
}

static void _sort_keys(LocalVector<AsosdotGraphEditor::TrackKey> &r_keys);

void AsosdotGraphEditor::_add_track_ops(EditorUndoRedoManager *p_undo_redo, const HashMap<int, LocalVector<TrackKey>> &p_before, const HashMap<int, LocalVector<TrackKey>> &p_after) {
	for (const KeyValue<int, LocalVector<TrackKey>> &E : p_after) {
		p_undo_redo->add_do_method(this, "_set_track_keys", animation, E.key, _track_keys_to_array(E.value));
	}
	for (const KeyValue<int, LocalVector<TrackKey>> &E : p_after) {
		const LocalVector<TrackKey> *before = p_before.getptr(E.key);
		if (before) {
			p_undo_redo->add_undo_method(this, "_set_track_keys", animation, E.key, _track_keys_to_array(*before));
		}
	}
}

void AsosdotGraphEditor::_commit_tracks(const String &p_action, const HashMap<int, LocalVector<TrackKey>> &p_before, const HashMap<int, LocalVector<TrackKey>> &p_after) {
	if (p_after.is_empty()) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, animation.ptr());
	_add_track_ops(undo_redo, p_before, p_after);
	undo_redo->commit_action();
}

static void _sort_keys(LocalVector<AsosdotGraphEditor::TrackKey> &r_keys) {
	struct ByTime {
		_FORCE_INLINE_ bool operator()(const AsosdotGraphEditor::TrackKey &p_a, const AsosdotGraphEditor::TrackKey &p_b) const {
			return p_a.time < p_b.time;
		}
	};
	r_keys.sort_custom<ByTime>();
}

// Blender's auto clamped handles: level at a peak or a dip, else along the line between its
// neighbours, a third of the way to each.
void AsosdotGraphEditor::_auto_handles(LocalVector<TrackKey> &r_keys, int p_index) {
	TrackKey &key = r_keys[p_index];
	const bool has_previous = p_index > 0;
	const bool has_next = p_index + 1 < int(r_keys.size());
	if (!has_previous && !has_next) {
		key.in_handle = Vector2(-0.1, 0);
		key.out_handle = Vector2(0.1, 0);
		return;
	}
	const double value = double(key.value);
	const double previous_time = has_previous ? key.time - r_keys[p_index - 1].time : r_keys[p_index + 1].time - key.time;
	const double next_time = has_next ? r_keys[p_index + 1].time - key.time : previous_time;
	double slope = 0.0;
	if (has_previous && has_next) {
		const double previous = double(r_keys[p_index - 1].value);
		const double next = double(r_keys[p_index + 1].value);
		const bool extreme = (value >= previous && value >= next) || (value <= previous && value <= next);
		if (!extreme) {
			slope = (next - previous) / (r_keys[p_index + 1].time - r_keys[p_index - 1].time);
		}
	}
	key.in_handle = Vector2(-previous_time / 3.0, -slope * previous_time / 3.0);
	key.out_handle = Vector2(next_time / 3.0, slope * next_time / 3.0);
	key.handle_mode = Animation::HANDLE_MODE_BALANCED;
}

// Blender's vector handles: pointing at the neighbouring keys, so the curve runs straight.
void AsosdotGraphEditor::_vector_handles(LocalVector<TrackKey> &r_keys, int p_index) {
	TrackKey &key = r_keys[p_index];
	const double value = double(key.value);
	if (p_index > 0) {
		const TrackKey &previous = r_keys[p_index - 1];
		key.in_handle = Vector2((previous.time - key.time) / 3.0, (double(previous.value) - value) / 3.0);
	} else {
		key.in_handle = Vector2(-0.1, 0);
	}
	if (p_index + 1 < int(r_keys.size())) {
		const TrackKey &next = r_keys[p_index + 1];
		key.out_handle = Vector2((next.time - key.time) / 3.0, (double(next.value) - value) / 3.0);
	} else {
		key.out_handle = Vector2(0.1, 0);
	}
	key.handle_mode = Animation::HANDLE_MODE_LINEAR;
}

void AsosdotGraphEditor::_begin_edit(Mode p_mode) {
	if (read_only || animation.is_null() || mode != MODE_NONE) {
		return;
	}
	_update_channels();
	_cache_curves();
	edit_items.clear();
	edit_original.clear();
	edit_changed = false;
	Vector2 min_point(1e20, 1e20);
	Vector2 max_point(-1e20, -1e20);
	for (const Selected &selected : selection) {
		const int channel = _channel_of(selected.track, selected.component);
		const int key = channel >= 0 ? _find_key(selected) : -1;
		if (key < 0 || _is_channel_locked(channel) || !_is_channel_shown(channel) || key >= int(curve_keys[channel].size())) {
			continue;
		}
		EditItem item;
		item.key = selected;
		item.channel = channel;
		item.key_index = key;
		item.shown = curve_keys[channel][key].value;
		edit_items.push_back(item);
		if (!edit_original.has(selected.track)) {
			edit_original[selected.track] = _read_track(selected.track);
		}
		const Vector2 point(_time_to_x(selected.time), _value_to_y(channel, item.shown));
		min_point = min_point.min(point);
		max_point = max_point.max(point);
	}
	if (edit_items.is_empty()) {
		edit_duplicating = false;
		return;
	}
	mode = p_mode;
	axis = AXIS_BOTH;
	drag_from = view->get_local_mouse_position();
	scale_pivot = (min_point + max_point) * 0.5;
	if (p_mode == MODE_SCALE && drag_from.distance_to(scale_pivot) < 8 * EDSCALE) {
		drag_from = scale_pivot + Vector2(40, 0) * EDSCALE;
	}
	_update_status();
	view->queue_redraw();
}

void AsosdotGraphEditor::_update_edit(const Vector2 &p_mouse) {
	if (mode != MODE_GRAB && mode != MODE_SCALE) {
		return;
	}
	Vector2 delta = p_mouse - drag_from;
	if (axis == AXIS_TIME) {
		delta.y = 0;
	} else if (axis == AXIS_VALUE) {
		delta.x = 0;
	}
	float scale = 1.0f;
	if (mode == MODE_SCALE) {
		const float from = MAX(drag_from.distance_to(scale_pivot), 1.0f);
		scale = p_mouse.distance_to(scale_pivot) / from;
	}

	// The time step every key moves by while grabbing, snapped as the timeline snaps.
	double time_offset = 0.0;
	if (mode == MODE_GRAB && edit_exact) {
		time_offset = edit_exact_offset.x;
	} else if (mode == MODE_GRAB) {
		const double base = edit_items[0].key.time;
		double moved = base + delta.x / timeline->get_zoom_scale();
		if (track_editor) {
			moved = track_editor->snap_time(moved);
		}
		time_offset = moved - base;
	}

	struct Change {
		double time = 0.0;
		Vector4 delta;
		bool set = false;
	};
	HashMap<int, LocalVector<Change>> changes;
	for (const KeyValue<int, LocalVector<TrackKey>> &E : edit_original) {
		LocalVector<Change> &track_changes = changes[E.key];
		track_changes.resize(E.value.size());
	}
	edit_new_selection.clear();
	for (const EditItem &item : edit_items) {
		double new_time = item.key.time;
		double new_shown = item.shown;
		if (mode == MODE_GRAB) {
			new_time += time_offset;
			new_shown += edit_exact ? edit_exact_offset.y : -delta.y * _value_per_pixel_for(item.channel);
		} else {
			const Vector2 from(_time_to_x(item.key.time), _value_to_y(item.channel, item.shown));
			const Vector2 factor(axis == AXIS_VALUE ? 1.0f : scale, axis == AXIS_TIME ? 1.0f : scale);
			const Vector2 to = scale_pivot + (from - scale_pivot) * factor;
			new_time = _x_to_time(to.x);
			if (track_editor && axis != AXIS_VALUE) {
				new_time = track_editor->snap_time(new_time);
			}
			new_shown = _y_to_value(item.channel, to.y);
		}
		new_time = MAX(0.0, new_time);
		Change &change = changes[item.key.track][item.key_index];
		change.time = new_time;
		change.delta[item.key.component] = new_shown - item.shown;
		change.set = true;
		Selected moved = item.key;
		moved.time = new_time;
		edit_new_selection.push_back(moved);
		if (!Math::is_equal_approx(new_time, item.key.time) || !Math::is_zero_approx(new_shown - item.shown)) {
			edit_changed = true;
		}
	}

	for (const KeyValue<int, LocalVector<TrackKey>> &E : edit_original) {
		const LocalVector<Change> &track_changes = changes[E.key];
		LocalVector<TrackKey> moved;
		LocalVector<TrackKey> kept;
		for (uint32_t k = 0; k < E.value.size(); k++) {
			const Change &change = track_changes[k];
			if (!change.set) {
				kept.push_back(E.value[k]);
				continue;
			}
			if (edit_duplicating) {
				kept.push_back(E.value[k]);
			}
			TrackKey key = E.value[k];
			key.time = change.time;
			for (int c = 0; c < 4; c++) {
				if (Math::is_zero_approx(change.delta[c])) {
					continue;
				}
				const int channel = _channel_of(E.key, c);
				if (channel >= 0) {
					key.value = _key_with_value(channel, key.value, change.delta[c]);
				}
			}
			moved.push_back(key);
		}
		// A moved key takes the place of one already at its time.
		LocalVector<TrackKey> result;
		for (const TrackKey &key : kept) {
			bool covered = false;
			for (const TrackKey &other : moved) {
				if (Math::abs(other.time - key.time) < 1e-5) {
					covered = true;
					break;
				}
			}
			if (!covered) {
				result.push_back(key);
			}
		}
		for (const TrackKey &key : moved) {
			result.push_back(key);
		}
		_sort_keys(result);
		_write_track(E.key, result);
	}
	view->queue_redraw();
}

void AsosdotGraphEditor::_finish_edit(bool p_confirm) {
	if (mode != MODE_GRAB && mode != MODE_SCALE && mode != MODE_HANDLE) {
		return;
	}
	const Mode finished = mode;
	mode = MODE_NONE;
	dragging_with_button = false;
	if (p_confirm && (edit_changed || edit_duplicating)) {
		HashMap<int, LocalVector<TrackKey>> after;
		for (const KeyValue<int, LocalVector<TrackKey>> &E : edit_original) {
			after[E.key] = _read_track(E.key);
		}
		for (const KeyValue<int, LocalVector<TrackKey>> &E : edit_original) {
			_write_track(E.key, E.value);
		}
		String action = finished == MODE_HANDLE ? TTR("Move Keyframe Handle") : (finished == MODE_SCALE ? TTR("Scale Keyframes") : TTR("Move Keyframes"));
		if (edit_duplicating) {
			action = TTR("Duplicate Keyframes");
		}
		_commit_tracks(action, edit_original, after);
		if (finished != MODE_HANDLE) {
			selection.clear();
			for (const Selected &selected : edit_new_selection) {
				selection.insert(selected);
			}
			if (has_active) {
				for (uint32_t i = 0; i < edit_items.size() && i < edit_new_selection.size(); i++) {
					if (edit_items[i].key == active) {
						active = edit_new_selection[i];
						break;
					}
				}
			}
		}
	} else {
		for (const KeyValue<int, LocalVector<TrackKey>> &E : edit_original) {
			_write_track(E.key, E.value);
		}
	}
	edit_items.clear();
	edit_original.clear();
	edit_new_selection.clear();
	edit_duplicating = false;
	_update_status();
	_update_sidebar();
	view->queue_redraw();
}

void AsosdotGraphEditor::_drag_handle(const Vector2 &p_mouse) {
	const int channel = _channel_of(handle_key.track, 0);
	const int key = _find_key(handle_key);
	if (channel < 0 || key < 0) {
		return;
	}
	const double key_time = animation->track_get_key_time(handle_key.track, key);
	const double key_value = animation->bezier_track_get_key_value(handle_key.track, key);
	const Vector2 key_point(_time_to_x(key_time), _value_to_y(channel, key_value));
	const double value_per_pixel_here = _value_per_pixel_for(channel);
	Vector2 handle((p_mouse.x - key_point.x) / timeline->get_zoom_scale(), -(p_mouse.y - key_point.y) * value_per_pixel_here);
	// An in handle points back in time and an out handle forward.
	handle.x = handle_side < 0 ? MIN(handle.x, 0.0f) : MAX(handle.x, 0.0f);
	const real_t ratio = timeline->get_zoom_scale() * value_per_pixel_here;
	writing_track = handle_key.track;
	if (handle_side < 0) {
		animation->bezier_track_set_key_in_handle(handle_key.track, key, handle, ratio);
	} else {
		animation->bezier_track_set_key_out_handle(handle_key.track, key, handle, ratio);
	}
	writing_track = -1;
	keys_dirty = true;
	track_samples.erase(handle_key.track);
	edit_changed = true;
	view->queue_redraw();
	_update_sidebar();
}

// Every key of a track goes with each of its curves, so removing a key on one removes it on all.
void AsosdotGraphEditor::_delete_selection() {
	if (read_only || selection.is_empty()) {
		return;
	}
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	for (const Selected &selected : selection) {
		const int channel = _channel_of(selected.track, selected.component);
		if (channel < 0 || _is_channel_locked(channel)) {
			continue;
		}
		if (!before.has(selected.track)) {
			before[selected.track] = _read_track(selected.track);
			after[selected.track] = before[selected.track];
		}
		LocalVector<TrackKey> &keys = after[selected.track];
		for (uint32_t k = 0; k < keys.size(); k++) {
			if (Math::abs(keys[k].time - selected.time) < 1e-5) {
				keys.remove_at(k);
				break;
			}
		}
	}
	_commit_tracks(TTR("Delete Keyframes"), before, after);
	selection.clear();
	has_active = false;
	_update_status();
	_update_sidebar();
}

void AsosdotGraphEditor::_duplicate_selection() {
	if (read_only || selection.is_empty()) {
		return;
	}
	edit_duplicating = true;
	_begin_edit(MODE_GRAB);
}

void AsosdotGraphEditor::_copy_selection() {
	clipboard.clear();
	HashSet<Pair<int, int64_t>> copied;
	for (const Selected &selected : selection) {
		const int key = _find_key(selected);
		if (key < 0) {
			continue;
		}
		const Pair<int, int64_t> id(selected.track, int64_t(Math::round(selected.time * 1e6)));
		if (copied.has(id)) {
			continue;
		}
		copied.insert(id);
		const LocalVector<TrackKey> keys = _read_track(selected.track);
		ClipboardKey entry;
		entry.track = selected.track;
		entry.type = animation->track_get_type(selected.track);
		entry.time = selected.time;
		entry.key = keys[key];
		clipboard.push_back(entry);
	}
	_set_status_message(vformat(TTR("Copied %d keyframes."), int(clipboard.size())));
}

void AsosdotGraphEditor::_paste() {
	if (read_only || clipboard.is_empty()) {
		return;
	}
	double first = 1e20;
	for (const ClipboardKey &entry : clipboard) {
		first = MIN(first, entry.time);
	}
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	selection.clear();
	for (const ClipboardKey &entry : clipboard) {
		if (entry.track >= animation->get_track_count() || animation->track_get_type(entry.track) != entry.type) {
			continue;
		}
		if (!before.has(entry.track)) {
			before[entry.track] = _read_track(entry.track);
			after[entry.track] = before[entry.track];
		}
		TrackKey key = entry.key;
		key.time = MAX(0.0, play_position + entry.time - first);
		LocalVector<TrackKey> &keys = after[entry.track];
		for (uint32_t k = 0; k < keys.size(); k++) {
			if (Math::abs(keys[k].time - key.time) < 1e-5) {
				keys.remove_at(k);
				break;
			}
		}
		keys.push_back(key);
		_sort_keys(keys);
		for (const Channel &channel : channels) {
			if (channel.track == entry.track) {
				Selected pasted;
				pasted.track = entry.track;
				pasted.component = channel.component;
				pasted.time = key.time;
				selection.insert(pasted);
			}
		}
	}
	_commit_tracks(TTR("Paste Keyframes"), before, after);
	_update_status();
}

// The value a track would key now: the curve's at the playhead, or the node's if it has no keys yet.
bool AsosdotGraphEditor::_value_at_playhead(int p_track, Variant &r_value) const {
	const Animation::TrackType type = animation->track_get_type(p_track);
	if (animation->track_get_key_count(p_track) > 0) {
		switch (type) {
			case Animation::TYPE_POSITION_3D:
				r_value = animation->position_track_interpolate(p_track, play_position);
				return true;
			case Animation::TYPE_ROTATION_3D:
				r_value = animation->rotation_track_interpolate(p_track, play_position);
				return true;
			case Animation::TYPE_SCALE_3D:
				r_value = animation->scale_track_interpolate(p_track, play_position);
				return true;
			case Animation::TYPE_BLEND_SHAPE:
				r_value = animation->blend_shape_track_interpolate(p_track, play_position);
				return true;
			case Animation::TYPE_VALUE:
				r_value = animation->value_track_interpolate(p_track, play_position);
				return true;
			case Animation::TYPE_BEZIER:
				r_value = animation->bezier_track_interpolate(p_track, play_position);
				return true;
			default:
				return false;
		}
	}
	const NodePath path = animation->track_get_path(p_track);
	Node *node = root ? root->get_node_or_null(NodePath(path.get_concatenated_names())) : nullptr;
	if (!node) {
		return false;
	}
	if (type == Animation::TYPE_VALUE || type == Animation::TYPE_BEZIER) {
		bool valid = false;
		r_value = node->get_indexed(path.get_as_property_path().get_subnames(), &valid);
		return valid;
	}
	Node3D *node_3d = Object::cast_to<Node3D>(node);
	if (!node_3d || path.get_subname_count() > 0) {
		return false;
	}
	switch (type) {
		case Animation::TYPE_POSITION_3D:
			r_value = node_3d->get_position();
			return true;
		case Animation::TYPE_ROTATION_3D:
			r_value = node_3d->get_quaternion();
			return true;
		case Animation::TYPE_SCALE_3D:
			r_value = node_3d->get_scale();
			return true;
		default:
			return false;
	}
}

// Blender's I in the Graph Editor: a key on every selected curve (or every curve shown) at the
// playhead.
void AsosdotGraphEditor::_insert_keys() {
	if (read_only) {
		return;
	}
	HashSet<int> tracks;
	for (int channel : selected_channels) {
		if (!_is_channel_locked(channel)) {
			tracks.insert(channels[channel].track);
		}
	}
	if (tracks.is_empty()) {
		for (uint32_t i = 0; i < channels.size(); i++) {
			if (_is_channel_shown(i) && !_is_channel_locked(i)) {
				tracks.insert(channels[i].track);
			}
		}
	}
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	selection.clear();
	for (int track : tracks) {
		Variant value;
		if (!_value_at_playhead(track, value)) {
			continue;
		}
		before[track] = _read_track(track);
		LocalVector<TrackKey> keys(before[track]);
		const double time = track_editor ? track_editor->snap_time(play_position) : play_position;
		for (uint32_t k = 0; k < keys.size(); k++) {
			if (Math::abs(keys[k].time - time) < 1e-5) {
				keys.remove_at(k);
				break;
			}
		}
		TrackKey key;
		key.time = time;
		key.value = animation->track_get_type(track) == Animation::TYPE_BEZIER ? Variant(double(value)) : value;
		keys.push_back(key);
		_sort_keys(keys);
		if (animation->track_get_type(track) == Animation::TYPE_BEZIER) {
			for (uint32_t k = 0; k < keys.size(); k++) {
				if (Math::abs(keys[k].time - time) < 1e-5) {
					_auto_handles(keys, k);
				}
			}
		}
		after[track] = keys;
		for (const Channel &channel : channels) {
			if (channel.track == track) {
				Selected inserted;
				inserted.track = track;
				inserted.component = channel.component;
				inserted.time = time;
				selection.insert(inserted);
			}
		}
	}
	_commit_tracks(TTR("Insert Keyframes"), before, after);
	_update_status();
}

HashSet<int> AsosdotGraphEditor::_selected_tracks() const {
	HashSet<int> tracks;
	for (const Selected &selected : selection) {
		const int channel = _channel_of(selected.track, selected.component);
		if (channel >= 0 && !_is_channel_locked(channel)) {
			tracks.insert(selected.track);
		}
	}
	return tracks;
}

void AsosdotGraphEditor::_set_interpolation(int p_option) {
	if (read_only || selection.is_empty()) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Keyframe Interpolation"), UndoRedo::MERGE_DISABLE, animation.ptr());
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	bool constant_bezier = false;
	for (int track : _selected_tracks()) {
		if (animation->track_get_type(track) == Animation::TYPE_BEZIER) {
			if (p_option == INTERPOLATION_CONSTANT) {
				constant_bezier = true;
				continue;
			}
			before[track] = _read_track(track);
			LocalVector<TrackKey> keys(before[track]);
			for (uint32_t k = 0; k < keys.size(); k++) {
				Selected probe;
				probe.track = track;
				probe.time = keys[k].time;
				if (!selection.has(probe)) {
					continue;
				}
				if (p_option == INTERPOLATION_LINEAR) {
					_vector_handles(keys, k);
				} else {
					_auto_handles(keys, k);
				}
			}
			after[track] = keys;
			continue;
		}
		// Other tracks interpolate the same way all along, as Godot keeps it per track.
		const Animation::InterpolationType type = p_option == INTERPOLATION_CONSTANT ? Animation::INTERPOLATION_NEAREST : (p_option == INTERPOLATION_LINEAR ? Animation::INTERPOLATION_LINEAR : Animation::INTERPOLATION_CUBIC);
		undo_redo->add_do_method(animation.ptr(), "track_set_interpolation_type", track, type);
		undo_redo->add_undo_method(animation.ptr(), "track_set_interpolation_type", track, animation->track_get_interpolation_type(track));
	}
	_add_track_ops(undo_redo, before, after);
	undo_redo->commit_action();
	if (constant_bezier) {
		_set_status_message(TTR("Bezier tracks can't hold a value until the next key; they were left as they were."));
	}
}

void AsosdotGraphEditor::_set_easing(int p_option) {
	if (read_only || selection.is_empty()) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Set Keyframe Easing"), UndoRedo::MERGE_DISABLE, animation.ptr());
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	HashSet<Pair<int, int>> done;
	for (const Selected &selected : selection) {
		const int channel = _channel_of(selected.track, selected.component);
		const int key = _find_key(selected);
		if (channel < 0 || key < 0 || _is_channel_locked(channel) || done.has(Pair<int, int>(selected.track, key))) {
			continue;
		}
		done.insert(Pair<int, int>(selected.track, key));
		if (animation->track_get_type(selected.track) == Animation::TYPE_BEZIER) {
			// On a Bezier curve, easing is level handles: going out of the key for a slow start,
			// into the next one for a slow end.
			if (!before.has(selected.track)) {
				before[selected.track] = _read_track(selected.track);
				after[selected.track] = before[selected.track];
			}
			LocalVector<TrackKey> &keys = after[selected.track];
			const bool ease_in = p_option == EASING_IN || p_option == EASING_IN_OUT;
			const bool ease_out = p_option == EASING_OUT || p_option == EASING_IN_OUT;
			if (p_option == EASING_LINEAR) {
				_vector_handles(keys, key);
			}
			if (ease_in) {
				keys[key].out_handle.y = 0;
				keys[key].handle_mode = Animation::HANDLE_MODE_FREE;
			}
			if (ease_out && key + 1 < int(keys.size())) {
				keys[key + 1].in_handle.y = 0;
				keys[key + 1].handle_mode = Animation::HANDLE_MODE_FREE;
			}
			continue;
		}
		undo_redo->add_do_method(animation.ptr(), "track_set_key_transition", selected.track, key, _easing_transition(p_option));
		undo_redo->add_undo_method(animation.ptr(), "track_set_key_transition", selected.track, key, animation->track_get_key_transition(selected.track, key));
	}
	_add_track_ops(undo_redo, before, after);
	undo_redo->commit_action();
}

void AsosdotGraphEditor::_set_handle_mode(int p_option) {
	if (read_only || selection.is_empty()) {
		return;
	}
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	for (const Selected &selected : selection) {
		const int channel = _channel_of(selected.track, selected.component);
		const int key = _find_key(selected);
		if (channel < 0 || key < 0 || _is_channel_locked(channel) || animation->track_get_type(selected.track) != Animation::TYPE_BEZIER) {
			continue;
		}
		if (!before.has(selected.track)) {
			before[selected.track] = _read_track(selected.track);
			after[selected.track] = before[selected.track];
		}
		LocalVector<TrackKey> &keys = after[selected.track];
		switch (p_option) {
			case HANDLE_AUTO_CLAMPED:
				_auto_handles(keys, key);
				break;
			case HANDLE_VECTOR:
				_vector_handles(keys, key);
				break;
			case HANDLE_ALIGNED:
				keys[key].handle_mode = Animation::HANDLE_MODE_BALANCED;
				break;
			case HANDLE_FREE:
				keys[key].handle_mode = Animation::HANDLE_MODE_FREE;
				break;
			case HANDLE_MIRRORED:
				keys[key].handle_mode = Animation::HANDLE_MODE_MIRRORED;
				break;
		}
	}
	if (after.is_empty()) {
		_set_status_message(TTR("Handle types are for Bezier tracks. Convert a track to Bezier curves (Channel menu) to shape it with handles."));
		return;
	}
	_commit_tracks(TTR("Set Keyframe Handle Type"), before, after);
}

void AsosdotGraphEditor::_snap(int p_option) {
	if (selection.is_empty()) {
		return;
	}
	if (p_option == KEY_CURRENT_TO_SELECTED) {
		double total = 0.0;
		for (const Selected &selected : selection) {
			total += selected.time;
		}
		emit_signal(SNAME("timeline_changed"), float(total / selection.size()), false);
		return;
	}
	if (read_only) {
		return;
	}
	const double step = animation->get_step();
	HashMap<int, LocalVector<TrackKey>> before;
	HashMap<int, LocalVector<TrackKey>> after;
	RBSet<Selected> moved_selection;
	HashSet<Pair<int, int>> done;
	for (const Selected &selected : selection) {
		const int channel = _channel_of(selected.track, selected.component);
		const int key = _find_key(selected);
		if (channel < 0 || key < 0 || _is_channel_locked(channel)) {
			continue;
		}
		double time = p_option == KEY_SNAP_TO_CURRENT ? play_position : (step > 0.0 ? Math::snapped(selected.time, step) : selected.time);
		Selected moved = selected;
		moved.time = time;
		moved_selection.insert(moved);
		if (done.has(Pair<int, int>(selected.track, key))) {
			continue;
		}
		done.insert(Pair<int, int>(selected.track, key));
		if (!before.has(selected.track)) {
			before[selected.track] = _read_track(selected.track);
			after[selected.track] = before[selected.track];
		}
		// Snapping moves the key: its old place is left, and its new one taken from any key there.
		LocalVector<TrackKey> &keys = after[selected.track];
		TrackKey snapped;
		for (uint32_t k = 0; k < keys.size(); k++) {
			if (Math::abs(keys[k].time - selected.time) < 1e-5) {
				snapped = keys[k];
				keys.remove_at(k);
				break;
			}
		}
		snapped.time = time;
		for (uint32_t k = 0; k < keys.size(); k++) {
			if (Math::abs(keys[k].time - time) < 1e-5) {
				keys.remove_at(k);
				break;
			}
		}
		keys.push_back(snapped);
		_sort_keys(keys);
	}
	_commit_tracks(TTR("Snap Keyframes"), before, after);
	selection = moved_selection;
}

void AsosdotGraphEditor::_jump(bool p_next) {
	_cache_curves();
	double best = p_next ? 1e20 : -1e20;
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			if (p_next && key.time > play_position + 1e-5 && key.time < best) {
				best = key.time;
			} else if (!p_next && key.time < play_position - 1e-5 && key.time > best) {
				best = key.time;
			}
		}
	}
	if (Math::abs(best) < 1e19) {
		emit_signal(SNAME("timeline_changed"), float(best), false);
	}
}

// Splits a track into Bezier curves, one per number, keyed where it was; each key then has handles
// to shape it, as Blender's F-curves do.
void AsosdotGraphEditor::_convert_to_bezier() {
	if (read_only || animation.is_null()) {
		return;
	}
	HashSet<int> tracks;
	for (int channel : selected_channels) {
		tracks.insert(channels[channel].track);
	}
	for (const Selected &selected : selection) {
		tracks.insert(selected.track);
	}
	if (tracks.is_empty()) {
		_set_status_message(TTR("Select the curves to convert, in the channel list or by their keys."));
		return;
	}

	struct Conversion {
		int track = -1;
		LocalVector<NodePath> paths;
		LocalVector<LocalVector<TrackKey>> curves;
	};
	LocalVector<Conversion> conversions;
	int skipped = 0;
	for (int track : tracks) {
		const Animation::TrackType type = animation->track_get_type(track);
		if (type == Animation::TYPE_BEZIER || animation->track_is_compressed(track) || animation->track_get_key_count(track) == 0) {
			continue;
		}
		const NodePath path = animation->track_get_path(track);
		const String node_path = path.get_concatenated_names();
		String property;
		int count = 0;
		bool rotation = false;
		switch (type) {
			case Animation::TYPE_POSITION_3D:
			case Animation::TYPE_SCALE_3D:
			case Animation::TYPE_ROTATION_3D: {
				const String name = type == Animation::TYPE_POSITION_3D ? "position" : (type == Animation::TYPE_SCALE_3D ? "scale" : "rotation");
				count = 3;
				rotation = type == Animation::TYPE_ROTATION_3D;
				if (path.get_subname_count() > 0) {
					// A bone's pose is a property of its skeleton, by index. Its rotation is a
					// quaternion there, which curves of their own can't keep whole, so it stays.
					Skeleton3D *skeleton = root ? Object::cast_to<Skeleton3D>(root->get_node_or_null(NodePath(node_path))) : nullptr;
					const int bone = skeleton ? skeleton->find_bone(path.get_concatenated_subnames()) : -1;
					if (bone < 0 || rotation) {
						skipped++;
						continue;
					}
					property = vformat("bones/%d/%s", bone, name);
				} else {
					property = name;
				}
			} break;
			case Animation::TYPE_BLEND_SHAPE:
				count = 1;
				property = "blend_shapes/" + path.get_concatenated_subnames();
				break;
			case Animation::TYPE_VALUE: {
				const Variant first = animation->track_get_key_value(track, 0);
				count = _component_count(first);
				if (count == 0 || first.get_type() == Variant::BOOL || first.get_type() == Variant::QUATERNION) {
					skipped++;
					continue;
				}
				property = path.get_concatenated_subnames();
			} break;
			default:
				continue;
		}

		Conversion conversion;
		conversion.track = track;
		const bool linear = animation->track_get_interpolation_type(track) == Animation::INTERPOLATION_LINEAR || animation->track_get_interpolation_type(track) == Animation::INTERPOLATION_LINEAR_ANGLE;
		const EulerOrder order = rotation ? _get_euler_order(track) : EulerOrder::YXZ;
		const Variant first = animation->track_get_key_value(track, 0);
		static const char *axes[4] = { "x", "y", "z", "w" };
		static const char *color_components[4] = { "r", "g", "b", "a" };
		for (int component = 0; component < count; component++) {
			String subname = property;
			if (count > 1) {
				subname += ":" + String(first.get_type() == Variant::COLOR ? color_components[component] : axes[component]);
			}
			conversion.paths.push_back(NodePath(node_path + ":" + subname));
			LocalVector<TrackKey> keys;
			double previous = 0.0;
			for (int k = 0; k < animation->track_get_key_count(track); k++) {
				const Variant value = animation->track_get_key_value(track, k);
				double number = 0.0;
				if (rotation) {
					// Node3D.rotation is in radians, in the node's own order; turns between keys are
					// kept whole so the curve doesn't jump.
					number = Basis(Quaternion(value).normalized()).get_euler(order)[component];
					if (k > 0) {
						number -= Math::round((number - previous) / Math::TAU) * Math::TAU;
					}
					previous = number;
				} else if (type == Animation::TYPE_BLEND_SHAPE) {
					number = value;
				} else {
					number = _get_component(value, component);
				}
				TrackKey key;
				key.time = animation->track_get_key_time(track, k);
				key.value = number;
				keys.push_back(key);
			}
			for (uint32_t k = 0; k < keys.size(); k++) {
				if (linear) {
					_vector_handles(keys, k);
				} else {
					_auto_handles(keys, k);
				}
			}
			conversion.curves.push_back(LocalVector<TrackKey>(keys));
		}
		conversions.push_back(conversion);
	}
	if (conversions.is_empty()) {
		_set_status_message(skipped > 0 ? TTR("These tracks can't be split into curves: bone rotations and quaternion values are kept whole.") : TTR("Nothing to convert: the selected curves are Bezier already."));
		return;
	}

	// Tracks are converted from the last back, so the tracks still to convert keep their places;
	// the new curves go at the end. Undo puts each track back where it was.
	struct ByTrackDescending {
		_FORCE_INLINE_ bool operator()(const Conversion &p_a, const Conversion &p_b) const {
			return p_a.track > p_b.track;
		}
	};
	conversions.sort_custom<ByTrackDescending>();

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("Convert to Bezier Curves"), UndoRedo::MERGE_DISABLE, animation.ptr());
	LocalVector<int> order;
	for (int i = 0; i < animation->get_track_count(); i++) {
		order.push_back(i);
	}
	int next_id = animation->get_track_count();
	struct Undo {
		int track = -1;
		int position = 0;
		LocalVector<int> new_ids;
	};
	LocalVector<Undo> undos;
	for (const Conversion &conversion : conversions) {
		Undo undo;
		undo.track = conversion.track;
		for (uint32_t c = 0; c < conversion.paths.size(); c++) {
			const int index = order.size();
			undo_redo->add_do_method(animation.ptr(), "add_track", Animation::TYPE_BEZIER, -1);
			undo_redo->add_do_method(animation.ptr(), "track_set_path", index, conversion.paths[c]);
			undo_redo->add_do_method(animation.ptr(), "track_set_enabled", index, animation->track_is_enabled(conversion.track));
			undo_redo->add_do_method(this, "_set_track_keys", animation, index, _track_keys_to_array(conversion.curves[c]));
			order.push_back(next_id);
			undo.new_ids.push_back(next_id);
			next_id++;
		}
		const int position = order.find(conversion.track);
		undo.position = position;
		undo_redo->add_do_method(animation.ptr(), "remove_track", position);
		order.remove_at(position);
		undos.push_back(undo);
	}
	// Undo runs its steps in the order given, so the last conversion is taken back first.
	for (int u = int(undos.size()) - 1; u >= 0; u--) {
		const Undo &undo = undos[u];
		const int track = undo.track;
		undo_redo->add_undo_method(animation.ptr(), "add_track", animation->track_get_type(track), undo.position);
		undo_redo->add_undo_method(animation.ptr(), "track_set_path", undo.position, animation->track_get_path(track));
		undo_redo->add_undo_method(animation.ptr(), "track_set_interpolation_type", undo.position, animation->track_get_interpolation_type(track));
		undo_redo->add_undo_method(animation.ptr(), "track_set_interpolation_loop_wrap", undo.position, animation->track_get_interpolation_loop_wrap(track));
		undo_redo->add_undo_method(animation.ptr(), "track_set_enabled", undo.position, animation->track_is_enabled(track));
		if (animation->track_get_type(track) == Animation::TYPE_VALUE) {
			undo_redo->add_undo_method(animation.ptr(), "value_track_set_update_mode", undo.position, animation->value_track_get_update_mode(track));
		}
		undo_redo->add_undo_method(this, "_set_track_keys", animation, undo.position, _track_keys_to_array(_read_track(track)));
		order.insert(undo.position, track);
		for (int n = int(undo.new_ids.size()) - 1; n >= 0; n--) {
			const int position = order.find(undo.new_ids[n]);
			undo_redo->add_undo_method(animation.ptr(), "remove_track", position);
			order.remove_at(position);
		}
	}
	undo_redo->commit_action();
	selection.clear();
	selected_channels.clear();
	has_active = false;
	_update_channels(true);
	if (skipped > 0) {
		_set_status_message(TTR("Converted. Bone rotations and quaternion values were kept as they were."));
	}
}

/// Selection.

void AsosdotGraphEditor::_layout_rows() {
	rows.clear();
	float y = -channel_scroll;
	const float row = _row_height();
	for (uint32_t g = 0; g < groups.size(); g++) {
		Row header_row;
		header_row.group = g;
		header_row.y = y;
		rows.push_back(header_row);
		y += row;
		if (collapsed_groups.has(groups[g].key)) {
			continue;
		}
		for (int channel : groups[g].channels) {
			Row channel_row;
			channel_row.group = g;
			channel_row.channel = channel;
			channel_row.y = y;
			rows.push_back(channel_row);
			y += row;
		}
	}
	channel_scroll_max = MAX(0.0f, y + channel_scroll - view->get_size().y + row);
}

bool AsosdotGraphEditor::_key_at(const Vector2 &p_pos, Selected &r_key) const {
	float best = PICK_DISTANCE * EDSCALE;
	bool found = false;
	for (uint32_t i = 0; i < channels.size() && i < curve_keys.size(); i++) {
		if (!_is_channel_shown(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			const float distance = Vector2(_time_to_x(key.time), _value_to_y(i, key.value)).distance_to(p_pos);
			// Ties go to the curve drawn last, and the selected ones are drawn last.
			if (distance < best || (found && Math::is_equal_approx(distance, best) && selected_channels.has(i))) {
				best = distance;
				r_key.track = channels[i].track;
				r_key.component = channels[i].component;
				r_key.time = key.time;
				found = true;
			}
		}
	}
	return found;
}

bool AsosdotGraphEditor::_handle_at(const Vector2 &p_pos, Selected &r_key, int &r_side) const {
	const float pick = PICK_DISTANCE * EDSCALE;
	for (uint32_t i = 0; i < channels.size() && i < curve_keys.size(); i++) {
		if (!_is_channel_shown(i) || _is_channel_locked(i) || animation->track_get_type(channels[i].track) != Animation::TYPE_BEZIER) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			Selected probe;
			probe.track = channels[i].track;
			probe.component = channels[i].component;
			probe.time = key.time;
			if (!handles_button->is_pressed() && !selection.has(probe)) {
				continue;
			}
			const Vector2 point(_time_to_x(key.time), _value_to_y(i, key.value));
			const double scale = _value_per_pixel_for(i);
			const Vector2 in_point = point + Vector2(key.in_handle.x * timeline->get_zoom_scale(), -key.in_handle.y / scale);
			const Vector2 out_point = point + Vector2(key.out_handle.x * timeline->get_zoom_scale(), -key.out_handle.y / scale);
			if (in_point.distance_to(p_pos) < pick) {
				r_key = probe;
				r_side = -1;
				return true;
			}
			if (out_point.distance_to(p_pos) < pick) {
				r_key = probe;
				r_side = 1;
				return true;
			}
		}
	}
	return false;
}

int AsosdotGraphEditor::_channel_at(const Vector2 &p_pos) const {
	const float row = _row_height();
	for (const Row &entry : rows) {
		if (p_pos.y >= entry.y && p_pos.y < entry.y + row) {
			return &entry - rows.ptr();
		}
	}
	return -1;
}

int AsosdotGraphEditor::_curve_at(const Vector2 &p_pos) const {
	const double time = _x_to_time(p_pos.x);
	int best = -1;
	float best_distance = 5 * EDSCALE;
	for (uint32_t i = 0; i < channels.size(); i++) {
		double value = 0.0;
		if (!_is_channel_shown(i) || !_evaluate(i, time, value)) {
			continue;
		}
		if (channels[i].rotation && !curve_keys[i].is_empty()) {
			// As drawn: near the shown value of the key before.
			double reference = curve_keys[i][0].value;
			for (const CurveKey &key : curve_keys[i]) {
				if (key.time <= time) {
					reference = key.value;
				}
			}
			value -= Math::round((value - reference) / 360.0) * 360.0;
		}
		const float distance = Math::abs(_value_to_y(i, value) - p_pos.y);
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return best;
}

void AsosdotGraphEditor::_select_all(bool p_select) {
	selection.clear();
	if (p_select) {
		_cache_curves();
		for (uint32_t i = 0; i < channels.size(); i++) {
			if (!_is_channel_shown(i) || _is_channel_locked(i)) {
				continue;
			}
			for (const CurveKey &key : curve_keys[i]) {
				Selected selected;
				selected.track = channels[i].track;
				selected.component = channels[i].component;
				selected.time = key.time;
				selection.insert(selected);
			}
		}
	}
	_update_status();
	_update_sidebar();
	view->queue_redraw();
}

void AsosdotGraphEditor::_select_invert() {
	_cache_curves();
	RBSet<Selected> inverted;
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i) || _is_channel_locked(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			Selected selected;
			selected.track = channels[i].track;
			selected.component = channels[i].component;
			selected.time = key.time;
			if (!selection.has(selected)) {
				inverted.insert(selected);
			}
		}
	}
	selection = inverted;
	_update_status();
	_update_sidebar();
	view->queue_redraw();
}

// Blender's K: every key at the times of the selected ones, on every curve shown.
void AsosdotGraphEditor::_select_columns() {
	_cache_curves();
	LocalVector<double> times;
	for (const Selected &selected : selection) {
		times.push_back(selected.time);
	}
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i) || _is_channel_locked(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			for (double time : times) {
				if (Math::abs(key.time - time) < 1e-5) {
					Selected selected;
					selected.track = channels[i].track;
					selected.component = channels[i].component;
					selected.time = key.time;
					selection.insert(selected);
				}
			}
		}
	}
	_update_status();
	view->queue_redraw();
}

void AsosdotGraphEditor::_select_side(bool p_after) {
	_cache_curves();
	selection.clear();
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i) || _is_channel_locked(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			if (p_after ? key.time >= play_position - 1e-5 : key.time <= play_position + 1e-5) {
				Selected selected;
				selected.track = channels[i].track;
				selected.component = channels[i].component;
				selected.time = key.time;
				selection.insert(selected);
			}
		}
	}
	_update_status();
	_update_sidebar();
	view->queue_redraw();
}

void AsosdotGraphEditor::_box_select(const Rect2 &p_rect, bool p_add, bool p_remove) {
	if (!p_add && !p_remove) {
		selection.clear();
	}
	for (uint32_t i = 0; i < channels.size() && i < curve_keys.size(); i++) {
		if (!_is_channel_shown(i) || _is_channel_locked(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			if (!p_rect.has_point(Vector2(_time_to_x(key.time), _value_to_y(i, key.value)))) {
				continue;
			}
			Selected selected;
			selected.track = channels[i].track;
			selected.component = channels[i].component;
			selected.time = key.time;
			if (p_remove) {
				selection.erase(selected);
			} else {
				selection.insert(selected);
			}
		}
	}
	_update_status();
	_update_sidebar();
}

void AsosdotGraphEditor::_set_active(const Selected &p_key) {
	active = p_key;
	has_active = true;
	_update_sidebar();
}

void AsosdotGraphEditor::_prune_selection() {
	if (animation.is_null()) {
		selection.clear();
		has_active = false;
		return;
	}
	RBSet<Selected> kept;
	for (const Selected &selected : selection) {
		if (_channel_of(selected.track, selected.component) >= 0 && _find_key(selected) >= 0) {
			kept.insert(selected);
		}
	}
	selection = kept;
	if (has_active && (_channel_of(active.track, active.component) < 0 || _find_key(active) < 0)) {
		has_active = false;
	}
}

/// View.

void AsosdotGraphEditor::_frame(bool p_selected_only, bool p_fit_time) {
	if (animation.is_null()) {
		return;
	}
	_update_channels();
	_cache_curves();
	double low = 1e20;
	double high = -1e20;
	double first = 1e20;
	double last = -1e20;
	const bool normalize = normalize_button->is_pressed();
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i)) {
			continue;
		}
		const Vector2 range = channel_ranges[i];
		const double half = MAX((range.y - range.x) * 0.5, 1e-6);
		const double middle = (range.x + range.y) * 0.5;
		for (const CurveKey &key : curve_keys[i]) {
			if (p_selected_only) {
				Selected probe;
				probe.track = channels[i].track;
				probe.component = channels[i].component;
				probe.time = key.time;
				if (!selection.has(probe)) {
					continue;
				}
			}
			const double shown = normalize ? (key.value - middle) / half : key.value;
			low = MIN(low, shown);
			high = MAX(high, shown);
			first = MIN(first, key.time);
			last = MAX(last, key.time);
		}
	}
	if (low > high) {
		return;
	}
	if (high - low < 1e-6) {
		const double pad = MAX(Math::abs(high) * 0.1, 0.5);
		low -= pad;
		high += pad;
	}
	value_center = (low + high) * 0.5;
	value_per_pixel = (high - low) / MAX(view->get_size().y * 0.8, 10.0);
	if (p_selected_only && p_fit_time) {
		// Brings the selection into view; the timeline's zoom stays as it is.
		const double visible_time = (view->get_size().x - _list_width()) / timeline->get_zoom_scale();
		if (first < timeline->get_value() || last > timeline->get_value() + visible_time) {
			timeline->set_value(MAX(0.0, (first + last) * 0.5 - visible_time * 0.5));
		}
	} else if (p_fit_time) {
		timeline->auto_fit();
	}
	framed_once = true;
	view->queue_redraw();
}

void AsosdotGraphEditor::_popup_at_mouse(PopupMenu *p_menu) {
	p_menu->set_position(view->get_screen_position() + view->get_local_mouse_position());
	p_menu->reset_size();
	p_menu->popup();
}

void AsosdotGraphEditor::_set_status_message(const String &p_message) {
	status_message = p_message;
	status_message_until = OS::get_singleton()->get_ticks_msec() + 4000;
	_update_status();
}

void AsosdotGraphEditor::_update_status() {
	String text;
	if (mode == MODE_GRAB || mode == MODE_SCALE) {
		const String what = mode == MODE_GRAB ? TTR("Move") : TTR("Scale");
		const String along = axis == AXIS_TIME ? TTR(" along time") : (axis == AXIS_VALUE ? TTR(" along value") : String());
		text = what + along + "   " + TTR("X: time only, Y: value only. Click or Enter to confirm, right-click or Esc to cancel.");
	} else if (OS::get_singleton()->get_ticks_msec() < status_message_until) {
		text = status_message;
	} else if (read_only) {
		text = TTR("This animation is read-only.");
	} else if (box_pending) {
		text = TTR("Box select: drag over keys. Shift adds, Ctrl takes away.");
	} else if (!selection.is_empty()) {
		text = vformat(TTR("%d keys selected"), int(selection.size()));
	}
	status->set_text(text);
}

void AsosdotGraphEditor::_update_menus() {
	PopupMenu *view_popup = view_menu->get_popup();
	view_popup->set_item_checked(view_popup->get_item_index(VIEW_SHOW_HANDLES), handles_button->is_pressed());
	view_popup->set_item_checked(view_popup->get_item_index(VIEW_NORMALIZE), normalize_button->is_pressed());
	view_popup->set_item_checked(view_popup->get_item_index(VIEW_ONLY_SELECTED), only_selected_button->is_pressed());
	view_popup->set_item_checked(view_popup->get_item_index(VIEW_SIDEBAR), sidebar->is_visible());
}

void AsosdotGraphEditor::_menu_option(int p_option) {
	switch (p_option) {
		case VIEW_FRAME_ALL:
			_frame(false, true);
			break;
		case VIEW_FRAME_SELECTED:
			_frame(true, true);
			break;
		case VIEW_SHOW_HANDLES:
			handles_button->set_pressed(!handles_button->is_pressed());
			break;
		case VIEW_NORMALIZE:
			normalize_button->set_pressed(!normalize_button->is_pressed());
			break;
		case VIEW_ONLY_SELECTED:
			only_selected_button->set_pressed(!only_selected_button->is_pressed());
			break;
		case VIEW_SIDEBAR:
			sidebar_button->set_pressed(!sidebar_button->is_pressed());
			break;
		case VIEW_DOPE_SHEET:
			if (track_editor) {
				track_editor->toggle_graph_editor();
			}
			break;
		case SELECT_ALL:
			_select_all(true);
			break;
		case SELECT_NONE:
			_select_all(false);
			break;
		case SELECT_INVERT:
			_select_invert();
			break;
		case SELECT_BOX:
			box_pending = true;
			_update_status();
			break;
		case SELECT_COLUMNS:
			_select_columns();
			break;
		case SELECT_BEFORE_CURRENT:
			_select_side(false);
			break;
		case SELECT_AFTER_CURRENT:
			_select_side(true);
			break;
		case CHANNEL_HIDE:
		case CHANNEL_HIDE_UNSELECTED: {
			HashSet<int> chosen(selected_channels);
			for (const Selected &selected : selection) {
				chosen.insert(_channel_of(selected.track, selected.component));
			}
			for (uint32_t i = 0; i < channels.size(); i++) {
				if (chosen.has(i) == (p_option == CHANNEL_HIDE)) {
					hidden_channels.insert(_channel_id(i));
				}
			}
			selected_channels.clear();
		} break;
		case CHANNEL_REVEAL:
			hidden_channels.clear();
			break;
		case CHANNEL_LOCK:
		case CHANNEL_UNLOCK:
			for (int channel : selected_channels) {
				if (p_option == CHANNEL_LOCK) {
					locked_channels.insert(_channel_id(channel));
				} else {
					locked_channels.erase(_channel_id(channel));
				}
			}
			break;
		case CHANNEL_EXPAND_ALL:
			collapsed_groups.clear();
			break;
		case CHANNEL_COLLAPSE_ALL:
			for (const Group &group : groups) {
				collapsed_groups.insert(group.key);
			}
			break;
		case CHANNEL_CONVERT_TO_BEZIER:
			_convert_to_bezier();
			break;
		case KEY_INSERT:
			_insert_keys();
			break;
		case KEY_DELETE:
			_delete_selection();
			break;
		case KEY_DUPLICATE:
			_duplicate_selection();
			break;
		case KEY_COPY:
			_copy_selection();
			break;
		case KEY_PASTE:
			_paste();
			break;
		case KEY_GRAB:
			_begin_edit(MODE_GRAB);
			break;
		case KEY_SCALE:
			_begin_edit(MODE_SCALE);
			break;
		case KEY_SNAP_TO_CURRENT:
		case KEY_SNAP_TO_FRAME:
		case KEY_CURRENT_TO_SELECTED:
			_snap(p_option);
			break;
		case KEY_JUMP_NEXT:
			_jump(true);
			break;
		case KEY_JUMP_PREVIOUS:
			_jump(false);
			break;
		case INTERPOLATION_CONSTANT:
		case INTERPOLATION_LINEAR:
		case INTERPOLATION_BEZIER:
			_set_interpolation(p_option);
			break;
		case EASING_LINEAR:
		case EASING_IN:
		case EASING_OUT:
		case EASING_IN_OUT:
		case EASING_OUT_IN:
			_set_easing(p_option);
			break;
		case HANDLE_AUTO_CLAMPED:
		case HANDLE_VECTOR:
		case HANDLE_ALIGNED:
		case HANDLE_FREE:
		case HANDLE_MIRRORED:
			_set_handle_mode(p_option);
			break;
	}
	_update_menus();
	_update_status();
	view->queue_redraw();
}

void AsosdotGraphEditor::_toggle_sidebar(bool p_shown) {
	sidebar->set_visible(p_shown);
	_update_sidebar();
	_update_menus();
}

void AsosdotGraphEditor::_update_sidebar() {
	if (!sidebar || !sidebar->is_visible() || animation.is_null()) {
		return;
	}
	updating_sidebar = true;
	// As in Blender, the active key shows while it's selected.
	const int channel = has_active && selection.has(active) ? _channel_of(active.track, active.component) : -1;
	const int key = channel >= 0 ? _find_key(active) : -1;
	const bool valid = key >= 0;
	sidebar_fields->set_visible(valid);
	if (!valid) {
		sidebar_channel->set_text(TTR("Click a key to see and edit it here."));
		updating_sidebar = false;
		return;
	}
	_cache_curves();
	const bool bezier = animation->track_get_type(active.track) == Animation::TYPE_BEZIER;
	const bool frames = timeline->is_using_fps() && animation->get_step() > 0.0;
	sidebar_channel->set_text(groups[channels[channel].group].label + "  " + channels[channel].label);
	sidebar_time->set_suffix(frames ? TTR("f") : TTR("s"));
	sidebar_time->set_step(frames ? 1.0 : 0.001);
	const double time = animation->track_get_key_time(active.track, key);
	sidebar_time->set_value_no_signal(frames ? Math::round(time / animation->get_step()) : time);
	sidebar_value->set_suffix(channels[channel].rotation ? String::chr(0x00B0) : String());
	sidebar_value->set_value_no_signal(key < int(curve_keys[channel].size()) ? curve_keys[channel][key].value : 0.0);
	sidebar_value->set_editable(!_is_channel_locked(channel));
	sidebar_time->set_editable(!_is_channel_locked(channel));

	sidebar_curve->set_visible(!bezier);
	sidebar_bezier->set_visible(bezier);
	if (bezier) {
		const Vector2 in_handle = animation->bezier_track_get_key_in_handle(active.track, key);
		const Vector2 out_handle = animation->bezier_track_get_key_out_handle(active.track, key);
		sidebar_in_time->set_value_no_signal(in_handle.x);
		sidebar_in_value->set_value_no_signal(in_handle.y);
		sidebar_out_time->set_value_no_signal(out_handle.x);
		sidebar_out_value->set_value_no_signal(out_handle.y);
		sidebar_handle_mode->select(animation->bezier_track_get_key_handle_mode(active.track, key));
	} else {
		const Animation::InterpolationType interpolation = animation->track_get_interpolation_type(active.track);
		sidebar_interpolation->select(interpolation == Animation::INTERPOLATION_NEAREST ? 0 : (interpolation == Animation::INTERPOLATION_LINEAR || interpolation == Animation::INTERPOLATION_LINEAR_ANGLE ? 1 : 2));
		const real_t transition = animation->track_get_key_transition(active.track, key);
		sidebar_easing->select(_easing_of_transition(transition));
		sidebar_transition->set_value_no_signal(transition);
	}
	updating_sidebar = false;
}

void AsosdotGraphEditor::_sidebar_changed(double p_value, int p_field) {
	if (updating_sidebar || !has_active || read_only) {
		return;
	}
	const int channel = _channel_of(active.track, active.component);
	const int key = channel >= 0 ? _find_key(active) : -1;
	if (key < 0 || _is_channel_locked(channel)) {
		return;
	}
	_cache_curves();
	HashMap<int, LocalVector<TrackKey>> before;
	before[active.track] = _read_track(active.track);
	HashMap<int, LocalVector<TrackKey>> after(before);
	LocalVector<TrackKey> &keys = after[active.track];
	TrackKey &edited = keys[key];
	Selected moved = active;
	switch (p_field) {
		case FIELD_TIME: {
			const bool frames = timeline->is_using_fps() && animation->get_step() > 0.0;
			const double time = MAX(0.0, frames ? p_value * animation->get_step() : p_value);
			TrackKey taken = edited;
			keys.remove_at(key);
			for (uint32_t k = 0; k < keys.size(); k++) {
				if (Math::abs(keys[k].time - time) < 1e-5) {
					keys.remove_at(k);
					break;
				}
			}
			taken.time = time;
			keys.push_back(taken);
			_sort_keys(keys);
			moved.time = time;
		} break;
		case FIELD_VALUE:
			edited.value = _key_with_value(channel, edited.value, p_value - curve_keys[channel][key].value);
			break;
		case FIELD_TRANSITION:
			edited.transition = p_value;
			break;
		case FIELD_IN_TIME:
			edited.in_handle.x = MIN(p_value, 0.0);
			break;
		case FIELD_IN_VALUE:
			edited.in_handle.y = p_value;
			break;
		case FIELD_OUT_TIME:
			edited.out_handle.x = MAX(p_value, 0.0);
			break;
		case FIELD_OUT_VALUE:
			edited.out_handle.y = p_value;
			break;
	}
	_commit_tracks(TTR("Edit Keyframe"), before, after);
	if (p_field == FIELD_TIME) {
		RBSet<Selected> moved_selection;
		for (const Selected &selected : selection) {
			if (selected.track == active.track && Math::abs(selected.time - active.time) < 1e-5) {
				Selected shifted = selected;
				shifted.time = moved.time;
				moved_selection.insert(shifted);
			} else {
				moved_selection.insert(selected);
			}
		}
		selection = moved_selection;
		active = moved;
	}
	_update_sidebar();
}

void AsosdotGraphEditor::_sidebar_option_changed(int p_index, int p_field) {
	if (updating_sidebar || !has_active || read_only) {
		return;
	}
	const int key = _find_key(active);
	if (key < 0) {
		return;
	}
	if (p_field == FIELD_INTERPOLATION) {
		const Animation::InterpolationType type = p_index == 0 ? Animation::INTERPOLATION_NEAREST : (p_index == 1 ? Animation::INTERPOLATION_LINEAR : Animation::INTERPOLATION_CUBIC);
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Set Keyframe Interpolation"), UndoRedo::MERGE_DISABLE, animation.ptr());
		undo_redo->add_do_method(animation.ptr(), "track_set_interpolation_type", active.track, type);
		undo_redo->add_undo_method(animation.ptr(), "track_set_interpolation_type", active.track, animation->track_get_interpolation_type(active.track));
		undo_redo->commit_action();
	} else if (p_field == FIELD_EASING) {
		if (p_index < 5) {
			_sidebar_changed(_easing_transition(EASING_LINEAR + p_index), FIELD_TRANSITION);
		}
	} else if (p_field == FIELD_HANDLE_MODE) {
		HashMap<int, LocalVector<TrackKey>> before;
		before[active.track] = _read_track(active.track);
		HashMap<int, LocalVector<TrackKey>> after(before);
		after[active.track][key].handle_mode = p_index;
		_commit_tracks(TTR("Set Keyframe Handle Type"), before, after);
	}
	_update_sidebar();
}

/// Drawing.

void AsosdotGraphEditor::_draw_view() {
	const Size2 size = view->get_size();
	const float list_width = _list_width();
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const Color background = get_theme_color(SNAME("dark_color_2"), EditorStringName(Editor));
	const Color font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
	view->draw_rect(Rect2(list_width, 0, size.x - list_width, size.y), background);

	if (animation.is_null() || !timeline) {
		view->draw_string(font, Point2(list_width + 12 * EDSCALE, size.y * 0.5), TTR("Pick an animation to see its curves."), HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, font_color * Color(1, 1, 1, 0.6));
		return;
	}
	_update_channels();
	_cache_curves();
	// Samples hold for this zoom, scroll and width; changing any of them samples the curves again.
	uint32_t view_hash = hash_murmur3_one_double(timeline->get_value());
	view_hash = hash_murmur3_one_double(timeline->get_zoom_scale(), view_hash);
	view_hash = hash_murmur3_one_float(size.x, view_hash);
	view_hash = hash_murmur3_one_float(list_width, view_hash);
	if (view_hash != samples_view_hash) {
		samples_view_hash = view_hash;
		track_samples.clear();
	}
	if (!framed_once && !channels.is_empty() && size.y > 20) {
		// The first time the curves show, their values are fitted in; time stays as the timeline has it.
		framed_once = true;
		callable_mp(this, &AsosdotGraphEditor::_frame).call_deferred(false, false);
	}
	// The timeline's zoom and name column are worked out each time they're asked for, so a draw
	// asks once; the curves have thousands of points.
	const float zoom = timeline->get_zoom_scale();
	const double time_origin = timeline->get_value();
	auto time_to_x = [&](double p_time) -> float {
		return (p_time - time_origin) * zoom + list_width;
	};

	// Outside the animation's length, darker.
	const Color outside = Color(0, 0, 0, 0.25);
	const float start_x = time_to_x(0.0);
	const float end_x = time_to_x(animation->get_length());
	if (start_x > list_width) {
		view->draw_rect(Rect2(list_width, 0, start_x - list_width, size.y), outside);
	}
	if (end_x < size.x) {
		view->draw_rect(Rect2(MAX(end_x, list_width), 0, size.x - MAX(end_x, list_width), size.y), outside);
	}

	// Time grid, on frames when the animation has a step.
	const Color grid_minor = font_color * Color(1, 1, 1, 0.05);
	const Color grid_major = font_color * Color(1, 1, 1, 0.1);
	double time_step = _nice_step(60.0 * EDSCALE / zoom);
	const double frame = animation->get_step();
	if (frame > 0.0) {
		time_step = MAX(frame, Math::round(time_step / frame) * frame);
	}
	for (double t = Math::floor(_x_to_time(list_width) / time_step) * time_step; t <= _x_to_time(size.x); t += time_step) {
		const float x = Math::round(time_to_x(t)) + 0.5f;
		if (x >= list_width) {
			view->draw_line(Point2(x, 0), Point2(x, size.y), grid_minor);
		}
	}

	// Value grid with its numbers, at round steps.
	const double value_step = _nice_step(36.0 * EDSCALE * value_per_pixel);
	const int decimals = MAX(0, int(-Math::floor(Math::log(value_step) / Math::log(10.0))));
	const double top = value_center + size.y * 0.5 * value_per_pixel;
	const double bottom = value_center - size.y * 0.5 * value_per_pixel;
	for (double v = Math::floor(bottom / value_step) * value_step; v <= top; v += value_step) {
		const float y = Math::round(size.y * 0.5 - (v - value_center) / value_per_pixel) + 0.5f;
		view->draw_line(Point2(list_width, y), Point2(size.x, y), Math::is_zero_approx(v) ? grid_major : grid_minor);
		view->draw_string(font, Point2(list_width + 4 * EDSCALE, y - 2 * EDSCALE), String::num(v, decimals), HORIZONTAL_ALIGNMENT_LEFT, -1, int(font_size * 0.85), font_color * Color(1, 1, 1, 0.45));
	}

	// Curves: the unselected first, dimmer, then the selected over them.
	const bool any_selected = !selected_channels.is_empty();
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < channels.size(); i++) {
			if (!_is_channel_shown(i) || selected_channels.has(i) != (pass == 1)) {
				continue;
			}
			if (curve_keys[i].is_empty()) {
				continue;
			}
			const TrackSamples &samples = _get_samples(channels[i].track);
			const LocalVector<double> &values = samples.values[channels[i].component];
			PackedVector2Array points;
			points.resize(MIN(samples.times.size(), values.size()));
			Vector2 *point = points.ptrw();
			for (int j = 0; j < points.size(); j++) {
				point[j] = Vector2(time_to_x(samples.times[j]), CLAMP(_value_to_y(i, values[j]), -size.y, size.y * 2));
			}
			Color color = channels[i].color;
			float width = 1.5f * EDSCALE;
			if (pass == 1) {
				width = 2.2f * EDSCALE;
			} else if (any_selected) {
				color.a = 0.45f;
			} else {
				color.a = 0.85f;
			}
			if (_is_channel_locked(i)) {
				color.a *= 0.6f;
			}
			_simplify_polyline(points, 0.35f);
			if (points.size() > 1) {
				view->draw_polyline(points, color, width, true);
			}
		}
	}

	// Handles of Bezier keys: those selected, or all with Show Handles on.
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i) || animation->track_get_type(channels[i].track) != Animation::TYPE_BEZIER) {
			continue;
		}
		const double scale = _value_per_pixel_for(i);
		for (const CurveKey &key : curve_keys[i]) {
			Selected probe;
			probe.track = channels[i].track;
			probe.component = channels[i].component;
			probe.time = key.time;
			const bool selected = selection.has(probe);
			if (!selected && !handles_button->is_pressed()) {
				continue;
			}
			const Vector2 point(time_to_x(key.time), _value_to_y(i, key.value));
			if (point.x < list_width - 50 || point.x > size.x + 50) {
				continue;
			}
			const int handle_mode = animation->bezier_track_get_key_handle_mode(channels[i].track, key.index);
			Color color = HANDLE_COLORS[CLAMP(handle_mode, 0, 3)];
			color.a = selected ? 1.0f : 0.5f;
			const Vector2 in_point = point + Vector2(key.in_handle.x * zoom, -key.in_handle.y / scale);
			const Vector2 out_point = point + Vector2(key.out_handle.x * zoom, -key.out_handle.y / scale);
			view->draw_line(point, in_point, color, 1.0f * EDSCALE, true);
			view->draw_line(point, out_point, color, 1.0f * EDSCALE, true);
			view->draw_circle(in_point, 3.0f * EDSCALE, color);
			view->draw_circle(out_point, 3.0f * EDSCALE, color);
		}
	}

	// Keys: black when not selected, orange when selected, white for the active one, as in Blender.
	const float radius = KEY_RADIUS * EDSCALE;
	const Ref<Texture2D> key_dot = _get_key_texture();
	auto dot = [&](const Vector2 &p_center, float p_radius, const Color &p_color) {
		view->draw_texture_rect(key_dot, Rect2(p_center - Vector2(p_radius, p_radius), Vector2(p_radius, p_radius) * 2), false, p_color);
	};
	for (uint32_t i = 0; i < channels.size(); i++) {
		if (!_is_channel_shown(i)) {
			continue;
		}
		for (const CurveKey &key : curve_keys[i]) {
			const Vector2 point(time_to_x(key.time), _value_to_y(i, key.value));
			if (point.x < list_width - radius || point.x > size.x + radius || point.y < -radius || point.y > size.y + radius) {
				continue;
			}
			Selected probe;
			probe.track = channels[i].track;
			probe.component = channels[i].component;
			probe.time = key.time;
			const bool selected = selection.has(probe);
			const bool is_active = selected && has_active && active == probe;
			if (is_active) {
				dot(point, radius + 1.5f * EDSCALE, SELECTED_KEY_COLOR);
				dot(point, radius, Color(1, 1, 1));
			} else if (selected) {
				dot(point, radius + 1.0f * EDSCALE, Color(0, 0, 0, 0.6));
				dot(point, radius, SELECTED_KEY_COLOR);
			} else {
				dot(point, radius + 1.0f * EDSCALE, Color(0.85, 0.85, 0.85, 0.8));
				dot(point, radius, Color(0.06, 0.06, 0.06));
			}
		}
	}

	if (mode == MODE_BOX_SELECT) {
		const Rect2 rect = Rect2(drag_from, drag_to - drag_from).abs();
		view->draw_rect(rect, get_theme_color(SNAME("box_selection_fill_color"), EditorStringName(Editor)));
		view->draw_rect(rect, get_theme_color(SNAME("box_selection_stroke_color"), EditorStringName(Editor)), false, EDSCALE);
	}
	if (mode == MODE_SCALE) {
		view->draw_line(scale_pivot, view->get_local_mouse_position(), font_color * Color(1, 1, 1, 0.5), EDSCALE, true);
	}

	if (channels.is_empty()) {
		const String hint = only_selected_button->is_pressed() ? TTR("No animated numbers on the selected nodes. Turn off Only Selected to see every curve.") : TTR("This animation has no curves yet: position, rotation, scale, blend shape, number and Bezier tracks show here.");
		view->draw_string(font, Point2(list_width + 12 * EDSCALE, size.y * 0.5), hint, HORIZONTAL_ALIGNMENT_LEFT, size.x - list_width - 24 * EDSCALE, font_size, font_color * Color(1, 1, 1, 0.6));
	}

	_draw_channel_list();
}

// A soft white dot, which every key is drawn with, tinted: a quad each, drawn together, where
// a circle each would be a polygon of its own.
Ref<Texture2D> AsosdotGraphEditor::_get_key_texture() {
	if (key_texture.is_valid()) {
		return key_texture;
	}
	const int size = 32;
	Ref<Image> image = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
	const float center = (size - 1) * 0.5f;
	const float circle = size * 0.5f - 1.0f;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			const float distance = Vector2(x - center, y - center).length();
			image->set_pixel(x, y, Color(1, 1, 1, CLAMP(circle - distance + 0.5f, 0.0f, 1.0f)));
		}
	}
	key_texture = ImageTexture::create_from_image(image);
	return key_texture;
}

void AsosdotGraphEditor::_draw_playhead() {
	if (animation.is_null() || !timeline) {
		return;
	}
	const float x = _time_to_x(play_position);
	if (x >= _list_width() && x <= view->get_size().x) {
		playhead_overlay->draw_line(Point2(x, 0), Point2(x, view->get_size().y), PLAYHEAD_COLOR, 2.0f * EDSCALE);
	}
}

void AsosdotGraphEditor::_draw_channel_list() {
	const Size2 size = view->get_size();
	const float list_width = _list_width();
	const float row = _row_height();
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const Color font_color = get_theme_color(SceneStringName(font_color), SNAME("Label"));
	const Color background = get_theme_color(SNAME("base_color"), EditorStringName(Editor));
	const Color header_color = get_theme_color(SNAME("dark_color_1"), EditorStringName(Editor));
	const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
	view->draw_rect(Rect2(0, 0, list_width, size.y), background);

	_layout_rows();
	const Ref<Texture2D> visible_icon = get_editor_theme_icon(SNAME("GuiVisibilityVisible"));
	const Ref<Texture2D> hidden_icon = get_editor_theme_icon(SNAME("GuiVisibilityHidden"));
	const Ref<Texture2D> lock_icon = get_editor_theme_icon(SNAME("Lock"));
	const Ref<Texture2D> unlock_icon = get_editor_theme_icon(SNAME("Unlock"));
	const Ref<Texture2D> open_icon = get_editor_theme_icon(SNAME("GuiTreeArrowDown"));
	const Ref<Texture2D> closed_icon = get_editor_theme_icon(SNAME("GuiTreeArrowRight"));
	const float icon_size = MIN(row - 4 * EDSCALE, 16 * EDSCALE);
	for (const Row &entry : rows) {
		if (entry.y + row < 0 || entry.y > size.y) {
			continue;
		}
		const float text_y = entry.y + (row - font->get_height(font_size)) * 0.5 + font->get_ascent(font_size);
		const float icon_y = entry.y + (row - icon_size) * 0.5;
		const Group &group = groups[entry.group];
		if (entry.channel < 0) {
			view->draw_rect(Rect2(0, entry.y, list_width, row), header_color);
			const Ref<Texture2D> arrow = collapsed_groups.has(group.key) ? closed_icon : open_icon;
			view->draw_texture_rect(arrow, Rect2(2 * EDSCALE, icon_y, icon_size, icon_size));
			if (group.icon.is_valid()) {
				view->draw_texture_rect(group.icon, Rect2(4 * EDSCALE + icon_size, icon_y, icon_size, icon_size));
			}
			view->draw_string(font, Point2(8 * EDSCALE + icon_size * 2, text_y), group.label, HORIZONTAL_ALIGNMENT_LEFT, list_width - icon_size * 2 - 12 * EDSCALE, font_size, font_color);
			continue;
		}
		const Channel &channel = channels[entry.channel];
		if (selected_channels.has(entry.channel)) {
			view->draw_rect(Rect2(0, entry.y, list_width, row), accent * Color(1, 1, 1, 0.22));
		}
		const bool shown = _is_channel_shown(entry.channel);
		const bool locked = locked_channels.has(_channel_id(entry.channel));
		view->draw_rect(Rect2(14 * EDSCALE, entry.y + row * 0.5 - 4 * EDSCALE, 8 * EDSCALE, 8 * EDSCALE), shown ? channel.color : channel.color * Color(1, 1, 1, 0.3));
		const float buttons = icon_size * 2 + 10 * EDSCALE;
		view->draw_string(font, Point2(28 * EDSCALE, text_y), channel.label, HORIZONTAL_ALIGNMENT_LEFT, list_width - 32 * EDSCALE - buttons, font_size, shown ? font_color : font_color * Color(1, 1, 1, 0.45));
		view->draw_texture_rect(shown ? visible_icon : hidden_icon, Rect2(list_width - buttons + 2 * EDSCALE, icon_y, icon_size, icon_size), false, Color(1, 1, 1, shown ? 0.85 : 0.4));
		view->draw_texture_rect(locked ? lock_icon : unlock_icon, Rect2(list_width - icon_size - 4 * EDSCALE, icon_y, icon_size, icon_size), false, Color(1, 1, 1, locked ? 0.9 : 0.3));
	}
	view->draw_line(Point2(list_width - 0.5f, 0), Point2(list_width - 0.5f, size.y), Color(0, 0, 0, 0.4));
}

/// Input.

bool AsosdotGraphEditor::_handle_shortcut(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed()) {
		return false;
	}

	if (mode == MODE_GRAB || mode == MODE_SCALE) {
		// While moving or scaling, as in Blender: X and Y lock to an axis, Esc cancels, Enter confirms.
		if (key->get_keycode() == Key::X && !key->is_echo()) {
			axis = axis == AXIS_TIME ? AXIS_BOTH : AXIS_TIME;
			_update_edit(view->get_local_mouse_position());
			_update_status();
			return true;
		}
		if (key->get_keycode() == Key::Y && !key->is_echo()) {
			axis = axis == AXIS_VALUE ? AXIS_BOTH : AXIS_VALUE;
			_update_edit(view->get_local_mouse_position());
			_update_status();
			return true;
		}
		if (key->get_keycode() == Key::ESCAPE) {
			_finish_edit(false);
			return true;
		}
		if (key->get_keycode() == Key::ENTER || key->get_keycode() == Key::KP_ENTER || key->get_keycode() == Key::SPACE) {
			_finish_edit(true);
			return true;
		}
		return true; // Other keys wait until the move is done.
	}
	if (key->is_echo()) {
		return false;
	}
	if (key->get_keycode() == Key::ESCAPE && box_pending) {
		box_pending = false;
		_update_status();
		return true;
	}
	if (key->get_keycode() == Key::KEY_DELETE && !key->is_command_or_control_pressed()) {
		_menu_option(KEY_DELETE);
		return true;
	}

	struct Binding {
		const char *shortcut;
		int option;
	};
	static const Binding bindings[] = {
		{ "graph_editor/frame_all", VIEW_FRAME_ALL },
		{ "graph_editor/frame_selected", VIEW_FRAME_SELECTED },
		{ "graph_editor/toggle_handles", VIEW_SHOW_HANDLES },
		{ "graph_editor/sidebar", VIEW_SIDEBAR },
		{ "graph_editor/toggle_dope_sheet", VIEW_DOPE_SHEET },
		{ "graph_editor/select_all", SELECT_ALL },
		{ "graph_editor/select_none", SELECT_NONE },
		{ "graph_editor/select_invert", SELECT_INVERT },
		{ "graph_editor/box_select", SELECT_BOX },
		{ "graph_editor/select_columns", SELECT_COLUMNS },
		{ "graph_editor/select_before", SELECT_BEFORE_CURRENT },
		{ "graph_editor/select_after", SELECT_AFTER_CURRENT },
		{ "graph_editor/hide", CHANNEL_HIDE },
		{ "graph_editor/hide_unselected", CHANNEL_HIDE_UNSELECTED },
		{ "graph_editor/reveal", CHANNEL_REVEAL },
		{ "graph_editor/insert_key", KEY_INSERT },
		{ "graph_editor/delete", KEY_DELETE },
		{ "graph_editor/duplicate", KEY_DUPLICATE },
		{ "graph_editor/copy", KEY_COPY },
		{ "graph_editor/paste", KEY_PASTE },
		{ "graph_editor/grab", KEY_GRAB },
		{ "graph_editor/scale", KEY_SCALE },
		{ "graph_editor/jump_next", KEY_JUMP_NEXT },
		{ "graph_editor/jump_previous", KEY_JUMP_PREVIOUS },
	};
	for (const Binding &binding : bindings) {
		if (ED_IS_SHORTCUT(binding.shortcut, p_event)) {
			_menu_option(binding.option);
			return true;
		}
	}
	// The menus Blender opens under the mouse.
	if (ED_IS_SHORTCUT("graph_editor/interpolation", p_event)) {
		_popup_at_mouse(interpolation_menu);
		return true;
	}
	if (ED_IS_SHORTCUT("graph_editor/easing", p_event)) {
		_popup_at_mouse(easing_menu);
		return true;
	}
	if (ED_IS_SHORTCUT("graph_editor/handle_type", p_event)) {
		_popup_at_mouse(handle_menu);
		return true;
	}
	if (ED_IS_SHORTCUT("graph_editor/snap", p_event)) {
		_popup_at_mouse(snap_menu);
		return true;
	}
	// Godot's own key for duplicating, alongside Blender's.
	if (ED_IS_SHORTCUT("animation_editor/duplicate_selected_keys", p_event)) {
		_menu_option(KEY_DUPLICATE);
		return true;
	}
	return false;
}

void AsosdotGraphEditor::_view_gui_input(const Ref<InputEvent> &p_event) {
	if (animation.is_null() || !timeline) {
		return;
	}
	if (_handle_shortcut(p_event)) {
		view->accept_event();
		return;
	}
	const float list_width = _list_width();
	const Ref<InputEventMouseButton> mb = p_event;
	const Ref<InputEventMouseMotion> mm = p_event;

	if (mb.is_valid() && mb->is_pressed()) {
		view->grab_focus();
	}

	// The channel list.
	if (mb.is_valid() && mb->get_position().x < list_width && mode == MODE_NONE) {
		if (mb->get_button_index() == MouseButton::WHEEL_UP || mb->get_button_index() == MouseButton::WHEEL_DOWN) {
			channel_scroll = CLAMP(channel_scroll + (mb->get_button_index() == MouseButton::WHEEL_DOWN ? 1 : -1) * _row_height() * 2, 0.0f, channel_scroll_max);
			view->queue_redraw();
			view->accept_event();
			return;
		}
		if (!mb->is_pressed() || mb->get_button_index() != MouseButton::LEFT) {
			return;
		}
		_layout_rows();
		const int row_index = _channel_at(mb->get_position());
		if (row_index < 0) {
			if (!mb->is_shift_pressed()) {
				selected_channels.clear();
			}
			view->queue_redraw();
			return;
		}
		const Row &entry = rows[row_index];
		const float icon_size = MIN(_row_height() - 4 * EDSCALE, 16 * EDSCALE);
		const float buttons = icon_size * 2 + 10 * EDSCALE;
		if (entry.channel < 0) {
			const Group &group = groups[entry.group];
			if (mb->get_position().x < 4 * EDSCALE + icon_size) {
				if (collapsed_groups.has(group.key)) {
					collapsed_groups.erase(group.key);
				} else {
					collapsed_groups.insert(group.key);
				}
			} else {
				if (!mb->is_shift_pressed()) {
					selected_channels.clear();
				}
				for (int channel : group.channels) {
					selected_channels.insert(channel);
				}
				if (mb->is_double_click()) {
					for (int channel : group.channels) {
						select_channel_keys(channel);
					}
				}
			}
		} else if (mb->get_position().x >= list_width - buttons && mb->get_position().x < list_width - icon_size - 6 * EDSCALE) {
			const String id = _channel_id(entry.channel);
			if (hidden_channels.has(id)) {
				hidden_channels.erase(id);
			} else {
				hidden_channels.insert(id);
			}
		} else if (mb->get_position().x >= list_width - icon_size - 6 * EDSCALE) {
			const String id = _channel_id(entry.channel);
			if (locked_channels.has(id)) {
				locked_channels.erase(id);
			} else {
				locked_channels.insert(id);
			}
		} else if (mb->is_double_click()) {
			// Double-clicking a curve's name selects its keys, as in Blender.
			select_channel_keys(entry.channel);
		} else {
			if (mb->is_shift_pressed() || mb->is_command_or_control_pressed()) {
				if (selected_channels.has(entry.channel)) {
					selected_channels.erase(entry.channel);
				} else {
					selected_channels.insert(entry.channel);
				}
			} else {
				selected_channels.clear();
				selected_channels.insert(entry.channel);
			}
		}
		view->queue_redraw();
		view->accept_event();
		return;
	}

	if (mb.is_valid()) {
		const Vector2 position = mb->get_position();
		switch (mb->get_button_index()) {
			case MouseButton::WHEEL_UP:
			case MouseButton::WHEEL_DOWN: {
				if (!mb->is_pressed()) {
					break;
				}
				const float factor = mb->get_button_index() == MouseButton::WHEEL_UP ? 1.1f : 1.0f / 1.1f;
				if (mb->is_command_or_control_pressed()) {
					// Ctrl zooms the values, about the mouse.
					const double under = value_center + (view->get_size().y * 0.5 - position.y) * value_per_pixel;
					value_per_pixel /= factor;
					value_center = under - (view->get_size().y * 0.5 - position.y) * value_per_pixel;
				} else if (mb->is_shift_pressed()) {
					value_center += (mb->get_button_index() == MouseButton::WHEEL_UP ? 1 : -1) * 40 * EDSCALE * value_per_pixel;
				} else {
					timeline->_zoom_callback(factor, Vector2(position.x, 0), p_event);
				}
				view->queue_redraw();
				view->accept_event();
			} break;
			case MouseButton::MIDDLE: {
				if (mb->is_pressed()) {
					mode = MODE_PAN;
					drag_from = position;
				} else if (mode == MODE_PAN) {
					mode = MODE_NONE;
				}
				view->accept_event();
			} break;
			case MouseButton::RIGHT: {
				if (!mb->is_pressed()) {
					break;
				}
				if (mode == MODE_GRAB || mode == MODE_SCALE) {
					_finish_edit(false);
				} else if (mb->is_command_or_control_pressed() && !read_only) {
					// Ctrl+right-click puts a key on the curve under the mouse, or the active one.
					int channel = _curve_at(position);
					if (channel < 0 && has_active) {
						channel = _channel_of(active.track, active.component);
					}
					if (channel >= 0 && !_is_channel_locked(channel)) {
						_insert_key_at(channel, _x_to_time(position.x), _y_to_value(channel, position.y));
					}
				} else {
					_popup_at_mouse(key_menu->get_popup());
				}
				view->accept_event();
			} break;
			case MouseButton::LEFT: {
				if (mb->is_pressed()) {
					if (mode == MODE_GRAB || mode == MODE_SCALE) {
						_finish_edit(true);
						view->accept_event();
						break;
					}
					drag_from = position;
					drag_to = position;
					if (box_pending) {
						box_pending = false;
						mode = MODE_BOX_SELECT;
						view->accept_event();
						break;
					}
					Selected handle;
					int side = 0;
					if (!read_only && _handle_at(position, handle, side)) {
						handle_key = handle;
						handle_side = side;
						edit_original.clear();
						edit_original[handle.track] = _read_track(handle.track);
						edit_changed = false;
						mode = MODE_HANDLE;
						view->accept_event();
						break;
					}
					Selected clicked;
					if (_key_at(position, clicked)) {
						const int channel = _channel_of(clicked.track, clicked.component);
						if (mb->is_shift_pressed()) {
							if (selection.has(clicked) && has_active && active == clicked) {
								selection.erase(clicked);
								has_active = false;
							} else {
								selection.insert(clicked);
								_set_active(clicked);
							}
						} else {
							if (!selection.has(clicked)) {
								selection.clear();
								selection.insert(clicked);
							}
							_set_active(clicked);
							if (!_is_channel_locked(channel)) {
								press_on_key = true;
							}
						}
						if (!mb->is_shift_pressed()) {
							selected_channels.clear();
							selected_channels.insert(channel);
						}
						_update_status();
						view->queue_redraw();
						view->accept_event();
						break;
					}
					// On empty space: a drag box-selects; a click on a curve selects it.
					press_on_empty = true;
					view->accept_event();
				} else {
					if (mode == MODE_HANDLE) {
						if (edit_changed) {
							HashMap<int, LocalVector<TrackKey>> after;
							after[handle_key.track] = _read_track(handle_key.track);
							_write_track(handle_key.track, edit_original[handle_key.track]);
							_commit_tracks(TTR("Move Keyframe Handle"), edit_original, after);
						}
						mode = MODE_NONE;
						edit_original.clear();
						view->queue_redraw();
					} else if (mode == MODE_BOX_SELECT) {
						_box_select(Rect2(drag_from, drag_to - drag_from).abs(), mb->is_shift_pressed(), mb->is_command_or_control_pressed());
						mode = MODE_NONE;
						view->queue_redraw();
					} else if ((mode == MODE_GRAB || mode == MODE_SCALE) && dragging_with_button) {
						_finish_edit(true);
					} else if (press_on_empty) {
						const int curve = _curve_at(position);
						if (!mb->is_shift_pressed()) {
							selection.clear();
							selected_channels.clear();
							has_active = false;
						}
						if (curve >= 0) {
							selected_channels.insert(curve);
						}
						_update_status();
						_update_sidebar();
						view->queue_redraw();
					}
					press_on_key = false;
					press_on_empty = false;
				}
			} break;
			default:
				break;
		}
		return;
	}

	if (mm.is_valid()) {
		const Vector2 position = mm->get_position();
		if (mode == MODE_PAN) {
			const Vector2 delta = position - drag_from;
			drag_from = position;
			timeline->set_value(timeline->get_value() - delta.x / timeline->get_zoom_scale());
			value_center += delta.y * value_per_pixel;
			view->queue_redraw();
		} else if (mode == MODE_GRAB || mode == MODE_SCALE) {
			_update_edit(position);
		} else if (mode == MODE_HANDLE) {
			_drag_handle(position);
		} else if (mode == MODE_BOX_SELECT) {
			drag_to = position;
			view->queue_redraw();
		} else if ((mm->get_button_mask().has_flag(MouseButtonMask::LEFT)) && position.distance_to(drag_from) > 4 * EDSCALE) {
			if (press_on_key) {
				// Dragging a key moves the selection as G would, measured from where it was pressed.
				press_on_key = false;
				const Vector2 pressed_at = drag_from;
				_begin_edit(MODE_GRAB);
				drag_from = pressed_at;
				dragging_with_button = mode == MODE_GRAB;
				_update_edit(position);
			} else if (press_on_empty) {
				press_on_empty = false;
				mode = MODE_BOX_SELECT;
				drag_to = position;
				view->queue_redraw();
			}
		}
	}
}

void AsosdotGraphEditor::_insert_key_at(int p_channel, double p_time, double p_value) {
	const int track = channels[p_channel].track;
	Variant value;
	const double time = MAX(0.0, track_editor ? double(track_editor->snap_time(p_time)) : p_time);
	const double saved_position = play_position;
	play_position = time;
	const bool found = _value_at_playhead(track, value);
	play_position = saved_position;
	if (!found) {
		return;
	}
	HashMap<int, LocalVector<TrackKey>> before;
	before[track] = _read_track(track);
	LocalVector<TrackKey> keys(before[track]);
	for (uint32_t k = 0; k < keys.size(); k++) {
		if (Math::abs(keys[k].time - time) < 1e-5) {
			keys.remove_at(k);
			break;
		}
	}
	const bool bezier = animation->track_get_type(track) == Animation::TYPE_BEZIER;
	TrackKey key;
	key.time = time;
	key.value = bezier ? Variant(double(value)) : value;
	// The curve's value under the mouse goes in the curve clicked; the track's others stay as they are.
	double current = 0.0;
	if (_evaluate(p_channel, time, current)) {
		if (channels[p_channel].rotation) {
			current = Math::rad_to_deg(Basis(Quaternion(key.value).normalized()).get_euler(channels[p_channel].euler_order)[channels[p_channel].component]);
		}
		key.value = _key_with_value(p_channel, key.value, p_value - current);
	}
	keys.push_back(key);
	_sort_keys(keys);
	if (bezier) {
		for (uint32_t k = 0; k < keys.size(); k++) {
			if (Math::abs(keys[k].time - time) < 1e-5) {
				_auto_handles(keys, k);
			}
		}
	}
	HashMap<int, LocalVector<TrackKey>> after;
	after[track] = keys;
	_commit_tracks(TTR("Insert Keyframe"), before, after);
	Selected inserted;
	inserted.track = track;
	inserted.component = channels[p_channel].component;
	inserted.time = time;
	selection.clear();
	selection.insert(inserted);
	_set_active(inserted);
	_update_status();
}

/// Public.

void AsosdotGraphEditor::set_track_editor(AnimationTrackEditor *p_editor) {
	track_editor = p_editor;
}

void AsosdotGraphEditor::set_timeline(AnimationTimelineEdit *p_timeline) {
	timeline = p_timeline;
}

void AsosdotGraphEditor::set_animation(const Ref<Animation> &p_animation, bool p_read_only) {
	if (mode != MODE_NONE) {
		_finish_edit(false);
		mode = MODE_NONE;
	}
	if (animation != p_animation) {
		selection.clear();
		selected_channels.clear();
		has_active = false;
		framed_once = false;
		if (animation.is_valid() && animation->is_connected(CoreStringName(changed), callable_mp(this, &AsosdotGraphEditor::_on_animation_changed))) {
			animation->disconnect_changed(callable_mp(this, &AsosdotGraphEditor::_on_animation_changed));
		}
	}
	animation = p_animation;
	read_only = p_read_only;
	channels_hash = 0;
	keys_dirty = true;
	track_samples.clear();
	if (animation.is_valid() && !animation->is_connected(CoreStringName(changed), callable_mp(this, &AsosdotGraphEditor::_on_animation_changed))) {
		animation->connect_changed(callable_mp(this, &AsosdotGraphEditor::_on_animation_changed));
	}
	if (is_visible_in_tree()) {
		_update_channels(true);
		_update_sidebar();
	}
	_update_status();
	view->queue_redraw();
}

void AsosdotGraphEditor::set_root(Node *p_root) {
	root = p_root;
	channels_hash = 0;
	view->queue_redraw();
}

void AsosdotGraphEditor::set_play_position(float p_position) {
	play_position = p_position;
	// Only the playhead moves; the curves stay as drawn.
	playhead_overlay->queue_redraw();
}

void AsosdotGraphEditor::update_view() {
	if (is_visible_in_tree()) {
		view->queue_redraw();
		playhead_overlay->queue_redraw();
	}
}

void AsosdotGraphEditor::animation_changed() {
	if (mode == MODE_GRAB || mode == MODE_SCALE || mode == MODE_HANDLE) {
		return; // The edit itself changes the animation.
	}
	if (animation.is_valid() && is_visible_in_tree()) {
		_update_channels();
		_prune_selection();
		_update_sidebar();
		_update_status();
	}
	view->queue_redraw();
}

void AsosdotGraphEditor::frame_all() {
	_frame(false, true);
}

void AsosdotGraphEditor::grab_view_focus() {
	view->grab_focus();
}

void AsosdotGraphEditor::delete_selection() {
	_delete_selection();
}

void AsosdotGraphEditor::duplicate_selection() {
	_duplicate_selection();
}

void AsosdotGraphEditor::copy_selection(bool p_cut) {
	_copy_selection();
	if (p_cut) {
		_delete_selection();
	}
}

void AsosdotGraphEditor::paste() {
	_paste();
}

PackedStringArray AsosdotGraphEditor::get_channel_labels() const {
	PackedStringArray labels;
	for (const Channel &channel : channels) {
		labels.push_back(groups[channel.group].label + "/" + channel.label);
	}
	return labels;
}

int AsosdotGraphEditor::get_selected_key_count() const {
	return selection.size();
}

void AsosdotGraphEditor::select_channel_keys(int p_channel) {
	ERR_FAIL_INDEX(p_channel, int(channels.size()));
	_cache_curves();
	for (const CurveKey &key : curve_keys[p_channel]) {
		Selected selected;
		selected.track = channels[p_channel].track;
		selected.component = channels[p_channel].component;
		selected.time = key.time;
		selection.insert(selected);
		// The first becomes the active key when no selected key is, for the sidebar.
		if (!has_active || !selection.has(active)) {
			_set_active(selected);
		}
	}
	_update_status();
	view->queue_redraw();
}

// Moves the selection as G would, by exactly a time and a value, in one undoable step.
void AsosdotGraphEditor::move_selection(double p_time_offset, double p_value_offset) {
	_begin_edit(MODE_GRAB);
	if (mode != MODE_GRAB) {
		return;
	}
	edit_exact = true;
	edit_exact_offset = Vector2(p_time_offset, p_value_offset);
	_update_edit(drag_from);
	edit_exact = false;
	_finish_edit(true);
}

void AsosdotGraphEditor::apply_menu_option(int p_option) {
	_menu_option(p_option);
}

// Where a key is drawn in the view, for driving the view with mouse events in checks.
Vector2 AsosdotGraphEditor::get_key_point(int p_channel, int p_key) {
	_update_channels();
	_cache_curves();
	ERR_FAIL_INDEX_V(p_channel, int(channels.size()), Vector2());
	ERR_FAIL_INDEX_V(p_key, int(curve_keys[p_channel].size()), Vector2());
	const CurveKey &key = curve_keys[p_channel][p_key];
	return Vector2(_time_to_x(key.time), _value_to_y(p_channel, key.value));
}

void AsosdotGraphEditor::send_view_input(const Ref<InputEvent> &p_event) {
	_view_gui_input(p_event);
}

void AsosdotGraphEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			only_selected_button->set_button_icon(get_editor_theme_icon(SNAME("AnimationFilter")));
			normalize_button->set_button_icon(get_editor_theme_icon(SNAME("CurveLinear")));
			handles_button->set_button_icon(get_editor_theme_icon(SNAME("BezierHandlesBalanced")));
			sidebar_button->set_button_icon(get_editor_theme_icon(SNAME("Panels2")));
			PopupMenu *popup = key_menu->get_popup();
			popup->set_item_icon(popup->get_item_index(INTERPOLATION_CONSTANT), get_editor_theme_icon(SNAME("CurveConstant")));
			popup->set_item_icon(popup->get_item_index(INTERPOLATION_LINEAR), get_editor_theme_icon(SNAME("CurveLinear")));
			popup->set_item_icon(popup->get_item_index(INTERPOLATION_BEZIER), get_editor_theme_icon(SNAME("CurveInOut")));
			popup->set_item_icon(popup->get_item_index(EASING_IN), get_editor_theme_icon(SNAME("CurveIn")));
			popup->set_item_icon(popup->get_item_index(EASING_OUT), get_editor_theme_icon(SNAME("CurveOut")));
			popup->set_item_icon(popup->get_item_index(EASING_IN_OUT), get_editor_theme_icon(SNAME("CurveInOut")));
			popup->set_item_icon(popup->get_item_index(EASING_OUT_IN), get_editor_theme_icon(SNAME("CurveOutIn")));
			popup->set_item_icon(popup->get_item_index(HANDLE_VECTOR), get_editor_theme_icon(SNAME("BezierHandlesLinear")));
			popup->set_item_icon(popup->get_item_index(HANDLE_ALIGNED), get_editor_theme_icon(SNAME("BezierHandlesBalanced")));
			popup->set_item_icon(popup->get_item_index(HANDLE_FREE), get_editor_theme_icon(SNAME("BezierHandlesFree")));
			popup->set_item_icon(popup->get_item_index(HANDLE_MIRRORED), get_editor_theme_icon(SNAME("BezierHandlesMirror")));
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible_in_tree() && animation.is_valid()) {
				_update_channels(true);
				_update_sidebar();
				view->queue_redraw();
			} else if (mode != MODE_NONE) {
				_finish_edit(false);
				mode = MODE_NONE;
			}
		} break;

		case NOTIFICATION_PROCESS: {
			// Status messages fade after a while; and the only-selected list follows the selection.
			if (!status_message.is_empty() && OS::get_singleton()->get_ticks_msec() >= status_message_until) {
				status_message = String();
				_update_status();
			}
			if (only_selected_button->is_pressed() && animation.is_valid()) {
				const uint32_t before = channels_hash;
				_update_channels();
				if (before != channels_hash) {
					view->queue_redraw();
				}
			}
		} break;
	}
}

void AsosdotGraphEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_track_keys", "animation", "track", "keys"), &AsosdotGraphEditor::_set_track_keys);
	// For the editor's own checks; the class is internal.
	ClassDB::bind_method(D_METHOD("get_channel_labels"), &AsosdotGraphEditor::get_channel_labels);
	ClassDB::bind_method(D_METHOD("get_selected_key_count"), &AsosdotGraphEditor::get_selected_key_count);
	ClassDB::bind_method(D_METHOD("select_channel_keys", "channel"), &AsosdotGraphEditor::select_channel_keys);
	ClassDB::bind_method(D_METHOD("move_selection", "time_offset", "value_offset"), &AsosdotGraphEditor::move_selection);
	ClassDB::bind_method(D_METHOD("apply_menu_option", "option"), &AsosdotGraphEditor::apply_menu_option);
	ClassDB::bind_method(D_METHOD("frame_all"), &AsosdotGraphEditor::frame_all);
	ClassDB::bind_method(D_METHOD("grab_view_focus"), &AsosdotGraphEditor::grab_view_focus);
	ClassDB::bind_method(D_METHOD("get_key_point", "channel", "key"), &AsosdotGraphEditor::get_key_point);
	ClassDB::bind_method(D_METHOD("send_view_input", "event"), &AsosdotGraphEditor::send_view_input);
	ADD_SIGNAL(MethodInfo("timeline_changed", PropertyInfo(Variant::FLOAT, "position"), PropertyInfo(Variant::BOOL, "timeline_only")));
}

static Button *_make_toggle(const String &p_tooltip, bool p_pressed) {
	Button *button = memnew(Button);
	button->set_theme_type_variation("FlatButton");
	button->set_toggle_mode(true);
	button->set_pressed(p_pressed);
	button->set_tooltip_text(p_tooltip);
	return button;
}

static void _add_shortcut_item(PopupMenu *p_menu, const char *p_shortcut, int p_id) {
	p_menu->add_shortcut(ED_GET_SHORTCUT(p_shortcut), p_id);
}

AsosdotGraphEditor::AsosdotGraphEditor() {
	set_name(TTRC("Graph Editor"));
	set_v_size_flags(SIZE_EXPAND_FILL);

	// Blender's keys, which can be changed under Graph Editor in the editor's shortcuts.
	ED_SHORTCUT("graph_editor/frame_all", TTRC("Frame All"), Key::HOME);
	ED_SHORTCUT("graph_editor/frame_selected", TTRC("Frame Selected"), Key::KP_PERIOD);
	ED_SHORTCUT("graph_editor/toggle_handles", TTRC("Show Handles"), KeyModifierMask::CMD_OR_CTRL | Key::H);
	ED_SHORTCUT("graph_editor/sidebar", TTRC("Sidebar"), Key::N);
	ED_SHORTCUT("graph_editor/toggle_dope_sheet", TTRC("Toggle Dope Sheet"), KeyModifierMask::CTRL | Key::TAB);
	ED_SHORTCUT("graph_editor/select_all", TTRC("Select All"), Key::A);
	ED_SHORTCUT("graph_editor/select_none", TTRC("Select None"), KeyModifierMask::ALT | Key::A);
	ED_SHORTCUT("graph_editor/select_invert", TTRC("Invert Selection"), KeyModifierMask::CMD_OR_CTRL | Key::I);
	ED_SHORTCUT("graph_editor/box_select", TTRC("Box Select"), Key::B);
	ED_SHORTCUT("graph_editor/select_columns", TTRC("Columns on Selected Keys"), Key::K);
	ED_SHORTCUT("graph_editor/select_before", TTRC("Before Current Frame"), Key::BRACKETLEFT);
	ED_SHORTCUT("graph_editor/select_after", TTRC("After Current Frame"), Key::BRACKETRIGHT);
	ED_SHORTCUT("graph_editor/hide", TTRC("Hide Selected Curves"), Key::H);
	ED_SHORTCUT("graph_editor/hide_unselected", TTRC("Hide Unselected Curves"), KeyModifierMask::SHIFT | Key::H);
	ED_SHORTCUT("graph_editor/reveal", TTRC("Reveal Curves"), KeyModifierMask::ALT | Key::H);
	ED_SHORTCUT("graph_editor/insert_key", TTRC("Insert Keyframes"), Key::I);
	ED_SHORTCUT("graph_editor/delete", TTRC("Delete Keyframes"), Key::X);
	ED_SHORTCUT("graph_editor/duplicate", TTRC("Duplicate"), KeyModifierMask::SHIFT | Key::D);
	ED_SHORTCUT("graph_editor/copy", TTRC("Copy"), KeyModifierMask::CMD_OR_CTRL | Key::C);
	ED_SHORTCUT("graph_editor/paste", TTRC("Paste"), KeyModifierMask::CMD_OR_CTRL | Key::V);
	ED_SHORTCUT("graph_editor/grab", TTRC("Move"), Key::G);
	ED_SHORTCUT("graph_editor/scale", TTRC("Scale"), Key::S);
	ED_SHORTCUT("graph_editor/interpolation", TTRC("Interpolation Mode"), Key::T);
	ED_SHORTCUT("graph_editor/easing", TTRC("Easing Mode"), KeyModifierMask::CMD_OR_CTRL | Key::E);
	ED_SHORTCUT("graph_editor/handle_type", TTRC("Handle Type"), Key::V);
	ED_SHORTCUT("graph_editor/snap", TTRC("Snap"), KeyModifierMask::SHIFT | Key::S);
	ED_SHORTCUT("graph_editor/jump_next", TTRC("Jump to Next Keyframe"), Key::UP);
	ED_SHORTCUT("graph_editor/jump_previous", TTRC("Jump to Previous Keyframe"), Key::DOWN);

	header = memnew(HBoxContainer);
	add_child(header);

	view_menu = memnew(MenuButton);
	view_menu->set_text(TTR("View"));
	view_menu->set_switch_on_hover(true);
	header->add_child(view_menu);
	PopupMenu *popup = view_menu->get_popup();
	_add_shortcut_item(popup, "graph_editor/frame_all", VIEW_FRAME_ALL);
	_add_shortcut_item(popup, "graph_editor/frame_selected", VIEW_FRAME_SELECTED);
	popup->add_separator();
	popup->add_check_shortcut(ED_GET_SHORTCUT("graph_editor/toggle_handles"), VIEW_SHOW_HANDLES);
	popup->add_check_item(TTR("Normalize"), VIEW_NORMALIZE);
	popup->add_check_item(TTR("Only Show Selected"), VIEW_ONLY_SELECTED);
	popup->add_check_shortcut(ED_GET_SHORTCUT("graph_editor/sidebar"), VIEW_SIDEBAR);
	popup->add_separator();
	_add_shortcut_item(popup, "graph_editor/toggle_dope_sheet", VIEW_DOPE_SHEET);
	popup->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));

	select_menu = memnew(MenuButton);
	select_menu->set_text(TTR("Select"));
	select_menu->set_switch_on_hover(true);
	header->add_child(select_menu);
	popup = select_menu->get_popup();
	_add_shortcut_item(popup, "graph_editor/select_all", SELECT_ALL);
	_add_shortcut_item(popup, "graph_editor/select_none", SELECT_NONE);
	_add_shortcut_item(popup, "graph_editor/select_invert", SELECT_INVERT);
	popup->add_separator();
	_add_shortcut_item(popup, "graph_editor/box_select", SELECT_BOX);
	_add_shortcut_item(popup, "graph_editor/select_columns", SELECT_COLUMNS);
	_add_shortcut_item(popup, "graph_editor/select_before", SELECT_BEFORE_CURRENT);
	_add_shortcut_item(popup, "graph_editor/select_after", SELECT_AFTER_CURRENT);
	popup->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));

	channel_menu = memnew(MenuButton);
	channel_menu->set_text(TTR("Channel"));
	channel_menu->set_switch_on_hover(true);
	header->add_child(channel_menu);
	popup = channel_menu->get_popup();
	_add_shortcut_item(popup, "graph_editor/hide", CHANNEL_HIDE);
	_add_shortcut_item(popup, "graph_editor/hide_unselected", CHANNEL_HIDE_UNSELECTED);
	_add_shortcut_item(popup, "graph_editor/reveal", CHANNEL_REVEAL);
	popup->add_separator();
	popup->add_item(TTR("Lock Curves"), CHANNEL_LOCK);
	popup->add_item(TTR("Unlock Curves"), CHANNEL_UNLOCK);
	popup->add_separator();
	popup->add_item(TTR("Expand All"), CHANNEL_EXPAND_ALL);
	popup->add_item(TTR("Collapse All"), CHANNEL_COLLAPSE_ALL);
	popup->add_separator();
	popup->add_item(TTR("Convert to Bezier Curves"), CHANNEL_CONVERT_TO_BEZIER);
	popup->set_item_tooltip(popup->get_item_count() - 1, TTR("Split the selected tracks into a Bezier curve per number, keyed where they were, so each key has handles to shape it."));
	popup->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));

	key_menu = memnew(MenuButton);
	key_menu->set_text(TTR("Key"));
	key_menu->set_switch_on_hover(true);
	header->add_child(key_menu);
	popup = key_menu->get_popup();
	_add_shortcut_item(popup, "graph_editor/insert_key", KEY_INSERT);
	_add_shortcut_item(popup, "graph_editor/grab", KEY_GRAB);
	_add_shortcut_item(popup, "graph_editor/scale", KEY_SCALE);
	_add_shortcut_item(popup, "graph_editor/duplicate", KEY_DUPLICATE);
	_add_shortcut_item(popup, "graph_editor/delete", KEY_DELETE);
	_add_shortcut_item(popup, "graph_editor/copy", KEY_COPY);
	_add_shortcut_item(popup, "graph_editor/paste", KEY_PASTE);
	popup->add_separator(TTR("Interpolation Mode (T)"));
	popup->add_item(TTR("Constant"), INTERPOLATION_CONSTANT);
	popup->add_item(TTR("Linear"), INTERPOLATION_LINEAR);
	popup->add_item(TTR("Bezier"), INTERPOLATION_BEZIER);
	popup->add_separator(TTR("Easing (Ctrl+E)"));
	popup->add_item(TTR("Linear"), EASING_LINEAR);
	popup->add_item(TTR("Ease In"), EASING_IN);
	popup->add_item(TTR("Ease Out"), EASING_OUT);
	popup->add_item(TTR("Ease In and Out"), EASING_IN_OUT);
	popup->add_item(TTR("Ease Out and In"), EASING_OUT_IN);
	popup->add_separator(TTR("Handle Type (V)"));
	popup->add_item(TTR("Auto Clamped"), HANDLE_AUTO_CLAMPED);
	popup->add_item(TTR("Vector"), HANDLE_VECTOR);
	popup->add_item(TTR("Aligned"), HANDLE_ALIGNED);
	popup->add_item(TTR("Free"), HANDLE_FREE);
	popup->add_item(TTR("Mirrored"), HANDLE_MIRRORED);
	popup->add_separator(TTR("Snap (Shift+S)"));
	popup->add_item(TTR("Selection to Current Frame"), KEY_SNAP_TO_CURRENT);
	popup->add_item(TTR("Selection to Nearest Frame"), KEY_SNAP_TO_FRAME);
	popup->add_item(TTR("Current Frame to Selection"), KEY_CURRENT_TO_SELECTED);
	popup->add_separator();
	_add_shortcut_item(popup, "graph_editor/jump_next", KEY_JUMP_NEXT);
	_add_shortcut_item(popup, "graph_editor/jump_previous", KEY_JUMP_PREVIOUS);
	popup->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));

	header->add_child(memnew(VSeparator));
	only_selected_button = _make_toggle(TTR("Only Show Selected: only the curves of the nodes selected in the scene."), false);
	only_selected_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotGraphEditor::_menu_option).bind(-1).unbind(1));
	header->add_child(only_selected_button);
	normalize_button = _make_toggle(TTR("Normalize: draw each curve from -1 to 1 over its own range, to compare their shapes."), false);
	normalize_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotGraphEditor::_frame).bind(false, false).unbind(1));
	header->add_child(normalize_button);
	handles_button = _make_toggle(TTR("Show Handles: the handles of every Bezier key, not only the selected ones (Ctrl+H)."), false);
	handles_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotGraphEditor::_menu_option).bind(-1).unbind(1));
	header->add_child(handles_button);

	status = memnew(Label);
	status->set_h_size_flags(SIZE_EXPAND_FILL);
	status->set_clip_text(true);
	status->set_theme_type_variation("HeaderSmall");
	status->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	header->add_child(status);
	sidebar_button = _make_toggle(TTR("Sidebar: the active keyframe's time, value, interpolation and handles (N)."), false);
	sidebar_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotGraphEditor::_toggle_sidebar));
	header->add_child(sidebar_button);

	// The menus Blender opens under the mouse for T, Ctrl+E, V and Shift+S.
	interpolation_menu = memnew(PopupMenu);
	interpolation_menu->add_separator(TTR("Interpolation Mode"));
	interpolation_menu->add_item(TTR("Constant"), INTERPOLATION_CONSTANT);
	interpolation_menu->add_item(TTR("Linear"), INTERPOLATION_LINEAR);
	interpolation_menu->add_item(TTR("Bezier"), INTERPOLATION_BEZIER);
	interpolation_menu->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));
	add_child(interpolation_menu);
	easing_menu = memnew(PopupMenu);
	easing_menu->add_separator(TTR("Easing"));
	easing_menu->add_item(TTR("Linear"), EASING_LINEAR);
	easing_menu->add_item(TTR("Ease In"), EASING_IN);
	easing_menu->add_item(TTR("Ease Out"), EASING_OUT);
	easing_menu->add_item(TTR("Ease In and Out"), EASING_IN_OUT);
	easing_menu->add_item(TTR("Ease Out and In"), EASING_OUT_IN);
	easing_menu->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));
	add_child(easing_menu);
	handle_menu = memnew(PopupMenu);
	handle_menu->add_separator(TTR("Handle Type"));
	handle_menu->add_item(TTR("Auto Clamped"), HANDLE_AUTO_CLAMPED);
	handle_menu->add_item(TTR("Vector"), HANDLE_VECTOR);
	handle_menu->add_item(TTR("Aligned"), HANDLE_ALIGNED);
	handle_menu->add_item(TTR("Free"), HANDLE_FREE);
	handle_menu->add_item(TTR("Mirrored"), HANDLE_MIRRORED);
	handle_menu->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));
	add_child(handle_menu);
	snap_menu = memnew(PopupMenu);
	snap_menu->add_separator(TTR("Snap"));
	snap_menu->add_item(TTR("Selection to Current Frame"), KEY_SNAP_TO_CURRENT);
	snap_menu->add_item(TTR("Selection to Nearest Frame"), KEY_SNAP_TO_FRAME);
	snap_menu->add_item(TTR("Current Frame to Selection"), KEY_CURRENT_TO_SELECTED);
	snap_menu->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotGraphEditor::_menu_option));
	add_child(snap_menu);

	view = memnew(Control);
	view->set_v_size_flags(SIZE_EXPAND_FILL);
	view->set_clip_contents(true);
	view->set_focus_mode(FOCUS_ALL);
	view->connect(SceneStringName(draw), callable_mp(this, &AsosdotGraphEditor::_draw_view));
	view->connect(SceneStringName(gui_input), callable_mp(this, &AsosdotGraphEditor::_view_gui_input));
	view->connect(SceneStringName(resized), callable_mp((CanvasItem *)view, &CanvasItem::queue_redraw));
	add_child(view);
	playhead_overlay = memnew(Control);
	playhead_overlay->set_mouse_filter(MOUSE_FILTER_IGNORE);
	playhead_overlay->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	playhead_overlay->connect(SceneStringName(draw), callable_mp(this, &AsosdotGraphEditor::_draw_playhead));
	view->add_child(playhead_overlay);
	for (MenuButton *menu : { view_menu, select_menu, channel_menu, key_menu }) {
		menu->set_shortcut_context(view);
	}

	// The N sidebar, over the right of the curves as Blender's is.
	sidebar = memnew(PanelContainer);
	sidebar->set_anchors_and_offsets_preset(PRESET_TOP_RIGHT);
	sidebar->set_h_grow_direction(GROW_DIRECTION_BEGIN);
	sidebar->set_custom_minimum_size(Size2(250 * EDSCALE, 0));
	sidebar->set_offset(SIDE_LEFT, -258 * EDSCALE);
	sidebar->set_offset(SIDE_RIGHT, -8 * EDSCALE);
	sidebar->set_offset(SIDE_TOP, 8 * EDSCALE);
	sidebar->set_theme_type_variation("TooltipPanel");
	sidebar->hide();
	view->add_child(sidebar);
	VBoxContainer *sidebar_box = memnew(VBoxContainer);
	sidebar->add_child(sidebar_box);
	Label *sidebar_title = memnew(Label(TTR("Active Keyframe")));
	sidebar_title->set_theme_type_variation("HeaderSmall");
	sidebar_box->add_child(sidebar_title);
	sidebar_channel = memnew(Label);
	sidebar_channel->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	sidebar_channel->set_custom_minimum_size(Size2(1, 0));
	sidebar_box->add_child(sidebar_channel);
	sidebar_fields = memnew(VBoxContainer);
	sidebar_box->add_child(sidebar_fields);

	auto make_spin = [&](Control *p_parent, const String &p_label, int p_field, double p_step) {
		Label *label = memnew(Label(p_label));
		p_parent->add_child(label);
		SpinBox *spin = memnew(SpinBox);
		spin->set_min(-1e9);
		spin->set_max(1e9);
		spin->set_step(p_step);
		spin->set_allow_greater(true);
		spin->set_allow_lesser(true);
		spin->set_h_size_flags(SIZE_EXPAND_FILL);
		spin->set_select_all_on_focus(true);
		spin->connect(SceneStringName(value_changed), callable_mp(this, &AsosdotGraphEditor::_sidebar_changed).bind(p_field));
		p_parent->add_child(spin);
		return spin;
	};
	GridContainer *key_grid = memnew(GridContainer);
	key_grid->set_columns(2);
	sidebar_fields->add_child(key_grid);
	sidebar_time = make_spin(key_grid, TTR("Time"), FIELD_TIME, 0.001);
	sidebar_value = make_spin(key_grid, TTR("Value"), FIELD_VALUE, 0.001);

	sidebar_curve = memnew(GridContainer);
	Object::cast_to<GridContainer>(sidebar_curve)->set_columns(2);
	sidebar_fields->add_child(sidebar_curve);
	sidebar_curve->add_child(memnew(Label(TTR("Interpolation"))));
	sidebar_interpolation = memnew(OptionButton);
	sidebar_interpolation->add_item(TTR("Constant"));
	sidebar_interpolation->add_item(TTR("Linear"));
	sidebar_interpolation->add_item(TTR("Bezier"));
	sidebar_interpolation->set_tooltip_text(TTR("How the whole track moves between keys; Godot keeps this per track for this kind of track."));
	sidebar_interpolation->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotGraphEditor::_sidebar_option_changed).bind(FIELD_INTERPOLATION));
	sidebar_curve->add_child(sidebar_interpolation);
	sidebar_curve->add_child(memnew(Label(TTR("Easing"))));
	sidebar_easing = memnew(OptionButton);
	sidebar_easing->add_item(TTR("Linear"));
	sidebar_easing->add_item(TTR("Ease In"));
	sidebar_easing->add_item(TTR("Ease Out"));
	sidebar_easing->add_item(TTR("Ease In and Out"));
	sidebar_easing->add_item(TTR("Ease Out and In"));
	sidebar_easing->add_item(TTR("Custom"));
	sidebar_easing->set_tooltip_text(TTR("How the stretch to the next key starts and ends."));
	sidebar_easing->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotGraphEditor::_sidebar_option_changed).bind(FIELD_EASING));
	sidebar_curve->add_child(sidebar_easing);
	sidebar_transition = make_spin(sidebar_curve, TTR("Transition"), FIELD_TRANSITION, 0.01);
	sidebar_transition->set_tooltip_text(TTR("Godot's easing curve exponent: above 1 eases in, between 0 and 1 eases out, below 0 eases both ways."));

	sidebar_bezier = memnew(GridContainer);
	Object::cast_to<GridContainer>(sidebar_bezier)->set_columns(2);
	sidebar_fields->add_child(sidebar_bezier);
	sidebar_in_time = make_spin(sidebar_bezier, TTR("Left Handle Time"), FIELD_IN_TIME, 0.001);
	sidebar_in_value = make_spin(sidebar_bezier, TTR("Left Handle Value"), FIELD_IN_VALUE, 0.001);
	sidebar_out_time = make_spin(sidebar_bezier, TTR("Right Handle Time"), FIELD_OUT_TIME, 0.001);
	sidebar_out_value = make_spin(sidebar_bezier, TTR("Right Handle Value"), FIELD_OUT_VALUE, 0.001);
	sidebar_bezier->add_child(memnew(Label(TTR("Handle Type"))));
	sidebar_handle_mode = memnew(OptionButton);
	sidebar_handle_mode->add_item(TTR("Free"));
	sidebar_handle_mode->add_item(TTR("Vector"));
	sidebar_handle_mode->add_item(TTR("Aligned"));
	sidebar_handle_mode->add_item(TTR("Mirrored"));
	sidebar_handle_mode->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotGraphEditor::_sidebar_option_changed).bind(FIELD_HANDLE_MODE));
	sidebar_bezier->add_child(sidebar_handle_mode);

	set_process(true);
	_update_menus();
}
