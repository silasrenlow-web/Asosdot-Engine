/**************************************************************************/
/*  pose_target_3d.h                                                      */
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

#pragma once

#include "scene/3d/node_3d.h"
#include "scene/resources/anchored_pose.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"

class AnchoredPoseModifier3D;
class Skeleton3D;

// A place in the world that a part of a character (a hand, a foot ...) is posed against, with the pose.
class PoseTarget3D : public Node3D {
	GDCLASS(PoseTarget3D, Node3D);

	StringName slot;
	Ref<AnchoredPose> pose;
	// A Skeleton3D child that holds the pose as bone poses: pose it with the normal tools and animate it with an
	// AnimationPlayer, and the character's slot follows it.
	NodePath pose_skeleton;
	PackedStringArray body_bones;
	float fade_in = 0.2;
	float fade_out = 0.2;

	bool preview = true;
	Ref<PackedScene> preview_rig;
	float preview_distance = 0.6;
	float preview_height = 0.35;
	float preview_yaw = 0.0;
	float game_scale = 1.0;
	Color preview_color = Color(0.45, 0.8, 1.0, 0.4);
	bool posing = false;
	bool pose_dirty = false; // Changed while posing: saved to its file with the scene.
	void _pose_changed();
	Skeleton3D *_get_pose_skeleton() const;

#ifdef TOOLS_ENABLED
	ObjectID ghost_id;
	Ref<StandardMaterial3D> ghost_material;
	AnchoredPoseModifier3D *_get_ghost_modifier() const;
	void _apply_ghost_look(Node3D *p_ghost);
	void _select_in_editor(Node *p_node);
	String ghost_source;
	Transform3D ghost_camera_in_root;
	void _update_preview();
	void _free_preview();
#endif // TOOLS_ENABLED

	static AnchoredPoseModifier3D *_find_modifier(Node *p_node, const StringName &p_slot, int p_depth = 0);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	PackedStringArray get_configuration_warnings() const override;

	void set_slot(const StringName &p_slot);
	StringName get_slot() const;
	void set_pose(const Ref<AnchoredPose> &p_pose);
	Ref<AnchoredPose> get_pose() const;
	void set_pose_skeleton(const NodePath &p_path);
	NodePath get_pose_skeleton() const;
	void set_body_bones(const PackedStringArray &p_bones);
	PackedStringArray get_body_bones() const;
	// Makes the pose skeleton (a Skeleton3D child with the bones the slot of `p_rig` poses) and puts it in use.
	bool create_pose_skeleton_from(Node *p_rig);
	void create_pose_skeleton(); // Editor: from the preview character.
	Callable get_create_pose_skeleton_button() const;
	void set_fade_in(float p_seconds);
	float get_fade_in() const;
	void set_fade_out(float p_seconds);
	float get_fade_out() const;

	void set_preview(bool p_preview);
	bool is_preview() const;
	void set_preview_rig(const Ref<PackedScene> &p_scene);
	Ref<PackedScene> get_preview_rig() const;
	void set_preview_distance(float p_distance);
	float get_preview_distance() const;
	void set_preview_height(float p_height);
	float get_preview_height() const;
	void set_preview_yaw(float p_degrees);
	float get_preview_yaw() const;
	void set_game_scale(float p_scale);
	float get_game_scale() const;
	void set_preview_color(const Color &p_color);
	Color get_preview_color() const;
	void set_posing(bool p_posing);
	bool is_posing() const;

	// Poses the part of `p_rig` (any node above the character's skeleton) that has this target's slot.
	bool engage(Node *p_rig);
	void disengage(Node *p_rig);

	PoseTarget3D();
};
