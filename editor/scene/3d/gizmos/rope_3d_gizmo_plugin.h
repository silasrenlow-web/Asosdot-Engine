/**************************************************************************/
/*  rope_3d_gizmo_plugin.h                                                */
/**************************************************************************/
/*  Editor gizmo for Asosdot's Rope3D node.                               */
/**************************************************************************/

#pragma once

#include "editor/scene/3d/node_3d_editor_gizmos.h"

class Rope3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(Rope3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	Rope3DGizmoPlugin();
};
