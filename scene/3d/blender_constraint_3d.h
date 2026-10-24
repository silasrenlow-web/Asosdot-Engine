/**************************************************************************/
/*  blender_constraint_3d.h                                               */
/**************************************************************************/
/*  Asosdot: a Blender bone or object constraint, with Blender's math.    */
/**************************************************************************/

#pragma once

#include "scene/3d/skeleton_modifier_3d.h"

// One Blender constraint, evaluated the way Blender evaluates it: its owner and
// target spaces, its mix modes, Euler orders and offsets. The owner is a bone of
// the parent Skeleton3D, or with owner_node set, another node such as a prop that
// follows a bone. Settings use Blender's names, so a constraint reads as it does in
// Blender's constraint panel. Coordinates are Blender's: armature space is Z up, and
// a Skeleton3D imported from Blender is its armature turned Y up, bone axes kept.
class BlenderConstraint3D : public SkeletonModifier3D {
	GDCLASS(BlenderConstraint3D, SkeletonModifier3D);

public:
	enum ConstraintType {
		TYPE_COPY_LOCATION,
		TYPE_COPY_ROTATION,
		TYPE_COPY_SCALE,
		TYPE_COPY_TRANSFORMS,
		TYPE_LIMIT_LOCATION,
		TYPE_LIMIT_ROTATION,
		TYPE_LIMIT_SCALE,
		TYPE_CHILD_OF,
		TYPE_DAMPED_TRACK,
		TYPE_TRACK_TO,
		TYPE_LOCKED_TRACK,
		TYPE_STRETCH_TO,
		TYPE_TRANSFORMATION,
		TYPE_MAINTAIN_VOLUME,
		TYPE_MAX,
	};

	enum Space {
		SPACE_WORLD,
		SPACE_POSE,
		SPACE_LOCAL_WITH_PARENT,
		SPACE_LOCAL,
	};

private:
	ConstraintType constraint_type = TYPE_COPY_LOCATION;
	String bone_name;
	int bone = -1;
	NodePath owner_node;
	String target_bone_name;
	NodePath target_node;
	float head_tail = 0.0;
	float target_bone_length = 0.0;
	Space owner_space = SPACE_WORLD;
	Space target_space = SPACE_WORLD;
	String owner_rotation_order = "XYZ";
	String target_rotation_order = "XYZ";
	// The settings of the constraint type, by their names in Blender.
	Dictionary settings;

	// A node owner's own transform, before this constraint moved it.
	Transform3D node_input;
	Transform3D node_output;
	bool node_input_known = false;

	static const char *_get_type_name(ConstraintType p_type);
	static void _get_type_settings(ConstraintType p_type, List<PropertyInfo> *r_list, Dictionary *r_defaults);
	Variant _get_setting(const StringName &p_name) const;

	Transform3D _get_armature_world(const Skeleton3D *p_skeleton) const;
	Transform3D _get_bone_parent_matrix(const Skeleton3D *p_skeleton, int p_bone) const;
	Transform3D _to_space(const Skeleton3D *p_skeleton, const Transform3D &p_world, Space p_space, int p_bone) const;
	Transform3D _from_space(const Skeleton3D *p_skeleton, const Transform3D &p_matrix, Space p_space, int p_bone) const;
	bool _get_target_matrix(const Skeleton3D *p_skeleton, Transform3D &r_matrix) const;
	void _evaluate(Transform3D &r_owner, const Transform3D &p_target, bool p_has_target) const;

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _validate_property(PropertyInfo &p_property) const;
	static void _bind_methods();

	virtual void _validate_bone_names() override;
	virtual void _process_modification(double p_delta) override;

public:
	void set_constraint_type(ConstraintType p_type);
	ConstraintType get_constraint_type() const;
	void set_bone_name(const String &p_bone_name);
	String get_bone_name() const;
	void set_bone(int p_bone);
	int get_bone() const;
	void set_owner_node(const NodePath &p_node);
	NodePath get_owner_node() const;
	void set_target_bone_name(const String &p_bone_name);
	String get_target_bone_name() const;
	void set_target_node(const NodePath &p_node);
	NodePath get_target_node() const;
	void set_head_tail(float p_head_tail);
	float get_head_tail() const;
	void set_target_bone_length(float p_length);
	float get_target_bone_length() const;
	void set_owner_space(Space p_space);
	Space get_owner_space() const;
	void set_target_space(Space p_space);
	Space get_target_space() const;
	void set_owner_rotation_order(const String &p_order);
	String get_owner_rotation_order() const;
	void set_target_rotation_order(const String &p_order);
	String get_target_rotation_order() const;
	void set_settings(const Dictionary &p_settings);
	Dictionary get_settings() const;
	void set_setting(const StringName &p_name, const Variant &p_value);
	Variant get_setting(const StringName &p_name) const;
	bool needs_target() const;
};

VARIANT_ENUM_CAST(BlenderConstraint3D::ConstraintType);
VARIANT_ENUM_CAST(BlenderConstraint3D::Space);
