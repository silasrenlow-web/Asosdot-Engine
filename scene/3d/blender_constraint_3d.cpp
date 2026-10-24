/**************************************************************************/
/*  blender_constraint_3d.cpp                                             */
/**************************************************************************/
/*  Asosdot: a Blender bone or object constraint, with Blender's math.    */
/**************************************************************************/

#include "blender_constraint_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/skeleton_3d.h"

namespace {

// A Skeleton3D imported from Blender is its armature turned from Z up to Y up.
// Bones keep their own axes, so only armature and world space need turning.
Transform3D godot_from_blender() {
	return Transform3D(Basis(Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0)), Vector3());
}

Transform3D blender_from_godot() {
	return godot_from_blender().affine_inverse();
}

// Blender writes matrices by column: m[column][row].
real_t at(const Basis &p_basis, int p_column, int p_row) {
	return p_basis.rows[p_row][p_column];
}

void put(Basis &r_basis, int p_column, int p_row, real_t p_value) {
	r_basis.rows[p_row][p_column] = p_value;
}

Basis normalized_columns(const Basis &p_basis) {
	Basis result = p_basis;
	for (int i = 0; i < 3; i++) {
		const Vector3 column = p_basis.get_column(i);
		const real_t length = column.length();
		result.set_column(i, length != 0 ? column / length : column);
	}
	return result;
}

Vector3 matrix_size(const Basis &p_basis) {
	return Vector3(p_basis.get_column(0).length(), p_basis.get_column(1).length(), p_basis.get_column(2).length());
}

// Location, rotation and scale, as Blender splits a matrix: a mirrored matrix
// gets a negative scale and a proper rotation.
void to_loc_rot_size(const Transform3D &p_matrix, Vector3 &r_loc, Basis &r_rot, Vector3 &r_size) {
	r_loc = p_matrix.origin;
	r_size = matrix_size(p_matrix.basis);
	r_rot = normalized_columns(p_matrix.basis);
	if (r_rot.determinant() < 0) {
		r_rot = r_rot * -1.0;
		r_size = -r_size;
	}
}

Transform3D from_loc_rot_size(const Vector3 &p_loc, const Basis &p_rot, const Vector3 &p_size) {
	Basis basis;
	for (int i = 0; i < 3; i++) {
		basis.set_column(i, p_rot.get_column(i) * p_size[i]);
	}
	return Transform3D(basis, p_loc);
}

// Shear removed with Y as the main axis, as Blender does for bones: X and Z leave
// Y's direction, then turn apart evenly until they are square to each other.
Basis orthogonalized_y(const Basis &p_basis, bool p_normalize) {
	Vector3 y = p_basis.get_column(1);
	Vector3 x = p_basis.get_column(0);
	Vector3 z = p_basis.get_column(2);
	const real_t y_length_squared = y.length_squared();
	if (y_length_squared > 0.0f) {
		x -= y * (x.dot(y) / y_length_squared);
		z -= y * (z.dot(y) / y_length_squared);
		if (p_normalize) {
			y /= Math::sqrt(y_length_squared);
		}
	}
	const real_t x_length = x.length();
	const real_t z_length = z.length();
	Vector3 x_direction = x_length > 0.0f ? x / x_length : x;
	Vector3 z_direction = z_length > 0.0f ? z / z_length : z;
	const real_t cosine = x_direction.dot(z_direction);
	if (Math::abs(cosine) > 1e-4f && Math::abs(cosine) < 1.0f - FLT_EPSILON) {
		const Vector3 middle = (x_direction + z_direction).normalized();
		const Vector3 apart = (x_direction - z_direction).normalized();
		x_direction = (middle + apart) * Math::SQRT12;
		z_direction = (middle - apart) * Math::SQRT12;
	}
	if (p_normalize) {
		return Basis(x_direction, y, z_direction);
	}
	return Basis(x_direction * x_length, y, z_direction * z_length);
}

// Blender's Euler rotation orders: the axes in order, and whether they turn the other way.
struct RotationOrder {
	int axis[3];
	bool parity;
};

const RotationOrder ROTATION_ORDERS[6] = {
	{ { 0, 1, 2 }, false }, // XYZ
	{ { 0, 2, 1 }, true }, // XZY
	{ { 1, 0, 2 }, true }, // YXZ
	{ { 1, 2, 0 }, false }, // YZX
	{ { 2, 0, 1 }, false }, // ZXY
	{ { 2, 1, 0 }, true }, // ZYX
};

int rotation_order_index(const String &p_order) {
	static const char *names[6] = { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" };
	for (int i = 0; i < 6; i++) {
		if (p_order == names[i]) {
			return i;
		}
	}
	return 0;
}

Basis euler_to_basis(const Vector3 &p_euler, int p_order) {
	const RotationOrder &order = ROTATION_ORDERS[p_order];
	const int i = order.axis[0];
	const int j = order.axis[1];
	const int k = order.axis[2];
	const double sign = order.parity ? -1.0 : 1.0;
	const double ti = p_euler[i] * sign;
	const double tj = p_euler[j] * sign;
	const double th = p_euler[k] * sign;
	const double ci = Math::cos(ti), cj = Math::cos(tj), ch = Math::cos(th);
	const double si = Math::sin(ti), sj = Math::sin(tj), sh = Math::sin(th);
	const double cc = ci * ch, cs = ci * sh, sc = si * ch, ss = si * sh;
	Basis m;
	put(m, i, i, cj * ch);
	put(m, j, i, sj * sc - cs);
	put(m, k, i, sj * cc + ss);
	put(m, i, j, cj * sh);
	put(m, j, j, sj * ss + cc);
	put(m, k, j, sj * cs - sc);
	put(m, i, k, -sj);
	put(m, j, k, cj * si);
	put(m, k, k, cj * ci);
	return m;
}

// The two Euler solutions of a rotation.
void basis_to_euler_pair(const Basis &p_basis, int p_order, Vector3 &r_first, Vector3 &r_second) {
	const RotationOrder &order = ROTATION_ORDERS[p_order];
	const int i = order.axis[0];
	const int j = order.axis[1];
	const int k = order.axis[2];
	const real_t cy = Math::sqrt(at(p_basis, i, i) * at(p_basis, i, i) + at(p_basis, i, j) * at(p_basis, i, j));
	if (cy > 16.0f * FLT_EPSILON) {
		r_first[i] = Math::atan2(at(p_basis, j, k), at(p_basis, k, k));
		r_first[j] = Math::atan2(-at(p_basis, i, k), cy);
		r_first[k] = Math::atan2(at(p_basis, i, j), at(p_basis, i, i));
		r_second[i] = Math::atan2(-at(p_basis, j, k), -at(p_basis, k, k));
		r_second[j] = Math::atan2(-at(p_basis, i, k), -cy);
		r_second[k] = Math::atan2(-at(p_basis, i, j), -at(p_basis, i, i));
	} else {
		r_first[i] = Math::atan2(-at(p_basis, k, j), at(p_basis, j, j));
		r_first[j] = Math::atan2(-at(p_basis, i, k), cy);
		r_first[k] = 0;
		r_second = r_first;
	}
	if (order.parity) {
		r_first = -r_first;
		r_second = -r_second;
	}
}

// Of the two solutions, the one with the smaller angles.
Vector3 basis_to_euler(const Basis &p_basis, int p_order) {
	Vector3 first, second;
	basis_to_euler_pair(normalized_columns(p_basis), p_order, first, second);
	const real_t d1 = Math::abs(first.x) + Math::abs(first.y) + Math::abs(first.z);
	const real_t d2 = Math::abs(second.x) + Math::abs(second.y) + Math::abs(second.z);
	return d1 > d2 ? second : first;
}

// Moves each angle by whole turns to be near the old one, as Blender does when it keys rotations.
void make_euler_compatible(Vector3 &r_euler, const Vector3 &p_old) {
	const real_t pi_thresh = 5.1f;
	const real_t pi_x2 = 2.0f * Math::PI;
	Vector3 delta;
	for (int i = 0; i < 3; i++) {
		delta[i] = r_euler[i] - p_old[i];
		if (delta[i] > pi_thresh) {
			r_euler[i] -= Math::floor((delta[i] / pi_x2) + 0.5f) * pi_x2;
			delta[i] = r_euler[i] - p_old[i];
		} else if (delta[i] < -pi_thresh) {
			r_euler[i] += Math::floor((-delta[i] / pi_x2) + 0.5f) * pi_x2;
			delta[i] = r_euler[i] - p_old[i];
		}
	}
	for (int i = 0; i < 3; i++) {
		const int a = (i + 1) % 3;
		const int b = (i + 2) % 3;
		if (Math::abs(delta[i]) > 3.2f && Math::abs(delta[a]) < 1.6f && Math::abs(delta[b]) < 1.6f) {
			r_euler[i] += delta[i] > 0.0f ? -pi_x2 : pi_x2;
		}
	}
}

Vector3 basis_to_compatible_euler(const Basis &p_basis, const Vector3 &p_old, int p_order) {
	Vector3 first, second;
	basis_to_euler_pair(normalized_columns(p_basis), p_order, first, second);
	make_euler_compatible(first, p_old);
	make_euler_compatible(second, p_old);
	const real_t d1 = Math::abs(first.x - p_old.x) + Math::abs(first.y - p_old.y) + Math::abs(first.z - p_old.z);
	const real_t d2 = Math::abs(second.x - p_old.x) + Math::abs(second.y - p_old.y) + Math::abs(second.z - p_old.z);
	return d1 > d2 ? second : first;
}

// Turns an Euler rotation about one axis, as the old Copy Rotation offset did.
void rotate_euler(Vector3 &r_euler, int p_order, int p_axis, real_t p_angle) {
	Vector3 turn;
	turn[p_axis] = p_angle;
	r_euler = basis_to_euler(euler_to_basis(r_euler, p_order) * euler_to_basis(turn, p_order), p_order);
}

// A rotation's angle, held to a range of angles. Outside it, the angle goes to the
// nearer end, counting the way around through a half turn.
real_t limit_angle(real_t p_angle, real_t p_min, real_t p_max, bool p_legacy) {
	if (p_legacy) {
		return CLAMP(p_angle, p_min, p_max);
	}
	if (p_max - p_min >= Math::TAU) {
		return p_angle;
	}
	if (p_max <= p_min) {
		return p_min;
	}
	const real_t min_wrapped = Math::wrapf(p_min - p_angle, (real_t)-Math::PI, (real_t)Math::PI);
	const real_t max_wrapped = Math::wrapf(p_max - p_angle, (real_t)-Math::PI, (real_t)Math::PI);
	if (min_wrapped < max_wrapped) {
		return p_angle + CLAMP((real_t)0.0, min_wrapped, max_wrapped);
	}
	// The range crosses the half turn.
	if (max_wrapped >= 0.0f || min_wrapped <= 0.0f) {
		return p_angle;
	}
	return Math::abs(max_wrapped) < Math::abs(min_wrapped) ? p_angle + max_wrapped : p_angle + min_wrapped;
}

int axis_index(const String &p_value, const char *const *p_names, int p_count, int p_default) {
	for (int i = 0; i < p_count; i++) {
		if (p_value == p_names[i]) {
			return i;
		}
	}
	return p_default;
}

const char *const TRACK_AXES[6] = { "TRACK_X", "TRACK_Y", "TRACK_Z", "TRACK_NEGATIVE_X", "TRACK_NEGATIVE_Y", "TRACK_NEGATIVE_Z" };
const char *const AXIS_LETTERS[3] = { "X", "Y", "Z" };

// Damped Track: the least turn that points the axis at the target.
void damped_track(Transform3D &r_matrix, const Vector3 &p_target, int p_track_axis) {
	static const Vector3 directions[6] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(-1, 0, 0), Vector3(0, -1, 0), Vector3(0, 0, -1) };
	Vector3 target = p_target - r_matrix.origin;
	if (target.length() == 0.0f) {
		return;
	}
	target.normalize();
	Vector3 current = r_matrix.basis.xform(directions[p_track_axis]);
	if (current.length() == 0.0f) {
		current = directions[p_track_axis];
	}
	current.normalize();
	Vector3 axis = current.cross(target);
	real_t angle = Math::acos(CLAMP(current.dot(target), -1.0f, 1.0f));
	const real_t norm = axis.length();
	if (norm < FLT_EPSILON) {
		// Opposite directions: turn half a turn about another of the owner's axes.
		if (Math::abs(angle) < Math::PI - 0.01f) {
			return;
		}
		angle = Math::PI;
		axis = current.cross(r_matrix.basis.xform(directions[(p_track_axis + 1) % 6]));
		if (axis.length() == 0.0f) {
			return;
		}
		axis.normalize();
	} else {
		axis /= norm;
		if (norm < 0.1f) {
			// Near no turn or half a turn, the arcsine is the more exact.
			angle = angle > Math::PI / 2 ? Math::PI - Math::asin(norm) : Math::asin(norm);
		}
	}
	const Vector3 origin = r_matrix.origin;
	r_matrix = Transform3D(Basis(axis, angle), Vector3()) * r_matrix;
	r_matrix.origin = origin;
}

// Track To's rotation: the track axis toward the target, the up axis toward up.
Basis track_to_basis(const Vector3 &p_vector, const Vector3 &p_up, int p_track_axis, int p_up_axis) {
	Vector3 n = p_vector;
	if (n.length() == 0.0f) {
		n = Vector3(0, 0, 1);
	} else {
		n.normalize();
	}
	int axis = p_track_axis;
	if (axis > 2) {
		axis -= 3;
	} else {
		n = -n;
	}
	Vector3 projection = p_up - n * (p_up.dot(n) / n.dot(n));
	if (projection.length() == 0.0f) {
		projection = Vector3(0, 1, 0);
	} else {
		projection.normalize();
	}
	Vector3 right = projection.cross(n).normalized();
	if (axis == p_up_axis) {
		return Basis();
	}
	const int right_index = 3 - axis - p_up_axis;
	const int difference = axis - p_up_axis;
	const real_t sign = (difference == 1 || difference == -2) ? 1.0f : -1.0f;
	Basis m;
	m.set_column(right_index, right * sign);
	m.set_column(p_up_axis, projection);
	m.set_column(axis, n);
	return m;
}

// The turn between two directions.
Basis rotation_between(const Vector3 &p_from, const Vector3 &p_to) {
	Vector3 axis = p_from.cross(p_to);
	const real_t sine = axis.length();
	const real_t cosine = p_from.dot(p_to);
	if (sine > FLT_EPSILON) {
		return Basis(axis / sine, Math::atan2(sine, cosine));
	}
	if (cosine > 0.0f) {
		return Basis();
	}
	// Opposite: half a turn about any perpendicular axis.
	Vector3 perpendicular = Math::abs(p_from.x) < 0.9f ? p_from.cross(Vector3(1, 0, 0)) : p_from.cross(Vector3(0, 1, 0));
	return Basis(perpendicular.normalized(), Math::PI);
}

} // namespace

const char *BlenderConstraint3D::_get_type_name(ConstraintType p_type) {
	static const char *names[TYPE_MAX] = { "COPY_LOCATION", "COPY_ROTATION", "COPY_SCALE", "COPY_TRANSFORMS", "LIMIT_LOCATION", "LIMIT_ROTATION", "LIMIT_SCALE", "CHILD_OF", "DAMPED_TRACK", "TRACK_TO", "LOCKED_TRACK", "STRETCH_TO", "TRANSFORM", "MAINTAIN_VOLUME" };
	return p_type >= 0 && p_type < TYPE_MAX ? names[p_type] : "";
}

// The settings of each type, with Blender's names and defaults.
void BlenderConstraint3D::_get_type_settings(ConstraintType p_type, List<PropertyInfo> *r_list, Dictionary *r_defaults) {
	auto add = [&](const PropertyInfo &p_info, const Variant &p_default) {
		if (r_list) {
			r_list->push_back(p_info);
		}
		if (r_defaults) {
			(*r_defaults)[p_info.name] = p_default;
		}
	};
	auto add_bool = [&](const String &p_name, bool p_default) {
		add(PropertyInfo(Variant::BOOL, p_name), p_default);
	};
	auto add_float = [&](const String &p_name, float p_default, const String &p_hint = String()) {
		add(PropertyInfo(Variant::FLOAT, p_name, p_hint.is_empty() ? PROPERTY_HINT_NONE : PROPERTY_HINT_RANGE, p_hint), p_default);
	};
	auto add_enum = [&](const String &p_name, const String &p_options, const String &p_default) {
		add(PropertyInfo(Variant::STRING, p_name, PROPERTY_HINT_ENUM, p_options), p_default);
	};
	auto add_axes = [&](const String &p_prefix, const String &p_suffix, bool p_default) {
		for (const char *axis : { "x", "y", "z" }) {
			add_bool(p_prefix + axis + p_suffix, p_default);
		}
	};
	auto add_limits = [&](const String &p_suffix, float p_default) {
		for (const char *axis : { "x", "y", "z" }) {
			add_float(String("min_") + axis + p_suffix, p_default);
			add_float(String("max_") + axis + p_suffix, p_default);
		}
	};
	const String euler_orders = "AUTO,XYZ,XZY,YXZ,YZX,ZXY,ZYX";
	const String track_axes = "TRACK_X,TRACK_Y,TRACK_Z,TRACK_NEGATIVE_X,TRACK_NEGATIVE_Y,TRACK_NEGATIVE_Z";
	switch (p_type) {
		case TYPE_COPY_LOCATION:
			add_axes("use_", "", true);
			add_axes("invert_", "", false);
			add_bool("use_offset", false);
			break;
		case TYPE_COPY_ROTATION:
			add_axes("use_", "", true);
			add_axes("invert_", "", false);
			add_enum("mix_mode", "REPLACE,ADD,BEFORE,AFTER,OFFSET", "REPLACE");
			add_enum("euler_order", euler_orders, "AUTO");
			break;
		case TYPE_COPY_SCALE:
			add_axes("use_", "", true);
			add_float("power", 1.0);
			add_bool("use_make_uniform", false);
			add_bool("use_offset", false);
			add_bool("use_add", false);
			break;
		case TYPE_COPY_TRANSFORMS:
			add_enum("mix_mode", "REPLACE,BEFORE_FULL,BEFORE,BEFORE_SPLIT,AFTER_FULL,AFTER,AFTER_SPLIT", "REPLACE");
			add_bool("remove_target_shear", false);
			break;
		case TYPE_LIMIT_LOCATION:
		case TYPE_LIMIT_SCALE:
			add_axes("use_min_", "", false);
			add_axes("use_max_", "", false);
			add_limits("", p_type == TYPE_LIMIT_SCALE ? 1.0 : 0.0);
			break;
		case TYPE_LIMIT_ROTATION:
			add_axes("use_limit_", "", false);
			add_limits("", 0.0);
			add_enum("euler_order", euler_orders, "AUTO");
			add_bool("use_legacy_behavior", false);
			break;
		case TYPE_CHILD_OF:
			add_axes("use_location_", "", true);
			add_axes("use_rotation_", "", true);
			add_axes("use_scale_", "", true);
			add(PropertyInfo(Variant::TRANSFORM3D, "inverse_matrix"), Transform3D());
			break;
		case TYPE_DAMPED_TRACK:
			add_enum("track_axis", track_axes, "TRACK_Y");
			break;
		case TYPE_TRACK_TO:
			add_enum("track_axis", track_axes, "TRACK_Y");
			add_enum("up_axis", "UP_X,UP_Y,UP_Z", "UP_Z");
			add_bool("use_target_z", false);
			break;
		case TYPE_LOCKED_TRACK:
			add_enum("track_axis", track_axes, "TRACK_Y");
			add_enum("lock_axis", "LOCK_X,LOCK_Y,LOCK_Z", "LOCK_Z");
			break;
		case TYPE_STRETCH_TO:
			add_float("rest_length", 0.0, "0,100,0.001,or_greater,suffix:m");
			add_float("bulge", 1.0);
			add_bool("use_bulge_min", false);
			add_bool("use_bulge_max", false);
			add_float("bulge_min", 1.0);
			add_float("bulge_max", 1.0);
			add_float("bulge_smooth", 0.0, "0,1,0.01");
			add_enum("volume", "VOLUME_XZX,VOLUME_X,VOLUME_Z,NO_VOLUME", "VOLUME_XZX");
			add_enum("keep_axis", "PLANE_X,PLANE_Z,SWING_Y", "PLANE_X");
			break;
		case TYPE_TRANSFORMATION:
			add_enum("map_from", "LOCATION,ROTATION,SCALE", "LOCATION");
			add_enum("map_to", "LOCATION,ROTATION,SCALE", "LOCATION");
			for (const char *axis : { "x", "y", "z" }) {
				add_enum(String("map_to_") + axis + "_from", "X,Y,Z", String(axis).to_upper());
			}
			add_bool("use_motion_extrapolate", false);
			add_enum("from_rotation_mode", euler_orders + ",QUATERNION", "AUTO");
			add_enum("to_euler_order", euler_orders, "AUTO");
			add_enum("mix_mode", "REPLACE,ADD", "ADD");
			add_enum("mix_mode_rot", "REPLACE,ADD,BEFORE,AFTER", "ADD");
			add_enum("mix_mode_scale", "REPLACE,MULTIPLY", "REPLACE");
			for (const char *direction : { "from", "to" }) {
				for (const char *axis : { "x", "y", "z" }) {
					for (const char *bound : { "min", "max" }) {
						const String name = String(direction) + "_" + bound + "_" + axis;
						add_float(name, 0.0);
						add_float(name + "_rot", 0.0);
						add_float(name + "_scale", 1.0);
					}
				}
			}
			break;
		case TYPE_MAINTAIN_VOLUME:
			add_enum("mode", "STRICT,UNIFORM,SINGLE_AXIS", "STRICT");
			add_enum("free_axis", "SAMEVOL_X,SAMEVOL_Y,SAMEVOL_Z", "SAMEVOL_Y");
			add_float("volume", 1.0);
			break;
		default:
			break;
	}
}

Variant BlenderConstraint3D::_get_setting(const StringName &p_name) const {
	if (settings.has(p_name)) {
		return settings[p_name];
	}
	Dictionary defaults;
	_get_type_settings(constraint_type, nullptr, &defaults);
	return defaults.get(p_name, Variant());
}

bool BlenderConstraint3D::_set(const StringName &p_name, const Variant &p_value) {
	Dictionary defaults;
	_get_type_settings(constraint_type, nullptr, &defaults);
	if (!defaults.has(p_name)) {
		return false;
	}
	settings[p_name] = p_value;
	return true;
}

bool BlenderConstraint3D::_get(const StringName &p_name, Variant &r_ret) const {
	Dictionary defaults;
	_get_type_settings(constraint_type, nullptr, &defaults);
	if (!defaults.has(p_name)) {
		return false;
	}
	r_ret = settings.get(p_name, defaults[p_name]);
	return true;
}

void BlenderConstraint3D::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::NIL, vformat("%s Settings", String(_get_type_name(constraint_type)).capitalize()), PROPERTY_HINT_NONE, "", PROPERTY_USAGE_GROUP));
	_get_type_settings(constraint_type, p_list, nullptr);
}

void BlenderConstraint3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "bone_name" || p_property.name == "target_bone_name") {
		Skeleton3D *skeleton = get_skeleton();
		if (skeleton) {
			p_property.hint = PROPERTY_HINT_ENUM_SUGGESTION;
			p_property.hint_string = skeleton->get_concatenated_bone_names();
		}
	}
	if (p_property.name == "bone_name" && !owner_node.is_empty()) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if ((p_property.name == "target_bone_name" || p_property.name == "target_node" || p_property.name == "target_space") && !needs_target()) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if ((p_property.name == "head_tail" || p_property.name == "target_bone_length") && target_bone_name.is_empty()) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

void BlenderConstraint3D::_validate_bone_names() {
	if (!bone_name.is_empty()) {
		set_bone_name(bone_name);
	}
}

void BlenderConstraint3D::set_constraint_type(ConstraintType p_type) {
	ERR_FAIL_INDEX(p_type, TYPE_MAX);
	constraint_type = p_type;
	notify_property_list_changed();
}

BlenderConstraint3D::ConstraintType BlenderConstraint3D::get_constraint_type() const {
	return constraint_type;
}

void BlenderConstraint3D::set_bone_name(const String &p_bone_name) {
	bone_name = p_bone_name;
	Skeleton3D *skeleton = get_skeleton();
	if (skeleton) {
		bone = skeleton->find_bone(bone_name);
	}
}

String BlenderConstraint3D::get_bone_name() const {
	return bone_name;
}

void BlenderConstraint3D::set_bone(int p_bone) {
	bone = p_bone;
	Skeleton3D *skeleton = get_skeleton();
	if (skeleton && bone >= 0 && bone < skeleton->get_bone_count()) {
		bone_name = skeleton->get_bone_name(bone);
	}
}

int BlenderConstraint3D::get_bone() const {
	return bone;
}

void BlenderConstraint3D::set_owner_node(const NodePath &p_node) {
	owner_node = p_node;
	node_input_known = false;
	notify_property_list_changed();
}

NodePath BlenderConstraint3D::get_owner_node() const {
	return owner_node;
}

void BlenderConstraint3D::set_target_bone_name(const String &p_bone_name) {
	target_bone_name = p_bone_name;
	notify_property_list_changed();
}

String BlenderConstraint3D::get_target_bone_name() const {
	return target_bone_name;
}

void BlenderConstraint3D::set_target_node(const NodePath &p_node) {
	target_node = p_node;
}

NodePath BlenderConstraint3D::get_target_node() const {
	return target_node;
}

void BlenderConstraint3D::set_head_tail(float p_head_tail) {
	head_tail = p_head_tail;
}

float BlenderConstraint3D::get_head_tail() const {
	return head_tail;
}

void BlenderConstraint3D::set_target_bone_length(float p_length) {
	target_bone_length = p_length;
}

float BlenderConstraint3D::get_target_bone_length() const {
	return target_bone_length;
}

void BlenderConstraint3D::set_owner_space(Space p_space) {
	owner_space = p_space;
}

BlenderConstraint3D::Space BlenderConstraint3D::get_owner_space() const {
	return owner_space;
}

void BlenderConstraint3D::set_target_space(Space p_space) {
	target_space = p_space;
}

BlenderConstraint3D::Space BlenderConstraint3D::get_target_space() const {
	return target_space;
}

void BlenderConstraint3D::set_owner_rotation_order(const String &p_order) {
	owner_rotation_order = p_order;
}

String BlenderConstraint3D::get_owner_rotation_order() const {
	return owner_rotation_order;
}

void BlenderConstraint3D::set_target_rotation_order(const String &p_order) {
	target_rotation_order = p_order;
}

String BlenderConstraint3D::get_target_rotation_order() const {
	return target_rotation_order;
}

void BlenderConstraint3D::set_settings(const Dictionary &p_settings) {
	settings = p_settings.duplicate();
	notify_property_list_changed();
}

Dictionary BlenderConstraint3D::get_settings() const {
	Dictionary result;
	Dictionary defaults;
	_get_type_settings(constraint_type, nullptr, &defaults);
	for (const KeyValue<Variant, Variant> &E : defaults) {
		result[E.key] = settings.get(E.key, E.value);
	}
	return result;
}

void BlenderConstraint3D::set_setting(const StringName &p_name, const Variant &p_value) {
	settings[p_name] = p_value;
}

Variant BlenderConstraint3D::get_setting(const StringName &p_name) const {
	return _get_setting(p_name);
}

bool BlenderConstraint3D::needs_target() const {
	return constraint_type != TYPE_LIMIT_LOCATION && constraint_type != TYPE_LIMIT_ROTATION && constraint_type != TYPE_LIMIT_SCALE && constraint_type != TYPE_MAINTAIN_VOLUME;
}

// The armature object's world matrix, in Blender's axes.
Transform3D BlenderConstraint3D::_get_armature_world(const Skeleton3D *p_skeleton) const {
	return blender_from_godot() * p_skeleton->get_global_transform() * godot_from_blender();
}

// Where a bone's local space sits in the armature: its parent's pose and its rest offset.
Transform3D BlenderConstraint3D::_get_bone_parent_matrix(const Skeleton3D *p_skeleton, int p_bone) const {
	const int parent = p_skeleton->get_bone_parent(p_bone);
	const Transform3D parent_pose = parent >= 0 ? blender_from_godot() * p_skeleton->get_bone_global_pose(parent) : blender_from_godot();
	return parent_pose * p_skeleton->get_bone_rest(p_bone);
}

Transform3D BlenderConstraint3D::_to_space(const Skeleton3D *p_skeleton, const Transform3D &p_world, Space p_space, int p_bone) const {
	if (p_space == SPACE_WORLD || p_bone < 0) {
		return p_world;
	}
	const Transform3D pose = _get_armature_world(p_skeleton).affine_inverse() * p_world;
	switch (p_space) {
		case SPACE_LOCAL_WITH_PARENT:
			return (blender_from_godot() * p_skeleton->get_bone_global_rest(p_bone)).affine_inverse() * pose;
		case SPACE_LOCAL:
			return _get_bone_parent_matrix(p_skeleton, p_bone).affine_inverse() * pose;
		default:
			return pose;
	}
}

Transform3D BlenderConstraint3D::_from_space(const Skeleton3D *p_skeleton, const Transform3D &p_matrix, Space p_space, int p_bone) const {
	if (p_space == SPACE_WORLD || p_bone < 0) {
		return p_matrix;
	}
	Transform3D pose = p_matrix;
	if (p_space == SPACE_LOCAL_WITH_PARENT) {
		pose = (blender_from_godot() * p_skeleton->get_bone_global_rest(p_bone)) * p_matrix;
	} else if (p_space == SPACE_LOCAL) {
		pose = _get_bone_parent_matrix(p_skeleton, p_bone) * p_matrix;
	}
	return _get_armature_world(p_skeleton) * pose;
}

bool BlenderConstraint3D::_get_target_matrix(const Skeleton3D *p_skeleton, Transform3D &r_matrix) const {
	if (!target_bone_name.is_empty()) {
		const int target_bone = p_skeleton->find_bone(target_bone_name);
		if (target_bone < 0) {
			return false;
		}
		Transform3D pose = blender_from_godot() * p_skeleton->get_bone_global_pose(target_bone);
		if (head_tail > 0.000001f) {
			// Along the bone from its head, toward its tail.
			pose.origin = pose.xform(Vector3(0, target_bone_length * head_tail, 0));
		}
		r_matrix = _to_space(p_skeleton, _get_armature_world(p_skeleton) * pose, target_space, target_bone);
		return true;
	}
	if (!target_node.is_empty()) {
		const Node3D *node = Object::cast_to<Node3D>(get_node_or_null(target_node));
		if (!node) {
			return false;
		}
		// An object's own axes are turned as well, not only its place.
		r_matrix = blender_from_godot() * node->get_global_transform() * godot_from_blender();
		return true;
	}
	return false;
}

void BlenderConstraint3D::_evaluate(Transform3D &r_owner, const Transform3D &p_target, bool p_has_target) const {
	auto flag = [&](const String &p_name) -> bool {
		return _get_setting(p_name);
	};
	auto number = [&](const String &p_name) -> real_t {
		return _get_setting(p_name);
	};
	auto text = [&](const String &p_name) -> String {
		return _get_setting(p_name);
	};
	static const char *axes[3] = { "x", "y", "z" };
	const int owner_order = rotation_order_index(owner_rotation_order);
	auto order_setting = [&](const String &p_name) -> int {
		const String order = text(p_name);
		return order == "AUTO" ? owner_order : rotation_order_index(order);
	};

	switch (constraint_type) {
		case TYPE_COPY_LOCATION: {
			const Vector3 offset = flag("use_offset") ? r_owner.origin : Vector3();
			for (int i = 0; i < 3; i++) {
				if (flag(String("use_") + axes[i])) {
					r_owner.origin[i] = (flag(String("invert_") + axes[i]) ? -p_target.origin[i] : p_target.origin[i]) + offset[i];
				}
			}
		} break;

		case TYPE_COPY_ROTATION: {
			Vector3 loc, size;
			Basis old_rotation;
			to_loc_rot_size(r_owner, loc, old_rotation, size);
			const int order = order_setting("euler_order");
			const Vector3 owner_euler = basis_to_euler(r_owner.basis, order);
			Vector3 euler = basis_to_compatible_euler(orthogonalized_y(p_target.basis, true), owner_euler, order);
			const String mix = text("mix_mode");
			const bool legacy_offset = mix == "OFFSET";
			const Vector3 kept = (mix == "REPLACE" || legacy_offset) ? owner_euler : Vector3();
			for (int i = 0; i < 3; i++) {
				if (!flag(String("use_") + axes[i])) {
					euler[i] = kept[i];
				} else {
					if (legacy_offset) {
						rotate_euler(euler, order, i, owner_euler[i]);
					}
					if (flag(String("invert_") + axes[i])) {
						euler[i] = -euler[i];
					}
				}
			}
			if (mix == "ADD") {
				euler += owner_euler;
			}
			make_euler_compatible(euler, owner_euler);
			Basis rotation = euler_to_basis(euler, order);
			if (mix == "BEFORE") {
				rotation = rotation * old_rotation;
			} else if (mix == "AFTER") {
				rotation = old_rotation * rotation;
			}
			r_owner = from_loc_rot_size(loc, rotation, size);
		} break;

		case TYPE_COPY_SCALE: {
			const Vector3 owner_size = matrix_size(r_owner.basis);
			Vector3 size;
			if (flag("use_make_uniform")) {
				real_t total = 1.0;
				if (flag("use_x") && flag("use_y") && flag("use_z")) {
					total = Math::abs(p_target.basis.determinant());
				} else {
					const Vector3 target_size = matrix_size(p_target.basis);
					for (int i = 0; i < 3; i++) {
						if (flag(String("use_") + axes[i])) {
							total *= target_size[i];
						}
					}
				}
				const real_t uniform = Math::pow(total, (real_t)(1.0 / 3.0));
				size = Vector3(uniform, uniform, uniform);
			} else {
				size = matrix_size(p_target.basis);
			}
			const real_t power = number("power");
			for (int i = 0; i < 3; i++) {
				size[i] = Math::pow(size[i], power);
			}
			if (flag("use_offset")) {
				if (flag("use_add")) {
					size += owner_size - Vector3(1, 1, 1);
				} else {
					size *= owner_size;
				}
			}
			for (int i = 0; i < 3; i++) {
				if ((flag(String("use_") + axes[i]) || flag("use_make_uniform")) && owner_size[i] != 0) {
					r_owner.basis.set_column(i, r_owner.basis.get_column(i) * (size[i] / owner_size[i]));
				}
			}
		} break;

		case TYPE_COPY_TRANSFORMS: {
			Transform3D target = p_target;
			if (flag("remove_target_shear")) {
				target.basis = orthogonalized_y(target.basis, false);
			}
			const String mix = text("mix_mode");
			// Inherit Scale emulations: location and rotation as a parent, scales multiplied per axis.
			auto aligned = [](const Transform3D &p_a, const Transform3D &p_b, bool p_split) {
				Vector3 loc_a, size_a, loc_b, size_b;
				Basis rot_a, rot_b;
				to_loc_rot_size(p_a, loc_a, rot_a, size_a);
				to_loc_rot_size(p_b, loc_b, rot_b, size_b);
				const Vector3 loc = p_split ? loc_a + loc_b : p_a.xform(loc_b);
				return from_loc_rot_size(loc, rot_a * rot_b, size_a * size_b);
			};
			if (mix == "BEFORE_FULL") {
				r_owner = target * r_owner;
			} else if (mix == "AFTER_FULL") {
				r_owner = r_owner * target;
			} else if (mix == "BEFORE") {
				r_owner = aligned(target, r_owner, false);
			} else if (mix == "AFTER") {
				r_owner = aligned(r_owner, target, false);
			} else if (mix == "BEFORE_SPLIT") {
				r_owner = aligned(target, r_owner, true);
			} else if (mix == "AFTER_SPLIT") {
				r_owner = aligned(r_owner, target, true);
			} else {
				r_owner = target;
			}
		} break;

		case TYPE_LIMIT_LOCATION: {
			for (int i = 0; i < 3; i++) {
				if (flag(String("use_min_") + axes[i])) {
					r_owner.origin[i] = MAX(r_owner.origin[i], number(String("min_") + axes[i]));
				}
				if (flag(String("use_max_") + axes[i])) {
					r_owner.origin[i] = MIN(r_owner.origin[i], number(String("max_") + axes[i]));
				}
			}
		} break;

		case TYPE_LIMIT_SCALE: {
			const Vector3 old_size = matrix_size(r_owner.basis);
			Vector3 size = old_size;
			for (int i = 0; i < 3; i++) {
				if (flag(String("use_min_") + axes[i])) {
					size[i] = MAX(size[i], number(String("min_") + axes[i]));
				}
				if (flag(String("use_max_") + axes[i])) {
					size[i] = MIN(size[i], number(String("max_") + axes[i]));
				}
				if (old_size[i] != 0) {
					r_owner.basis.set_column(i, r_owner.basis.get_column(i) * (size[i] / old_size[i]));
				}
			}
		} break;

		case TYPE_LIMIT_ROTATION: {
			const Vector3 size = matrix_size(r_owner.basis);
			const int order = order_setting("euler_order");
			Vector3 euler = basis_to_euler(orthogonalized_y(r_owner.basis, true), order);
			const bool legacy = flag("use_legacy_behavior");
			for (int i = 0; i < 3; i++) {
				if (flag(String("use_limit_") + axes[i])) {
					euler[i] = limit_angle(euler[i], number(String("min_") + axes[i]), number(String("max_") + axes[i]), legacy);
				}
			}
			r_owner = from_loc_rot_size(r_owner.origin, euler_to_basis(euler, order), size);
		} break;

		case TYPE_CHILD_OF: {
			const Transform3D inverse_matrix = _get_setting("inverse_matrix");
			bool all = true;
			for (const char *channel : { "use_location_", "use_rotation_", "use_scale_" }) {
				for (int i = 0; i < 3; i++) {
					all = all && flag(String(channel) + axes[i]);
				}
			}
			Transform3D parent = p_target;
			Transform3D inverse = inverse_matrix;
			if (!all) {
				// Only the chosen channels of the target, and of the inverse, apply.
				const int target_order = rotation_order_index(target_rotation_order);
				Vector3 loc = p_target.origin;
				Vector3 euler = basis_to_euler(p_target.basis, target_order);
				Vector3 size = matrix_size(p_target.basis);
				Vector3 inverse_loc = inverse_matrix.origin;
				Vector3 inverse_euler = basis_to_euler(inverse_matrix.basis, owner_order);
				Vector3 inverse_size = matrix_size(inverse_matrix.basis);
				for (int i = 0; i < 3; i++) {
					if (!flag(String("use_location_") + axes[i])) {
						loc[i] = inverse_loc[i] = 0.0;
					}
					if (!flag(String("use_rotation_") + axes[i])) {
						euler[i] = inverse_euler[i] = 0.0;
					}
					if (!flag(String("use_scale_") + axes[i])) {
						size[i] = inverse_size[i] = 1.0;
					}
				}
				parent = from_loc_rot_size(loc, euler_to_basis(euler, target_order), size);
				inverse = from_loc_rot_size(inverse_loc, euler_to_basis(inverse_euler, owner_order), inverse_size);
			}
			r_owner = parent * inverse * r_owner;
		} break;

		case TYPE_DAMPED_TRACK: {
			damped_track(r_owner, p_target.origin, axis_index(text("track_axis"), TRACK_AXES, 6, 1));
		} break;

		case TYPE_TRACK_TO: {
			static const char *up_axes[3] = { "UP_X", "UP_Y", "UP_Z" };
			const Vector3 size = matrix_size(r_owner.basis);
			const Vector3 up = flag("use_target_z") ? p_target.basis.get_column(2) : Vector3(0, 0, 1);
			const Basis rotation = track_to_basis(r_owner.origin - p_target.origin, up, axis_index(text("track_axis"), TRACK_AXES, 6, 1), axis_index(text("up_axis"), up_axes, 3, 2));
			r_owner.basis = rotation * Basis::from_scale(size);
		} break;

		case TYPE_LOCKED_TRACK: {
			static const char *lock_axes[3] = { "LOCK_X", "LOCK_Y", "LOCK_Z" };
			const int lock = axis_index(text("lock_axis"), lock_axes, 3, 2);
			const int track = axis_index(text("track_axis"), TRACK_AXES, 6, 1);
			const int track_axis = track % 3;
			if (track_axis == lock) {
				break;
			}
			const Vector3 to_target = p_target.origin - r_owner.origin;
			const Vector3 locked = r_owner.basis.get_column(lock).normalized();
			Vector3 tracked = (to_target - locked * to_target.dot(locked)).normalized();
			if (track >= 3) {
				tracked = -tracked;
			}
			Basis rotation;
			rotation.set_column(lock, locked);
			rotation.set_column(track_axis, tracked);
			const int third = 3 - lock - track_axis;
			rotation.set_column(third, rotation.get_column((third + 1) % 3).cross(rotation.get_column((third + 2) % 3)));
			// The turn from the owner's rotation to the tracking one, applied to the owner.
			Basis relative = rotation * normalized_columns(r_owner.basis).inverse();
			if (relative.determinant() == 0) {
				relative = Basis();
			}
			r_owner.basis = relative * r_owner.basis;
		} break;

		case TYPE_STRETCH_TO: {
			Vector3 size = matrix_size(r_owner.basis);
			const Basis normalized = normalized_columns(r_owner.basis);
			const Vector3 old_x = normalized.get_column(0);
			const Vector3 old_y = normalized.get_column(1);
			const Vector3 old_z = normalized.get_column(2);
			Vector3 direction = p_target.origin - r_owner.origin;
			real_t distance = direction.length();
			if (distance == 0.0f) {
				break;
			}
			direction /= distance;
			distance /= size.y;
			real_t rest_length = number("rest_length");
			if (rest_length == 0.0f) {
				rest_length = distance;
			}
			Vector3 scale;
			scale.y = distance / rest_length;
			real_t bulge = Math::pow(rest_length / distance, number("bulge"));
			if (bulge > 1.0f && flag("use_bulge_max")) {
				const real_t bulge_max = MAX(number("bulge_max"), (real_t)1.0);
				const real_t hard = MIN(bulge, bulge_max);
				const real_t range = bulge_max - 1.0f;
				const real_t factor = range > 0.0f ? 1.0f / range : 0.0f;
				const real_t soft = 1.0f + range * Math::atan((bulge - 1.0f) * factor) / (Math::PI / 2);
				bulge = Math::lerp(soft, hard, number("bulge_smooth"));
			}
			if (bulge < 1.0f && flag("use_bulge_min")) {
				const real_t bulge_min = CLAMP(number("bulge_min"), (real_t)0.0, (real_t)1.0);
				const real_t hard = MAX(bulge, bulge_min);
				const real_t range = 1.0f - bulge_min;
				const real_t factor = range > 0.0f ? 1.0f / range : 0.0f;
				const real_t soft = 1.0f - range * Math::atan((1.0f - bulge) * factor) / (Math::PI / 2);
				bulge = Math::lerp(soft, hard, number("bulge_smooth"));
			}
			const String volume = text("volume");
			if (volume == "VOLUME_X") {
				scale.x = bulge;
				scale.z = 1.0;
			} else if (volume == "VOLUME_Z") {
				scale.x = 1.0;
				scale.z = bulge;
			} else if (volume == "NO_VOLUME") {
				scale.x = 1.0;
				scale.z = 1.0;
			} else {
				scale.x = Math::sqrt(bulge);
				scale.z = scale.x;
			}
			size *= scale;
			Basis rotation;
			const String keep_axis = text("keep_axis");
			if (keep_axis == "SWING_Y") {
				const Basis swing = rotation_between(old_y, direction);
				rotation = Basis(swing.xform(old_x), direction, swing.xform(old_z));
			} else if (keep_axis == "PLANE_Z") {
				const Vector3 orthogonal = old_z.cross(direction).normalized();
				rotation = Basis(-orthogonal, direction, orthogonal.cross(direction).normalized());
			} else {
				const Vector3 orthogonal = old_x.cross(direction).normalized();
				rotation = Basis(direction.cross(orthogonal).normalized(), direction, orthogonal);
			}
			r_owner.basis = rotation * Basis::from_scale(size);
		} break;

		case TYPE_TRANSFORMATION: {
			const String from = text("map_from");
			Vector3 input;
			String suffix;
			if (from == "SCALE") {
				input = matrix_size(p_target.basis);
				if (p_target.basis.determinant() < 0) {
					input = -input;
				}
				suffix = "_scale";
			} else if (from == "ROTATION") {
				if (text("from_rotation_mode") == "QUATERNION") {
					// Blender's stand-in angles for a quaternion: twice the arcsine of each part.
					Quaternion rotation = normalized_columns(p_target.basis).get_rotation_quaternion();
					if (rotation.w < 0.0f) {
						rotation = -rotation;
					}
					for (int i = 0; i < 3; i++) {
						input[i] = 2.0f * Math::asin(CLAMP(rotation[i], (real_t)-1.0, (real_t)1.0));
					}
				} else {
					input = basis_to_euler(p_target.basis, order_setting("from_rotation_mode"));
				}
				suffix = "_rot";
			} else {
				input = p_target.origin;
			}
			Vector3 amount;
			const bool extrapolate = flag("use_motion_extrapolate");
			for (int i = 0; i < 3; i++) {
				const real_t low = number(String("from_min_") + axes[i] + suffix);
				const real_t high = number(String("from_max_") + axes[i] + suffix);
				real_t value = input[i];
				if (!extrapolate) {
					value = CLAMP(value, low, high);
				}
				amount[i] = high - low != 0.0f ? (value - low) / (high - low) : 0.0f;
			}
			Vector3 loc, size;
			Basis rotation;
			to_loc_rot_size(r_owner, loc, rotation, size);
			const String to = text("map_to");
			const String to_suffix = to == "SCALE" ? "_scale" : (to == "ROTATION" ? "_rot" : "");
			Vector3 output;
			for (int i = 0; i < 3; i++) {
				const int source = axis_index(text(String("map_to_") + axes[i] + "_from"), AXIS_LETTERS, 3, i);
				const real_t low = number(String("to_min_") + axes[i] + to_suffix);
				const real_t high = number(String("to_max_") + axes[i] + to_suffix);
				output[i] = low + amount[source] * (high - low);
			}
			if (to == "SCALE") {
				size = text("mix_mode_scale") == "MULTIPLY" ? size * output : output;
			} else if (to == "ROTATION") {
				const int order = order_setting("to_euler_order");
				const String mix = text("mix_mode_rot");
				if (mix == "REPLACE") {
					rotation = euler_to_basis(output, order);
				} else if (mix == "BEFORE") {
					rotation = euler_to_basis(output, order) * rotation;
				} else if (mix == "AFTER") {
					rotation = rotation * euler_to_basis(output, order);
				} else {
					rotation = euler_to_basis(output + basis_to_euler(rotation, order), order);
				}
			} else {
				loc = text("mix_mode") == "REPLACE" ? output : loc + output;
			}
			r_owner = from_loc_rot_size(loc, rotation, size);
		} break;

		case TYPE_MAINTAIN_VOLUME: {
			static const char *free_axes[3] = { "SAMEVOL_X", "SAMEVOL_Y", "SAMEVOL_Z" };
			const int free_axis = axis_index(text("free_axis"), free_axes, 3, 1);
			const Vector3 size = matrix_size(r_owner.basis);
			const String mode = text("mode");
			real_t total = size.x * size.y * size.z;
			if (mode == "UNIFORM") {
				total = size[free_axis] * size[free_axis] * size[free_axis];
			} else if (mode == "SINGLE_AXIS") {
				total = size[free_axis];
			}
			if (total != 0) {
				const real_t factor = Math::sqrt(number("volume") / total);
				for (int i = 0; i < 3; i++) {
					if (i != free_axis) {
						r_owner.basis.set_column(i, r_owner.basis.get_column(i) * factor);
					}
				}
			}
		} break;

		default:
			break;
	}
}

void BlenderConstraint3D::_process_modification(double p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton) {
		return;
	}
	Transform3D target;
	const bool has_target = _get_target_matrix(skeleton, target);
	if (needs_target() && !has_target) {
		return;
	}

	if (!owner_node.is_empty()) {
		// A node owner, such as a prop that follows a bone. The skeleton blends bones by
		// influence; a node is blended here.
		Node3D *node = Object::cast_to<Node3D>(get_node_or_null(owner_node));
		if (!node) {
			return;
		}
		// The node's own transform is what it holds, unless that is what this constraint wrote.
		if (!node_input_known || !node->get_transform().is_equal_approx(node_output)) {
			node_input = node->get_transform();
			node_input_known = true;
		}
		const Node3D *parent = node->get_parent_node_3d();
		const Transform3D parent_global = parent ? parent->get_global_transform() : Transform3D();
		const Transform3D world = blender_from_godot() * (parent_global * node_input) * godot_from_blender();
		Transform3D result = world;
		_evaluate(result, target, has_target);
		if (get_influence() < 1.0) {
			result = world.interpolate_with(result, get_influence());
		}
		node_output = parent_global.affine_inverse() * (godot_from_blender() * result * blender_from_godot());
		node->set_transform(node_output);
		return;
	}

	if (bone < 0 || bone >= skeleton->get_bone_count()) {
		return;
	}
	const Transform3D armature = _get_armature_world(skeleton);
	Transform3D owner = _to_space(skeleton, armature * (blender_from_godot() * skeleton->get_bone_global_pose(bone)), owner_space, bone);
	_evaluate(owner, target, has_target);
	Transform3D pose = godot_from_blender() * (armature.affine_inverse() * _from_space(skeleton, owner, owner_space, bone));
	// A bone holds no shear here, as in glTF, though some results in Blender have it, such as
	// Child Of an unevenly scaled target. The nearest pose keeps the bone's Y axis, as Blender
	// removes shear itself.
	pose.basis = orthogonalized_y(pose.basis, false);
	const int parent = skeleton->get_bone_parent(bone);
	skeleton->set_bone_pose(bone, parent >= 0 ? skeleton->get_bone_global_pose(parent).affine_inverse() * pose : pose);
}

void BlenderConstraint3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_constraint_type", "type"), &BlenderConstraint3D::set_constraint_type);
	ClassDB::bind_method(D_METHOD("get_constraint_type"), &BlenderConstraint3D::get_constraint_type);
	ClassDB::bind_method(D_METHOD("set_bone_name", "bone_name"), &BlenderConstraint3D::set_bone_name);
	ClassDB::bind_method(D_METHOD("get_bone_name"), &BlenderConstraint3D::get_bone_name);
	ClassDB::bind_method(D_METHOD("set_bone", "bone"), &BlenderConstraint3D::set_bone);
	ClassDB::bind_method(D_METHOD("get_bone"), &BlenderConstraint3D::get_bone);
	ClassDB::bind_method(D_METHOD("set_owner_node", "node"), &BlenderConstraint3D::set_owner_node);
	ClassDB::bind_method(D_METHOD("get_owner_node"), &BlenderConstraint3D::get_owner_node);
	ClassDB::bind_method(D_METHOD("set_target_bone_name", "bone_name"), &BlenderConstraint3D::set_target_bone_name);
	ClassDB::bind_method(D_METHOD("get_target_bone_name"), &BlenderConstraint3D::get_target_bone_name);
	ClassDB::bind_method(D_METHOD("set_target_node", "node"), &BlenderConstraint3D::set_target_node);
	ClassDB::bind_method(D_METHOD("get_target_node"), &BlenderConstraint3D::get_target_node);
	ClassDB::bind_method(D_METHOD("set_head_tail", "head_tail"), &BlenderConstraint3D::set_head_tail);
	ClassDB::bind_method(D_METHOD("get_head_tail"), &BlenderConstraint3D::get_head_tail);
	ClassDB::bind_method(D_METHOD("set_target_bone_length", "length"), &BlenderConstraint3D::set_target_bone_length);
	ClassDB::bind_method(D_METHOD("get_target_bone_length"), &BlenderConstraint3D::get_target_bone_length);
	ClassDB::bind_method(D_METHOD("set_owner_space", "space"), &BlenderConstraint3D::set_owner_space);
	ClassDB::bind_method(D_METHOD("get_owner_space"), &BlenderConstraint3D::get_owner_space);
	ClassDB::bind_method(D_METHOD("set_target_space", "space"), &BlenderConstraint3D::set_target_space);
	ClassDB::bind_method(D_METHOD("get_target_space"), &BlenderConstraint3D::get_target_space);
	ClassDB::bind_method(D_METHOD("set_owner_rotation_order", "order"), &BlenderConstraint3D::set_owner_rotation_order);
	ClassDB::bind_method(D_METHOD("get_owner_rotation_order"), &BlenderConstraint3D::get_owner_rotation_order);
	ClassDB::bind_method(D_METHOD("set_target_rotation_order", "order"), &BlenderConstraint3D::set_target_rotation_order);
	ClassDB::bind_method(D_METHOD("get_target_rotation_order"), &BlenderConstraint3D::get_target_rotation_order);
	ClassDB::bind_method(D_METHOD("set_settings", "settings"), &BlenderConstraint3D::set_settings);
	ClassDB::bind_method(D_METHOD("get_settings"), &BlenderConstraint3D::get_settings);
	ClassDB::bind_method(D_METHOD("set_setting", "name", "value"), &BlenderConstraint3D::set_setting);
	ClassDB::bind_method(D_METHOD("get_setting", "name"), &BlenderConstraint3D::get_setting);
	ClassDB::bind_method(D_METHOD("needs_target"), &BlenderConstraint3D::needs_target);

	const String rotation_orders = "XYZ,XZY,YXZ,YZX,ZXY,ZYX";
	ADD_PROPERTY(PropertyInfo(Variant::INT, "constraint_type", PROPERTY_HINT_ENUM, "Copy Location,Copy Rotation,Copy Scale,Copy Transforms,Limit Location,Limit Rotation,Limit Scale,Child Of,Damped Track,Track To,Locked Track,Stretch To,Transformation,Maintain Volume"), "set_constraint_type", "get_constraint_type");
	ADD_GROUP("Owner", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "bone_name"), "set_bone_name", "get_bone_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bone", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_bone", "get_bone");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "owner_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_owner_node", "get_owner_node");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "owner_space", PROPERTY_HINT_ENUM, "World,Pose,Local With Parent,Local"), "set_owner_space", "get_owner_space");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "owner_rotation_order", PROPERTY_HINT_ENUM, rotation_orders), "set_owner_rotation_order", "get_owner_rotation_order");
	ADD_GROUP("Target", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "target_bone_name"), "set_target_bone_name", "get_target_bone_name");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_target_node", "get_target_node");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_tail", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_head_tail", "get_head_tail");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "target_bone_length", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater,suffix:m"), "set_target_bone_length", "get_target_bone_length");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "target_space", PROPERTY_HINT_ENUM, "World,Pose,Local With Parent,Local"), "set_target_space", "get_target_space");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "target_rotation_order", PROPERTY_HINT_ENUM, rotation_orders), "set_target_rotation_order", "get_target_rotation_order");

	BIND_ENUM_CONSTANT(TYPE_COPY_LOCATION);
	BIND_ENUM_CONSTANT(TYPE_COPY_ROTATION);
	BIND_ENUM_CONSTANT(TYPE_COPY_SCALE);
	BIND_ENUM_CONSTANT(TYPE_COPY_TRANSFORMS);
	BIND_ENUM_CONSTANT(TYPE_LIMIT_LOCATION);
	BIND_ENUM_CONSTANT(TYPE_LIMIT_ROTATION);
	BIND_ENUM_CONSTANT(TYPE_LIMIT_SCALE);
	BIND_ENUM_CONSTANT(TYPE_CHILD_OF);
	BIND_ENUM_CONSTANT(TYPE_DAMPED_TRACK);
	BIND_ENUM_CONSTANT(TYPE_TRACK_TO);
	BIND_ENUM_CONSTANT(TYPE_LOCKED_TRACK);
	BIND_ENUM_CONSTANT(TYPE_STRETCH_TO);
	BIND_ENUM_CONSTANT(TYPE_TRANSFORMATION);
	BIND_ENUM_CONSTANT(TYPE_MAINTAIN_VOLUME);
	BIND_ENUM_CONSTANT(TYPE_MAX);
	BIND_ENUM_CONSTANT(SPACE_WORLD);
	BIND_ENUM_CONSTANT(SPACE_POSE);
	BIND_ENUM_CONSTANT(SPACE_LOCAL_WITH_PARENT);
	BIND_ENUM_CONSTANT(SPACE_LOCAL);
}
