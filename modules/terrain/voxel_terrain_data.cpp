/**************************************************************************/
/*  voxel_terrain_data.cpp                                                */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "voxel_terrain_data.h"

#include "core/core_string_names.h"
#include "core/io/compression.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

constexpr float SDF_QUANTUM = 1024.0;
constexpr int BS = VoxelTerrainData::BLOCK_SIZE;

_FORCE_INLINE_ int16_t encode_sdf(float p_sdf) {
	return (int16_t)Math::round(CLAMP(p_sdf, -VoxelGenerator::SDF_LIMIT, VoxelGenerator::SDF_LIMIT) * SDF_QUANTUM);
}

_FORCE_INLINE_ float decode_sdf(int16_t p_value) {
	return float(p_value) / SDF_QUANTUM;
}

_FORCE_INLINE_ int local_index(int p_x, int p_y, int p_z) {
	return (p_z * BS + p_y) * BS + p_x;
}

_FORCE_INLINE_ int ceil_div(int p_a, int p_b) {
	return -VoxelTerrainData::floor_div(-p_a, p_b);
}

// 1 inside the hardness radius, then eased down to 0 at the edge.
float brush_falloff(float p_distance, float p_radius, float p_hardness) {
	if (p_distance >= p_radius) {
		return 0.0;
	}
	const float inner = p_radius * CLAMP(p_hardness, 0.0f, 0.999f);
	if (p_distance <= inner) {
		return 1.0;
	}
	const float t = (p_distance - inner) / (p_radius - inner);
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

float hash01(const Vector3i &p_voxel, uint32_t p_seed) {
	uint32_t h = p_seed * 0x9e3779b9u;
	h ^= (uint32_t)p_voxel.x * 0x85ebca6bu;
	h = (h << 13) | (h >> 19);
	h ^= (uint32_t)p_voxel.y * 0xc2b2ae35u;
	h = (h << 11) | (h >> 21);
	h ^= (uint32_t)p_voxel.z * 0x27d4eb2fu;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	return float(h & 0xffffff) / float(0x1000000);
}

PackedByteArray pack(const uint8_t *p_data, int p_size) {
	PackedByteArray out;
	out.resize(Compression::get_max_compressed_buffer_size(p_size, Compression::MODE_ZSTD));
	const int64_t written = Compression::compress(out.ptrw(), p_data, p_size, Compression::MODE_ZSTD);
	out.resize(MAX(written, 0));
	return out;
}

bool unpack(const PackedByteArray &p_packed, uint8_t *r_data, int p_size) {
	if (p_packed.is_empty()) {
		return false;
	}
	return Compression::decompress(r_data, p_size, p_packed.ptr(), p_packed.size(), Compression::MODE_ZSTD) == p_size;
}

} // namespace

VoxelTerrainData::VoxelTerrainData() {
}

void VoxelTerrainData::_mark_edited() {
#ifdef TOOLS_ENABLED
	// The editor saves external resources that are marked edited along with
	// the scene, which is how a terrain's data file gets written.
	set_edited(true);
#endif
}

void VoxelTerrainData::_emit_changed_area(const AABB &p_voxel_box) {
	emit_signal(SNAME("changed_area"), p_voxel_box);
}

void VoxelTerrainData::_generator_changed() {
	emit_signal(SNAME("generator_changed"));
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */

void VoxelTerrainData::set_voxel_size(float p_size) {
	p_size = CLAMP(p_size, 0.05f, 16.0f);
	if (voxel_size == p_size) {
		return;
	}
	voxel_size = p_size;
	_mark_edited();
	_generator_changed();
}

void VoxelTerrainData::set_generator(const Ref<VoxelGenerator> &p_generator) {
	if (generator == p_generator) {
		return;
	}
	if (generator.is_valid()) {
		generator->disconnect_changed(callable_mp(this, &VoxelTerrainData::_generator_changed));
	}
	generator = p_generator;
	if (generator.is_valid()) {
		generator->connect_changed(callable_mp(this, &VoxelTerrainData::_generator_changed));
	}
	_mark_edited();
	_generator_changed();
}

/* -------------------------------------------------------------------------- */
/*                                   Layers                                   */
/* -------------------------------------------------------------------------- */

void VoxelTerrainData::_layer_changed() {
	_mark_edited();
	emit_signal(SNAME("layers_changed"));
}

Ref<TerrainLayer> VoxelTerrainData::get_layer(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, layers.size(), Ref<TerrainLayer>());
	return layers[p_index];
}

void VoxelTerrainData::set_layer(int p_index, const Ref<TerrainLayer> &p_layer) {
	ERR_FAIL_INDEX(p_index, layers.size());
	ERR_FAIL_COND(p_layer.is_null());
	const Ref<TerrainLayer> old = layers[p_index];
	layers.write[p_index] = p_layer;
	if (old.is_valid() && old != p_layer && !layers.has(old)) {
		old->disconnect_changed(callable_mp(this, &VoxelTerrainData::_layer_changed));
	}
	if (!p_layer->is_connected(CoreStringName(changed), callable_mp(this, &VoxelTerrainData::_layer_changed))) {
		p_layer->connect_changed(callable_mp(this, &VoxelTerrainData::_layer_changed));
	}
	_layer_changed();
}

int VoxelTerrainData::add_layer(const Ref<TerrainLayer> &p_layer) {
	ERR_FAIL_COND_V(p_layer.is_null(), -1);
	ERR_FAIL_COND_V_MSG(layers.size() >= MAX_LAYERS, -1, vformat("A terrain can have at most %d layers.", MAX_LAYERS));
	layers.push_back(p_layer);
	if (!p_layer->is_connected(CoreStringName(changed), callable_mp(this, &VoxelTerrainData::_layer_changed))) {
		p_layer->connect_changed(callable_mp(this, &VoxelTerrainData::_layer_changed));
	}
	_layer_changed();
	notify_property_list_changed();
	return layers.size() - 1;
}

void VoxelTerrainData::_remap_materials(const Vector<int> &p_old_to_new) {
	AABB changed;
	bool any = false;
	{
		RWLockWrite guard(lock);
		for (KeyValue<Vector3i, Block> &E : blocks) {
			uint8_t *m = E.value.material.ptrw();
			bool block_changed = false;
			for (int i = 0; i < BLOCK_VOLUME; i++) {
				const int to = m[i] < p_old_to_new.size() ? p_old_to_new[m[i]] : m[i];
				const uint8_t value = to < 0 ? 0 : to;
				if (value != m[i]) {
					m[i] = value;
					block_changed = true;
				}
			}
			if (block_changed) {
				const AABB box(Vector3(E.key * BS), Vector3(BS, BS, BS));
				changed = any ? changed.merge(box) : box;
				any = true;
			}
		}
	}
	if (any) {
		_emit_changed_area(changed);
	}
}

void VoxelTerrainData::remove_layer(int p_index) {
	ERR_FAIL_INDEX(p_index, layers.size());
	// Voxels made of the removed layer fall back to the first one.
	Vector<int> remap;
	for (int l = 0; l < MAX_LAYERS; l++) {
		remap.push_back(l < p_index ? l : (l == p_index ? 0 : l - 1));
	}
	const Ref<TerrainLayer> old = layers[p_index];
	layers.remove_at(p_index);
	if (old.is_valid() && !layers.has(old)) {
		old->disconnect_changed(callable_mp(this, &VoxelTerrainData::_layer_changed));
	}
	_remap_materials(remap);
	_layer_changed();
	notify_property_list_changed();
}

void VoxelTerrainData::move_layer(int p_from, int p_to) {
	ERR_FAIL_INDEX(p_from, layers.size());
	ERR_FAIL_INDEX(p_to, layers.size());
	if (p_from == p_to) {
		return;
	}
	Vector<int> order;
	for (int l = 0; l < layers.size(); l++) {
		order.push_back(l);
	}
	order.remove_at(p_from);
	order.insert(p_to, p_from);
	Vector<int> remap;
	remap.resize(MAX_LAYERS);
	for (int l = 0; l < MAX_LAYERS; l++) {
		remap.write[l] = l;
	}
	for (int n = 0; n < order.size(); n++) {
		remap.write[order[n]] = n;
	}
	const Ref<TerrainLayer> moving = layers[p_from];
	layers.remove_at(p_from);
	layers.insert(p_to, moving);
	_remap_materials(remap);
	_layer_changed();
}

void VoxelTerrainData::_set_layers(const Array &p_layers) {
	for (const Ref<TerrainLayer> &layer : layers) {
		if (layer.is_valid() && layer->is_connected(CoreStringName(changed), callable_mp(this, &VoxelTerrainData::_layer_changed))) {
			layer->disconnect_changed(callable_mp(this, &VoxelTerrainData::_layer_changed));
		}
	}
	layers.clear();
	for (int i = 0; i < MIN(p_layers.size(), MAX_LAYERS); i++) {
		Ref<TerrainLayer> layer = p_layers[i];
		if (layer.is_null()) {
			layer.instantiate();
		}
		layers.push_back(layer);
		if (!layer->is_connected(CoreStringName(changed), callable_mp(this, &VoxelTerrainData::_layer_changed))) {
			layer->connect_changed(callable_mp(this, &VoxelTerrainData::_layer_changed));
		}
	}
	emit_signal(SNAME("layers_changed"));
	notify_property_list_changed();
}

Array VoxelTerrainData::_get_layers() const {
	Array result;
	for (const Ref<TerrainLayer> &layer : layers) {
		result.push_back(layer);
	}
	return result;
}

Array VoxelTerrainData::_get_layers_snapshot() const {
	Array result;
	RWLockRead guard(lock);
	for (const KeyValue<Vector3i, Block> &E : blocks) {
		Dictionary d;
		d["block"] = E.key;
		d["exists"] = true;
		d["sdf"] = pack((const uint8_t *)E.value.sdf.ptr(), BLOCK_VOLUME * sizeof(int16_t));
		d["material"] = pack(E.value.material.ptr(), BLOCK_VOLUME);
		result.push_back(d);
	}
	return result;
}

void VoxelTerrainData::_apply_layers_snapshot(const Array &p_layers, const Array &p_snapshots) {
	_set_layers(p_layers);
	_mark_edited();
	_apply_snapshots(p_snapshots);
}

bool VoxelTerrainData::_set(const StringName &p_name, const Variant &p_value) {
	const String prop = p_name;
	if (!prop.begins_with("layer_") || !prop.trim_prefix("layer_").is_valid_int()) {
		return false;
	}
	const int index = prop.trim_prefix("layer_").to_int();
	if (index < 0 || index >= layers.size()) {
		return false;
	}
	Ref<TerrainLayer> layer = p_value;
	if (layer.is_null()) {
		// A slot is never empty; clearing it starts the layer over.
		layer.instantiate();
	}
	set_layer(index, layer);
	return true;
}

bool VoxelTerrainData::_get(const StringName &p_name, Variant &r_ret) const {
	const String prop = p_name;
	if (!prop.begins_with("layer_") || !prop.trim_prefix("layer_").is_valid_int()) {
		return false;
	}
	const int index = prop.trim_prefix("layer_").to_int();
	if (index < 0 || index >= layers.size()) {
		return false;
	}
	r_ret = layers[index];
	return true;
}

void VoxelTerrainData::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::NIL, "Layers", PROPERTY_HINT_NONE, "layer_", PROPERTY_USAGE_GROUP));
	for (int i = 0; i < layers.size(); i++) {
		p_list->push_back(PropertyInfo(Variant::OBJECT, vformat("layer_%d", i), PROPERTY_HINT_RESOURCE_TYPE, "TerrainLayer", PROPERTY_USAGE_EDITOR));
	}
}

/* -------------------------------------------------------------------------- */
/*                                   Blocks                                   */
/* -------------------------------------------------------------------------- */

int VoxelTerrainData::get_block_count() const {
	RWLockRead guard(lock);
	return blocks.size();
}

bool VoxelTerrainData::has_block(const Vector3i &p_block) const {
	RWLockRead guard(lock);
	return blocks.has(p_block);
}

TypedArray<Vector3i> VoxelTerrainData::_get_block_coords_bind() const {
	TypedArray<Vector3i> result;
	RWLockRead guard(lock);
	for (const KeyValue<Vector3i, Block> &E : blocks) {
		result.push_back(E.key);
	}
	return result;
}

void VoxelTerrainData::remove_block(const Vector3i &p_block) {
	{
		RWLockWrite guard(lock);
		if (!blocks.erase(p_block)) {
			return;
		}
	}
	_mark_edited();
	_emit_changed_area(AABB(Vector3(p_block * BS), Vector3(BS, BS, BS)));
}

void VoxelTerrainData::clear() {
	AABB bounds = get_edited_bounds();
	{
		RWLockWrite guard(lock);
		if (blocks.is_empty()) {
			return;
		}
		blocks.clear();
	}
	_mark_edited();
	_emit_changed_area(bounds);
}

AABB VoxelTerrainData::get_edited_bounds() const {
	RWLockRead guard(lock);
	AABB bounds;
	bool first = true;
	for (const KeyValue<Vector3i, Block> &E : blocks) {
		const AABB box(Vector3(E.key * BS), Vector3(BS, BS, BS));
		bounds = first ? box : bounds.merge(box);
		first = false;
	}
	return bounds;
}

bool VoxelTerrainData::has_edits_in(const AABB &p_voxel_box) const {
	RWLockRead guard(lock);
	if (blocks.is_empty()) {
		return false;
	}
	const Vector3i b0 = voxel_to_block(Vector3i(p_voxel_box.position.floor()));
	const Vector3i b1 = voxel_to_block(Vector3i(p_voxel_box.get_end().ceil()));
	const Vector3i span = b1 - b0 + Vector3i(1, 1, 1);
	if ((int64_t)span.x * span.y * span.z <= (int64_t)blocks.size()) {
		for (int z = b0.z; z <= b1.z; z++) {
			for (int y = b0.y; y <= b1.y; y++) {
				for (int x = b0.x; x <= b1.x; x++) {
					if (blocks.has(Vector3i(x, y, z))) {
						return true;
					}
				}
			}
		}
		return false;
	}
	for (const KeyValue<Vector3i, Block> &E : blocks) {
		const Vector3i &c = E.key;
		if (c.x >= b0.x && c.y >= b0.y && c.z >= b0.z && c.x <= b1.x && c.y <= b1.y && c.z <= b1.z) {
			return true;
		}
	}
	return false;
}

bool VoxelTerrainData::get_edited_span(const Rect2i &p_columns, int &r_lo, int &r_hi) const {
	RWLockRead guard(lock);
	const Vector2i b0(floor_div(p_columns.position.x, BS), floor_div(p_columns.position.y, BS));
	const Vector2i b1(floor_div(p_columns.get_end().x - 1, BS), floor_div(p_columns.get_end().y - 1, BS));
	bool any = false;
	for (const KeyValue<Vector3i, Block> &E : blocks) {
		const Vector3i &c = E.key;
		if (c.x < b0.x || c.x > b1.x || c.z < b0.y || c.z > b1.y) {
			continue;
		}
		const int lo = c.y * BS;
		const int hi = lo + BS - 1;
		r_lo = any ? MIN(r_lo, lo) : lo;
		r_hi = any ? MAX(r_hi, hi) : hi;
		any = true;
	}
	return any;
}

VoxelTerrainData::Block &VoxelTerrainData::_ensure_block(const Vector3i &p_block) {
	Block *existing = blocks.getptr(p_block);
	if (existing) {
		return *existing;
	}
	// A block is edited starting from what the generator made there.
	Vector<float> sdf;
	Vector<uint8_t> material;
	sdf.resize(BLOCK_VOLUME);
	material.resize(BLOCK_VOLUME);
	const Vector3i size(BS, BS, BS);
	if (generator.is_valid()) {
		generator->generate(p_block * BS, 1, size, voxel_size, sdf.ptrw(), material.ptrw());
	} else {
		sdf.fill(VoxelGenerator::SDF_LIMIT);
		material.fill(0);
	}
	Block block;
	block.sdf.resize(BLOCK_VOLUME);
	int16_t *dst = block.sdf.ptrw();
	for (int i = 0; i < BLOCK_VOLUME; i++) {
		dst[i] = encode_sdf(sdf[i]);
	}
	block.material = material;
	return blocks.insert(p_block, block)->value;
}

/* -------------------------------------------------------------------------- */
/*                                   Voxels                                   */
/* -------------------------------------------------------------------------- */

void VoxelTerrainData::_read_unlocked(const Vector3i &p_origin, int p_step, const Vector3i &p_size, float *r_sdf, uint8_t *r_material) const {
	if (generator.is_valid()) {
		generator->generate(p_origin, p_step, p_size, voxel_size, r_sdf, r_material);
	} else {
		const int count = p_size.x * p_size.y * p_size.z;
		for (int i = 0; i < count; i++) {
			r_sdf[i] = VoxelGenerator::SDF_LIMIT;
			r_material[i] = 0;
		}
	}
	if (blocks.is_empty()) {
		return;
	}

	const Vector3i last = p_origin + (p_size - Vector3i(1, 1, 1)) * p_step;
	const Vector3i b0 = voxel_to_block(p_origin);
	const Vector3i b1 = voxel_to_block(last);

	const auto copy_block = [&](const Vector3i &p_coord, const Block &p_block) {
		const Vector3i base = p_coord * BS;
		int lo[3];
		int hi[3];
		for (int a = 0; a < 3; a++) {
			lo[a] = MAX(0, ceil_div(base[a] - p_origin[a], p_step));
			hi[a] = MIN(p_size[a] - 1, floor_div(base[a] + BS - 1 - p_origin[a], p_step));
			if (lo[a] > hi[a]) {
				return;
			}
		}
		const int16_t *src_sdf = p_block.sdf.ptr();
		const uint8_t *src_material = p_block.material.ptr();
		for (int z = lo[2]; z <= hi[2]; z++) {
			const int lz = p_origin.z + z * p_step - base.z;
			for (int y = lo[1]; y <= hi[1]; y++) {
				const int ly = p_origin.y + y * p_step - base.y;
				for (int x = lo[0]; x <= hi[0]; x++) {
					const int lx = p_origin.x + x * p_step - base.x;
					const int src = local_index(lx, ly, lz);
					const int dst = (z * p_size.y + y) * p_size.x + x;
					r_sdf[dst] = decode_sdf(src_sdf[src]);
					r_material[dst] = src_material[src];
				}
			}
		}
	};

	// A distant, coarse read can span thousands of blocks; walk whichever of
	// the span and the stored blocks is smaller.
	const Vector3i span = b1 - b0 + Vector3i(1, 1, 1);
	if ((int64_t)span.x * span.y * span.z <= (int64_t)blocks.size()) {
		for (int z = b0.z; z <= b1.z; z++) {
			for (int y = b0.y; y <= b1.y; y++) {
				for (int x = b0.x; x <= b1.x; x++) {
					const Vector3i c(x, y, z);
					const Block *block = blocks.getptr(c);
					if (block) {
						copy_block(c, *block);
					}
				}
			}
		}
	} else {
		for (const KeyValue<Vector3i, Block> &E : blocks) {
			const Vector3i &c = E.key;
			if (c.x >= b0.x && c.y >= b0.y && c.z >= b0.z && c.x <= b1.x && c.y <= b1.y && c.z <= b1.z) {
				copy_block(c, E.value);
			}
		}
	}
}

void VoxelTerrainData::read(const Vector3i &p_origin, int p_step, const Vector3i &p_size, float *r_sdf, uint8_t *r_material) const {
	ERR_FAIL_COND(p_step < 1 || p_size.x < 1 || p_size.y < 1 || p_size.z < 1);
	RWLockRead guard(lock);
	_read_unlocked(p_origin, p_step, p_size, r_sdf, r_material);
}

float VoxelTerrainData::get_sdf(const Vector3i &p_voxel) const {
	float sdf;
	uint8_t material;
	read(p_voxel, 1, Vector3i(1, 1, 1), &sdf, &material);
	return sdf;
}

int VoxelTerrainData::get_material(const Vector3i &p_voxel) const {
	float sdf;
	uint8_t material;
	read(p_voxel, 1, Vector3i(1, 1, 1), &sdf, &material);
	return material;
}

void VoxelTerrainData::set_voxel(const Vector3i &p_voxel, float p_sdf, int p_material) {
	{
		RWLockWrite guard(lock);
		Block &block = _ensure_block(voxel_to_block(p_voxel));
		const Vector3i local = p_voxel - voxel_to_block(p_voxel) * BS;
		const int i = local_index(local.x, local.y, local.z);
		block.sdf.write[i] = encode_sdf(p_sdf);
		if (p_material >= 0) {
			block.material.write[i] = MIN(p_material, MAX_LAYERS - 1);
		}
	}
	_mark_edited();
	_emit_changed_area(AABB(Vector3(p_voxel), Vector3(1, 1, 1)));
}

float VoxelTerrainData::sample_sdf(const Vector3 &p_voxel_position) const {
	const Vector3 base = p_voxel_position.floor();
	const Vector3 f = p_voxel_position - base;
	float s[8];
	uint8_t m[8];
	read(Vector3i(base), 1, Vector3i(2, 2, 2), s, m);
	const float x00 = Math::lerp(s[0], s[1], f.x);
	const float x10 = Math::lerp(s[2], s[3], f.x);
	const float x01 = Math::lerp(s[4], s[5], f.x);
	const float x11 = Math::lerp(s[6], s[7], f.x);
	return Math::lerp(Math::lerp(x00, x10, f.y), Math::lerp(x01, x11, f.y), f.z);
}

Vector3 VoxelTerrainData::sample_normal(const Vector3 &p_voxel_position) const {
	const float e = 0.5;
	const Vector3 g(
			sample_sdf(p_voxel_position + Vector3(e, 0, 0)) - sample_sdf(p_voxel_position - Vector3(e, 0, 0)),
			sample_sdf(p_voxel_position + Vector3(0, e, 0)) - sample_sdf(p_voxel_position - Vector3(0, e, 0)),
			sample_sdf(p_voxel_position + Vector3(0, 0, e)) - sample_sdf(p_voxel_position - Vector3(0, 0, e)));
	return g.length_squared() > 1e-12f ? g.normalized() : Vector3(0, 1, 0);
}

void VoxelTerrainData::find_top_surface(const Vector2i &p_origin, const Vector2i &p_size, int p_top, int p_bottom, float *r_heights, uint8_t *r_layers) const {
	const int columns = p_size.x * p_size.y;
	for (int i = 0; i < columns; i++) {
		r_heights[i] = Math::NaN;
		r_layers[i] = 0;
	}
	if (columns == 0 || p_top <= p_bottom) {
		return;
	}
	// Down in slabs, most columns finding their ground in the first. Slabs
	// share their edge sample, so a surface between two is never missed.
	constexpr int SLAB = 32;
	LocalVector<uint8_t> state; // 0 looking, 1 found, 2 ground at the very top.
	state.resize(columns);
	memset(state.ptr(), 0, columns);
	int remaining = columns;
	LocalVector<float> sdf;
	LocalVector<uint8_t> material;
	for (int y_hi = p_top; y_hi > p_bottom && remaining > 0; y_hi -= SLAB) {
		const int y_lo = MAX(y_hi - SLAB, p_bottom);
		const int ny = y_hi - y_lo + 1;
		sdf.resize(columns * ny);
		material.resize(columns * ny);
		read(Vector3i(p_origin.x, y_lo, p_origin.y), 1, Vector3i(p_size.x, ny, p_size.y), sdf.ptr(), material.ptr());
		for (int z = 0; z < p_size.y; z++) {
			for (int x = 0; x < p_size.x; x++) {
				const int c = z * p_size.x + x;
				if (state[c]) {
					continue;
				}
				const auto at = [&](int p_y) {
					return (z * ny + p_y) * p_size.x + x;
				};
				if (y_hi == p_top && sdf[at(ny - 1)] < 0.0f) {
					// Ground all the way up: the search did not start in the air.
					state[c] = 2;
					remaining--;
					continue;
				}
				for (int k = ny - 1; k > 0; k--) {
					const float above = sdf[at(k)];
					const float below = sdf[at(k - 1)];
					if (above >= 0.0f && below < 0.0f) {
						r_heights[c] = float(y_lo + k - 1) + (-below) / (above - below);
						r_layers[c] = material[at(k - 1)];
						state[c] = 1;
						remaining--;
						break;
					}
				}
			}
		}
	}
}

bool VoxelTerrainData::find_ground(const Vector3 &p_position, float p_range, float &r_height, int &r_layer) const {
	const float fx = p_position.x - Math::floor(p_position.x);
	const float fz = p_position.z - Math::floor(p_position.z);
	const int y0 = (int)Math::floor(p_position.y - p_range);
	const int y1 = (int)Math::ceil(p_position.y + p_range);
	const int ny = y1 - y0 + 1;
	LocalVector<float> sdf;
	LocalVector<uint8_t> material;
	sdf.resize(4 * ny);
	material.resize(4 * ny);
	read(Vector3i((int)Math::floor(p_position.x), y0, (int)Math::floor(p_position.z)), 1, Vector3i(2, ny, 2), sdf.ptr(), material.ptr());
	// Blended between the four columns around the point, as the mesh is.
	const auto value = [&](int p_y) {
		const float s00 = sdf[(0 * ny + p_y) * 2 + 0];
		const float s10 = sdf[(0 * ny + p_y) * 2 + 1];
		const float s01 = sdf[(1 * ny + p_y) * 2 + 0];
		const float s11 = sdf[(1 * ny + p_y) * 2 + 1];
		return Math::lerp(Math::lerp(s00, s10, fx), Math::lerp(s01, s11, fx), fz);
	};
	bool found = false;
	float best = 0.0;
	int best_k = 0;
	float below = value(0);
	for (int k = 1; k < ny; k++) {
		const float above = value(k);
		if (below < 0.0f && above >= 0.0f) {
			const float h = float(y0 + k - 1) + (-below) / (above - below);
			if (!found || Math::abs(h - p_position.y) < Math::abs(best - p_position.y)) {
				best = h;
				best_k = k - 1;
				found = true;
			}
		}
		below = above;
	}
	if (!found) {
		return false;
	}
	r_height = best;
	// The layer of whichever of the four voxels under the surface is deepest.
	int deepest = 0;
	for (int i = 1; i < 4; i++) {
		if (sdf[((i >> 1) * ny + best_k) * 2 + (i & 1)] < sdf[((deepest >> 1) * ny + best_k) * 2 + (deepest & 1)]) {
			deepest = i;
		}
	}
	r_layer = material[((deepest >> 1) * ny + best_k) * 2 + (deepest & 1)];
	return true;
}

/* -------------------------------------------------------------------------- */
/*                                   Editing                                  */
/* -------------------------------------------------------------------------- */

AABB VoxelTerrainData::get_edit_bounds(Operation p_operation, const Vector3 &p_center, float p_radius) {
	// The brush reaches a voxel past its radius, and smoothing reads one more.
	const float reach = p_radius + 2.0f;
	return AABB(p_center - Vector3(reach, reach, reach), Vector3(reach, reach, reach) * 2.0f);
}

AABB VoxelTerrainData::edit_sphere(Operation p_operation, const Vector3 &p_center, float p_radius, float p_amount, float p_hardness, int p_layer, float p_height, uint32_t p_seed) {
	ERR_FAIL_COND_V(p_radius <= 0.0f, AABB());
	const float reach = p_radius + 1.0f;
	const Vector3i lo = Vector3i((p_center - Vector3(reach, reach, reach)).floor());
	const Vector3i hi = Vector3i((p_center + Vector3(reach, reach, reach)).ceil());
	const Vector3i size = hi - lo + Vector3i(1, 1, 1);
	// Read with a margin of one, for smoothing's neighbours.
	const Vector3i read_origin = lo - Vector3i(1, 1, 1);
	const Vector3i read_size = size + Vector3i(2, 2, 2);
	const int read_count = read_size.x * read_size.y * read_size.z;
	const bool set_layer = p_layer >= 0;
	const uint8_t layer = CLAMP(p_layer, 0, MAX_LAYERS - 1);

	Vector<float> sdf;
	Vector<uint8_t> material;
	sdf.resize(read_count);
	material.resize(read_count);
	Vector<float> generated_sdf;
	Vector<uint8_t> generated_material;

	Vector<float> out_sdf;
	Vector<uint8_t> out_material;
	Vector3i changed_lo = hi;
	Vector3i changed_hi = lo;
	bool any = false;

	{
		RWLockWrite guard(lock);
		_read_unlocked(read_origin, 1, read_size, sdf.ptrw(), material.ptrw());
		if (p_operation == OPERATION_ERASE_PAINT) {
			// Erasing paint returns voxels to the layer the generator gave them.
			generated_sdf.resize(read_count);
			generated_material.resize(read_count);
			if (generator.is_valid()) {
				generator->generate(read_origin, 1, read_size, voxel_size, generated_sdf.ptrw(), generated_material.ptrw());
			} else {
				generated_material.fill(0);
			}
		}

		out_sdf = sdf;
		out_material = material;
		const float *s = sdf.ptr();
		float *os = out_sdf.ptrw();
		uint8_t *om = out_material.ptrw();
		const int sx = 1;
		const int sy = read_size.x;
		const int sz = read_size.x * read_size.y;

		for (int z = 0; z < size.z; z++) {
			for (int y = 0; y < size.y; y++) {
				for (int x = 0; x < size.x; x++) {
					const Vector3i voxel = lo + Vector3i(x, y, z);
					const float d = (Vector3(voxel) - p_center).length();
					const float w = brush_falloff(d, reach, p_hardness);
					if (w <= 0.0f) {
						continue;
					}
					const int i = ((z + 1) * read_size.y + (y + 1)) * read_size.x + (x + 1);
					const float old = s[i];
					float value = old;
					uint8_t mat = material[i];
					switch (p_operation) {
						case OPERATION_ADD: {
							// Grow the surface outward by up to p_amount, but never
							// past the sphere, so holding the brush fills it and
							// then stops.
							const float target = d - p_radius;
							value = MAX(old - p_amount * w, MIN(old, target));
							if (set_layer && value < 0.0f && old >= 0.0f) {
								mat = layer;
							}
						} break;
						case OPERATION_REMOVE: {
							const float target = p_radius - d;
							value = MIN(old + p_amount * w, MAX(old, target));
						} break;
						case OPERATION_SMOOTH: {
							const float average = (s[i - sx] + s[i + sx] + s[i - sy] + s[i + sy] + s[i - sz] + s[i + sz]) / 6.0f;
							value = old + (average - old) * MIN(p_amount * w, 1.0f);
						} break;
						case OPERATION_FLATTEN: {
							const float target = float(voxel.y) - p_height;
							value = old + (target - old) * MIN(p_amount * w, 1.0f);
							if (set_layer && value < 0.0f && old >= 0.0f) {
								mat = layer;
							}
						} break;
						case OPERATION_PAINT: {
							// Only voxels at or under the surface show, and a voxel
							// holds one layer, so a soft brush dithers its edge;
							// the mesh blends between neighbouring voxels.
							if (old < 1.5f && hash01(voxel, p_seed) < p_amount * w) {
								mat = layer;
							}
						} break;
						case OPERATION_ERASE_PAINT: {
							if (old < 1.5f && hash01(voxel, p_seed) < p_amount * w) {
								mat = generated_material[i];
							}
						} break;
					}
					value = CLAMP(value, -VoxelGenerator::SDF_LIMIT, VoxelGenerator::SDF_LIMIT);
					if (Math::abs(value - old) > 1e-4f || mat != material[i]) {
						os[i] = value;
						om[i] = mat;
						changed_lo = changed_lo.min(voxel);
						changed_hi = changed_hi.max(voxel);
						any = true;
					}
				}
			}
		}

		if (any) {
			const Vector3i b0 = voxel_to_block(changed_lo);
			const Vector3i b1 = voxel_to_block(changed_hi);
			for (int bz = b0.z; bz <= b1.z; bz++) {
				for (int by = b0.y; by <= b1.y; by++) {
					for (int bx = b0.x; bx <= b1.x; bx++) {
						const Vector3i coord(bx, by, bz);
						Block &block = _ensure_block(coord);
						int16_t *dst_sdf = block.sdf.ptrw();
						uint8_t *dst_material = block.material.ptrw();
						const Vector3i base = coord * BS;
						const Vector3i from = changed_lo.max(base);
						const Vector3i to = changed_hi.min(base + Vector3i(BS - 1, BS - 1, BS - 1));
						for (int z = from.z; z <= to.z; z++) {
							for (int y = from.y; y <= to.y; y++) {
								for (int x = from.x; x <= to.x; x++) {
									const int i = ((z - read_origin.z) * read_size.y + (y - read_origin.y)) * read_size.x + (x - read_origin.x);
									const int l = local_index(x - base.x, y - base.y, z - base.z);
									dst_sdf[l] = encode_sdf(out_sdf[i]);
									dst_material[l] = out_material[i];
								}
							}
						}
					}
				}
			}
		}
	}

	if (!any) {
		return AABB();
	}
	const AABB changed(Vector3(changed_lo), Vector3(changed_hi - changed_lo + Vector3i(1, 1, 1)));
	_mark_edited();
	_emit_changed_area(changed);
	return changed;
}

/* -------------------------------------------------------------------------- */
/*                              Storage and undo                              */
/* -------------------------------------------------------------------------- */

Dictionary VoxelTerrainData::_get_snapshot(const Vector3i &p_block) const {
	Dictionary d;
	d["block"] = p_block;
	RWLockRead guard(lock);
	const Block *block = blocks.getptr(p_block);
	d["exists"] = block != nullptr;
	if (block) {
		d["sdf"] = pack((const uint8_t *)block->sdf.ptr(), BLOCK_VOLUME * sizeof(int16_t));
		d["material"] = pack(block->material.ptr(), BLOCK_VOLUME);
	}
	return d;
}

void VoxelTerrainData::_apply_snapshots(const Array &p_snapshots) {
	AABB changed;
	bool any = false;
	{
		RWLockWrite guard(lock);
		for (int i = 0; i < p_snapshots.size(); i++) {
			const Dictionary d = p_snapshots[i];
			const Vector3i coord = d.get("block", Vector3i());
			if ((bool)d.get("exists", false)) {
				Block block;
				block.sdf.resize(BLOCK_VOLUME);
				block.material.resize(BLOCK_VOLUME);
				if (!unpack(d.get("sdf", PackedByteArray()), (uint8_t *)block.sdf.ptrw(), BLOCK_VOLUME * sizeof(int16_t)) || !unpack(d.get("material", PackedByteArray()), block.material.ptrw(), BLOCK_VOLUME)) {
					ERR_CONTINUE_MSG(true, "Corrupt voxel block snapshot.");
				}
				blocks.insert(coord, block);
			} else {
				blocks.erase(coord);
			}
			const AABB box(Vector3(coord * BS), Vector3(BS, BS, BS));
			changed = any ? changed.merge(box) : box;
			any = true;
		}
	}
	if (any) {
		_mark_edited();
		_emit_changed_area(changed);
	}
}

void VoxelTerrainData::_set_blocks(const Array &p_blocks) {
	{
		RWLockWrite guard(lock);
		blocks.clear();
		// Stored as [coord, sdf, material, ...], each compressed.
		for (int i = 0; i + 2 < p_blocks.size(); i += 3) {
			const Vector3i coord = p_blocks[i];
			Block block;
			block.sdf.resize(BLOCK_VOLUME);
			block.material.resize(BLOCK_VOLUME);
			if (!unpack(p_blocks[i + 1], (uint8_t *)block.sdf.ptrw(), BLOCK_VOLUME * sizeof(int16_t)) || !unpack(p_blocks[i + 2], block.material.ptrw(), BLOCK_VOLUME)) {
				ERR_CONTINUE_MSG(true, vformat("Voxel block %s is corrupt and was skipped.", coord));
			}
			blocks.insert(coord, block);
		}
	}
	_generator_changed();
}

Array VoxelTerrainData::_get_blocks() const {
	RWLockRead guard(lock);
	// Sorted, so saving the same terrain twice writes the same file.
	Vector<Vector3i> coords;
	for (const KeyValue<Vector3i, Block> &E : blocks) {
		coords.push_back(E.key);
	}
	coords.sort();
	Array result;
	for (const Vector3i &coord : coords) {
		const Block &block = blocks[coord];
		result.push_back(coord);
		result.push_back(pack((const uint8_t *)block.sdf.ptr(), BLOCK_VOLUME * sizeof(int16_t)));
		result.push_back(pack(block.material.ptr(), BLOCK_VOLUME));
	}
	return result;
}

/* -------------------------------------------------------------------------- */
/*                                  Bindings                                  */
/* -------------------------------------------------------------------------- */

void VoxelTerrainData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_voxel_size", "size"), &VoxelTerrainData::set_voxel_size);
	ClassDB::bind_method(D_METHOD("get_voxel_size"), &VoxelTerrainData::get_voxel_size);
	ClassDB::bind_method(D_METHOD("set_generator", "generator"), &VoxelTerrainData::set_generator);
	ClassDB::bind_method(D_METHOD("get_generator"), &VoxelTerrainData::get_generator);

	ClassDB::bind_method(D_METHOD("get_layer_count"), &VoxelTerrainData::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "index"), &VoxelTerrainData::get_layer);
	ClassDB::bind_method(D_METHOD("set_layer", "index", "layer"), &VoxelTerrainData::set_layer);
	ClassDB::bind_method(D_METHOD("add_layer", "layer"), &VoxelTerrainData::add_layer);
	ClassDB::bind_method(D_METHOD("remove_layer", "index"), &VoxelTerrainData::remove_layer);
	ClassDB::bind_method(D_METHOD("move_layer", "from", "to"), &VoxelTerrainData::move_layer);

	ClassDB::bind_method(D_METHOD("get_block_count"), &VoxelTerrainData::get_block_count);
	ClassDB::bind_method(D_METHOD("get_block_coords"), &VoxelTerrainData::_get_block_coords_bind);
	ClassDB::bind_method(D_METHOD("has_block", "block"), &VoxelTerrainData::has_block);
	ClassDB::bind_method(D_METHOD("remove_block", "block"), &VoxelTerrainData::remove_block);
	ClassDB::bind_method(D_METHOD("clear"), &VoxelTerrainData::clear);
	ClassDB::bind_method(D_METHOD("get_edited_bounds"), &VoxelTerrainData::get_edited_bounds);

	ClassDB::bind_method(D_METHOD("get_sdf", "voxel"), &VoxelTerrainData::get_sdf);
	ClassDB::bind_method(D_METHOD("get_material", "voxel"), &VoxelTerrainData::get_material);
	ClassDB::bind_method(D_METHOD("set_voxel", "voxel", "sdf", "material"), &VoxelTerrainData::set_voxel, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("sample_sdf", "voxel_position"), &VoxelTerrainData::sample_sdf);
	ClassDB::bind_method(D_METHOD("sample_normal", "voxel_position"), &VoxelTerrainData::sample_normal);
	ClassDB::bind_method(D_METHOD("edit_sphere", "operation", "center", "radius", "amount", "hardness", "layer", "height", "seed"), &VoxelTerrainData::edit_sphere, DEFVAL(1.0), DEFVAL(-1), DEFVAL(0.0), DEFVAL(0));

	ClassDB::bind_method(D_METHOD("_set_blocks", "blocks"), &VoxelTerrainData::_set_blocks);
	ClassDB::bind_method(D_METHOD("_get_blocks"), &VoxelTerrainData::_get_blocks);
	ClassDB::bind_method(D_METHOD("_set_layers", "layers"), &VoxelTerrainData::_set_layers);
	ClassDB::bind_method(D_METHOD("_get_layers"), &VoxelTerrainData::_get_layers);
	ClassDB::bind_method(D_METHOD("_get_snapshot", "block"), &VoxelTerrainData::_get_snapshot);
	ClassDB::bind_method(D_METHOD("_apply_snapshots", "snapshots"), &VoxelTerrainData::_apply_snapshots);
	ClassDB::bind_method(D_METHOD("_get_layers_snapshot"), &VoxelTerrainData::_get_layers_snapshot);
	ClassDB::bind_method(D_METHOD("_apply_layers_snapshot", "layers", "snapshots"), &VoxelTerrainData::_apply_layers_snapshot);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_size", PROPERTY_HINT_RANGE, "0.05,16,0.01,suffix:m"), "set_voxel_size", "get_voxel_size");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "generator", PROPERTY_HINT_RESOURCE_TYPE, "VoxelGenerator"), "set_generator", "get_generator");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, "TerrainLayer", PROPERTY_USAGE_STORAGE), "_set_layers", "_get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "blocks", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_blocks", "_get_blocks");

	ADD_SIGNAL(MethodInfo("changed_area", PropertyInfo(Variant::AABB, "voxel_box")));
	ADD_SIGNAL(MethodInfo("generator_changed"));
	ADD_SIGNAL(MethodInfo("layers_changed"));

	BIND_CONSTANT(BLOCK_SIZE);
	BIND_CONSTANT(MAX_LAYERS);

	BIND_ENUM_CONSTANT(OPERATION_ADD);
	BIND_ENUM_CONSTANT(OPERATION_REMOVE);
	BIND_ENUM_CONSTANT(OPERATION_SMOOTH);
	BIND_ENUM_CONSTANT(OPERATION_FLATTEN);
	BIND_ENUM_CONSTANT(OPERATION_PAINT);
	BIND_ENUM_CONSTANT(OPERATION_ERASE_PAINT);
}
