/**************************************************************************/
/*  foliage_data.cpp                                                      */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "foliage_data.h"

#include "terrain_data.h"

#include "core/core_string_names.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

Ref<Image> copy_image(const Ref<Image> &p_image) {
	if (p_image.is_null()) {
		return Ref<Image>();
	}
	return Image::create_from_data(p_image->get_width(), p_image->get_height(), false, p_image->get_format(), p_image->get_data());
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                    Types                                   */
/* -------------------------------------------------------------------------- */

void FoliageData::_mark_edited() {
#ifdef TOOLS_ENABLED
	set_edited(true);
#endif
}

void FoliageData::_type_changed(int p_index) {
	_mark_edited();
	emit_signal(SNAME("type_changed"), p_index);
}

void FoliageData::_connect_type(int p_index) {
	const Ref<FoliageType> &type = types[p_index];
	if (type.is_valid()) {
		type->connect_changed(callable_mp(this, &FoliageData::_type_changed).bind(p_index));
	}
}

void FoliageData::_disconnect_type(int p_index) {
	const Ref<FoliageType> &type = types[p_index];
	const Callable c = callable_mp(this, &FoliageData::_type_changed).bind(p_index);
	if (type.is_valid() && type->is_connected(CoreStringName(changed), c)) {
		type->disconnect_changed(c);
	}
}

void FoliageData::set_density_texel_size(float p_size) {
	p_size = CLAMP(p_size, 0.1f, 16.0f);
	if (p_size == density_texel_size) {
		return;
	}
	for (const TypeData &td : type_data) {
		ERR_FAIL_COND_MSG(!td.density.is_empty(), "The density texel size can only be changed before any grass is painted.");
	}
	density_texel_size = p_size;
	emit_changed();
}

Ref<FoliageType> FoliageData::get_type(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, types.size(), Ref<FoliageType>());
	return types[p_index];
}

void FoliageData::set_type(int p_index, const Ref<FoliageType> &p_type) {
	ERR_FAIL_INDEX(p_index, types.size());
	ERR_FAIL_COND(p_type.is_null());
	_disconnect_type(p_index);
	types.write[p_index] = p_type;
	_connect_type(p_index);
	_mark_edited();
	emit_signal(SNAME("types_changed"));
}

int FoliageData::add_type(const Ref<FoliageType> &p_type) {
	ERR_FAIL_COND_V(p_type.is_null(), -1);
	types.push_back(p_type);
	type_data.push_back(TypeData());
	_connect_type(types.size() - 1);
	_mark_edited();
	emit_signal(SNAME("types_changed"));
	return types.size() - 1;
}

void FoliageData::remove_type(int p_index) {
	ERR_FAIL_INDEX(p_index, types.size());
	// Indices after this one shift down, so every binding is redone.
	for (int i = 0; i < types.size(); i++) {
		_disconnect_type(i);
	}
	types.remove_at(p_index);
	type_data.remove_at(p_index);
	for (int i = 0; i < types.size(); i++) {
		_connect_type(i);
	}
	_mark_edited();
	emit_signal(SNAME("types_changed"));
}

/* -------------------------------------------------------------------------- */
/*                                    Grass                                   */
/* -------------------------------------------------------------------------- */

Vector2i FoliageData::local_to_texel(const Vector2 &p_local) const {
	return Vector2i((int)Math::floor(p_local.x / density_texel_size), (int)Math::floor(p_local.y / density_texel_size));
}

uint8_t *FoliageData::density_texel(int p_type, const Vector2i &p_texel, bool p_create) {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), nullptr);
	const Vector2i cell(TerrainData::floor_div(p_texel.x, DENSITY_CELL), TerrainData::floor_div(p_texel.y, DENSITY_CELL));
	Ref<Image> *image = type_data.write[p_type].density.getptr(cell);
	if (!image) {
		if (!p_create) {
			return nullptr;
		}
		Ref<Image> created = Image::create_empty(DENSITY_CELL, DENSITY_CELL, false, Image::FORMAT_R8);
		created->fill(Color(0, 0, 0, 0));
		image = &type_data.write[p_type].density.insert(cell, created)->value;
	}
	const Vector2i t = p_texel - cell * DENSITY_CELL;
	return (*image)->ptrw() + t.y * DENSITY_CELL + t.x;
}

float FoliageData::get_density(int p_type, const Vector2 &p_local) const {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), 0.0);
	const Vector2i texel = local_to_texel(p_local);
	const Vector2i cell(TerrainData::floor_div(texel.x, DENSITY_CELL), TerrainData::floor_div(texel.y, DENSITY_CELL));
	const Ref<Image> *image = type_data[p_type].density.getptr(cell);
	if (!image) {
		return 0.0;
	}
	const Vector2i t = texel - cell * DENSITY_CELL;
	return (*image)->ptr()[t.y * DENSITY_CELL + t.x] / 255.0f;
}

void FoliageData::set_density(int p_type, const Vector2 &p_local, float p_density) {
	ERR_FAIL_INDEX(p_type, type_data.size());
	uint8_t *px = density_texel(p_type, local_to_texel(p_local), p_density > 0.0f);
	if (px) {
		*px = (uint8_t)CLAMP((int)Math::round(p_density * 255.0f), 0, 255);
		notify_density_changed(p_type, Rect2(p_local - Vector2(1, 1) * density_texel_size, Vector2(2, 2) * density_texel_size));
	}
}

float FoliageData::sample_density(int p_type, const Vector2 &p_local) const {
	// Bilinear between texel centres, so painted edges fade instead of stepping.
	const Vector2 p = p_local / density_texel_size - Vector2(0.5, 0.5);
	const Vector2 base = p.floor();
	const Vector2 f = p - base;
	const Vector2 c = (base + Vector2(0.5, 0.5)) * density_texel_size;
	const float d00 = get_density(p_type, c);
	const float d10 = get_density(p_type, c + Vector2(density_texel_size, 0));
	const float d01 = get_density(p_type, c + Vector2(0, density_texel_size));
	const float d11 = get_density(p_type, c + Vector2(density_texel_size, density_texel_size));
	return Math::lerp(Math::lerp(d00, d10, f.x), Math::lerp(d01, d11, f.x), f.y);
}

void FoliageData::clear_density(int p_type) {
	ERR_FAIL_INDEX(p_type, type_data.size());
	type_data.write[p_type].density.clear();
	notify_density_changed(p_type, Rect2());
}

const FoliageData::TypeData *FoliageData::get_type_data(int p_type) const {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), nullptr);
	return &type_data[p_type];
}

void FoliageData::notify_density_changed(int p_type, const Rect2 &p_local_area) {
	_mark_edited();
	emit_signal(SNAME("density_changed"), p_type, p_local_area);
}

/* -------------------------------------------------------------------------- */
/*                                   Objects                                  */
/* -------------------------------------------------------------------------- */

int FoliageData::get_instance_count(int p_type) const {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), 0);
	return type_data[p_type].instances.size() / INSTANCE_STRIDE;
}

PackedFloat32Array FoliageData::get_instances(int p_type) const {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), PackedFloat32Array());
	return type_data[p_type].instances;
}

void FoliageData::set_instances(int p_type, const PackedFloat32Array &p_instances) {
	ERR_FAIL_INDEX(p_type, type_data.size());
	ERR_FAIL_COND_MSG(p_instances.size() % INSTANCE_STRIDE != 0, vformat("Instances are packed %d floats apiece.", INSTANCE_STRIDE));
	type_data.write[p_type].instances = p_instances;
	_mark_edited();
	emit_signal(SNAME("instances_changed"), p_type, Rect2());
}

void FoliageData::add_instance(int p_type, const Vector3 &p_local_position, float p_yaw, float p_scale, float p_variation) {
	ERR_FAIL_INDEX(p_type, type_data.size());
	PackedFloat32Array &list = type_data.write[p_type].instances;
	list.push_back(p_local_position.x);
	list.push_back(p_local_position.z);
	list.push_back(p_local_position.y);
	list.push_back(p_yaw);
	list.push_back(p_scale);
	list.push_back(p_variation);
	_mark_edited();
	emit_signal(SNAME("instances_changed"), p_type, Rect2(Vector2(p_local_position.x, p_local_position.z), Vector2()).grow(0.01));
}

int FoliageData::remove_instances_in_radius(int p_type, const Vector2 &p_local_center, float p_radius, float p_probability, uint32_t p_seed) {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), 0);
	PackedFloat32Array &list = type_data.write[p_type].instances;
	const int count = list.size() / INSTANCE_STRIDE;
	if (count == 0) {
		return 0;
	}
	RandomPCG rng(p_seed);
	const float r2 = p_radius * p_radius;
	PackedFloat32Array kept;
	kept.resize(list.size());
	float *dst = kept.ptrw();
	const float *src = list.ptr();
	int written = 0;
	int removed = 0;
	for (int i = 0; i < count; i++) {
		const float *inst = src + i * INSTANCE_STRIDE;
		const bool inside = Vector2(inst[0], inst[1]).distance_squared_to(p_local_center) <= r2;
		if (inside && (p_probability >= 1.0f || rng.randf() < p_probability)) {
			removed++;
			continue;
		}
		memcpy(dst + written * INSTANCE_STRIDE, inst, sizeof(float) * INSTANCE_STRIDE);
		written++;
	}
	if (removed == 0) {
		return 0;
	}
	kept.resize(written * INSTANCE_STRIDE);
	list = kept;
	_mark_edited();
	emit_signal(SNAME("instances_changed"), p_type, Rect2(p_local_center - Vector2(p_radius, p_radius), Vector2(p_radius, p_radius) * 2.0));
	return removed;
}

void FoliageData::clear_instances(int p_type) {
	ERR_FAIL_INDEX(p_type, type_data.size());
	type_data.write[p_type].instances.clear();
	_mark_edited();
	emit_signal(SNAME("instances_changed"), p_type, Rect2());
}

bool FoliageData::has_instance_near(int p_type, const Vector2 &p_local, float p_distance) const {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), false);
	const PackedFloat32Array &list = type_data[p_type].instances;
	const float *src = list.ptr();
	const int count = list.size() / INSTANCE_STRIDE;
	const float d2 = p_distance * p_distance;
	for (int i = 0; i < count; i++) {
		const float dx = src[i * INSTANCE_STRIDE] - p_local.x;
		const float dz = src[i * INSTANCE_STRIDE + 1] - p_local.y;
		if (dx * dx + dz * dz < d2) {
			return true;
		}
	}
	return false;
}

/* -------------------------------------------------------------------------- */
/*                               Storage and undo                             */
/* -------------------------------------------------------------------------- */

void FoliageData::_set_types(const Array &p_types) {
	for (int i = 0; i < types.size(); i++) {
		_disconnect_type(i);
	}
	types.clear();
	for (int i = 0; i < p_types.size(); i++) {
		Ref<FoliageType> type = p_types[i];
		if (type.is_null()) {
			type.instantiate();
		}
		types.push_back(type);
	}
	type_data.resize(types.size());
	for (int i = 0; i < types.size(); i++) {
		_connect_type(i);
	}
	emit_signal(SNAME("types_changed"));
}

Array FoliageData::_get_types() const {
	Array result;
	for (const Ref<FoliageType> &type : types) {
		result.push_back(type);
	}
	return result;
}

void FoliageData::_set_type_data(const Array &p_data) {
	type_data.resize(MAX(types.size(), p_data.size()));
	for (int i = 0; i < p_data.size(); i++) {
		const Dictionary d = p_data[i];
		TypeData td;
		const Dictionary density = d.get("density", Dictionary());
		for (const KeyValue<Variant, Variant> &E : density) {
			const Ref<Image> image = E.value;
			if (image.is_valid() && image->get_width() == DENSITY_CELL && image->get_height() == DENSITY_CELL && image->get_format() == Image::FORMAT_R8) {
				td.density.insert(E.key, image);
			}
		}
		td.instances = d.get("instances", PackedFloat32Array());
		if (td.instances.size() % INSTANCE_STRIDE != 0) {
			td.instances.resize(td.instances.size() - td.instances.size() % INSTANCE_STRIDE);
		}
		type_data.write[i] = td;
	}
	emit_signal(SNAME("types_changed"));
}

Array FoliageData::_get_type_data() const {
	Array result;
	for (const TypeData &td : type_data) {
		Dictionary d;
		Dictionary density;
		for (const KeyValue<Vector2i, Ref<Image>> &E : td.density) {
			density[E.key] = E.value;
		}
		d["density"] = density;
		d["instances"] = td.instances;
		result.push_back(d);
	}
	return result;
}

Dictionary FoliageData::_get_type_snapshot(int p_type) const {
	ERR_FAIL_INDEX_V(p_type, type_data.size(), Dictionary());
	Dictionary d;
	Dictionary density;
	for (const KeyValue<Vector2i, Ref<Image>> &E : type_data[p_type].density) {
		density[E.key] = copy_image(E.value);
	}
	d["density"] = density;
	d["instances"] = type_data[p_type].instances;
	return d;
}

void FoliageData::_apply_type_snapshot(int p_type, const Dictionary &p_snapshot) {
	ERR_FAIL_INDEX(p_type, type_data.size());
	TypeData &td = type_data.write[p_type];
	td.density.clear();
	const Dictionary density = p_snapshot["density"];
	for (const KeyValue<Variant, Variant> &E : density) {
		td.density.insert(E.key, copy_image(E.value));
	}
	td.instances = p_snapshot["instances"];
	emit_signal(SNAME("density_changed"), p_type, Rect2());
	_mark_edited();
	emit_signal(SNAME("instances_changed"), p_type, Rect2());
}

Array FoliageData::_get_all_snapshot() const {
	Array result;
	for (int i = 0; i < type_data.size(); i++) {
		result.push_back(_get_type_snapshot(i));
	}
	return result;
}

void FoliageData::_apply_all_snapshot(const Array &p_types, const Array &p_snapshots) {
	_set_types(p_types);
	for (int i = 0; i < MIN(p_snapshots.size(), type_data.size()); i++) {
		_apply_type_snapshot(i, p_snapshots[i]);
	}
}

void FoliageData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_density_texel_size", "size"), &FoliageData::set_density_texel_size);
	ClassDB::bind_method(D_METHOD("get_density_texel_size"), &FoliageData::get_density_texel_size);

	ClassDB::bind_method(D_METHOD("get_type_count"), &FoliageData::get_type_count);
	ClassDB::bind_method(D_METHOD("get_type", "index"), &FoliageData::get_type);
	ClassDB::bind_method(D_METHOD("set_type", "index", "type"), &FoliageData::set_type);
	ClassDB::bind_method(D_METHOD("add_type", "type"), &FoliageData::add_type);
	ClassDB::bind_method(D_METHOD("remove_type", "index"), &FoliageData::remove_type);

	ClassDB::bind_method(D_METHOD("get_density", "type", "local_position"), &FoliageData::get_density);
	ClassDB::bind_method(D_METHOD("set_density", "type", "local_position", "density"), &FoliageData::set_density);
	ClassDB::bind_method(D_METHOD("clear_density", "type"), &FoliageData::clear_density);

	ClassDB::bind_method(D_METHOD("get_instance_count", "type"), &FoliageData::get_instance_count);
	ClassDB::bind_method(D_METHOD("get_instances", "type"), &FoliageData::get_instances);
	ClassDB::bind_method(D_METHOD("set_instances", "type", "instances"), &FoliageData::set_instances);
	ClassDB::bind_method(D_METHOD("add_instance", "type", "local_position", "yaw", "scale", "variation"), &FoliageData::add_instance, DEFVAL(0.0), DEFVAL(1.0), DEFVAL(0.5));
	ClassDB::bind_method(D_METHOD("remove_instances_in_radius", "type", "local_center", "radius", "probability", "seed"), &FoliageData::remove_instances_in_radius, DEFVAL(1.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("clear_instances", "type"), &FoliageData::clear_instances);

	ClassDB::bind_method(D_METHOD("_set_types", "types"), &FoliageData::_set_types);
	ClassDB::bind_method(D_METHOD("_get_types"), &FoliageData::_get_types);
	ClassDB::bind_method(D_METHOD("_set_type_data", "data"), &FoliageData::_set_type_data);
	ClassDB::bind_method(D_METHOD("_get_type_data"), &FoliageData::_get_type_data);
	ClassDB::bind_method(D_METHOD("_get_type_snapshot", "type"), &FoliageData::_get_type_snapshot);
	ClassDB::bind_method(D_METHOD("_apply_type_snapshot", "type", "snapshot"), &FoliageData::_apply_type_snapshot);
	ClassDB::bind_method(D_METHOD("_get_all_snapshot"), &FoliageData::_get_all_snapshot);
	ClassDB::bind_method(D_METHOD("_apply_all_snapshot", "types", "snapshots"), &FoliageData::_apply_all_snapshot);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density_texel_size", PROPERTY_HINT_RANGE, "0.1,16,0.01,suffix:m"), "set_density_texel_size", "get_density_texel_size");
	// Types before their data, so the data knows how many slots there are.
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "types", PROPERTY_HINT_ARRAY_TYPE, "FoliageType", PROPERTY_USAGE_STORAGE), "_set_types", "_get_types");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "type_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_type_data", "_get_type_data");

	ADD_SIGNAL(MethodInfo("types_changed"));
	ADD_SIGNAL(MethodInfo("type_changed", PropertyInfo(Variant::INT, "type")));
	ADD_SIGNAL(MethodInfo("density_changed", PropertyInfo(Variant::INT, "type"), PropertyInfo(Variant::RECT2, "area")));
	ADD_SIGNAL(MethodInfo("instances_changed", PropertyInfo(Variant::INT, "type"), PropertyInfo(Variant::RECT2, "area")));

	BIND_CONSTANT(DENSITY_CELL);
	BIND_CONSTANT(INSTANCE_STRIDE);
}
