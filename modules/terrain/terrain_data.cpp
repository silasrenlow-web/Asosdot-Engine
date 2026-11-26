/**************************************************************************/
/*  terrain_data.cpp                                                      */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_data.h"

#include "core/core_string_names.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

constexpr uint8_t SPLAT_FILL_FIRST[4] = { 255, 0, 0, 0 };
constexpr uint8_t SPLAT_FILL_REST[4] = { 0, 0, 0, 0 };
constexpr uint8_t COLOR_FILL[4] = { 255, 255, 255, 0 };
constexpr uint8_t HOLE_FILL[1] = { 0 };

Ref<Image> copy_image(const Ref<Image> &p_image) {
	if (p_image.is_null()) {
		return Ref<Image>();
	}
	// The data Vector is copy on write, so this costs nothing until one side
	// is modified, which is exactly what an undo snapshot wants.
	return Image::create_from_data(p_image->get_width(), p_image->get_height(), p_image->has_mipmaps(), p_image->get_format(), p_image->get_data());
}

bool is_valid_map(const Ref<Image> &p_image, int p_size, Image::Format p_format) {
	return p_image.is_valid() && p_image->get_width() == p_size && p_image->get_height() == p_size && p_image->get_format() == p_format && !p_image->has_mipmaps();
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                  Regions                                   */
/* -------------------------------------------------------------------------- */

Ref<Image> TerrainData::_make_map(int p_size, Image::Format p_format, const uint8_t *p_fill, int p_fill_size) {
	Vector<uint8_t> bytes;
	bytes.resize(p_size * p_size * p_fill_size);
	uint8_t *w = bytes.ptrw();
	const int count = p_size * p_size;
	for (int i = 0; i < count; i++) {
		memcpy(w + i * p_fill_size, p_fill, p_fill_size);
	}
	return Image::create_from_data(p_size, p_size, false, p_format, bytes);
}

TerrainData::Region *TerrainData::_ensure_region(const Vector2i &p_region) {
	Region *region = regions.getptr(p_region);
	if (region) {
		return region;
	}
	const float zero = 0.0;
	Region created;
	created.height = _make_map(region_size, Image::FORMAT_RF, (const uint8_t *)&zero, sizeof(float));
	regions.insert(p_region, created);
	return regions.getptr(p_region);
}

Ref<Image> TerrainData::_ensure_splat(Region &r_region, int p_map) {
	if (r_region.splat[p_map].is_null()) {
		r_region.splat[p_map] = _make_map(region_size, Image::FORMAT_RGBA8, p_map == 0 ? SPLAT_FILL_FIRST : SPLAT_FILL_REST, 4);
	}
	return r_region.splat[p_map];
}

Ref<Image> TerrainData::_ensure_color(Region &r_region) {
	if (r_region.color.is_null()) {
		r_region.color = _make_map(region_size, Image::FORMAT_RGBA8, COLOR_FILL, 4);
	}
	return r_region.color;
}

Ref<Image> TerrainData::_ensure_holes(Region &r_region) {
	if (r_region.holes.is_null()) {
		r_region.holes = _make_map(region_size, Image::FORMAT_R8, HOLE_FILL, 1);
	}
	return r_region.holes;
}

void TerrainData::_mark_edited() {
#ifdef TOOLS_ENABLED
	// The editor saves external resources that are marked edited along with
	// the scene, which is how a terrain's data file gets written.
	set_edited(true);
#endif
}

void TerrainData::_emit_region_changed(const Vector2i &p_region, uint32_t p_maps) {
	_mark_edited();
	emit_signal(SNAME("region_changed"), p_region, (int)p_maps);
}

void TerrainData::_emit_regions_changed() {
	_mark_edited();
	emit_signal(SNAME("regions_changed"));
}

void TerrainData::notify_region_changed(const Vector2i &p_region, int p_maps) {
	_emit_region_changed(p_region, p_maps);
}

void TerrainData::set_region_size(int p_size) {
	ERR_FAIL_COND_MSG(p_size < 32 || p_size > 2048 || (p_size & (p_size - 1)) != 0, "Region size must be a power of two between 32 and 2048.");
	if (p_size == region_size) {
		return;
	}
	ERR_FAIL_COND_MSG(!regions.is_empty(), "The region size can only be changed while the terrain has no regions. Export the heightmap, clear the terrain and import it again instead.");
	region_size = p_size;
	_emit_regions_changed();
	emit_changed();
}

TypedArray<Vector2i> TerrainData::get_region_coords() const {
	TypedArray<Vector2i> result;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		result.push_back(E.key);
	}
	return result;
}

Vector<Vector2i> TerrainData::get_region_coord_list() const {
	Vector<Vector2i> result;
	result.resize(regions.size());
	int i = 0;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		result.write[i++] = E.key;
	}
	return result;
}

bool TerrainData::add_region(const Vector2i &p_region, float p_height) {
	if (regions.has(p_region)) {
		return false;
	}
	Region created;
	created.height = _make_map(region_size, Image::FORMAT_RF, (const uint8_t *)&p_height, sizeof(float));
	regions.insert(p_region, created);
	_emit_regions_changed();
	return true;
}

void TerrainData::remove_region(const Vector2i &p_region) {
	if (regions.erase(p_region)) {
		_emit_regions_changed();
	}
}

void TerrainData::clear() {
	if (regions.is_empty()) {
		return;
	}
	regions.clear();
	_emit_regions_changed();
}

Rect2i TerrainData::get_vertex_bounds() const {
	Rect2i bounds;
	bool first = true;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		const Rect2i rect(E.key * region_size, Size2i(region_size, region_size));
		bounds = first ? rect : bounds.merge(rect);
		first = false;
	}
	return bounds;
}

Vector2 TerrainData::get_height_range() const {
	float lo = Math::INF;
	float hi = -Math::INF;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		const float *h = (const float *)E.value.height->ptr();
		const int count = region_size * region_size;
		for (int i = 0; i < count; i++) {
			lo = MIN(lo, h[i]);
			hi = MAX(hi, h[i]);
		}
	}
	if (lo > hi) {
		return Vector2();
	}
	return Vector2(lo, hi);
}

/* -------------------------------------------------------------------------- */
/*                                    Maps                                    */
/* -------------------------------------------------------------------------- */

Ref<Image> TerrainData::get_height_map(const Vector2i &p_region) const {
	const Region *region = regions.getptr(p_region);
	return region ? region->height : Ref<Image>();
}

void TerrainData::set_height_map(const Vector2i &p_region, const Ref<Image> &p_image) {
	ERR_FAIL_COND_MSG(!is_valid_map(p_image, region_size, Image::FORMAT_RF), vformat("A height map must be a %dx%d FORMAT_RF image without mipmaps.", region_size, region_size));
	const bool created = !regions.has(p_region);
	_ensure_region(p_region)->height = p_image;
	if (created) {
		_emit_regions_changed();
	} else {
		_emit_region_changed(p_region, MAP_HEIGHT);
	}
}

Ref<Image> TerrainData::get_splat_map(const Vector2i &p_region, int p_index) const {
	ERR_FAIL_INDEX_V(p_index, SPLAT_MAP_COUNT, Ref<Image>());
	const Region *region = regions.getptr(p_region);
	return region ? region->splat[p_index] : Ref<Image>();
}

void TerrainData::set_splat_map(const Vector2i &p_region, int p_index, const Ref<Image> &p_image) {
	ERR_FAIL_INDEX(p_index, SPLAT_MAP_COUNT);
	Region *region = regions.getptr(p_region);
	ERR_FAIL_NULL_MSG(region, "No region at those coordinates.");
	ERR_FAIL_COND_MSG(p_image.is_valid() && !is_valid_map(p_image, region_size, Image::FORMAT_RGBA8), vformat("A splat map must be a %dx%d FORMAT_RGBA8 image without mipmaps, or null.", region_size, region_size));
	region->splat[p_index] = p_image;
	_emit_region_changed(p_region, MAP_SPLAT);
}

Ref<Image> TerrainData::get_color_map(const Vector2i &p_region) const {
	const Region *region = regions.getptr(p_region);
	return region ? region->color : Ref<Image>();
}

void TerrainData::set_color_map(const Vector2i &p_region, const Ref<Image> &p_image) {
	Region *region = regions.getptr(p_region);
	ERR_FAIL_NULL_MSG(region, "No region at those coordinates.");
	ERR_FAIL_COND_MSG(p_image.is_valid() && !is_valid_map(p_image, region_size, Image::FORMAT_RGBA8), vformat("A color map must be a %dx%d FORMAT_RGBA8 image without mipmaps, or null.", region_size, region_size));
	region->color = p_image;
	_emit_region_changed(p_region, MAP_COLOR);
}

Ref<Image> TerrainData::get_hole_map(const Vector2i &p_region) const {
	const Region *region = regions.getptr(p_region);
	return region ? region->holes : Ref<Image>();
}

void TerrainData::set_hole_map(const Vector2i &p_region, const Ref<Image> &p_image) {
	Region *region = regions.getptr(p_region);
	ERR_FAIL_NULL_MSG(region, "No region at those coordinates.");
	ERR_FAIL_COND_MSG(p_image.is_valid() && !is_valid_map(p_image, region_size, Image::FORMAT_R8), vformat("A hole map must be a %dx%d FORMAT_R8 image without mipmaps, or null.", region_size, region_size));
	region->holes = p_image;
	_emit_region_changed(p_region, MAP_HOLES);
}

/* -------------------------------------------------------------------------- */
/*                               Single vertices                              */
/* -------------------------------------------------------------------------- */

float TerrainData::get_height(const Vector2i &p_vertex) const {
	const Vector2i r = vertex_to_region(p_vertex);
	const Region *region = regions.getptr(r);
	if (!region) {
		return Math::NaN;
	}
	const Vector2i t = p_vertex - r * region_size;
	return ((const float *)region->height->ptr())[t.y * region_size + t.x];
}

void TerrainData::set_height(const Vector2i &p_vertex, float p_height) {
	const Vector2i r = vertex_to_region(p_vertex);
	Region *region = regions.getptr(r);
	ERR_FAIL_NULL_MSG(region, "No region covers that vertex. Add one with add_region() first.");
	const Vector2i t = p_vertex - r * region_size;
	((float *)region->height->ptrw())[t.y * region_size + t.x] = p_height;
	_emit_region_changed(r, MAP_HEIGHT);
}

bool TerrainData::is_hole(const Vector2i &p_vertex) const {
	const Vector2i r = vertex_to_region(p_vertex);
	const Region *region = regions.getptr(r);
	if (!region || region->holes.is_null()) {
		return false;
	}
	const Vector2i t = p_vertex - r * region_size;
	return region->holes->ptr()[t.y * region_size + t.x] != 0;
}

void TerrainData::set_hole(const Vector2i &p_vertex, bool p_hole) {
	const Vector2i r = vertex_to_region(p_vertex);
	Region *region = regions.getptr(r);
	ERR_FAIL_NULL_MSG(region, "No region covers that vertex.");
	if (!p_hole && region->holes.is_null()) {
		return;
	}
	const Vector2i t = p_vertex - r * region_size;
	_ensure_holes(*region)->ptrw()[t.y * region_size + t.x] = p_hole ? 255 : 0;
	_emit_region_changed(r, MAP_HOLES);
}

Color TerrainData::get_color(const Vector2i &p_vertex) const {
	const Vector2i r = vertex_to_region(p_vertex);
	const Region *region = regions.getptr(r);
	if (!region || region->color.is_null()) {
		return Color(1, 1, 1, 0);
	}
	const Vector2i t = p_vertex - r * region_size;
	const uint8_t *px = region->color->ptr() + (t.y * region_size + t.x) * 4;
	return Color(px[0] / 255.0, px[1] / 255.0, px[2] / 255.0, px[3] / 255.0);
}

void TerrainData::set_color(const Vector2i &p_vertex, const Color &p_color) {
	const Vector2i r = vertex_to_region(p_vertex);
	Region *region = regions.getptr(r);
	ERR_FAIL_NULL_MSG(region, "No region covers that vertex.");
	const Vector2i t = p_vertex - r * region_size;
	uint8_t *px = _ensure_color(*region)->ptrw() + (t.y * region_size + t.x) * 4;
	px[0] = (uint8_t)CLAMP(Math::round(p_color.r * 255.0), 0, 255);
	px[1] = (uint8_t)CLAMP(Math::round(p_color.g * 255.0), 0, 255);
	px[2] = (uint8_t)CLAMP(Math::round(p_color.b * 255.0), 0, 255);
	px[3] = (uint8_t)CLAMP(Math::round(p_color.a * 255.0), 0, 255);
	_emit_region_changed(r, MAP_COLOR);
}

PackedFloat32Array TerrainData::get_layer_weights(const Vector2i &p_vertex) const {
	PackedFloat32Array result;
	result.resize(MAX_LAYERS);
	uint8_t w[MAX_LAYERS];
	read_splat(Rect2i(p_vertex, Size2i(1, 1)), w);
	for (int i = 0; i < MAX_LAYERS; i++) {
		result.set(i, w[i] / 255.0);
	}
	return result;
}

void TerrainData::set_layer_weights(const Vector2i &p_vertex, const PackedFloat32Array &p_weights) {
	ERR_FAIL_COND(!regions.has(vertex_to_region(p_vertex)));
	float sum = 0.0;
	for (int i = 0; i < MIN(p_weights.size(), MAX_LAYERS); i++) {
		sum += MAX(p_weights[i], 0.0f);
	}
	ERR_FAIL_COND_MSG(sum <= 0.0, "At least one layer must have a positive weight.");
	uint8_t w[MAX_LAYERS] = {};
	for (int i = 0; i < MIN(p_weights.size(), MAX_LAYERS); i++) {
		w[i] = (uint8_t)CLAMP(Math::round(MAX(p_weights[i], 0.0f) / sum * 255.0), 0, 255);
	}
	write_splat(Rect2i(p_vertex, Size2i(1, 1)), w);
}

bool TerrainData::get_render_height(const Vector2i &p_vertex, float &r_height) const {
	Vector2i v = p_vertex;
	Vector2i r = vertex_to_region(v);
	const Region *region = regions.getptr(r);
	if (!region) {
		// Only a vertex that closes off a region's far edge may borrow from it.
		const bool on_x = v.x - r.x * region_size == 0;
		const bool on_z = v.y - r.y * region_size == 0;
		const Vector2i candidates[3] = {
			Vector2i(on_x ? v.x - 1 : v.x, v.y),
			Vector2i(v.x, on_z ? v.y - 1 : v.y),
			Vector2i(on_x ? v.x - 1 : v.x, on_z ? v.y - 1 : v.y),
		};
		for (const Vector2i &c : candidates) {
			if (c == v) {
				continue;
			}
			const Vector2i cr = vertex_to_region(c);
			region = regions.getptr(cr);
			if (region) {
				v = c;
				r = cr;
				break;
			}
		}
		if (!region) {
			return false;
		}
	}
	const Vector2i t = v - r * region_size;
	const int index = t.y * region_size + t.x;
	if (region->holes.is_valid() && region->holes->ptr()[index] != 0) {
		return false;
	}
	r_height = ((const float *)region->height->ptr())[index];
	return true;
}

bool TerrainData::get_height_interpolated(const Vector2 &p_vertex_pos, float &r_height) const {
	const Vector2 base = p_vertex_pos.floor();
	const Vector2i v((int)base.x, (int)base.y);
	const float fx = p_vertex_pos.x - base.x;
	const float fz = p_vertex_pos.y - base.y;

	// The diagonal runs from (x + 1, z) to (x, z + 1), matching the render
	// mesh and HeightMapShape3D.
	float h10, h01;
	if (!get_render_height(v + Vector2i(1, 0), h10) || !get_render_height(v + Vector2i(0, 1), h01)) {
		return false;
	}
	if (fx + fz <= 1.0f) {
		float h00;
		if (!get_render_height(v, h00)) {
			return false;
		}
		r_height = h00 + fx * (h10 - h00) + fz * (h01 - h00);
	} else {
		float h11;
		if (!get_render_height(v + Vector2i(1, 1), h11)) {
			return false;
		}
		r_height = h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fz) * (h10 - h11);
	}
	return true;
}

Vector3 TerrainData::get_normal_interpolated(const Vector2 &p_vertex_pos, float p_spacing) const {
	float c, l, r, d, u;
	if (!get_height_interpolated(p_vertex_pos, c)) {
		return Vector3();
	}
	// A hole next door must not tip the normal over, so fall back to the
	// centre sample on that side.
	if (!get_height_interpolated(p_vertex_pos - Vector2(1, 0), l)) {
		l = c;
	}
	if (!get_height_interpolated(p_vertex_pos + Vector2(1, 0), r)) {
		r = c;
	}
	if (!get_height_interpolated(p_vertex_pos - Vector2(0, 1), d)) {
		d = c;
	}
	if (!get_height_interpolated(p_vertex_pos + Vector2(0, 1), u)) {
		u = c;
	}
	return Vector3(l - r, 2.0f * p_spacing, d - u).normalized();
}

void TerrainData::get_layer_weights_interpolated(const Vector2 &p_vertex_pos, float *r_weights) const {
	const Vector2 base = p_vertex_pos.floor();
	const Vector2 f = p_vertex_pos - base;
	uint8_t w[4 * MAX_LAYERS];
	read_splat(Rect2i(Vector2i((int)base.x, (int)base.y), Size2i(2, 2)), w);
	const float fx = f.x;
	const float fy = f.y;
	const float k[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
	for (int i = 0; i < MAX_LAYERS; i++) {
		r_weights[i] = (w[i] * k[0] + w[MAX_LAYERS + i] * k[1] + w[2 * MAX_LAYERS + i] * k[2] + w[3 * MAX_LAYERS + i] * k[3]) / 255.0f;
	}
}

Color TerrainData::get_color_interpolated(const Vector2 &p_vertex_pos) const {
	const Vector2 base = p_vertex_pos.floor();
	const Vector2 f = p_vertex_pos - base;
	Color c[4];
	read_colors(Rect2i(Vector2i((int)base.x, (int)base.y), Size2i(2, 2)), c);
	return c[0].lerp(c[1], f.x).lerp(c[2].lerp(c[3], f.x), f.y);
}

/* -------------------------------------------------------------------------- */
/*                                   Windows                                  */
/* -------------------------------------------------------------------------- */

// Calls p_func(region, region_rect, intersection) for every region overlapping
// p_rect. Region is null for a gap.
template <typename F>
static void for_each_region_in(const TerrainData *p_data, const Rect2i &p_rect, F &&p_func) {
	if (p_rect.size.x <= 0 || p_rect.size.y <= 0) {
		return;
	}
	const int rs = p_data->get_region_size();
	const Vector2i r0 = p_data->vertex_to_region(p_rect.position);
	const Vector2i r1 = p_data->vertex_to_region(p_rect.position + p_rect.size - Vector2i(1, 1));
	for (int rz = r0.y; rz <= r1.y; rz++) {
		for (int rx = r0.x; rx <= r1.x; rx++) {
			const Vector2i coord(rx, rz);
			const Rect2i region_rect(coord * rs, Size2i(rs, rs));
			const Rect2i isect = region_rect.intersection(p_rect);
			if (isect.size.x <= 0 || isect.size.y <= 0) {
				continue;
			}
			p_func(coord, region_rect, isect);
		}
	}
}

void TerrainData::read_heights(const Rect2i &p_rect, float *r_heights, uint8_t *r_valid) const {
	const int w = p_rect.size.x;
	memset(r_heights, 0, sizeof(float) * p_rect.size.x * p_rect.size.y);
	if (r_valid) {
		memset(r_valid, 0, p_rect.size.x * p_rect.size.y);
	}
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		const Region *region = regions.getptr(p_coord);
		if (!region) {
			return;
		}
		const float *src = (const float *)region->height->ptr();
		for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
			const int dst = (z - p_rect.position.y) * w + (p_isect.position.x - p_rect.position.x);
			memcpy(r_heights + dst, src + (z - p_region_rect.position.y) * region_size + (p_isect.position.x - p_region_rect.position.x), sizeof(float) * p_isect.size.x);
			if (r_valid) {
				memset(r_valid + dst, 1, p_isect.size.x);
			}
		}
	});
}

void TerrainData::write_heights(const Rect2i &p_rect, const float *p_heights) {
	const int w = p_rect.size.x;
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		Region *region = regions.getptr(p_coord);
		if (!region) {
			return;
		}
		float *dst = (float *)region->height->ptrw();
		for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
			memcpy(dst + (z - p_region_rect.position.y) * region_size + (p_isect.position.x - p_region_rect.position.x), p_heights + (z - p_rect.position.y) * w + (p_isect.position.x - p_rect.position.x), sizeof(float) * p_isect.size.x);
		}
		_emit_region_changed(p_coord, MAP_HEIGHT);
	});
}

void TerrainData::read_splat(const Rect2i &p_rect, uint8_t *r_weights) const {
	const int w = p_rect.size.x;
	const int count = p_rect.size.x * p_rect.size.y;
	// Outside any region reads as layer 0, the same as an unpainted region.
	memset(r_weights, 0, count * MAX_LAYERS);
	for (int i = 0; i < count; i++) {
		r_weights[i * MAX_LAYERS] = 255;
	}
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		const Region *region = regions.getptr(p_coord);
		if (!region) {
			return;
		}
		for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
			if (region->splat[m].is_null()) {
				continue;
			}
			const uint8_t *src = region->splat[m]->ptr();
			for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
				for (int x = p_isect.position.x; x < p_isect.get_end().x; x++) {
					const uint8_t *s = src + ((z - p_region_rect.position.y) * region_size + (x - p_region_rect.position.x)) * 4;
					uint8_t *d = r_weights + ((z - p_rect.position.y) * w + (x - p_rect.position.x)) * MAX_LAYERS + m * 4;
					d[0] = s[0];
					d[1] = s[1];
					d[2] = s[2];
					d[3] = s[3];
				}
			}
		}
	});
}

void TerrainData::write_splat(const Rect2i &p_rect, const uint8_t *p_weights) {
	const int w = p_rect.size.x;
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		Region *region = regions.getptr(p_coord);
		if (!region) {
			return;
		}
		for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
			// Leave a map unallocated for as long as nothing in it differs from
			// its default, so painting four layers never allocates the other
			// twelve.
			if (region->splat[m].is_null()) {
				bool needed = false;
				for (int z = p_isect.position.y; z < p_isect.get_end().y && !needed; z++) {
					for (int x = p_isect.position.x; x < p_isect.get_end().x && !needed; x++) {
						const uint8_t *s = p_weights + ((z - p_rect.position.y) * w + (x - p_rect.position.x)) * MAX_LAYERS + m * 4;
						const uint8_t *def = m == 0 ? SPLAT_FILL_FIRST : SPLAT_FILL_REST;
						needed = s[0] != def[0] || s[1] != def[1] || s[2] != def[2] || s[3] != def[3];
					}
				}
				if (!needed) {
					continue;
				}
			}
			uint8_t *dst = _ensure_splat(*region, m)->ptrw();
			for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
				for (int x = p_isect.position.x; x < p_isect.get_end().x; x++) {
					const uint8_t *s = p_weights + ((z - p_rect.position.y) * w + (x - p_rect.position.x)) * MAX_LAYERS + m * 4;
					uint8_t *d = dst + ((z - p_region_rect.position.y) * region_size + (x - p_region_rect.position.x)) * 4;
					d[0] = s[0];
					d[1] = s[1];
					d[2] = s[2];
					d[3] = s[3];
				}
			}
		}
		_emit_region_changed(p_coord, MAP_SPLAT);
	});
}

void TerrainData::read_colors(const Rect2i &p_rect, Color *r_colors) const {
	const int w = p_rect.size.x;
	const int count = p_rect.size.x * p_rect.size.y;
	for (int i = 0; i < count; i++) {
		r_colors[i] = Color(1, 1, 1, 0);
	}
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		const Region *region = regions.getptr(p_coord);
		if (!region || region->color.is_null()) {
			return;
		}
		const uint8_t *src = region->color->ptr();
		for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
			for (int x = p_isect.position.x; x < p_isect.get_end().x; x++) {
				const uint8_t *s = src + ((z - p_region_rect.position.y) * region_size + (x - p_region_rect.position.x)) * 4;
				r_colors[(z - p_rect.position.y) * w + (x - p_rect.position.x)] = Color(s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f);
			}
		}
	});
}

void TerrainData::write_colors(const Rect2i &p_rect, const Color *p_colors) {
	const int w = p_rect.size.x;
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		Region *region = regions.getptr(p_coord);
		if (!region) {
			return;
		}
		uint8_t *dst = _ensure_color(*region)->ptrw();
		for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
			for (int x = p_isect.position.x; x < p_isect.get_end().x; x++) {
				const Color &c = p_colors[(z - p_rect.position.y) * w + (x - p_rect.position.x)];
				uint8_t *d = dst + ((z - p_region_rect.position.y) * region_size + (x - p_region_rect.position.x)) * 4;
				d[0] = (uint8_t)CLAMP((int)Math::round(c.r * 255.0f), 0, 255);
				d[1] = (uint8_t)CLAMP((int)Math::round(c.g * 255.0f), 0, 255);
				d[2] = (uint8_t)CLAMP((int)Math::round(c.b * 255.0f), 0, 255);
				d[3] = (uint8_t)CLAMP((int)Math::round(c.a * 255.0f), 0, 255);
			}
		}
		_emit_region_changed(p_coord, MAP_COLOR);
	});
}

void TerrainData::read_holes(const Rect2i &p_rect, uint8_t *r_holes) const {
	const int w = p_rect.size.x;
	memset(r_holes, 0, p_rect.size.x * p_rect.size.y);
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		const Region *region = regions.getptr(p_coord);
		if (!region || region->holes.is_null()) {
			return;
		}
		const uint8_t *src = region->holes->ptr();
		for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
			memcpy(r_holes + (z - p_rect.position.y) * w + (p_isect.position.x - p_rect.position.x), src + (z - p_region_rect.position.y) * region_size + (p_isect.position.x - p_region_rect.position.x), p_isect.size.x);
		}
	});
}

void TerrainData::write_holes(const Rect2i &p_rect, const uint8_t *p_holes) {
	const int w = p_rect.size.x;
	for_each_region_in(this, p_rect, [&](const Vector2i &p_coord, const Rect2i &p_region_rect, const Rect2i &p_isect) {
		Region *region = regions.getptr(p_coord);
		if (!region) {
			return;
		}
		if (region->holes.is_null()) {
			bool any = false;
			for (int z = p_isect.position.y; z < p_isect.get_end().y && !any; z++) {
				for (int x = p_isect.position.x; x < p_isect.get_end().x && !any; x++) {
					any = p_holes[(z - p_rect.position.y) * w + (x - p_rect.position.x)] != 0;
				}
			}
			if (!any) {
				return;
			}
		}
		uint8_t *dst = _ensure_holes(*region)->ptrw();
		for (int z = p_isect.position.y; z < p_isect.get_end().y; z++) {
			memcpy(dst + (z - p_region_rect.position.y) * region_size + (p_isect.position.x - p_region_rect.position.x), p_holes + (z - p_rect.position.y) * w + (p_isect.position.x - p_rect.position.x), p_isect.size.x);
		}
		_emit_region_changed(p_coord, MAP_HOLES);
	});
}

/* -------------------------------------------------------------------------- */
/*                                   Layers                                   */
/* -------------------------------------------------------------------------- */

void TerrainData::_layer_changed() {
	_mark_edited();
	emit_signal(SNAME("layers_changed"));
}

Ref<TerrainLayer> TerrainData::get_layer(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, layers.size(), Ref<TerrainLayer>());
	return layers[p_index];
}

void TerrainData::set_layer(int p_index, const Ref<TerrainLayer> &p_layer) {
	ERR_FAIL_INDEX(p_index, layers.size());
	ERR_FAIL_COND(p_layer.is_null());
	const Ref<TerrainLayer> old = layers[p_index];
	layers.write[p_index] = p_layer;
	// The same layer may sit in several slots; only let go of it once it is in none.
	if (old.is_valid() && old != p_layer && !layers.has(old)) {
		old->disconnect_changed(callable_mp(this, &TerrainData::_layer_changed));
	}
	if (!p_layer->is_connected(CoreStringName(changed), callable_mp(this, &TerrainData::_layer_changed))) {
		p_layer->connect_changed(callable_mp(this, &TerrainData::_layer_changed));
	}
	_layer_changed();
}

int TerrainData::add_layer(const Ref<TerrainLayer> &p_layer) {
	ERR_FAIL_COND_V(p_layer.is_null(), -1);
	ERR_FAIL_COND_V_MSG(layers.size() >= MAX_LAYERS, -1, vformat("A terrain can have at most %d layers.", MAX_LAYERS));
	layers.push_back(p_layer);
	if (!p_layer->is_connected(CoreStringName(changed), callable_mp(this, &TerrainData::_layer_changed))) {
		p_layer->connect_changed(callable_mp(this, &TerrainData::_layer_changed));
	}
	_layer_changed();
	notify_property_list_changed();
	return layers.size() - 1;
}

void TerrainData::_remap_layers(const Vector<int> &p_old_to_new) {
	// Weights follow their layer to its new slot; a layer that is gone hands its
	// share to whatever else is painted there, or to layer 0 if nothing is.
	for (KeyValue<Vector2i, Region> &E : regions) {
		const Rect2i rect(E.key * region_size, Size2i(region_size, region_size));
		const int count = region_size * region_size;
		Vector<uint8_t> weights;
		weights.resize(count * MAX_LAYERS);
		read_splat(rect, weights.ptrw());
		uint8_t *w = weights.ptrw();
		bool changed = false;
		for (int i = 0; i < count; i++) {
			uint8_t *px = w + i * MAX_LAYERS;
			uint8_t moved[MAX_LAYERS] = {};
			int sum = 0;
			for (int l = 0; l < MAX_LAYERS; l++) {
				const int to = l < p_old_to_new.size() ? p_old_to_new[l] : -1;
				if (to >= 0 && px[l]) {
					moved[to] = px[l];
					sum += px[l];
				}
			}
			if (sum == 0) {
				moved[0] = 255;
			} else if (sum != 255) {
				int total = 0;
				int largest = 0;
				for (int l = 0; l < MAX_LAYERS; l++) {
					moved[l] = (uint8_t)((int)moved[l] * 255 / sum);
					total += moved[l];
					largest = moved[l] > moved[largest] ? l : largest;
				}
				moved[largest] += 255 - total;
			}
			if (memcmp(moved, px, MAX_LAYERS) != 0) {
				memcpy(px, moved, MAX_LAYERS);
				changed = true;
			}
		}
		if (changed) {
			write_splat(rect, w);
		}
	}
}

void TerrainData::remove_layer(int p_index) {
	ERR_FAIL_INDEX(p_index, layers.size());
	Vector<int> remap;
	for (int l = 0; l < MAX_LAYERS; l++) {
		remap.push_back(l < p_index ? l : (l == p_index ? -1 : l - 1));
	}
	const Ref<TerrainLayer> old = layers[p_index];
	layers.remove_at(p_index);
	if (old.is_valid() && !layers.has(old)) {
		old->disconnect_changed(callable_mp(this, &TerrainData::_layer_changed));
	}
	_remap_layers(remap);
	_layer_changed();
	notify_property_list_changed();
}

void TerrainData::move_layer(int p_from, int p_to) {
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
	// order[new] = old; invert it for the remap.
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
	_remap_layers(remap);
	_layer_changed();
}

/* -------------------------------------------------------------------------- */
/*                                 Heightmaps                                 */
/* -------------------------------------------------------------------------- */

Error TerrainData::import_height_image(const Ref<Image> &p_image, const Vector2i &p_vertex_offset, float p_min_height, float p_max_height, bool p_normalized) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), ERR_INVALID_PARAMETER);
	Ref<Image> image = copy_image(p_image);
	if (image->is_compressed()) {
		ERR_FAIL_COND_V_MSG(image->decompress() != OK, ERR_UNAVAILABLE, "The heightmap is compressed in a format this build cannot decompress.");
	}
	image->clear_mipmaps();
	image->convert(Image::FORMAT_RF);

	const int w = image->get_width();
	const int h = image->get_height();
	const Rect2i rect(p_vertex_offset, Size2i(w, h));

	bool regions_added = false;
	const Vector2i r0 = vertex_to_region(rect.position);
	const Vector2i r1 = vertex_to_region(rect.get_end() - Vector2i(1, 1));
	for (int rz = r0.y; rz <= r1.y; rz++) {
		for (int rx = r0.x; rx <= r1.x; rx++) {
			if (!regions.has(Vector2i(rx, rz))) {
				_ensure_region(Vector2i(rx, rz));
				regions_added = true;
			}
		}
	}

	Vector<float> heights;
	heights.resize(w * h);
	const float *src = (const float *)image->ptr();
	float *dst = heights.ptrw();
	for (int i = 0; i < w * h; i++) {
		dst[i] = p_normalized ? Math::lerp(p_min_height, p_max_height, src[i]) : src[i];
	}
	write_heights(rect, dst);

	if (regions_added) {
		_emit_regions_changed();
	}
	return OK;
}

Ref<Image> TerrainData::export_height_image(const Rect2i &p_vertex_rect) const {
	const Rect2i rect = p_vertex_rect.has_area() ? p_vertex_rect : get_vertex_bounds();
	ERR_FAIL_COND_V_MSG(!rect.has_area(), Ref<Image>(), "The terrain is empty.");
	Vector<uint8_t> bytes;
	bytes.resize(rect.size.x * rect.size.y * sizeof(float));
	read_heights(rect, (float *)bytes.ptrw());
	return Image::create_from_data(rect.size.x, rect.size.y, false, Image::FORMAT_RF, bytes);
}

Ref<TerrainData> TerrainData::duplicate_data() const {
	Ref<TerrainData> copy;
	copy.instantiate();
	copy->region_size = region_size;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		Region r;
		r.height = copy_image(E.value.height);
		for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
			r.splat[m] = copy_image(E.value.splat[m]);
		}
		r.color = copy_image(E.value.color);
		r.holes = copy_image(E.value.holes);
		copy->regions.insert(E.key, r);
	}
	copy->_set_layers(_get_layers());
	return copy;
}

/* -------------------------------------------------------------------------- */
/*                                   Storage                                  */
/* -------------------------------------------------------------------------- */

void TerrainData::_set_regions(const Array &p_regions) {
	regions.clear();
	for (int i = 0; i < p_regions.size(); i++) {
		const Dictionary d = p_regions[i];
		const Vector2i coord = d.get("coord", Vector2i());
		Region region;
		region.height = d.get("height", Ref<Image>());
		if (!is_valid_map(region.height, region_size, Image::FORMAT_RF)) {
			ERR_PRINT(vformat("Terrain region %s has a malformed height map and was skipped.", coord));
			continue;
		}
		for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
			const Ref<Image> splat = d.get(vformat("splat%d", m), Ref<Image>());
			region.splat[m] = is_valid_map(splat, region_size, Image::FORMAT_RGBA8) ? splat : Ref<Image>();
		}
		const Ref<Image> color = d.get("color", Ref<Image>());
		region.color = is_valid_map(color, region_size, Image::FORMAT_RGBA8) ? color : Ref<Image>();
		const Ref<Image> holes = d.get("holes", Ref<Image>());
		region.holes = is_valid_map(holes, region_size, Image::FORMAT_R8) ? holes : Ref<Image>();
		regions.insert(coord, region);
	}
	emit_signal(SNAME("regions_changed"));
}

Array TerrainData::_get_regions() const {
	Array result;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		Dictionary d;
		d["coord"] = E.key;
		d["height"] = E.value.height;
		for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
			if (E.value.splat[m].is_valid()) {
				d[vformat("splat%d", m)] = E.value.splat[m];
			}
		}
		if (E.value.color.is_valid()) {
			d["color"] = E.value.color;
		}
		if (E.value.holes.is_valid()) {
			d["holes"] = E.value.holes;
		}
		result.push_back(d);
	}
	return result;
}

void TerrainData::_set_layers(const Array &p_layers) {
	for (const Ref<TerrainLayer> &layer : layers) {
		if (layer.is_valid() && layer->is_connected(CoreStringName(changed), callable_mp(this, &TerrainData::_layer_changed))) {
			layer->disconnect_changed(callable_mp(this, &TerrainData::_layer_changed));
		}
	}
	layers.clear();
	for (int i = 0; i < MIN(p_layers.size(), MAX_LAYERS); i++) {
		Ref<TerrainLayer> layer = p_layers[i];
		if (layer.is_null()) {
			layer.instantiate();
		}
		layers.push_back(layer);
		if (!layer->is_connected(CoreStringName(changed), callable_mp(this, &TerrainData::_layer_changed))) {
			layer->connect_changed(callable_mp(this, &TerrainData::_layer_changed));
		}
	}
	emit_signal(SNAME("layers_changed"));
	notify_property_list_changed();
}

Array TerrainData::_get_layers() const {
	Array result;
	for (const Ref<TerrainLayer> &layer : layers) {
		result.push_back(layer);
	}
	return result;
}

Dictionary TerrainData::_get_snapshot(const Vector2i &p_region, int p_maps) const {
	Dictionary d;
	d["coord"] = p_region;
	d["maps"] = p_maps;
	const Region *region = regions.getptr(p_region);
	if (!region) {
		d["exists"] = false;
		return d;
	}
	d["exists"] = true;
	if (p_maps & MAP_HEIGHT) {
		d["height"] = copy_image(region->height);
	}
	if (p_maps & MAP_SPLAT) {
		Array splat;
		for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
			splat.push_back(copy_image(region->splat[m]));
		}
		d["splat"] = splat;
	}
	if (p_maps & MAP_COLOR) {
		d["color"] = copy_image(region->color);
	}
	if (p_maps & MAP_HOLES) {
		d["holes"] = copy_image(region->holes);
	}
	return d;
}

void TerrainData::_apply_snapshots(const Array &p_snapshots) {
	bool layout_changed = false;
	for (int i = 0; i < p_snapshots.size(); i++) {
		const Dictionary d = p_snapshots[i];
		const Vector2i coord = d["coord"];
		const int maps = d["maps"];
		if (!(bool)d.get("exists", true)) {
			if (regions.erase(coord)) {
				layout_changed = true;
			}
			continue;
		}
		if (!regions.has(coord)) {
			if (!(maps & MAP_HEIGHT)) {
				continue;
			}
			_ensure_region(coord);
			layout_changed = true;
		}
		Region *region = regions.getptr(coord);
		// Copies again, so the snapshot survives being applied more than once
		// by repeated undo and redo.
		if (maps & MAP_HEIGHT) {
			region->height = copy_image(d["height"]);
		}
		if (maps & MAP_SPLAT) {
			const Array splat = d["splat"];
			for (int m = 0; m < SPLAT_MAP_COUNT; m++) {
				region->splat[m] = copy_image(splat[m]);
			}
		}
		if (maps & MAP_COLOR) {
			region->color = copy_image(d["color"]);
		}
		if (maps & MAP_HOLES) {
			region->holes = copy_image(d["holes"]);
		}
		if (!layout_changed) {
			_emit_region_changed(coord, maps);
		}
	}
	if (layout_changed) {
		_emit_regions_changed();
	}
}

Array TerrainData::_get_layers_snapshot() const {
	Array splat;
	for (const KeyValue<Vector2i, Region> &E : regions) {
		splat.push_back(_get_snapshot(E.key, MAP_SPLAT));
	}
	return splat;
}

void TerrainData::_apply_layers_snapshot(const Array &p_layers, const Array &p_splat_snapshots) {
	_set_layers(p_layers);
	_mark_edited();
	_apply_snapshots(p_splat_snapshots);
}

bool TerrainData::_set(const StringName &p_name, const Variant &p_value) {
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

bool TerrainData::_get(const StringName &p_name, Variant &r_ret) const {
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

void TerrainData::_get_property_list(List<PropertyInfo> *p_list) const {
	p_list->push_back(PropertyInfo(Variant::NIL, "Layers", PROPERTY_HINT_NONE, "layer_", PROPERTY_USAGE_GROUP));
	for (int i = 0; i < layers.size(); i++) {
		p_list->push_back(PropertyInfo(Variant::OBJECT, vformat("layer_%d", i), PROPERTY_HINT_RESOURCE_TYPE, "TerrainLayer", PROPERTY_USAGE_EDITOR));
	}
}

void TerrainData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_region_size", "size"), &TerrainData::set_region_size);
	ClassDB::bind_method(D_METHOD("get_region_size"), &TerrainData::get_region_size);

	ClassDB::bind_method(D_METHOD("get_region_count"), &TerrainData::get_region_count);
	ClassDB::bind_method(D_METHOD("get_region_coords"), &TerrainData::get_region_coords);
	ClassDB::bind_method(D_METHOD("has_region", "region"), &TerrainData::has_region);
	ClassDB::bind_method(D_METHOD("add_region", "region", "height"), &TerrainData::add_region, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("remove_region", "region"), &TerrainData::remove_region);
	ClassDB::bind_method(D_METHOD("clear"), &TerrainData::clear);
	ClassDB::bind_method(D_METHOD("get_vertex_bounds"), &TerrainData::get_vertex_bounds);
	ClassDB::bind_method(D_METHOD("get_height_range"), &TerrainData::get_height_range);

	ClassDB::bind_method(D_METHOD("get_height_map", "region"), &TerrainData::get_height_map);
	ClassDB::bind_method(D_METHOD("set_height_map", "region", "image"), &TerrainData::set_height_map);
	ClassDB::bind_method(D_METHOD("get_splat_map", "region", "index"), &TerrainData::get_splat_map);
	ClassDB::bind_method(D_METHOD("set_splat_map", "region", "index", "image"), &TerrainData::set_splat_map);
	ClassDB::bind_method(D_METHOD("get_color_map", "region"), &TerrainData::get_color_map);
	ClassDB::bind_method(D_METHOD("set_color_map", "region", "image"), &TerrainData::set_color_map);
	ClassDB::bind_method(D_METHOD("get_hole_map", "region"), &TerrainData::get_hole_map);
	ClassDB::bind_method(D_METHOD("set_hole_map", "region", "image"), &TerrainData::set_hole_map);

	ClassDB::bind_method(D_METHOD("get_height", "vertex"), &TerrainData::get_height);
	ClassDB::bind_method(D_METHOD("set_height", "vertex", "height"), &TerrainData::set_height);
	ClassDB::bind_method(D_METHOD("is_hole", "vertex"), &TerrainData::is_hole);
	ClassDB::bind_method(D_METHOD("set_hole", "vertex", "hole"), &TerrainData::set_hole);
	ClassDB::bind_method(D_METHOD("get_color", "vertex"), &TerrainData::get_color);
	ClassDB::bind_method(D_METHOD("set_color", "vertex", "color"), &TerrainData::set_color);
	ClassDB::bind_method(D_METHOD("get_layer_weights", "vertex"), &TerrainData::get_layer_weights);
	ClassDB::bind_method(D_METHOD("set_layer_weights", "vertex", "weights"), &TerrainData::set_layer_weights);

	ClassDB::bind_method(D_METHOD("get_layer_count"), &TerrainData::get_layer_count);
	ClassDB::bind_method(D_METHOD("get_layer", "index"), &TerrainData::get_layer);
	ClassDB::bind_method(D_METHOD("set_layer", "index", "layer"), &TerrainData::set_layer);
	ClassDB::bind_method(D_METHOD("add_layer", "layer"), &TerrainData::add_layer);
	ClassDB::bind_method(D_METHOD("remove_layer", "index"), &TerrainData::remove_layer);
	ClassDB::bind_method(D_METHOD("move_layer", "from", "to"), &TerrainData::move_layer);

	ClassDB::bind_method(D_METHOD("import_height_image", "image", "vertex_offset", "min_height", "max_height", "normalized"), &TerrainData::import_height_image, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("export_height_image", "vertex_rect"), &TerrainData::export_height_image, DEFVAL(Rect2i()));
	ClassDB::bind_method(D_METHOD("notify_region_changed", "region", "maps"), &TerrainData::notify_region_changed);

	ClassDB::bind_method(D_METHOD("_set_regions", "regions"), &TerrainData::_set_regions);
	ClassDB::bind_method(D_METHOD("_get_regions"), &TerrainData::_get_regions);
	ClassDB::bind_method(D_METHOD("_set_layers", "layers"), &TerrainData::_set_layers);
	ClassDB::bind_method(D_METHOD("_get_layers"), &TerrainData::_get_layers);
	ClassDB::bind_method(D_METHOD("_get_snapshot", "region", "maps"), &TerrainData::_get_snapshot);
	ClassDB::bind_method(D_METHOD("_apply_snapshots", "snapshots"), &TerrainData::_apply_snapshots);
	ClassDB::bind_method(D_METHOD("_get_layers_snapshot"), &TerrainData::_get_layers_snapshot);
	ClassDB::bind_method(D_METHOD("_apply_layers_snapshot", "layers", "splat_snapshots"), &TerrainData::_apply_layers_snapshot);

	// region_size has to load before the regions it describes, so it is
	// declared first.
	ADD_PROPERTY(PropertyInfo(Variant::INT, "region_size", PROPERTY_HINT_ENUM, "32:32,64:64,128:128,256:256,512:512,1024:1024,2048:2048"), "set_region_size", "get_region_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "layers", PROPERTY_HINT_ARRAY_TYPE, "TerrainLayer", PROPERTY_USAGE_STORAGE), "_set_layers", "_get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "regions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_regions", "_get_regions");

	ADD_SIGNAL(MethodInfo("region_changed", PropertyInfo(Variant::VECTOR2I, "region"), PropertyInfo(Variant::INT, "maps")));
	ADD_SIGNAL(MethodInfo("regions_changed"));
	ADD_SIGNAL(MethodInfo("layers_changed"));

	BIND_ENUM_CONSTANT(MAP_HEIGHT);
	BIND_ENUM_CONSTANT(MAP_SPLAT);
	BIND_ENUM_CONSTANT(MAP_COLOR);
	BIND_ENUM_CONSTANT(MAP_HOLES);
	BIND_ENUM_CONSTANT(MAP_ALL);

	BIND_CONSTANT(MAX_LAYERS);
}
