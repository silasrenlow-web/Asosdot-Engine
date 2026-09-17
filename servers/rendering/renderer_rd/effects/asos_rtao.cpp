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

	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));

	shadow_shader.initialize(shader_modes);
	shadow_shader_version = shadow_shader.version_create();
	shadow_pipeline = RD::get_singleton()->compute_pipeline_create(shadow_shader.version_get_shader(shadow_shader_version, 0));
}

AsosRTAO::~AsosRTAO() {
	shader.version_free(shader_version);
	shadow_shader.version_free(shadow_shader_version);
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
