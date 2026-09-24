/**************************************************************************/
/*  terrain_layer_textures.cpp                                            */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_layer_textures.h"

#include "servers/rendering/rendering_server.h"

namespace {

Ref<Image> layer_image(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> image = p_texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}
	return image;
}

// A texture array needs every slice the same size, format and mip count. When
// the layers' textures already agree they go in untouched, compression and
// all; otherwise everything is brought to uncompressed RGBA8 at the largest
// size, which costs video memory but always works.
Vector<Ref<Image>> normalize_slices(const Vector<Ref<Image>> &p_images, bool &r_converted) {
	r_converted = false;
	if (p_images.is_empty()) {
		return p_images;
	}
	const Ref<Image> &first = p_images[0];
	bool uniform = first->has_mipmaps();
	int max_w = 0;
	int max_h = 0;
	for (const Ref<Image> &image : p_images) {
		uniform = uniform && image->get_width() == first->get_width() && image->get_height() == first->get_height() && image->get_format() == first->get_format() && image->has_mipmaps() == first->has_mipmaps();
		max_w = MAX(max_w, image->get_width());
		max_h = MAX(max_h, image->get_height());
	}
	if (uniform) {
		return p_images;
	}
	r_converted = true;
	max_w = MIN((int)Math::next_power_of_2((uint32_t)max_w), 4096);
	max_h = MIN((int)Math::next_power_of_2((uint32_t)max_h), 4096);
	Vector<Ref<Image>> result;
	for (const Ref<Image> &source : p_images) {
		Ref<Image> image = source->duplicate();
		if (image->is_compressed()) {
			image->decompress();
		}
		image->clear_mipmaps();
		image->convert(Image::FORMAT_RGBA8);
		if (image->get_width() != max_w || image->get_height() != max_h) {
			image->resize(max_w, max_h, Image::INTERPOLATE_BILINEAR);
		}
		image->generate_mipmaps();
		result.push_back(image);
	}
	return result;
}

} // namespace

TerrainLayerTextures::TerrainLayerTextures() {
	for (int i = 0; i < MAX_LAYERS; i++) {
		albedo_slices[i] = -1;
		normal_slices[i] = -1;
	}
}

TerrainLayerTextures::~TerrainLayerTextures() {
	clear();
}

Vector<uint64_t> TerrainLayerTextures::_make_key(const Vector<Ref<TerrainLayer>> &p_layers) {
	Vector<uint64_t> result;
	for (const Ref<TerrainLayer> &layer : p_layers) {
		if (layer.is_null()) {
			result.push_back(0);
			continue;
		}
		result.push_back(layer->get_instance_id());
		result.push_back(layer->get_texture_version());
		result.push_back(layer->get_albedo_texture().is_valid() ? (uint64_t)layer->get_albedo_texture()->get_instance_id() : 0);
		result.push_back(layer->get_normal_texture().is_valid() ? (uint64_t)layer->get_normal_texture()->get_instance_id() : 0);
	}
	return result;
}

void TerrainLayerTextures::clear() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs) {
		if (albedo_array.is_valid()) {
			rs->free_rid(albedo_array);
		}
		if (normal_array.is_valid()) {
			rs->free_rid(normal_array);
		}
	}
	albedo_array = RID();
	normal_array = RID();
	key.clear();
	built = false;
	converted = false;
	for (int i = 0; i < MAX_LAYERS; i++) {
		albedo_slices[i] = -1;
		normal_slices[i] = -1;
	}
}

bool TerrainLayerTextures::update(const Vector<Ref<TerrainLayer>> &p_layers, bool p_force) {
	Vector<uint64_t> new_key = _make_key(p_layers);
	if (!p_force && built && new_key == key) {
		// Tints, tiling and the like are uniforms; only new pixels need the
		// arrays built again.
		return false;
	}
	clear();
	key = new_key;
	built = true;

	Vector<Ref<Image>> albedo;
	Vector<Ref<Image>> normal;
	for (int i = 0; i < MIN(p_layers.size(), MAX_LAYERS); i++) {
		const Ref<TerrainLayer> &layer = p_layers[i];
		if (layer.is_null()) {
			continue;
		}
		const Ref<Image> a = layer_image(layer->get_albedo_texture());
		if (a.is_valid()) {
			albedo_slices[i] = albedo.size();
			albedo.push_back(a);
		}
		const Ref<Image> n = layer_image(layer->get_normal_texture());
		if (n.is_valid()) {
			normal_slices[i] = normal.size();
			normal.push_back(n);
		}
	}

	bool converted_a = false;
	bool converted_n = false;
	albedo = normalize_slices(albedo, converted_a);
	normal = normalize_slices(normal, converted_n);
	converted = converted_a || converted_n;

	RenderingServer *rs = RenderingServer::get_singleton();
	if (!albedo.is_empty()) {
		albedo_array = rs->texture_2d_layered_create(albedo, RSE::TEXTURE_LAYERED_2D_ARRAY);
	}
	if (!normal.is_empty()) {
		normal_array = rs->texture_2d_layered_create(normal, RSE::TEXTURE_LAYERED_2D_ARRAY);
	}
	return true;
}

void TerrainLayerTextures::set_layer_params(RID p_material, const Vector<Ref<TerrainLayer>> &p_layers) const {
	RenderingServer *rs = RenderingServer::get_singleton();
	const int layer_count = MIN(p_layers.size(), MAX_LAYERS);
	rs->material_set_param(p_material, SNAME("layer_count"), layer_count);
	rs->material_set_param(p_material, SNAME("layer_albedo"), albedo_array);
	rs->material_set_param(p_material, SNAME("layer_normal"), normal_array);

	PackedFloat32Array tint;
	PackedFloat32Array params;
	PackedFloat32Array textures;
	tint.resize(MAX_LAYERS * 4);
	params.resize(MAX_LAYERS * 4);
	textures.resize(MAX_LAYERS * 4);
	tint.fill(1.0);
	params.fill(0.0);
	textures.fill(-1.0);
	for (int i = 0; i < layer_count; i++) {
		const Ref<TerrainLayer> &layer = p_layers[i];
		if (layer.is_null()) {
			continue;
		}
		// Linear, because the shader multiplies the sampled albedo with it.
		const Color c = layer->get_albedo_color().srgb_to_linear();
		tint.set(i * 4 + 0, c.r);
		tint.set(i * 4 + 1, c.g);
		tint.set(i * 4 + 2, c.b);
		tint.set(i * 4 + 3, layer->get_roughness());
		params.set(i * 4 + 0, 1.0 / layer->get_tile_size());
		params.set(i * 4 + 1, layer->get_normal_strength());
		params.set(i * 4 + 2, layer->is_triplanar() ? 1.0 : 0.0);
		params.set(i * 4 + 3, layer->get_detiling());
		textures.set(i * 4 + 0, albedo_slices[i]);
		textures.set(i * 4 + 1, normal_slices[i]);
	}
	rs->material_set_param(p_material, SNAME("layer_tint"), tint);
	rs->material_set_param(p_material, SNAME("layer_params"), params);
	rs->material_set_param(p_material, SNAME("layer_textures"), textures);
}

void TerrainLayerTextures::set_material_params(RID p_material, const Ref<TerrainMaterial> &p_terrain_material) {
	RenderingServer *rs = RenderingServer::get_singleton();
	Ref<TerrainMaterial> tm = p_terrain_material;
	if (tm.is_null()) {
		tm.instantiate();
	}
	rs->material_set_param(p_material, SNAME("height_blending"), tm->is_height_blending());
	rs->material_set_param(p_material, SNAME("height_blend_sharpness"), tm->get_height_blend_sharpness());
	rs->material_set_param(p_material, SNAME("auto_slope"), tm->is_auto_slope());
	rs->material_set_param(p_material, SNAME("auto_slope_layer"), tm->get_auto_slope_layer());
	const float lo = Math::deg_to_rad(MAX(tm->get_auto_slope_angle() - tm->get_auto_slope_blend() * 0.5f, 0.0f));
	const float hi = Math::deg_to_rad(MIN(tm->get_auto_slope_angle() + tm->get_auto_slope_blend() * 0.5f, 90.0f));
	rs->material_set_param(p_material, SNAME("auto_slope_range"), Vector2(1.0 - Math::cos(lo), MAX(1.0 - Math::cos(hi), 1.0 - Math::cos(lo) + 0.001)));
	rs->material_set_param(p_material, SNAME("macro_variation"), tm->get_macro_variation());
	rs->material_set_param(p_material, SNAME("macro_variation_size"), tm->get_macro_variation_size());
	rs->material_set_param(p_material, SNAME("wetness_darkening"), tm->get_wetness_darkening());
}
