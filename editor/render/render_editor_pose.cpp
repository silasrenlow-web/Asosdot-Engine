/**************************************************************************/
/*  render_editor_pose.cpp                                                */
/**************************************************************************/
/*  Asosdot: posing and keyframing in the Render workspace.               */
/**************************************************************************/

#include "render_editor_plugin.h"

#include "core/input/input.h"
#include "core/math/plane.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_toaster.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/slider.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"

namespace {

// How near the cursor a bone has to be to be picked, before editor scale.
constexpr float PICK_DISTANCE = 8.0;
// Radians a pixel of dragging turns by, for free turns and twists.
constexpr float TURN_SPEED = 0.01;
// The player the Render tab keeps clips in when the scene has none of its
// own that can take them.
constexpr const char *OWN_PLAYER_NAME = "RenderAnimation";

void collect_skeletons(Node *p_node, LocalVector<Skeleton3D *> &r_list) {
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_node);
	if (skeleton && skeleton->is_visible_in_tree() && skeleton->get_bone_count() > 0) {
		r_list.push_back(skeleton);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_skeletons(p_node->get_child(i), r_list);
	}
}

// A bone as it's drawn and picked, in the world: its joint, and a line to
// each child's joint, or a stub along its length at the end of a chain.
void bone_lines(Skeleton3D *p_skeleton, int p_bone, Vector3 &r_joint, LocalVector<Vector3> &r_ends) {
	const Transform3D skeleton_xform = p_skeleton->get_global_transform();
	const Transform3D pose = p_skeleton->get_bone_global_pose(p_bone);
	r_joint = skeleton_xform.xform(pose.origin);
	const Vector<int> children = p_skeleton->get_bone_children(p_bone);
	for (int child : children) {
		r_ends.push_back(skeleton_xform.xform(p_skeleton->get_bone_global_pose(child).origin));
	}
	if (children.is_empty()) {
		const int parent = p_skeleton->get_bone_parent(p_bone);
		float length = parent >= 0 ? pose.origin.distance_to(p_skeleton->get_bone_global_pose(parent).origin) * 0.5f : 0.1f;
		length = MAX(length, 0.02f);
		r_ends.push_back(skeleton_xform.xform(pose.origin + pose.basis.get_column(1).normalized() * length));
	}
}

Quaternion global_rotation(Skeleton3D *p_skeleton, int p_bone) {
	return p_bone >= 0 ? p_skeleton->get_bone_global_pose(p_bone).basis.get_rotation_quaternion() : Quaternion();
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                   Modes                                    */
/* -------------------------------------------------------------------------- */

void AsosdotRenderEditor::_set_pose_mode(bool p_enabled) {
	if (pose_mode != p_enabled) {
		_cancel_pose_drag();
		pose_mode = p_enabled;
		pose_hovered = PoseTarget();
		_update_output_label();
		preview_surface->queue_redraw();
	}
	_update_pose_bar();
}

void AsosdotRenderEditor::_set_pose_tool(int p_tool) {
	pose_tool = (PoseTool)CLAMP(p_tool, 0, POSE_TOOL_MAX - 1);
	_update_pose_bar();
}

void AsosdotRenderEditor::_set_auto_key(bool p_enabled) {
	auto_key = p_enabled;
	_update_pose_bar();
}

void AsosdotRenderEditor::_update_pose_bar() {
	pose_scroll->set_visible(pose_mode);
	// The bone and clip read in the status line while posing.
	pose_label->set_visible(pose_mode);
	clip_label->set_visible(pose_mode);
	pose_button->set_pressed_no_signal(pose_mode);
	for (int i = 0; i < POSE_TOOL_MAX; i++) {
		pose_tool_buttons[i]->set_pressed_no_signal(i == pose_tool);
	}
	auto_key_button->set_pressed_no_signal(auto_key);

	Object *object = ObjectDB::get_instance(pose_selected.node);
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object);
	if (pose_selected.bone >= 0 && skeleton && pose_selected.bone < skeleton->get_bone_count()) {
		pose_label->set_text(vformat(TTR("Bone: %s"), skeleton->get_bone_name(pose_selected.bone)));
	} else if (Node *node = Object::cast_to<Node>(object)) {
		pose_label->set_text(vformat(TTR("Part: %s"), node->get_name()));
	} else {
		pose_label->set_text(TTR("Click a bone or a part to pose it."));
	}
	parent_button->set_disabled(object == nullptr);
	reset_pose_button->set_disabled(pose_selected.bone < 0 || !skeleton);

	// Where keys land.
	AnimationPlayer *player = _get_animation_player();
	const StringName chosen = String(shot->get_value("animation"));
	const Ref<Animation> clip = (player && chosen != StringName() && player->has_animation(chosen)) ? player->get_animation(chosen) : Ref<Animation>();
	if (clip.is_valid() && _is_clip_editable(player, clip)) {
		clip_label->set_text(vformat(TTR("Keys go to %s"), chosen));
		clip_label->set_tooltip_text(vformat(TTR("Keys are set in the clip \"%s\" of %s, at the timeline's time."), chosen, player->get_name()));
	} else if (clip.is_valid()) {
		clip_label->set_text(vformat(TTR("Keys go to a copy of %s"), chosen));
		clip_label->set_tooltip_text(vformat(TTR("\"%s\" comes from an imported file, which can't keep changes. The first key makes a copy of it that the scene keeps, and keys go there."), chosen));
	} else {
		clip_label->set_text(TTR("Keys go to a new clip"));
		clip_label->set_tooltip_text(TTR("No clip is chosen under Animation. The first key makes one that the scene keeps."));
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Overlay                                   */
/* -------------------------------------------------------------------------- */

LocalVector<Skeleton3D *> AsosdotRenderEditor::_get_skeletons() const {
	LocalVector<Skeleton3D *> list;
	if (Node *root = _get_scene_root()) {
		collect_skeletons(root, list);
	}
	return list;
}

void AsosdotRenderEditor::_draw_pose_overlay() {
	if (!pose_mode || !active) {
		return;
	}
	const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
	const Color line_color(1, 1, 1, 0.7);
	const Color hover_color(1, 1, 1, 1);
	const Color shadow(0, 0, 0, 0.5);
	const float width = 1.5f * EDSCALE;
	const float joint = 2.5f * EDSCALE;

	// Bones, the selected one last so it stays on top.
	for (int pass = 0; pass < 2; pass++) {
		for (Skeleton3D *skeleton : _get_skeletons()) {
			for (int b = 0; b < skeleton->get_bone_count(); b++) {
				const PoseTarget target = { skeleton->get_instance_id(), b };
				const bool selected = target == pose_selected;
				if (selected != (pass == 1)) {
					continue;
				}
				Vector3 origin;
				LocalVector<Vector3> ends;
				bone_lines(skeleton, b, origin, ends);
				if (preview_camera->is_position_behind(origin)) {
					continue;
				}
				const Point2 from = preview_camera->unproject_position(origin);
				const Color color = selected ? accent : (target == pose_hovered ? hover_color : line_color);
				for (const Vector3 &end : ends) {
					if (preview_camera->is_position_behind(end)) {
						continue;
					}
					const Point2 to = preview_camera->unproject_position(end);
					preview_surface->draw_line(from, to, shadow, width + 2.0f * EDSCALE, true);
					preview_surface->draw_line(from, to, color, selected ? width * 2.0f : width, true);
				}
				preview_surface->draw_circle(from, joint + EDSCALE, shadow);
				preview_surface->draw_circle(from, joint, color);
			}
		}
	}

	// A part being posed: a ring at its origin.
	Node3D *part = pose_selected.bone < 0 ? ObjectDB::get_instance<Node3D>(pose_selected.node) : nullptr;
	if (part && part->is_inside_tree() && !preview_camera->is_position_behind(part->get_global_position())) {
		const Point2 at = preview_camera->unproject_position(part->get_global_position());
		preview_surface->draw_arc(at, 9.0f * EDSCALE, 0, Math::TAU, 32, shadow, 4.0f * EDSCALE, true);
		preview_surface->draw_arc(at, 9.0f * EDSCALE, 0, Math::TAU, 32, accent, 2.0f * EDSCALE, true);
		preview_surface->draw_circle(at, joint, accent);
	}

	// Turning in the view: a line from what turns to the cursor.
	if (pose_drag.kind == DRAG_VIEW_ROTATE && pose_drag.moved) {
		preview_surface->draw_dashed_line(pose_drag.pivot_screen, preview_surface->get_local_mouse_position(), accent, EDSCALE, 4.0f * EDSCALE);
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Picking                                   */
/* -------------------------------------------------------------------------- */

Vector3 AsosdotRenderEditor::_get_target_origin(const PoseTarget &p_target) const {
	Object *object = ObjectDB::get_instance(p_target.node);
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object); skeleton && p_target.bone >= 0) {
		return skeleton->get_global_transform().xform(skeleton->get_bone_global_pose(p_target.bone).origin);
	}
	if (Node3D *node = Object::cast_to<Node3D>(object)) {
		return node->get_global_position();
	}
	return Vector3();
}

Vector3 AsosdotRenderEditor::_get_bone_axis(Skeleton3D *p_skeleton, int p_bone) const {
	// Along the bone: to its only child, or its own length axis, which is Y
	// for bones from Blender.
	const Transform3D skeleton_xform = p_skeleton->get_global_transform();
	const Transform3D pose = p_skeleton->get_bone_global_pose(p_bone);
	const Vector<int> children = p_skeleton->get_bone_children(p_bone);
	Vector3 axis;
	if (children.size() == 1) {
		axis = skeleton_xform.basis.xform(p_skeleton->get_bone_global_pose(children[0]).origin - pose.origin);
	}
	if (axis.length_squared() < 1e-10f) {
		axis = skeleton_xform.basis.xform(pose.basis.get_column(1));
	}
	return axis.length_squared() > 1e-10f ? axis.normalized() : Vector3(0, 1, 0);
}

AsosdotRenderEditor::PoseTarget AsosdotRenderEditor::_pick_bone(const Point2 &p_point, Skeleton3D *p_only, float p_max_distance) const {
	PoseTarget best;
	float best_distance = p_max_distance >= 0.0f ? p_max_distance : PICK_DISTANCE * EDSCALE;
	LocalVector<Skeleton3D *> skeletons;
	if (p_only) {
		skeletons.push_back(p_only);
	} else {
		skeletons = _get_skeletons();
	}
	for (Skeleton3D *skeleton : skeletons) {
		for (int b = 0; b < skeleton->get_bone_count(); b++) {
			Vector3 origin;
			LocalVector<Vector3> ends;
			bone_lines(skeleton, b, origin, ends);
			if (preview_camera->is_position_behind(origin)) {
				continue;
			}
			const Point2 from = preview_camera->unproject_position(origin);
			float distance = p_point.distance_to(from);
			for (const Vector3 &end : ends) {
				if (preview_camera->is_position_behind(end)) {
					continue;
				}
				const Vector2 segment = preview_camera->unproject_position(end) - from;
				const float length_squared = segment.length_squared();
				const float t = length_squared > 0.0f ? CLAMP((p_point - from).dot(segment) / length_squared, 0.0f, 1.0f) : 0.0f;
				// Along a line, a little further from the far end, so a click
				// on a joint takes the bone that starts there.
				distance = MIN(distance, p_point.distance_to(from + segment * t) + t * 2.0f * EDSCALE);
			}
			if (distance < best_distance) {
				best_distance = distance;
				best = { skeleton->get_instance_id(), b };
			}
		}
	}
	return best;
}

void AsosdotRenderEditor::_pose_pick(const Point2 &p_point) {
	PoseTarget target = _pick_bone(p_point);
	if (!target.is_set()) {
		GeometryInstance3D *hit = _raycast_geometry(p_point);
		MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(hit);
		Skeleton3D *skin_skeleton = (mesh && mesh->get_skin().is_valid()) ? Object::cast_to<Skeleton3D>(mesh->get_node_or_null(mesh->get_skeleton_path())) : nullptr;
		if (skin_skeleton) {
			// A skinned mesh is posed through its bones: the one nearest the
			// click.
			target = _pick_bone(p_point, skin_skeleton, Math::INF);
		} else if (hit) {
			target.node = hit->get_instance_id();
		}
	}
	_select_pose_target(target);
}

void AsosdotRenderEditor::_select_pose_target(const PoseTarget &p_target) {
	pose_selected = p_target;
	if (Node *part = p_target.bone < 0 ? ObjectDB::get_instance<Node>(p_target.node) : nullptr) {
		// A part is selected for the Inspector too, as clicking does outside
		// Pose mode.
		EditorNode::get_singleton()->get_editor_selection()->clear();
		EditorNode::get_singleton()->get_editor_selection()->add_node(part);
		EditorNode::get_singleton()->push_item(part, String(), true);
	}
	_update_pose_bar();
	preview_surface->queue_redraw();
	sequence_strip->queue_redraw();
}

void AsosdotRenderEditor::_select_parent() {
	Object *object = ObjectDB::get_instance(pose_selected.node);
	Node *root = _get_scene_root();
	if (!object || !root) {
		return;
	}
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object); skeleton && pose_selected.bone >= 0) {
		const int parent = skeleton->get_bone_parent(pose_selected.bone);
		// Above the first bone is the skeleton itself, which moves them all.
		_select_pose_target(parent >= 0 ? PoseTarget{ skeleton->get_instance_id(), parent } : PoseTarget{ skeleton->get_instance_id(), -1 });
		return;
	}
	Node *node = Object::cast_to<Node>(object);
	if (node && node != root && Object::cast_to<Node3D>(node->get_parent())) {
		_select_pose_target({ node->get_parent()->get_instance_id(), -1 });
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Posing                                    */
/* -------------------------------------------------------------------------- */

AsosdotRenderEditor::PoseValue AsosdotRenderEditor::_get_pose_value(const PoseTarget &p_target) const {
	PoseValue value;
	value.target = p_target;
	Object *object = ObjectDB::get_instance(p_target.node);
	if (p_target.bone >= 0) {
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object);
		if (skeleton && p_target.bone < skeleton->get_bone_count()) {
			value.position = skeleton->get_bone_pose_position(p_target.bone);
			value.rotation = skeleton->get_bone_pose_rotation(p_target.bone);
			value.scale = skeleton->get_bone_pose_scale(p_target.bone);
		}
	} else if (Node3D *node = Object::cast_to<Node3D>(object)) {
		value.transform = node->get_transform();
	}
	return value;
}

void AsosdotRenderEditor::_set_pose_value(const PoseValue &p_value) {
	Object *object = ObjectDB::get_instance(p_value.target.node);
	if (p_value.target.bone >= 0) {
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object);
		if (skeleton && p_value.target.bone < skeleton->get_bone_count()) {
			skeleton->set_bone_pose_position(p_value.target.bone, p_value.position);
			skeleton->set_bone_pose_rotation(p_value.target.bone, p_value.rotation);
			skeleton->set_bone_pose_scale(p_value.target.bone, p_value.scale);
		}
	} else if (Node3D *node = Object::cast_to<Node3D>(object)) {
		node->set_transform(p_value.transform);
	}
}

void AsosdotRenderEditor::_add_pose_undo(EditorUndoRedoManager *p_undo_redo, const PoseValue &p_before, const PoseValue &p_after) {
	Object *object = ObjectDB::get_instance(p_after.target.node);
	if (!object) {
		return;
	}
	const int bone = p_after.target.bone;
	if (bone >= 0) {
		p_undo_redo->add_do_method(object, "set_bone_pose_position", bone, p_after.position);
		p_undo_redo->add_do_method(object, "set_bone_pose_rotation", bone, p_after.rotation);
		p_undo_redo->add_do_method(object, "set_bone_pose_scale", bone, p_after.scale);
		p_undo_redo->add_undo_method(object, "set_bone_pose_position", bone, p_before.position);
		p_undo_redo->add_undo_method(object, "set_bone_pose_rotation", bone, p_before.rotation);
		p_undo_redo->add_undo_method(object, "set_bone_pose_scale", bone, p_before.scale);
	} else {
		p_undo_redo->add_do_method(object, "set_transform", p_after.transform);
		p_undo_redo->add_undo_method(object, "set_transform", p_before.transform);
	}
}

void AsosdotRenderEditor::_touch(const PoseTarget &p_target) {
	if (!pose_touched.has(p_target)) {
		pose_touched.push_back(p_target);
	}
}

bool AsosdotRenderEditor::_intersect_view_plane(const Point2 &p_point, const Vector3 &p_through, Vector3 &r_hit) const {
	const Plane plane(preview_camera->get_global_transform().basis.get_column(2).normalized(), p_through);
	return plane.intersects_ray(preview_camera->project_ray_origin(p_point), preview_camera->project_ray_normal(p_point), &r_hit);
}

bool AsosdotRenderEditor::_begin_pose_drag(const Point2 &p_point, bool p_shift, bool p_alt) {
	// A drag starts on a bone, which it selects, or on the selected part.
	PoseTarget target = _pick_bone(p_point);
	if (!target.is_set() && pose_selected.is_set() && pose_selected.bone < 0) {
		Node *selected = ObjectDB::get_instance<Node>(pose_selected.node);
		GeometryInstance3D *hit = _raycast_geometry(p_point);
		if (selected && hit && (hit == selected || selected->is_ancestor_of(hit))) {
			target = pose_selected;
		}
	}
	if (!target.is_set()) {
		return false;
	}
	Skeleton3D *skeleton = target.bone >= 0 ? ObjectDB::get_instance<Skeleton3D>(target.node) : nullptr;
	Node3D *node = target.bone < 0 ? ObjectDB::get_instance<Node3D>(target.node) : nullptr;
	if (!skeleton && !node) {
		return false;
	}
	if (!(target == pose_selected)) {
		_select_pose_target(target);
	}

	pose_drag = PoseDrag();
	pose_drag.target = target;
	pose_drag.start_mouse = p_point;
	pose_drag.pivot = _get_target_origin(target);
	pose_drag.pivot_screen = preview_camera->unproject_position(pose_drag.pivot);
	switch (pose_tool) {
		case POSE_TOOL_ROTATE:
			pose_drag.kind = p_alt ? DRAG_TWIST : (p_shift ? DRAG_TRACKBALL : DRAG_VIEW_ROTATE);
			break;
		case POSE_TOOL_REACH:
			// Only a bone with one above it can reach; anything else moves.
			pose_drag.kind = (skeleton && skeleton->get_bone_parent(target.bone) >= 0) ? DRAG_REACH : DRAG_MOVE;
			break;
		default:
			pose_drag.kind = DRAG_MOVE;
	}
	if (pose_drag.kind == DRAG_TWIST) {
		pose_drag.axis = skeleton ? _get_bone_axis(skeleton, target.bone) : node->get_global_transform().basis.get_column(1).normalized();
	}
	if (!_intersect_view_plane(p_point, pose_drag.pivot, pose_drag.grab)) {
		pose_drag.grab = pose_drag.pivot;
	}
	// Everything the drag may change, to undo it or put it back.
	pose_drag.before.push_back(_get_pose_value(target));
	if (pose_drag.kind == DRAG_REACH) {
		const int parent = skeleton->get_bone_parent(target.bone);
		pose_drag.before.push_back(_get_pose_value({ target.node, parent }));
		const int grandparent = skeleton->get_bone_parent(parent);
		if (grandparent >= 0) {
			pose_drag.before.push_back(_get_pose_value({ target.node, grandparent }));
		}
	}
	return true;
}

void AsosdotRenderEditor::_turn_target(const PoseTarget &p_target, const Quaternion &p_turn) {
	Object *object = ObjectDB::get_instance(p_target.node);
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object); skeleton && p_target.bone >= 0) {
		// The turn, from the world into the skeleton, then into the bone's
		// parent, so it's applied to the bone's own pose.
		const Quaternion skeleton_rotation = skeleton->get_global_transform().basis.get_rotation_quaternion();
		const Quaternion turn = skeleton_rotation.inverse() * p_turn * skeleton_rotation;
		const Quaternion parent = global_rotation(skeleton, skeleton->get_bone_parent(p_target.bone));
		skeleton->set_bone_pose_rotation(p_target.bone, (parent.inverse() * turn * parent * skeleton->get_bone_pose_rotation(p_target.bone)).normalized());
	} else if (Node3D *node = Object::cast_to<Node3D>(object)) {
		const Transform3D xform = node->get_global_transform();
		node->set_global_transform(Transform3D(Basis(p_turn) * xform.basis, xform.origin));
	}
}

void AsosdotRenderEditor::_move_target(const PoseTarget &p_target, const Vector3 &p_offset) {
	Object *object = ObjectDB::get_instance(p_target.node);
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object); skeleton && p_target.bone >= 0) {
		Vector3 offset = skeleton->get_global_transform().basis.inverse().xform(p_offset);
		const int parent = skeleton->get_bone_parent(p_target.bone);
		if (parent >= 0) {
			offset = skeleton->get_bone_global_pose(parent).basis.inverse().xform(offset);
		}
		skeleton->set_bone_pose_position(p_target.bone, skeleton->get_bone_pose_position(p_target.bone) + offset);
	} else if (Node3D *node = Object::cast_to<Node3D>(object)) {
		node->set_global_position(node->get_global_position() + p_offset);
	}
}

void AsosdotRenderEditor::_reach(const Vector3 &p_target) {
	Skeleton3D *skeleton = ObjectDB::get_instance<Skeleton3D>(pose_drag.target.node);
	const int bone = pose_drag.target.bone;
	if (!skeleton || bone < 0) {
		return;
	}
	const int parent = skeleton->get_bone_parent(bone);
	if (parent < 0) {
		return;
	}
	const int grandparent = skeleton->get_bone_parent(parent);
	const Vector3 target = skeleton->get_global_transform().affine_inverse().xform(p_target);
	const Transform3D bone_pose = skeleton->get_bone_global_pose(bone);
	const Transform3D parent_pose = skeleton->get_bone_global_pose(parent);
	const Quaternion bone_rotation = bone_pose.basis.get_rotation_quaternion();
	const Quaternion parent_rotation = parent_pose.basis.get_rotation_quaternion();
	const Vector3 end = bone_pose.origin;
	const Vector3 middle = parent_pose.origin;
	constexpr float EPSILON = 1e-5f;

	Quaternion new_parent_rotation;
	bool solved = false;
	if (grandparent >= 0) {
		// Two bones, like an upper arm and a forearm: bent in the plane they
		// bend in now, the way an elbow or knee does.
		const Transform3D grandparent_pose = skeleton->get_bone_global_pose(grandparent);
		const Vector3 start = grandparent_pose.origin;
		const float upper = start.distance_to(middle);
		const float lower = middle.distance_to(end);
		const Vector3 to_target = target - start;
		if (upper > EPSILON && lower > EPSILON && to_target.length() > EPSILON) {
			Vector3 bend = (middle - start).cross(end - middle);
			if (bend.length_squared() < 1e-8f * upper * upper * lower * lower) {
				// Straight: bend across the view.
				const Vector3 view = skeleton->get_global_transform().basis.inverse().xform(-preview_camera->get_global_transform().basis.get_column(2));
				bend = to_target.cross(view);
				if (bend.length_squared() < 1e-12f) {
					bend = to_target.get_any_perpendicular();
				}
			}
			bend.normalize();
			const Vector3 direction = to_target.normalized();
			const float distance = CLAMP(to_target.length(), Math::abs(upper - lower) + 1e-4f, upper + lower - 1e-4f);
			const float cos_start = CLAMP((upper * upper + distance * distance - lower * lower) / (2.0f * upper * distance), -1.0f, 1.0f);
			const float sin_start = Math::sqrt(MAX(0.0f, 1.0f - cos_start * cos_start));
			const Vector3 side = direction.cross(bend).normalized();
			const Vector3 new_middle = start + (direction * cos_start + side * sin_start) * upper;
			const Quaternion upper_turn(middle - start, new_middle - start);
			const Vector3 turned_end = start + upper_turn.xform(end - start);
			const Vector3 reached = start + direction * distance;
			Quaternion lower_turn;
			if ((turned_end - new_middle).length_squared() > EPSILON && (reached - new_middle).length_squared() > EPSILON) {
				lower_turn = Quaternion(turned_end - new_middle, reached - new_middle);
			}
			const Quaternion new_grandparent_rotation = upper_turn * grandparent_pose.basis.get_rotation_quaternion();
			new_parent_rotation = lower_turn * upper_turn * parent_rotation;
			const Quaternion above = global_rotation(skeleton, skeleton->get_bone_parent(grandparent));
			skeleton->set_bone_pose_rotation(grandparent, (above.inverse() * new_grandparent_rotation).normalized());
			skeleton->set_bone_pose_rotation(parent, (new_grandparent_rotation.inverse() * new_parent_rotation).normalized());
			solved = true;
		}
	}
	if (!solved) {
		// One bone: the parent swings to point at the target.
		const Vector3 from = end - middle;
		const Vector3 to = target - middle;
		if (from.length_squared() < EPSILON || to.length_squared() < EPSILON) {
			return;
		}
		new_parent_rotation = Quaternion(from, to) * parent_rotation;
		const Quaternion above = global_rotation(skeleton, grandparent);
		skeleton->set_bone_pose_rotation(parent, (above.inverse() * new_parent_rotation).normalized());
	}
	// The hand or foot keeps its turn in the world.
	skeleton->set_bone_pose_rotation(bone, (new_parent_rotation.inverse() * bone_rotation).normalized());
}

void AsosdotRenderEditor::_update_pose_drag(const Point2 &p_point) {
	if (!pose_drag.moved && p_point.distance_to(pose_drag.start_mouse) < 2.0f * EDSCALE) {
		return;
	}
	pose_drag.moved = true;
	// From the pose at the start each time, so nothing drifts.
	for (const PoseValue &value : pose_drag.before) {
		_set_pose_value(value);
	}
	const Basis view = preview_camera->get_global_transform().basis.orthonormalized();
	const Vector2 dragged = p_point - pose_drag.start_mouse;
	switch (pose_drag.kind) {
		case DRAG_VIEW_ROTATE: {
			// Screen angles run clockwise, and so does a turn about the line
			// of sight looking in.
			const Vector2 from = pose_drag.start_mouse - pose_drag.pivot_screen;
			const Vector2 to = p_point - pose_drag.pivot_screen;
			if (from.length_squared() > 1.0f && to.length_squared() > 1.0f) {
				_turn_target(pose_drag.target, Quaternion(-view.get_column(2), to.angle() - from.angle()));
			}
		} break;
		case DRAG_TRACKBALL:
			_turn_target(pose_drag.target, Quaternion(view.get_column(1), dragged.x * TURN_SPEED) * Quaternion(view.get_column(0), dragged.y * TURN_SPEED));
			break;
		case DRAG_TWIST:
			_turn_target(pose_drag.target, Quaternion(pose_drag.axis, dragged.x * TURN_SPEED));
			break;
		case DRAG_MOVE: {
			Vector3 hit;
			if (_intersect_view_plane(p_point, pose_drag.pivot, hit)) {
				_move_target(pose_drag.target, hit - pose_drag.grab);
			}
		} break;
		case DRAG_REACH: {
			Vector3 hit;
			if (_intersect_view_plane(p_point, pose_drag.pivot, hit)) {
				_reach(hit + pose_drag.pivot - pose_drag.grab);
			}
		} break;
		default:
			break;
	}
	preview_surface->queue_redraw();
}

void AsosdotRenderEditor::_end_pose_drag() {
	if (pose_drag.kind == DRAG_NONE) {
		return;
	}
	const PoseDrag drag = pose_drag;
	pose_drag = PoseDrag();
	preview_surface->queue_redraw();
	Node *root = _get_scene_root();
	if (!drag.moved || !root) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(drag.target.bone >= 0 ? TTR("Pose Bone") : TTR("Pose Part"), UndoRedo::MERGE_DISABLE, root, true);
	undo_redo->force_fixed_history();
	LocalVector<PoseTarget> changed;
	for (const PoseValue &before : drag.before) {
		_add_pose_undo(undo_redo, before, _get_pose_value(before.target));
		changed.push_back(before.target);
		_touch(before.target);
	}
	if (auto_key) {
		_add_key_ops(undo_redo, changed, false);
	}
	undo_redo->commit_action();
	if (auto_key) {
		for (const PoseTarget &target : changed) {
			pose_touched.erase(target);
		}
	}
	_update_pose_bar();
}

void AsosdotRenderEditor::_cancel_pose_drag() {
	if (pose_drag.kind == DRAG_NONE) {
		return;
	}
	for (const PoseValue &value : pose_drag.before) {
		_set_pose_value(value);
	}
	pose_drag = PoseDrag();
	preview_surface->queue_redraw();
}

void AsosdotRenderEditor::_reset_pose_pressed() {
	if (job.active) {
		return;
	}
	Skeleton3D *skeleton = pose_selected.bone >= 0 ? ObjectDB::get_instance<Skeleton3D>(pose_selected.node) : nullptr;
	Node *root = _get_scene_root();
	if (!skeleton || !root || pose_selected.bone >= skeleton->get_bone_count()) {
		return;
	}
	const bool whole = Input::get_singleton()->is_key_pressed(Key::SHIFT);
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(whole ? TTR("Reset Skeleton Pose") : TTR("Reset Bone Pose"), UndoRedo::MERGE_DISABLE, root, true);
	undo_redo->force_fixed_history();
	LocalVector<PoseTarget> changed;
	for (int b = whole ? 0 : pose_selected.bone; b < (whole ? skeleton->get_bone_count() : pose_selected.bone + 1); b++) {
		const PoseTarget target = { skeleton->get_instance_id(), b };
		const PoseValue before = _get_pose_value(target);
		PoseValue after = before;
		const Transform3D rest = skeleton->get_bone_rest(b);
		after.position = rest.origin;
		after.rotation = rest.basis.get_rotation_quaternion();
		after.scale = rest.basis.get_scale();
		_add_pose_undo(undo_redo, before, after);
		changed.push_back(target);
		_touch(target);
	}
	if (auto_key) {
		// Keyed as the pose will be once this is done.
		for (const PoseTarget &target : changed) {
			const Transform3D rest = skeleton->get_bone_rest(target.bone);
			skeleton->set_bone_pose_position(target.bone, rest.origin);
			skeleton->set_bone_pose_rotation(target.bone, rest.basis.get_rotation_quaternion());
			skeleton->set_bone_pose_scale(target.bone, rest.basis.get_scale());
		}
		_add_key_ops(undo_redo, changed, false);
		for (const PoseTarget &target : changed) {
			pose_touched.erase(target);
		}
	}
	undo_redo->commit_action();
}

/* -------------------------------------------------------------------------- */
/*                                Keyframing                                  */
/* -------------------------------------------------------------------------- */

double AsosdotRenderEditor::_get_key_time(const Ref<Animation> &p_clip) const {
	double at = double(shot->get_value("animation_start")) + double(time) * double(shot->get_value("animation_speed"));
	if (p_clip.is_valid() && p_clip->get_loop_mode() != Animation::LOOP_NONE && p_clip->get_length() > 0.0) {
		at = Math::fmod(at, (double)p_clip->get_length());
	}
	return Math::snapped(at, 0.0001);
}

bool AsosdotRenderEditor::_is_clip_editable(AnimationPlayer *p_player, const Ref<Animation> &p_clip) const {
	// A clip from an imported file, or kept in another scene, would lose
	// its keys.
	if (!p_player || p_clip.is_null() || EditorNode::get_singleton()->is_resource_read_only(p_clip)) {
		return false;
	}
	const Ref<AnimationLibrary> library = p_player->get_animation_library(p_player->find_animation_library(p_clip));
	if (library.is_valid() && EditorNode::get_singleton()->is_resource_read_only(library)) {
		return false;
	}
	for (int t = 0; t < p_clip->get_track_count(); t++) {
		if (p_clip->track_is_compressed(t)) {
			return false;
		}
	}
	return true;
}

Ref<Animation> AsosdotRenderEditor::_get_key_clip(EditorUndoRedoManager *p_undo_redo, bool p_new_clip, bool p_create, bool &r_created, Node *&r_animation_root) {
	r_created = false;
	r_animation_root = nullptr;
	Node *root = _get_scene_root();
	if (!root) {
		return Ref<Animation>();
	}
	AnimationPlayer *player = _get_animation_player();
	const StringName chosen = String(shot->get_value("animation"));
	const Ref<Animation> current = (player && chosen != StringName() && player->has_animation(chosen)) ? player->get_animation(chosen) : Ref<Animation>();
	if (!p_new_clip && current.is_valid() && _is_clip_editable(player, current)) {
		r_animation_root = player->get_node_or_null(player->get_root_node());
		return r_animation_root ? current : Ref<Animation>();
	}
	if (!p_create) {
		return Ref<Animation>();
	}

	// Where a new clip goes: the chosen player, when the scene owns it and
	// its own library can take one, or else a player of the scene's own.
	AnimationPlayer *target = nullptr;
	Ref<AnimationLibrary> library;
	bool new_library = false;
	bool new_player = false;
	for (AnimationPlayer *candidate : { player, Object::cast_to<AnimationPlayer>(root->get_node_or_null(NodePath(OWN_PLAYER_NAME))) }) {
		if (!candidate || (candidate != root && candidate->get_owner() != root)) {
			continue;
		}
		if (!candidate->has_animation_library(StringName())) {
			target = candidate;
			new_library = true;
			break;
		}
		const Ref<AnimationLibrary> own = candidate->get_animation_library(StringName());
		if (!EditorNode::get_singleton()->is_resource_read_only(own)) {
			target = candidate;
			library = own;
			break;
		}
	}
	if (!target) {
		target = memnew(AnimationPlayer);
		String name = OWN_PLAYER_NAME;
		for (int n = 2; root->has_node(NodePath(name)); n++) {
			name = String(OWN_PLAYER_NAME) + itos(n);
		}
		target->set_name(name);
		new_player = true;
		new_library = true;
	}
	if (new_library) {
		library.instantiate();
	}
	// A new player's tracks are found from the scene's root.
	r_animation_root = new_player ? root : target->get_node_or_null(target->get_root_node());
	if (!r_animation_root) {
		if (new_player) {
			memdelete(target);
		}
		return Ref<Animation>();
	}

	// The clip: a copy of an imported one, so it can be changed, or empty
	// and as long as the shot.
	Ref<Animation> clip;
	clip.instantiate();
	String name;
	const bool copy = !p_new_clip && current.is_valid();
	if (copy) {
		clip->set_length(current->get_length());
		clip->set_loop_mode(current->get_loop_mode());
		clip->set_step(current->get_step());
		Node *from_root = player->get_node_or_null(player->get_root_node());
		for (int t = 0; t < current->get_track_count(); t++) {
			current->copy_track(t, clip);
			const int copied = clip->get_track_count() - 1;
			clip->track_set_imported(copied, false);
			// Pointed from the new player's root at the same thing.
			const NodePath path = current->track_get_path(t);
			Node *node = from_root ? from_root->get_node_or_null(NodePath(path.get_concatenated_names())) : nullptr;
			if (node && from_root != r_animation_root) {
				String repointed = String(r_animation_root->get_path_to(node));
				if (path.get_subname_count() > 0) {
					repointed += ":" + String(path.get_concatenated_subnames());
				}
				clip->track_set_path(copied, NodePath(repointed));
			}
		}
		const String base = String(chosen).get_slicec('/', String(chosen).contains_char('/') ? 1 : 0);
		name = base;
		for (int n = 2; library->has_animation(name); n++) {
			name = base + "_" + itos(n);
		}
	} else {
		const float length = float(shot->get_value("animation_start")) + float(shot->get_value("duration")) * float(shot->get_value("animation_speed"));
		clip->set_length(MAX(length, 0.1f));
		clip->set_step(1.0 / MAX(int(sequence_output->get_value("fps")), 1));
		int n = 1;
		do {
			name = "take_" + itos(n++);
		} while (library->has_animation(name));
	}

	if (new_player) {
		library->add_animation(name, clip);
		target->add_animation_library(StringName(), library);
		p_undo_redo->add_do_method(root, "add_child", target, true);
		p_undo_redo->add_do_method(target, "set_owner", root);
		p_undo_redo->add_do_reference(target);
		p_undo_redo->add_undo_method(root, "remove_child", target);
	} else if (new_library) {
		library->add_animation(name, clip);
		p_undo_redo->add_do_method(target, "add_animation_library", StringName(), library);
		p_undo_redo->add_undo_method(target, "remove_animation_library", StringName());
	} else {
		p_undo_redo->add_do_method(library.ptr(), "add_animation", name, clip);
		p_undo_redo->add_undo_method(library.ptr(), "remove_animation", name);
	}
	// The shot follows the new clip, and goes back with undo.
	p_undo_redo->add_do_property(shot.ptr(), "animation_player", new_player ? NodePath(target->get_name()) : root->get_path_to(target));
	p_undo_redo->add_do_property(shot.ptr(), "animation", name);
	p_undo_redo->add_do_method(shot.ptr(), "notify_property_list_changed");
	p_undo_redo->add_undo_property(shot.ptr(), "animation_player", shot->get_value("animation_player"));
	p_undo_redo->add_undo_property(shot.ptr(), "animation", shot->get_value("animation"));
	p_undo_redo->add_undo_method(shot.ptr(), "notify_property_list_changed");
	r_created = true;
	return clip;
}

Ref<Animation> AsosdotRenderEditor::_snapshot_clip(const Ref<Animation> &p_clip) {
	Ref<Animation> copy;
	copy.instantiate();
	copy->set_length(p_clip->get_length());
	copy->set_loop_mode(p_clip->get_loop_mode());
	copy->set_step(p_clip->get_step());
	for (int t = 0; t < p_clip->get_track_count(); t++) {
		p_clip->copy_track(t, copy);
	}
	return copy;
}

void AsosdotRenderEditor::_set_clip_state(const Ref<Animation> &p_clip, const Ref<Animation> &p_state) {
	ERR_FAIL_COND(p_clip.is_null() || p_state.is_null());
	p_clip->clear();
	p_clip->set_length(p_state->get_length());
	p_clip->set_loop_mode(p_state->get_loop_mode());
	p_clip->set_step(p_state->get_step());
	for (int t = 0; t < p_state->get_track_count(); t++) {
		p_state->copy_track(t, p_clip);
	}
	_clip_changed();
}

void AsosdotRenderEditor::_clip_changed() {
	if (active && !job.active) {
		_sync_saved_pose();
		_apply_sequence_time(sequence_time);
	}
	sequence_strip->queue_redraw();
	_update_pose_bar();
}

LocalVector<AsosdotRenderEditor::PoseTarget> AsosdotRenderEditor::_get_key_targets(KeyScope p_scope) const {
	LocalVector<PoseTarget> targets;
	const auto add = [&](const PoseTarget &p_target) {
		if (p_target.is_set() && ObjectDB::get_instance(p_target.node) && !targets.has(p_target)) {
			targets.push_back(p_target);
		}
	};
	if (p_scope == KEY_SKELETON) {
		LocalVector<Skeleton3D *> skeletons;
		Skeleton3D *selected = pose_selected.bone >= 0 ? ObjectDB::get_instance<Skeleton3D>(pose_selected.node) : nullptr;
		if (selected) {
			skeletons.push_back(selected);
		} else {
			skeletons = _get_skeletons();
		}
		for (Skeleton3D *skeleton : skeletons) {
			for (int b = 0; b < skeleton->get_bone_count(); b++) {
				add({ skeleton->get_instance_id(), b });
			}
		}
		return targets;
	}
	if (p_scope == KEY_POSED) {
		for (const PoseTarget &target : pose_touched) {
			add(target);
		}
	}
	add(pose_selected);
	return targets;
}

NodePath AsosdotRenderEditor::_get_track_path(const PoseTarget &p_target, Node *p_animation_root) const {
	Node *node = ObjectDB::get_instance<Node>(p_target.node);
	if (!node || !p_animation_root) {
		return NodePath();
	}
	String path = String(p_animation_root->get_path_to(node));
	if (p_target.bone >= 0) {
		Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(node);
		if (!skeleton || p_target.bone >= skeleton->get_bone_count()) {
			return NodePath();
		}
		path += ":" + skeleton->get_bone_name(p_target.bone);
	}
	return NodePath(path);
}

void AsosdotRenderEditor::_write_keys(const Ref<Animation> &p_clip, const LocalVector<PoseTarget> &p_targets, Node *p_animation_root, double p_time, bool p_delete) const {
	if (p_delete) {
		// The selection's keys at this time, or every key here.
		LocalVector<NodePath> paths;
		for (const PoseTarget &target : p_targets) {
			paths.push_back(_get_track_path(target, p_animation_root));
		}
		for (int t = p_clip->get_track_count() - 1; t >= 0; t--) {
			if (!paths.is_empty() && !paths.has(p_clip->track_get_path(t))) {
				continue;
			}
			const int key = p_clip->track_find_key(t, p_time, Animation::FIND_MODE_APPROX);
			if (key < 0) {
				continue;
			}
			p_clip->track_remove_key(t, key);
			if (p_clip->track_get_key_count(t) == 0) {
				p_clip->remove_track(t);
			}
		}
		return;
	}

	if (p_clip->get_loop_mode() == Animation::LOOP_NONE && p_time > p_clip->get_length()) {
		p_clip->set_length(p_time);
	}
	const auto key = [&](const NodePath &p_path, Animation::TrackType p_type, const Variant &p_value) {
		int track = p_clip->find_track(p_path, p_type);
		if (track < 0) {
			track = p_clip->add_track(p_type);
			p_clip->track_set_path(track, p_path);
			p_clip->track_set_interpolation_type(track, Animation::INTERPOLATION_LINEAR);
		}
		p_clip->track_insert_key(track, p_time, p_value);
	};
	for (const PoseTarget &target : p_targets) {
		const NodePath path = _get_track_path(target, p_animation_root);
		if (path.is_empty()) {
			continue;
		}
		Object *object = ObjectDB::get_instance(target.node);
		if (target.bone >= 0) {
			// A bone is keyed where it turned, and where it moved or grew
			// when it did, so a turn alone doesn't pin the rest.
			Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(object);
			const Transform3D rest = skeleton->get_bone_rest(target.bone);
			key(path, Animation::TYPE_ROTATION_3D, skeleton->get_bone_pose_rotation(target.bone));
			const Vector3 position = skeleton->get_bone_pose_position(target.bone);
			if (p_clip->find_track(path, Animation::TYPE_POSITION_3D) >= 0 || !position.is_equal_approx(rest.origin)) {
				key(path, Animation::TYPE_POSITION_3D, position);
			}
			const Vector3 scale = skeleton->get_bone_pose_scale(target.bone);
			if (p_clip->find_track(path, Animation::TYPE_SCALE_3D) >= 0 || !scale.is_equal_approx(rest.basis.get_scale())) {
				key(path, Animation::TYPE_SCALE_3D, scale);
			}
		} else if (Node3D *node = Object::cast_to<Node3D>(object)) {
			key(path, Animation::TYPE_POSITION_3D, node->get_position());
			key(path, Animation::TYPE_ROTATION_3D, node->get_quaternion());
			if (p_clip->find_track(path, Animation::TYPE_SCALE_3D) >= 0 || !node->get_scale().is_equal_approx(Vector3(1, 1, 1))) {
				key(path, Animation::TYPE_SCALE_3D, node->get_scale());
			}
		}
	}
}

bool AsosdotRenderEditor::_add_key_ops(EditorUndoRedoManager *p_undo_redo, const LocalVector<PoseTarget> &p_targets, bool p_delete) {
	bool created = false;
	Node *animation_root = nullptr;
	const Ref<Animation> clip = _get_key_clip(p_undo_redo, false, true, created, animation_root);
	if (clip.is_null()) {
		return false;
	}
	const double at = _get_key_time(clip);
	if (created) {
		// Not in the scene yet: keyed now, and added with them.
		_write_keys(clip, p_targets, animation_root, at, p_delete);
		p_undo_redo->add_do_method(this, "_clip_changed");
		p_undo_redo->add_undo_method(this, "_clip_changed");
		return true;
	}
	const Ref<Animation> before = _snapshot_clip(clip);
	const Ref<Animation> after = _snapshot_clip(clip);
	_write_keys(after, p_targets, animation_root, at, p_delete);
	p_undo_redo->add_do_method(this, "_set_clip_state", clip, after);
	p_undo_redo->add_undo_method(this, "_set_clip_state", clip, before);
	return true;
}

void AsosdotRenderEditor::_key(int p_scope) {
	Node *root = _get_scene_root();
	if (!root || job.active || pose_drag.kind != DRAG_NONE) {
		return;
	}
	const KeyScope scope = (KeyScope)p_scope;
	const LocalVector<PoseTarget> targets = _get_key_targets(scope);
	if (scope == KEY_DELETE) {
		AnimationPlayer *player = _get_animation_player();
		const StringName chosen = String(shot->get_value("animation"));
		if (!player || chosen == StringName() || !player->has_animation(chosen)) {
			EditorToaster::get_singleton()->popup_str(TTR("There's no clip to take keys out of."), EditorToaster::SEVERITY_WARNING);
			return;
		}
	} else if (targets.is_empty()) {
		EditorToaster::get_singleton()->popup_str(TTR("Pose a bone or a part first, or select one, to key it."), EditorToaster::SEVERITY_WARNING);
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(scope == KEY_DELETE ? TTR("Delete Keys") : TTR("Key Pose"), UndoRedo::MERGE_DISABLE, root, true);
	undo_redo->force_fixed_history();
	_add_key_ops(undo_redo, targets, scope == KEY_DELETE);
	undo_redo->commit_action();
	if (scope != KEY_DELETE) {
		for (const PoseTarget &target : targets) {
			pose_touched.erase(target);
		}
	}
}

void AsosdotRenderEditor::_new_clip_pressed() {
	Node *root = _get_scene_root();
	if (!root || job.active) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(TTR("New Clip"), UndoRedo::MERGE_DISABLE, root, true);
	undo_redo->force_fixed_history();
	bool created = false;
	Node *animation_root = nullptr;
	_get_key_clip(undo_redo, true, true, created, animation_root);
	undo_redo->add_do_method(this, "_clip_changed");
	undo_redo->add_undo_method(this, "_clip_changed");
	undo_redo->commit_action();
}

Vector<double> AsosdotRenderEditor::_get_key_times(bool p_selected_only) const {
	Vector<double> times;
	AnimationPlayer *player = _get_animation_player();
	const StringName chosen = String(shot->get_value("animation"));
	if (!player || chosen == StringName() || !player->has_animation(chosen)) {
		return times;
	}
	const Ref<Animation> clip = player->get_animation(chosen);
	NodePath selected_path;
	if (p_selected_only) {
		selected_path = _get_track_path(pose_selected, player->get_node_or_null(player->get_root_node()));
		if (selected_path.is_empty()) {
			return times;
		}
	}
	for (int t = 0; t < clip->get_track_count(); t++) {
		if (p_selected_only && clip->track_get_path(t) != selected_path) {
			continue;
		}
		for (int k = 0; k < clip->track_get_key_count(t); k++) {
			times.push_back(clip->track_get_key_time(t, k));
		}
	}
	times.sort();
	Vector<double> unique;
	for (double t : times) {
		if (unique.is_empty() || t - unique[unique.size() - 1] > 0.0005) {
			unique.push_back(t);
		}
	}
	return unique;
}

void AsosdotRenderEditor::_jump_to_key(bool p_next) {
	Vector<double> times = _get_key_times(true);
	if (times.is_empty()) {
		times = _get_key_times(false);
	}
	const float speed = MAX(float(shot->get_value("animation_speed")), 0.0001f);
	const float duration = MAX(float(shot->get_value("duration")), 0.01f);
	float best = -1.0f;
	for (double key_time : times) {
		const float at = (key_time - double(shot->get_value("animation_start"))) / speed;
		if (at < -0.0001f || at > duration + 0.0001f) {
			continue;
		}
		if (p_next ? at > time + 0.0001f : at < time - 0.0001f) {
			if (best < 0.0f || (p_next ? at < best : at > best)) {
				best = at;
			}
		}
	}
	if (best >= 0.0f) {
		playing = false;
		play_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
		_stop_audio();
		_apply_sequence_time(_shot_start(selected_shot) + best);
	}
}
