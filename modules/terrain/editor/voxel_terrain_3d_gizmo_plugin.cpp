/**************************************************************************/
/*  voxel_terrain_3d_gizmo_plugin.cpp                                     */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "voxel_terrain_3d_gizmo_plugin.h"

#include "../voxel_terrain_3d.h"

#include "scene/3d/camera_3d.h"

bool AsosdotVoxelTerrain3DGizmo::intersect_ray(Camera3D *p_camera, const Point2 &p_point, Vector3 &r_pos, Vector3 &r_normal) {
	const VoxelTerrain3D *terrain = Object::cast_to<VoxelTerrain3D>(get_node_3d());
	if (!terrain || !terrain->is_visible_in_tree()) {
		return false;
	}
	const Vector3 from = p_camera->project_ray_origin(p_point);
	const Vector3 dir = p_camera->project_ray_normal(p_point);
	if (!terrain->intersect_ray_fast(from, dir, p_camera->get_far(), r_pos)) {
		return false;
	}
	r_normal = terrain->get_normal(r_pos);
	return true;
}

bool AsosdotVoxelTerrain3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<VoxelTerrain3D>(p_spatial) != nullptr;
}

Ref<EditorNode3DGizmo> AsosdotVoxelTerrain3DGizmoPlugin::create_gizmo(Node3D *p_spatial) {
	Ref<AsosdotVoxelTerrain3DGizmo> gizmo;
	if (has_gizmo(p_spatial)) {
		gizmo.instantiate();
	}
	return gizmo;
}

String AsosdotVoxelTerrain3DGizmoPlugin::get_gizmo_name() const {
	return "VoxelTerrain3D";
}

int AsosdotVoxelTerrain3DGizmoPlugin::get_priority() const {
	return -1;
}
