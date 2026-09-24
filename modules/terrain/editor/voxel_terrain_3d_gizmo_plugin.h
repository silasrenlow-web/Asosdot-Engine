/**************************************************************************/
/*  voxel_terrain_3d_gizmo_plugin.h                                       */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "editor/scene/3d/node_3d_editor_gizmos.h"

// Voxel terrain goes on for as far as its generator does, so there is no
// finite copy of it to click on. Its gizmo asks the terrain itself where a
// ray meets the ground instead.
class VoxelTerrain3DGizmo : public EditorNode3DGizmo {
	GDCLASS(VoxelTerrain3DGizmo, EditorNode3DGizmo);

public:
	bool intersect_ray(Camera3D *p_camera, const Point2 &p_point, Vector3 &r_pos, Vector3 &r_normal) override;
};

class VoxelTerrain3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(VoxelTerrain3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	Ref<EditorNode3DGizmo> create_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
};
