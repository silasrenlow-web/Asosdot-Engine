/**************************************************************************/
/*  asos_rtao.cpp                                                         */
/**************************************************************************/
/*  Asosdot ray traced ambient occlusion and sun shadows.                 */
/**************************************************************************/

#include "asos_rtao.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

AsosRTAO::AsosRTAO() {
	Vector<String> shader_modes;
	shader_modes.push_back("");

	// The denoiser needs a second variant because the destination image format
	// qualifier differs between the single channel and colour buffers.
	Vector<String> denoise_modes;
	denoise_modes.push_back("");
	denoise_modes.push_back("#define MODE_COLOR\n");

	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));

	shadow_shader.initialize(shader_modes);
	shadow_shader_version = shadow_shader.version_create();
	shadow_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_shader.version_get_shader(shadow_shader_version, 0));

	denoise_shader.initialize(denoise_modes);
	denoise_shader_version = denoise_shader.version_create();
	denoise_pipeline = RD::get_singleton()->compute_pipeline_create(denoise_shader.version_get_shader(denoise_shader_version, 0));
	denoise_color_pipeline = RD::get_singleton()->compute_pipeline_create(denoise_shader.version_get_shader(denoise_shader_version, 1));

	// Diffuse bounce and mirror reflection share everything except how the ray
	// direction is chosen, so they are variants of one shader.
	Vector<String> gi_modes;
	gi_modes.push_back("");
	gi_modes.push_back("#define MODE_REFLECTION\n");

	gi_shader.initialize(gi_modes);
	gi_shader_version = gi_shader.version_create();
	gi_pipeline = RD::get_singleton()->compute_pipeline_create(gi_shader.version_get_shader(gi_shader_version, 0));
	reflection_pipeline = RD::get_singleton()->compute_pipeline_create(gi_shader.version_get_shader(gi_shader_version, 1));
}

AsosRTAO::~AsosRTAO() {
	shader.version_free(shader_version);
	shadow_shader.version_free(shadow_shader_version);
	denoise_shader.version_free(denoise_shader_version);
	gi_shader.version_free(gi_shader_version);
}

void AsosRTAO::_dispatch(RID p_pipeline, RID p_shader, RID p_tlas, RID p_depth_texture, RID p_dest,
		const Size2i &p_size, const void *p_push_constant, uint32_t p_push_constant_size) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	// Depth is sampled point-wise; filtering it would blend across silhouettes
	// and reconstruct positions that are on neither surface.
	RID depth_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ p_tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ depth_sampler, p_depth_texture }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_dest }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, p_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list,
			uniform_set_cache->get_cache(p_shader, 0, u_tlas, u_depth, u_dest), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, p_push_constant, p_push_constant_size);
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}

void AsosRTAO::render(RID p_tlas, RID p_depth_texture, RID p_dest_ao, const Size2i &p_size,
		const Projection &p_inv_view_proj, const Vector3 &p_cam_pos,
		float p_radius, float p_intensity, uint32_t p_ray_count, uint32_t p_frame) {
	ERR_FAIL_COND(p_tlas.is_null());
	ERR_FAIL_COND(p_depth_texture.is_null());
	ERR_FAIL_COND(p_dest_ao.is_null());
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0);

	PushConstant push_constant = {};
	MaterialStorage::store_camera(p_inv_view_proj, push_constant.inv_view_proj);
	push_constant.cam_pos[0] = p_cam_pos.x;
	push_constant.cam_pos[1] = p_cam_pos.y;
	push_constant.cam_pos[2] = p_cam_pos.z;
	push_constant.cam_pos[3] = 1.0f;
	push_constant.screen_size[0] = float(p_size.x);
	push_constant.screen_size[1] = float(p_size.y);
	push_constant.radius = p_radius;
	push_constant.intensity = p_intensity;
	push_constant.ray_count = p_ray_count;
	push_constant.frame = p_frame;

	_dispatch(pipeline, shader.version_get_shader(shader_version, 0), p_tlas, p_depth_texture,
			p_dest_ao, p_size, &push_constant, sizeof(PushConstant));
}

void AsosRTAO::render_shadow(RID p_tlas, RID p_depth_texture, RID p_dest_shadow, const Size2i &p_size,
		const Projection &p_inv_view_proj, const Vector3 &p_cam_pos,
		const Vector3 &p_sun_dir, float p_sun_angular_radius,
		uint32_t p_ray_count, uint32_t p_frame) {
	ERR_FAIL_COND(p_tlas.is_null());
	ERR_FAIL_COND(p_depth_texture.is_null());
	ERR_FAIL_COND(p_dest_shadow.is_null());
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0);

	ShadowPushConstant push_constant = {};
	MaterialStorage::store_camera(p_inv_view_proj, push_constant.inv_view_proj);
	push_constant.cam_pos[0] = p_cam_pos.x;
	push_constant.cam_pos[1] = p_cam_pos.y;
	push_constant.cam_pos[2] = p_cam_pos.z;
	push_constant.cam_pos[3] = 1.0f;
	push_constant.sun[0] = p_sun_dir.x;
	push_constant.sun[1] = p_sun_dir.y;
	push_constant.sun[2] = p_sun_dir.z;
	push_constant.sun[3] = p_sun_angular_radius;
	push_constant.screen_size[0] = float(p_size.x);
	push_constant.screen_size[1] = float(p_size.y);
	push_constant.ray_count = p_ray_count;
	push_constant.frame = p_frame;

	_dispatch(shadow_pipeline, shadow_shader.version_get_shader(shadow_shader_version, 0), p_tlas,
			p_depth_texture, p_dest_shadow, p_size, &push_constant, sizeof(ShadowPushConstant));
}

void AsosRTAO::denoise(RID p_source, RID p_depth_texture, RID p_dest, const Size2i &p_size,
		int p_radius, float p_depth_tolerance, bool p_color) {
	ERR_FAIL_COND(p_source.is_null());
	ERR_FAIL_COND(p_depth_texture.is_null());
	ERR_FAIL_COND(p_dest.is_null());
	ERR_FAIL_COND(p_source == p_dest);
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	DenoisePushConstant push_constant = {};
	push_constant.screen_size[0] = float(p_size.x);
	push_constant.screen_size[1] = float(p_size.y);
	push_constant.radius = p_radius;
	push_constant.depth_tolerance = p_depth_tolerance;

	// The visibility buffer is sampled linearly (neighbours are the point),
	// depth point-wise so edges stay crisp.
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID point_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID dn_shader = denoise_shader.version_get_shader(denoise_shader_version, p_color ? 1 : 0);

	RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, p_source }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ point_sampler, p_depth_texture }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_dest }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, p_color ? denoise_color_pipeline : denoise_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list,
			uniform_set_cache->get_cache(dn_shader, 0, u_source, u_depth, u_dest), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DenoisePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}

void AsosRTAO::render_gi(RID p_tlas, RID p_depth_texture, RID p_instance_buffer, RID p_dest_gi,
		const Size2i &p_size, const Projection &p_inv_view_proj, const Vector3 &p_cam_pos,
		const Vector3 &p_sun_dir, float p_sun_energy, const Color &p_sun_color,
		float p_gi_intensity, uint32_t p_ray_count, uint32_t p_frame, bool p_reflection) {
	ERR_FAIL_COND(p_tlas.is_null());
	ERR_FAIL_COND(p_depth_texture.is_null());
	ERR_FAIL_COND(p_instance_buffer.is_null());
	ERR_FAIL_COND(p_dest_gi.is_null());
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0);

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	GiPushConstant push_constant = {};
	MaterialStorage::store_camera(p_inv_view_proj, push_constant.inv_view_proj);
	push_constant.cam_pos[0] = p_cam_pos.x;
	push_constant.cam_pos[1] = p_cam_pos.y;
	push_constant.cam_pos[2] = p_cam_pos.z;
	push_constant.cam_pos[3] = 1.0f;
	push_constant.sun_dir[0] = p_sun_dir.x;
	push_constant.sun_dir[1] = p_sun_dir.y;
	push_constant.sun_dir[2] = p_sun_dir.z;
	push_constant.sun_dir[3] = p_sun_energy;
	push_constant.sun_color[0] = p_sun_color.r;
	push_constant.sun_color[1] = p_sun_color.g;
	push_constant.sun_color[2] = p_sun_color.b;
	push_constant.sun_color[3] = p_gi_intensity;
	push_constant.screen_size[0] = float(p_size.x);
	push_constant.screen_size[1] = float(p_size.y);
	push_constant.ray_count = p_ray_count;
	push_constant.frame = p_frame;

	RID depth_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID gi_shader_rid = gi_shader.version_get_shader(gi_shader_version, p_reflection ? 1 : 0);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ p_tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ depth_sampler, p_depth_texture }));
	RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_dest_gi }));
	RD::Uniform u_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ p_instance_buffer }));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, p_reflection ? reflection_pipeline : gi_pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list,
			uniform_set_cache->get_cache(gi_shader_rid, 0, u_tlas, u_depth, u_dest, u_instances), 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(GiPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.x, p_size.y, 1);
	RD::get_singleton()->compute_list_end();
}
