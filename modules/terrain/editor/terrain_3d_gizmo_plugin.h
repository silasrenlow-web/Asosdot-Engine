/**************************************************************************/
/*  terrain_3d_gizmo_plugin.h                                             */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "editor/scene/3d/node_3d_editor_gizmos.h"

// The viewport only selects nodes through their gizmos, and a terrain has no
// handles to draw, so this gizmo exists to give it a surface to click on: a
// coarse copy of the terrain, invisible, used only for picking.
class Terrain3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(Terrain3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;
};
