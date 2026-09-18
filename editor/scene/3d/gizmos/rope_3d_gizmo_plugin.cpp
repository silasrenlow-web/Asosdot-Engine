/**************************************************************************/
/*  rope_3d_gizmo_plugin.cpp                                              */
/**************************************************************************/
/*  Editor gizmo for Asosdot's Rope3D node.                               */
/**************************************************************************/

#include "rope_3d_gizmo_plugin.h"

#include "scene/3d/rope_3d.h"

Rope3DGizmoPlugin::Rope3DGizmoPlugin() {
	create_material("rope_material", Color(0.99, 0.5, 0.5));
}

bool Rope3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Rope3D>(p_spatial) != nullptr;
}

String Rope3DGizmoPlugin::get_gizmo_name() const {
	return "Rope3D";
}

int Rope3DGizmoPlugin::get_priority() const {
	return -1;
}

void Rope3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	Rope3D *rope = Object::cast_to<Rope3D>(p_gizmo->get_node_3d());

	p_gizmo->clear();

	if (rope == nullptr) {
		return;
	}

	const int count = rope->get_point_count();
	if (count < 2) {
		return;
	}

	// Rope3D simulates in world space; a gizmo draws in the node's local space.
	const Transform3D inv = rope->get_global_transform().affine_inverse();

	Vector<Vector3> lines;
	lines.resize((count - 1) * 2);
	Vector3 *w = lines.ptrw();
	for (int i = 0; i + 1 < count; i++) {
		w[i * 2 + 0] = inv.xform(rope->get_point_position(i));
		w[i * 2 + 1] = inv.xform(rope->get_point_position(i + 1));
	}

	// Selection in the 3D editor goes through the gizmo and nothing else, so
	// without these segments a rope cannot be clicked at all. They are matched
	// in screen space, which is what makes a thin rope pickable from any
	// camera angle rather than only when it happens to face the viewport.
	p_gizmo->add_collision_segments(lines);

	// Drawing the centre line on top of a rope that already draws itself is
	// just clutter, so it only shows when the rope is selected or when nothing
	// else would be visible.
	if (p_gizmo->is_selected() || rope->get_render_mode() == Rope3D::RENDER_MODE_NONE) {
		p_gizmo->add_lines(lines, get_material("rope_material", p_gizmo));
	}

	// Small crosses mark where the rope is attached, which is otherwise
	// guesswork once the rope has settled somewhere else entirely.
	const real_t mark = MAX(rope->get_thickness() * 3.0, (real_t)0.06);
	Vector<Vector3> ends;
	for (int e = 0; e < 2; e++) {
		if (e == 0 && !rope->is_attach_start_enabled()) {
			continue;
		}
		if (e == 1 && !rope->is_attach_end_enabled()) {
			continue;
		}
		const Vector3 p = inv.xform(rope->get_point_position(e == 0 ? 0 : count - 1));
		for (int axis = 0; axis < 3; axis++) {
			Vector3 d;
			d[axis] = mark;
			ends.push_back(p - d);
			ends.push_back(p + d);
		}
	}
	if (!ends.is_empty()) {
		p_gizmo->add_lines(ends, get_material("rope_material", p_gizmo));
	}
}
