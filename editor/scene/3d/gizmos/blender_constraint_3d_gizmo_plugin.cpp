/**************************************************************************/
/*  blender_constraint_3d_gizmo_plugin.cpp                                */
/**************************************************************************/
/*  Asosdot: shows what a BlenderConstraint3D follows.                    */
/**************************************************************************/

#include "blender_constraint_3d_gizmo_plugin.h"

#include "editor/scene/3d/gizmos/two_bone_ik_3d_gizmo_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/blender_constraint_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/surface_tool.h"

AsosdotBlenderConstraint3DGizmoPlugin::AsosdotBlenderConstraint3DGizmoPlugin() {
	unselected_material.instantiate();
	unselected_material->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	unselected_material->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
	unselected_material->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	unselected_material->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
	unselected_material->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);

	// Drawn over the model while selected, as the IK gizmos are.
	selected_material.instantiate();
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(R"(
shader_type spatial;
render_mode unshaded, shadows_disabled;

void vertex() {
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)), COLOR.rgb * (1.0 / 12.92), lessThan(COLOR.rgb, vec3(0.04045)));
	}
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * MODEL_MATRIX * vec4(VERTEX.xyz, 1.0);
	POSITION.z = mix(POSITION.z, POSITION.w, 0.998);
}

void fragment() {
	ALBEDO = COLOR.rgb;
	ALPHA = COLOR.a;
}
)");
	selected_material->set_shader(shader);
}

bool AsosdotBlenderConstraint3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<BlenderConstraint3D>(p_spatial) != nullptr;
}

String AsosdotBlenderConstraint3DGizmoPlugin::get_gizmo_name() const {
	return "BlenderConstraint3D";
}

int AsosdotBlenderConstraint3DGizmoPlugin::get_priority() const {
	return -1;
}

void AsosdotBlenderConstraint3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	BlenderConstraint3D *constraint = Object::cast_to<BlenderConstraint3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();
	Skeleton3D *skeleton = constraint->get_skeleton();
	if (!skeleton || !constraint->needs_target()) {
		return;
	}
	bool skinned = false;
	Ref<ArrayMesh> mesh = _get_mesh(skeleton, constraint, p_gizmo->is_selected(), skinned);
	if (mesh.is_null()) {
		return;
	}
	const Transform3D skeleton_transform = constraint->get_global_transform().affine_inverse() * skeleton->get_global_transform();
	if (skinned) {
		p_gizmo->add_mesh(mesh, Ref<Material>(), skeleton_transform, skeleton->register_skin(skeleton->create_skin_from_rest_transforms()));
	} else {
		p_gizmo->add_mesh(mesh, Ref<Material>(), skeleton_transform);
	}
}

Ref<ArrayMesh> AsosdotBlenderConstraint3DGizmoPlugin::_get_mesh(Skeleton3D *p_skeleton, BlenderConstraint3D *p_constraint, bool p_is_selected, bool &r_skinned) {
	const Transform3D to_skeleton = p_skeleton->get_global_transform().affine_inverse();

	// Each end is a bone, drawn at rest and skinned to follow its pose, or a node, drawn where it is.
	int owner_bone = -1;
	Vector3 owner_point;
	if (p_constraint->get_owner_node().is_empty()) {
		owner_bone = p_constraint->get_bone();
		if (owner_bone < 0 || owner_bone >= p_skeleton->get_bone_count()) {
			return Ref<ArrayMesh>();
		}
		owner_point = p_skeleton->get_bone_global_rest(owner_bone).origin;
	} else {
		const Node3D *node = Object::cast_to<Node3D>(p_constraint->get_node_or_null(p_constraint->get_owner_node()));
		if (!node) {
			return Ref<ArrayMesh>();
		}
		owner_point = to_skeleton.xform(node->get_global_position());
	}
	int target_bone = -1;
	Vector3 target_point;
	if (!p_constraint->get_target_bone_name().is_empty()) {
		target_bone = p_skeleton->find_bone(p_constraint->get_target_bone_name());
		if (target_bone < 0) {
			return Ref<ArrayMesh>();
		}
		target_point = p_skeleton->get_bone_global_rest(target_bone).xform(Vector3(0, p_constraint->get_target_bone_length() * p_constraint->get_head_tail(), 0));
	} else {
		const Node3D *node = Object::cast_to<Node3D>(p_constraint->get_node_or_null(p_constraint->get_target_node()));
		if (!node) {
			return Ref<ArrayMesh>();
		}
		target_point = to_skeleton.xform(node->get_global_position());
	}
	// A line can only follow bones if both its ends do.
	r_skinned = owner_bone >= 0 && target_bone >= 0;
	if (!r_skinned) {
		if (owner_bone >= 0) {
			owner_point = p_skeleton->get_bone_global_pose(owner_bone).origin;
		}
		if (target_bone >= 0) {
			target_point = p_skeleton->get_bone_global_pose(target_bone).xform(Vector3(0, p_constraint->get_target_bone_length() * p_constraint->get_head_tail(), 0));
		}
	}

	Color color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/ik_chain");
	if (!p_constraint->is_active()) {
		color.a *= 0.4;
	}
	Ref<SurfaceTool> surface_tool;
	surface_tool.instantiate();
	surface_tool->begin(Mesh::PRIMITIVE_LINES);
	if (p_is_selected) {
		surface_tool->set_material(selected_material);
	} else {
		unselected_material->set_albedo(color);
		surface_tool->set_material(unselected_material);
	}

	PackedInt32Array bones;
	PackedFloat32Array weights;
	bones.resize(4);
	weights.resize(4);
	bones.fill(0);
	weights.fill(0);
	weights.write[0] = 1;
	auto attach = [&](int p_bone) {
		if (r_skinned) {
			bones.write[0] = p_bone;
			surface_tool->set_bones(bones);
			surface_tool->set_weights(weights);
		}
	};

	attach(owner_bone);
	surface_tool->set_color(color);
	surface_tool->add_vertex(owner_point);
	attach(target_bone);
	surface_tool->set_color(color);
	surface_tool->add_vertex(target_point);

	// A small mark at the point the constraint reads.
	const real_t size = MAX(owner_point.distance_to(target_point) * 0.06, (real_t)0.01);
	for (int axis = 0; axis < 3; axis++) {
		Vector3 offset;
		offset[axis] = size;
		TwoBoneIK3DGizmoPlugin::draw_line(surface_tool, target_point - offset, target_point + offset, color);
	}
	return surface_tool->commit();
}
