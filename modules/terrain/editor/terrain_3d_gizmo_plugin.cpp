/**************************************************************************/
/*  terrain_3d_gizmo_plugin.cpp                                           */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_3d_gizmo_plugin.h"

#include "../terrain_3d.h"

#include "core/math/triangle_mesh.h"

bool AsosdotTerrain3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Terrain3D>(p_spatial) != nullptr;
}

String AsosdotTerrain3DGizmoPlugin::get_gizmo_name() const {
	return "Terrain3D";
}

int AsosdotTerrain3DGizmoPlugin::get_priority() const {
	return -1;
}

void AsosdotTerrain3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	Terrain3D *terrain = Object::cast_to<Terrain3D>(p_gizmo->get_node_3d());
	if (!terrain) {
		return;
	}
	const PackedVector3Array faces = terrain->get_picking_faces();
	if (faces.is_empty()) {
		return;
	}
	Ref<TriangleMesh> mesh;
	mesh.instantiate();
	mesh->create(faces);
	p_gizmo->add_collision_triangles(mesh);
}
