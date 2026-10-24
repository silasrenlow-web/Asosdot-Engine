/**************************************************************************/
/*  blender_constraint_3d_gizmo_plugin.h                                  */
/**************************************************************************/
/*  Asosdot: shows what a BlenderConstraint3D follows.                    */
/**************************************************************************/

#pragma once

#include "editor/scene/3d/node_3d_editor_gizmos.h"

class BlenderConstraint3D;
class Skeleton3D;

// A line from a BlenderConstraint3D's owner to the point it reads on its target,
// with a mark at that point. Between two bones, the line follows their poses.
class AsosdotBlenderConstraint3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(AsosdotBlenderConstraint3DGizmoPlugin, EditorNode3DGizmoPlugin);

	Ref<StandardMaterial3D> unselected_material;
	Ref<ShaderMaterial> selected_material;

	Ref<ArrayMesh> _get_mesh(Skeleton3D *p_skeleton, BlenderConstraint3D *p_constraint, bool p_is_selected, bool &r_skinned);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;

	void redraw(EditorNode3DGizmo *p_gizmo) override;

	AsosdotBlenderConstraint3DGizmoPlugin();
};
