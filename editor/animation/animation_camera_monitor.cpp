/**************************************************************************/
/*  animation_camera_monitor.cpp                                          */
/**************************************************************************/
/*  Asosdot: the Animation workspace's camera monitor, beside the viewer: */
/*  any camera's picture or the live shot, with frame guides, and a       */
/*  multicam strip of every camera to cut between, live while playing.    */
/**************************************************************************/

#include "animation_camera_monitor.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/animation/animation_track_editor_plugins.h"
#include "editor/animation/animation_workspace.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/label.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/split_container.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/compositor.h"
#include "servers/rendering/rendering_server.h"

// The editor draws its gizmos, grid and tools on render layers above the 20 a scene can use.
static constexpr uint32_t SCENE_LAYERS = (1u << 20) - 1;
// How many strip cameras render in one frame, so a strip of many stays cheap while playing.
static constexpr int TILES_PER_FRAME = 2;
static constexpr float TILE_HEIGHT = 76.0f;

static const Color PROGRAM_COLOR = Color(0.92, 0.18, 0.16);
static const Color PREVIEW_COLOR = Color(0.22, 0.78, 0.32);
static const Color LABEL_BACKGROUND = Color(0, 0, 0, 0.6);

static uint32_t _hash_transform(const Transform3D &p_transform, uint32_t p_seed) {
	uint32_t hash = p_seed;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			hash = hash_murmur3_one_real(p_transform.basis[i][j], hash);
		}
		hash = hash_murmur3_one_real(p_transform.origin[i], hash);
	}
	return hash;
}

// The lens as a camera operator would read it: the focal length on a full-frame (36x24 mm) camera.
static String _get_lens_text(Camera3D *p_camera) {
	switch (p_camera->get_projection()) {
		case Camera3D::PROJECTION_ORTHOGONAL:
			return vformat(TTR("Orthogonal %s m"), String::num(p_camera->get_size(), 2));
		case Camera3D::PROJECTION_FRUSTUM:
			return TTR("Frustum");
		default:
			break;
	}
	const Ref<CameraAttributesPhysical> physical = p_camera->get_attributes();
	float focal_length = 0.0f;
	if (physical.is_valid()) {
		focal_length = physical->get_focal_length();
	} else {
		// The field of view is vertical, across the sensor's 24 mm, unless the camera keeps its width.
		const float half_sensor = p_camera->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH ? 18.0f : 12.0f;
		focal_length = half_sensor / Math::tan(Math::deg_to_rad(float(p_camera->get_fov())) * 0.5f);
	}
	return vformat(TTR("%d mm"), int(Math::round(focal_length))) + "  " + itos(int(Math::round(p_camera->get_fov()))) + String::chr(0x00B0);
}

static Rect2 _centered(const Rect2 &p_rect, float p_share) {
	const Size2 size = p_rect.size * p_share;
	return Rect2(p_rect.position + (p_rect.size - size) * 0.5, size);
}

/// AsosdotCameraView.

void AsosdotCameraView::set_camera(Camera3D *p_camera) {
	const ObjectID id = p_camera ? p_camera->get_instance_id() : ObjectID();
	if (id == camera) {
		return;
	}
	camera = id;
	synced_hash = 0;
	_sync_proxy();
	queue_redraw();
}

Camera3D *AsosdotCameraView::get_camera() const {
	Camera3D *found = ObjectDB::get_instance<Camera3D>(camera);
	return found && found->is_inside_tree() ? found : nullptr;
}

void AsosdotCameraView::set_aspect(float p_aspect) {
	if (Math::is_equal_approx(aspect, p_aspect) || p_aspect <= 0.0f) {
		return;
	}
	aspect = p_aspect;
	_update_viewport_size();
	queue_redraw();
}

Rect2 AsosdotCameraView::get_picture_rect() const {
	const Size2 size = get_size();
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return Rect2();
	}
	Size2 picture = size;
	if (size.x / size.y > aspect) {
		picture.x = size.y * aspect;
	} else {
		picture.y = size.x / aspect;
	}
	return Rect2(((size - picture) * 0.5).round(), picture.round());
}

void AsosdotCameraView::_update_viewport_size() {
	const Size2 picture = get_picture_rect().size;
	const Size2i size = Size2i(MAX(2, int(picture.x)), MAX(2, int(picture.y)));
	if (viewport->get_size() != size) {
		viewport->set_size(size);
	}
}

void AsosdotCameraView::_sync_proxy() {
	RenderingServer *rs = RenderingServer::get_singleton();
	Camera3D *source = get_camera();
	if (!source) {
		if (synced_hash != 0) {
			rs->viewport_attach_camera(viewport->get_viewport_rid(), RID());
			synced_hash = 0;
		}
		return;
	}

	// The scene the camera is in, which a camera inside a SubViewport with a world of its own sees.
	const Ref<World3D> world = source->get_world_3d();
	if (world.is_valid() && viewport->find_world_3d() != world) {
		viewport->set_world_3d(world);
	}

	// Every call to the rendering server makes the editor draw another frame, so the stand-in is only
	// touched when the camera changed.
	const Ref<Environment> environment = full_quality ? source->get_environment() : _get_light_environment(source);
	const Ref<CameraAttributes> attributes = source->get_attributes();
	const Ref<Compositor> compositor = source->get_compositor();
	const RID environment_rid = environment.is_valid() ? environment->get_rid() : RID();
	const RID attributes_rid = attributes.is_valid() ? attributes->get_rid() : RID();
	const RID compositor_rid = compositor.is_valid() ? compositor->get_rid() : RID();
	const Transform3D transform = source->get_camera_transform();
	const uint32_t flare_mask = source->is_lens_flare_enabled() ? source->get_lens_flare_cull_mask() : 0;

	uint32_t hash = hash_murmur3_one_64(uint64_t(source->get_instance_id()));
	hash = hash_murmur3_one_32(source->get_projection(), hash);
	hash = hash_murmur3_one_real(source->get_fov(), hash);
	hash = hash_murmur3_one_real(source->get_size(), hash);
	hash = hash_murmur3_one_real(source->get_near(), hash);
	hash = hash_murmur3_one_real(source->get_far(), hash);
	hash = hash_murmur3_one_real(source->get_frustum_offset().x, hash);
	hash = hash_murmur3_one_real(source->get_frustum_offset().y, hash);
	hash = hash_murmur3_one_32(source->get_keep_aspect_mode(), hash);
	hash = hash_murmur3_one_32(source->get_cull_mask(), hash);
	hash = hash_murmur3_one_32(flare_mask, hash);
	hash = hash_murmur3_one_32(source->get_lens_flare_max_count(), hash);
	hash = hash_murmur3_one_real(source->get_lens_flare_ghost_limit(), hash);
	hash = hash_murmur3_one_64(environment_rid.get_id(), hash);
	hash = hash_murmur3_one_64(attributes_rid.get_id(), hash);
	hash = hash_murmur3_one_64(compositor_rid.get_id(), hash);
	hash = _hash_transform(transform, hash);
	hash = MAX(hash, 1u);
	if (hash == synced_hash) {
		return;
	}
	synced_hash = hash;

	switch (source->get_projection()) {
		case Camera3D::PROJECTION_PERSPECTIVE:
			rs->camera_set_perspective(proxy, source->get_fov(), source->get_near(), source->get_far());
			break;
		case Camera3D::PROJECTION_ORTHOGONAL:
			rs->camera_set_orthogonal(proxy, source->get_size(), source->get_near(), source->get_far());
			break;
		case Camera3D::PROJECTION_FRUSTUM:
			rs->camera_set_frustum(proxy, source->get_size(), source->get_frustum_offset(), source->get_near(), source->get_far());
			break;
	}
	rs->camera_set_transform(proxy, transform);
	rs->camera_set_use_vertical_aspect(proxy, source->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH);
	rs->camera_set_cull_mask(proxy, source->get_cull_mask() & SCENE_LAYERS);
	rs->camera_set_lens_flare_cull_mask(proxy, flare_mask);
	rs->camera_set_lens_flare_max_count(proxy, source->get_lens_flare_max_count());
	rs->camera_set_lens_flare_ghost_limit(proxy, source->get_lens_flare_ghost_limit());
	rs->camera_set_environment(proxy, environment_rid);
	rs->camera_set_camera_attributes(proxy, attributes_rid);
	rs->camera_set_compositor(proxy, compositor_rid);
	rs->viewport_attach_camera(viewport->get_viewport_rid(), proxy);
}

Ref<Environment> AsosdotCameraView::_get_light_environment(Camera3D *p_source) {
	// The environment the camera sees: its own, or else its world's, or else the editor's preview one.
	Ref<Environment> source_environment = p_source->get_environment();
	const Ref<World3D> world = p_source->get_world_3d();
	if (source_environment.is_null() && world.is_valid()) {
		source_environment = world->get_environment().is_valid() ? world->get_environment() : world->get_fallback_environment();
	}
	if (source_environment.is_null()) {
		return Ref<Environment>();
	}
	// Changes to the environment come through the scene's undo history.
	const uint64_t version = AsosdotAnimationWorkspace::get_scene_history_version();
	if (source_environment != light_source || version != light_version || light_environment.is_null()) {
		light_source = source_environment;
		light_version = version;
		light_environment = source_environment->duplicate();
		// Each of these keeps buffers per viewport, and global illumination takes many frames to settle,
		// which a picture rendered now and then never gives it.
		light_environment->set_sdfgi_enabled(false);
		light_environment->set_sdfgi_asos_lighting(false);
		light_environment->set_ssao_enabled(false);
		light_environment->set_ssil_enabled(false);
		light_environment->set_ssr_enabled(false);
		light_environment->set_volumetric_fog_enabled(false);
	}
	return light_environment;
}

void AsosdotCameraView::set_continuous(bool p_continuous) {
	continuous = p_continuous;
	viewport->set_update_mode(continuous ? SubViewport::UPDATE_WHEN_VISIBLE : SubViewport::UPDATE_DISABLED);
	set_process_internal(continuous && is_visible_in_tree());
}

void AsosdotCameraView::refresh() {
	_sync_proxy();
	viewport->set_update_mode(SubViewport::UPDATE_ONCE);
}

void AsosdotCameraView::set_full_quality(bool p_full_quality) {
	full_quality = p_full_quality;
	apply_project_settings();
}

void AsosdotCameraView::set_half_resolution(bool p_half) {
	half_resolution = p_half;
	apply_project_settings();
}

void AsosdotCameraView::apply_project_settings() {
	if (!full_quality) {
		// Small and rendered now and then: no temporal effects to smear between refreshes, and no
		// omni and spot light shadows, which take a render pass per light (six per omni light) in
		// every viewport that draws them.
		viewport->set_positional_shadow_atlas_size(0);
		viewport->set_msaa_3d(Viewport::MSAA_DISABLED);
		viewport->set_screen_space_aa(Viewport::SCREEN_SPACE_AA_DISABLED);
		viewport->set_use_taa(false);
		viewport->set_scaling_3d_scale(half_resolution ? 0.5 : 1.0);
		return;
	}
	// As the 3D editor's viewports and the game do.
	viewport->set_positional_shadow_atlas_size(GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/atlas_size"));
	viewport->set_positional_shadow_atlas_16_bits(GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/atlas_16_bits"));
	for (int i = 0; i < 4; i++) {
		const int subdiv = GLOBAL_GET(vformat("rendering/lights_and_shadows/positional_shadow/atlas_quadrant_%d_subdiv", i));
		viewport->set_positional_shadow_atlas_quadrant_subdiv(i, Viewport::PositionalShadowAtlasQuadrantSubdiv(subdiv));
	}
	viewport->set_msaa_3d(Viewport::MSAA(int(GLOBAL_GET("rendering/anti_aliasing/quality/msaa_3d"))));
	viewport->set_screen_space_aa(Viewport::ScreenSpaceAA(int(GLOBAL_GET("rendering/anti_aliasing/quality/screen_space_aa"))));
	viewport->set_use_taa(GLOBAL_GET("rendering/anti_aliasing/quality/use_taa"));
	viewport->set_use_debanding(GLOBAL_GET("rendering/anti_aliasing/quality/use_debanding"));
	viewport->set_transparent_background(GLOBAL_GET("rendering/viewport/transparent_background"));
	viewport->set_use_occlusion_culling(GLOBAL_GET("rendering/occlusion_culling/use_occlusion_culling"));
	viewport->set_mesh_lod_threshold(GLOBAL_GET("rendering/mesh_lod/lod_change/threshold_pixels"));
	viewport->set_scaling_3d_mode(Viewport::Scaling3DMode(int(GLOBAL_GET("rendering/scaling_3d/mode"))));
	const float scaling_3d_scale = GLOBAL_GET("rendering/scaling_3d/scale");
	viewport->set_scaling_3d_scale(MAX(0.25f, scaling_3d_scale * (half_resolution ? 0.5f : 1.0f)));
	viewport->set_fsr_sharpness(GLOBAL_GET("rendering/scaling_3d/fsr_sharpness"));
	viewport->set_texture_mipmap_bias(GLOBAL_GET("rendering/textures/default_filters/texture_mipmap_bias"));
	viewport->set_anisotropic_filtering_level(Viewport::AnisotropicFiltering(int(GLOBAL_GET("rendering/textures/default_filters/anisotropic_filtering_level"))));
}

void AsosdotCameraView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_RESIZED: {
			_update_viewport_size();
		} break;

		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_VISIBILITY_CHANGED: {
			set_process_internal(continuous && is_visible_in_tree());
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_sync_proxy();
		} break;

		case NOTIFICATION_DRAW: {
			draw_rect(Rect2(Point2(), get_size()), Color(0, 0, 0));
			if (get_camera()) {
				draw_texture_rect(viewport->get_texture(), get_picture_rect(), false);
			}
		} break;
	}
}

void AsosdotCameraView::_bind_methods() {
	// For the editor's own checks; the class is internal.
	ClassDB::bind_method(D_METHOD("get_camera"), &AsosdotCameraView::get_camera);
}

AsosdotCameraView::AsosdotCameraView() {
	proxy = RenderingServer::get_singleton()->camera_create();
	viewport = memnew(SubViewport);
	viewport->set_disable_input(true);
	viewport->set_update_mode(SubViewport::UPDATE_WHEN_VISIBLE);
	add_child(viewport, false, INTERNAL_MODE_FRONT);
	if (EditorNode::get_singleton()) {
		EditorNode::get_singleton()->register_hdr_viewport(viewport);
	}
	set_clip_contents(true);
	apply_project_settings();
}

AsosdotCameraView::~AsosdotCameraView() {
	// The viewport, a child, is already gone, and with it the rendering server's use of the stand-in.
	RenderingServer::get_singleton()->free_rid(proxy);
}

/// AsosdotCameraMonitor.

Camera3D *AsosdotCameraMonitor::get_live_camera() {
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	if (!scene) {
		return nullptr;
	}
	Camera3D *camera = EditorNode::get_singleton()->get_scene_root()->get_camera_3d();
	return camera && (camera == scene || scene->is_ancestor_of(camera)) ? camera : nullptr;
}

Camera3D *AsosdotCameraMonitor::get_shown_camera() const {
	Camera3D *camera = ObjectDB::get_instance<Camera3D>(picked);
	return camera && camera->is_inside_tree() ? camera : get_live_camera();
}

Camera3D *AsosdotCameraMonitor::_get_camera(int p_index) const {
	if (p_index < 0 || p_index >= int(cameras.size())) {
		return nullptr;
	}
	Camera3D *camera = ObjectDB::get_instance<Camera3D>(cameras[p_index]);
	return camera && camera->is_inside_tree() ? camera : nullptr;
}

String AsosdotCameraMonitor::_get_camera_label(const Camera3D *p_camera) const {
	if (!p_camera) {
		return String();
	}
	const int64_t index = cameras.find(p_camera->get_instance_id());
	return index >= 0 && index < int64_t(camera_labels.size()) ? camera_labels[index] : String(p_camera->get_name());
}

void AsosdotCameraMonitor::show_camera(Camera3D *p_camera) {
	picked = p_camera ? p_camera->get_instance_id() : ObjectID();
	picked_path = String();
	int index = 0;
	for (uint32_t i = 0; i < cameras.size(); i++) {
		if (cameras[i] == picked) {
			index = i + 1;
		}
	}
	source->select(index);
	monitor->set_camera(get_shown_camera());
}

void AsosdotCameraMonitor::cut_to(int p_index) {
	Camera3D *camera = _get_camera(p_index);
	if (camera) {
		workspace->cut_to_camera_at_playhead(camera);
	}
}

void AsosdotCameraMonitor::_update_cameras(bool p_force) {
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	uint32_t hash = hash_murmur3_one_64(scene ? uint64_t(scene->get_instance_id()) : 0);
	LocalVector<Camera3D *> found;
	for (const ObjectID &id : workspace->scene_cameras) {
		Camera3D *camera = ObjectDB::get_instance<Camera3D>(id);
		if (camera && camera->is_inside_tree()) {
			found.push_back(camera);
			hash = hash_murmur3_one_64(uint64_t(id), hash);
			hash = hash_murmur3_one_32(String(camera->get_name()).hash(), hash);
			hash = hash_murmur3_one_32(camera->get_parent() ? String(camera->get_parent()->get_name()).hash() : 0, hash);
		}
	}
	if (!p_force && hash == listed_cameras_hash) {
		return;
	}
	listed_cameras_hash = hash;

	cameras.clear();
	camera_labels.clear();
	for (Camera3D *camera : found) {
		cameras.push_back(camera->get_instance_id());
		// Cameras that share a name, like several "Camera3D"s, are told apart by their parents.
		String label = camera->get_name();
		for (Camera3D *other : found) {
			if (other != camera && other->get_name() == camera->get_name() && camera->get_parent()) {
				label = String(camera->get_parent()->get_name()) + "/" + label;
				break;
			}
		}
		camera_labels.push_back(label);
	}
	// A camera the layout remembers is picked once its scene is open.
	if (!picked_path.is_empty() && scene) {
		Camera3D *remembered = Object::cast_to<Camera3D>(scene->get_node_or_null(NodePath(picked_path)));
		if (remembered && found.has(remembered)) {
			picked = remembered->get_instance_id();
			picked_path = String();
		}
	}
	if (picked.is_valid() && !cameras.has(picked)) {
		picked = ObjectID();
	}

	source->clear();
	source->add_icon_item(get_editor_theme_icon(SNAME("Camera3D")), TTR("Live Shot"));
	source->set_item_tooltip(0, TTR("The camera that's live at the playhead, switching as the cuts do."));
	for (uint32_t i = 0; i < found.size(); i++) {
		source->add_item(camera_labels[i]);
		source->set_item_tooltip(i + 1, String(scene->get_path_to(found[i])));
		if (found[i]->get_instance_id() == picked) {
			source->select(i + 1);
		}
	}
	if (!picked.is_valid()) {
		source->select(0);
	}

	for (AsosdotCameraView *tile : tiles) {
		strip->remove_child(tile);
		tile->queue_free();
	}
	tiles.clear();
	hovered_tile = -1;
	const float ratio = _get_aspect_ratio();
	for (uint32_t i = 0; i < found.size(); i++) {
		AsosdotCameraView *tile = memnew(AsosdotCameraView);
		tile->set_continuous(false);
		tile->set_aspect(ratio);
		tile->set_custom_minimum_size(Size2(Math::round(tile_height * ratio), tile_height));
		tile->set_default_cursor_shape(CURSOR_POINTING_HAND);
		if (int(i) < SHORTCUT_CAMERAS) {
			tile->set_tooltip_text(vformat(TTR("%s\nClick to cut to it at the playhead (key %d), live while playing.\nRight-click to show it in the monitor or select it."), camera_labels[i], i + 1));
		} else {
			tile->set_tooltip_text(vformat(TTR("%s\nClick to cut to it at the playhead, live while playing.\nRight-click to show it in the monitor or select it."), camera_labels[i]));
		}
		tile->connect(SceneStringName(draw), callable_mp(this, &AsosdotCameraMonitor::_draw_tile).bind(i));
		tile->connect(SceneStringName(gui_input), callable_mp(this, &AsosdotCameraMonitor::_tile_gui_input).bind(i));
		tile->connect(SceneStringName(mouse_entered), callable_mp(this, &AsosdotCameraMonitor::_tile_hovered).bind(i));
		tile->connect(SceneStringName(mouse_exited), callable_mp(this, &AsosdotCameraMonitor::_tile_hovered).bind(-1));
		strip->add_child(tile);
		tile->set_camera(found[i]);
		tiles.push_back(tile);
	}
	strip_hint->set_visible(found.is_empty());
	tiles_to_refresh = tiles.size();
	next_tile = 0;
	overlay_hash = 0;
}

float AsosdotCameraMonitor::_get_aspect_ratio() const {
	switch (aspect) {
		case ASPECT_16_9:
			return 16.0f / 9.0f;
		case ASPECT_4_3:
			return 4.0f / 3.0f;
		case ASPECT_21_9:
			return 21.0f / 9.0f;
		case ASPECT_9_16:
			return 9.0f / 16.0f;
		case ASPECT_1_1:
			return 1.0f;
		case ASPECT_GAME:
			break;
	}
	// The game window's shape, as Godot's own camera preview takes it.
	const float width = GLOBAL_GET("display/window/size/viewport_width");
	const float height = GLOBAL_GET("display/window/size/viewport_height");
	return width > 0.0f && height > 0.0f ? width / height : 16.0f / 9.0f;
}

String AsosdotCameraMonitor::_get_aspect_name() const {
	const float ratio = _get_aspect_ratio();
	struct Named {
		float ratio;
		const char *name;
	};
	static const Named names[] = { { 16.0f / 9.0f, "16:9" }, { 4.0f / 3.0f, "4:3" }, { 21.0f / 9.0f, "21:9" }, { 9.0f / 16.0f, "9:16" }, { 1.0f, "1:1" }, { 16.0f / 10.0f, "16:10" } };
	for (const Named &named : names) {
		if (Math::abs(ratio - named.ratio) < 0.005f) {
			return named.name;
		}
	}
	return String::num(ratio, 2) + ":1";
}

void AsosdotCameraMonitor::_apply_aspect() {
	const float ratio = _get_aspect_ratio();
	monitor->set_aspect(ratio);
	for (AsosdotCameraView *tile : tiles) {
		tile->set_aspect(ratio);
		tile->set_custom_minimum_size(Size2(Math::round(tile_height * ratio), tile_height));
	}
	strip_scroll->set_custom_minimum_size(Size2(0, tile_height + 14 * EDSCALE));
	tiles_to_refresh = tiles.size();
	overlay_hash = 0;
}

void AsosdotCameraMonitor::_update_view_menu() {
	PopupMenu *popup = view_menu->get_popup();
	const int width = GLOBAL_GET("display/window/size/viewport_width");
	const int height = GLOBAL_GET("display/window/size/viewport_height");
	popup->set_item_text(popup->get_item_index(VIEW_ASPECT_GAME), vformat(TTR("Game (%s)"), itos(width) + String::chr(0x00D7) + itos(height)));
	popup->set_item_checked(popup->get_item_index(VIEW_THIRDS), guides & GUIDE_THIRDS);
	popup->set_item_checked(popup->get_item_index(VIEW_CENTER), guides & GUIDE_CENTER);
	popup->set_item_checked(popup->get_item_index(VIEW_SAFE_AREAS), guides & GUIDE_SAFE_AREAS);
	popup->set_item_checked(popup->get_item_index(VIEW_BURN_IN), guides & GUIDE_BURN_IN);
	popup->set_item_checked(popup->get_item_index(VIEW_LETTERBOX_NONE), letterbox == LETTERBOX_NONE);
	popup->set_item_checked(popup->get_item_index(VIEW_LETTERBOX_1_85), letterbox == LETTERBOX_1_85);
	popup->set_item_checked(popup->get_item_index(VIEW_LETTERBOX_2_39), letterbox == LETTERBOX_2_39);
	const ViewMenu aspects[] = { VIEW_ASPECT_GAME, VIEW_ASPECT_16_9, VIEW_ASPECT_4_3, VIEW_ASPECT_21_9, VIEW_ASPECT_9_16, VIEW_ASPECT_1_1 };
	for (int i = 0; i < 6; i++) {
		popup->set_item_checked(popup->get_item_index(aspects[i]), aspect == Aspect(i));
	}
	popup->set_item_checked(popup->get_item_index(VIEW_HALF_RESOLUTION), half_resolution);
}

void AsosdotCameraMonitor::_source_selected(int p_index) {
	if (p_index <= 0) {
		show_camera(nullptr);
	} else {
		show_camera(_get_camera(p_index - 1));
	}
}

void AsosdotCameraMonitor::_view_menu_option(int p_option) {
	switch (p_option) {
		case VIEW_THIRDS:
			guides ^= GUIDE_THIRDS;
			break;
		case VIEW_CENTER:
			guides ^= GUIDE_CENTER;
			break;
		case VIEW_SAFE_AREAS:
			guides ^= GUIDE_SAFE_AREAS;
			break;
		case VIEW_BURN_IN:
			guides ^= GUIDE_BURN_IN;
			break;
		case VIEW_LETTERBOX_NONE:
		case VIEW_LETTERBOX_1_85:
		case VIEW_LETTERBOX_2_39:
			letterbox = Letterbox(p_option - VIEW_LETTERBOX_NONE);
			break;
		case VIEW_ASPECT_GAME:
		case VIEW_ASPECT_16_9:
		case VIEW_ASPECT_4_3:
		case VIEW_ASPECT_21_9:
		case VIEW_ASPECT_9_16:
		case VIEW_ASPECT_1_1:
			aspect = Aspect(p_option - VIEW_ASPECT_GAME);
			_apply_aspect();
			break;
		case VIEW_HALF_RESOLUTION:
			half_resolution = !half_resolution;
			monitor->set_half_resolution(half_resolution);
			break;
	}
	_update_view_menu();
	overlay_hash = 0;
}

void AsosdotCameraMonitor::_multicam_toggled(bool p_pressed) {
	strip_scroll->set_visible(p_pressed);
	tiles_to_refresh = tiles.size();
}

void AsosdotCameraMonitor::_select_camera(Camera3D *p_camera) {
	if (!p_camera) {
		return;
	}
	EditorSelection *selection = EditorNode::get_singleton()->get_editor_selection();
	selection->clear();
	selection->add_node(p_camera);
	EditorNode::get_singleton()->edit_node(p_camera);
}

void AsosdotCameraMonitor::_monitor_gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT && mb->is_double_click()) {
		// Double-clicking the picture selects its camera, for its lens and settings in the Inspector.
		_select_camera(monitor->get_camera());
		monitor->accept_event();
	}
}

void AsosdotCameraMonitor::_tile_gui_input(const Ref<InputEvent> &p_event, int p_index) {
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed() || p_index >= int(tiles.size())) {
		return;
	}
	if (mb->get_button_index() == MouseButton::LEFT) {
		// A double-click's second press would cut again at the same moment.
		if (!mb->is_double_click()) {
			cut_to(p_index);
		}
		tiles[p_index]->accept_event();
	} else if (mb->get_button_index() == MouseButton::RIGHT) {
		menu_tile = p_index;
		tile_menu->set_item_text(tile_menu->get_item_index(TILE_CUT), p_index < SHORTCUT_CAMERAS ? vformat(TTR("Cut Here (%d)"), p_index + 1) : TTR("Cut Here"));
		tile_menu->set_position(tiles[p_index]->get_screen_position() + mb->get_position());
		tile_menu->reset_size();
		tile_menu->popup();
		tiles[p_index]->accept_event();
	}
}

void AsosdotCameraMonitor::_tile_hovered(int p_index) {
	hovered_tile = p_index;
	for (AsosdotCameraView *tile : tiles) {
		tile->queue_redraw();
	}
}

void AsosdotCameraMonitor::_tile_menu_option(int p_option) {
	Camera3D *camera = _get_camera(menu_tile);
	if (!camera) {
		return;
	}
	switch (p_option) {
		case TILE_CUT:
			cut_to(menu_tile);
			break;
		case TILE_SHOW:
			show_camera(camera);
			break;
		case TILE_SELECT:
			_select_camera(camera);
			break;
	}
}

void AsosdotCameraMonitor::_refresh_strip() {
	if (!strip_scroll->is_visible() || tiles.is_empty()) {
		return;
	}
	// What the cameras see changes when the playhead moves, the scene is edited or a camera moves;
	// otherwise the strip keeps the pictures it has and the editor can stay idle.
	uint32_t state = hash_murmur3_one_double(workspace->_get_playhead());
	AnimationPlayer *player = workspace->_get_player();
	state = hash_murmur3_one_32(player && player->is_playing(), state);
	state = hash_murmur3_one_64(AsosdotAnimationWorkspace::get_scene_history_version(), state);
	for (const ObjectID &id : cameras) {
		Camera3D *camera = ObjectDB::get_instance<Camera3D>(id);
		if (camera && camera->is_inside_tree()) {
			state = _hash_transform(camera->get_global_transform(), state);
			state = hash_murmur3_one_real(camera->get_fov(), state);
		}
	}
	if (state != strip_state_hash) {
		strip_state_hash = state;
		tiles_to_refresh = tiles.size();
	}
	for (int i = 0; i < TILES_PER_FRAME && tiles_to_refresh > 0; i++) {
		next_tile %= tiles.size();
		tiles[next_tile]->refresh();
		next_tile++;
		tiles_to_refresh--;
	}
}

Size2 AsosdotCameraMonitor::_get_label_size(const String &p_text) const {
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	return font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size) + Size2(5, 2) * EDSCALE * 2;
}

float AsosdotCameraMonitor::_draw_label(Control *p_canvas, const Point2 &p_corner, HorizontalAlignment p_align, bool p_top, const String &p_text, const Color &p_background) const {
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
	const Size2 padding = Size2(5, 2) * EDSCALE;
	const Size2 box = _get_label_size(p_text);
	Point2 position = p_corner;
	if (p_align == HORIZONTAL_ALIGNMENT_RIGHT) {
		position.x -= box.x;
	}
	if (!p_top) {
		position.y -= box.y;
	}
	p_canvas->draw_rect(Rect2(position, box), p_background);
	p_canvas->draw_string(font, position + Point2(padding.x, padding.y + font->get_ascent(font_size)), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(1, 1, 1));
	return box.x;
}

void AsosdotCameraMonitor::_draw_monitor() {
	Camera3D *shown = monitor->get_camera();
	if (!shown) {
		const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
		const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
		const String hint = cameras.is_empty() ? TTR("No Camera3D in this scene.\nAdd one to see its shot here.") : TTR("No camera is live.");
		monitor->draw_multiline_string(font, Point2(0, monitor->get_size().y * 0.5 - font_size), hint, HORIZONTAL_ALIGNMENT_CENTER, monitor->get_size().x, font_size, -1, Color(1, 1, 1, 0.6));
		return;
	}
	const Rect2 picture = monitor->get_picture_rect();

	// Letterbox bars, with the frame they leave for the guides.
	Rect2 frame = picture;
	if (letterbox != LETTERBOX_NONE) {
		const float ratio = letterbox == LETTERBOX_1_85 ? 1.85f : 2.39f;
		const Color bar = Color(0, 0, 0, 0.82);
		if (ratio > picture.size.aspect()) {
			frame.size.y = Math::round(picture.size.x / ratio);
			frame.position.y += Math::round((picture.size.y - frame.size.y) * 0.5f);
			monitor->draw_rect(Rect2(picture.position, Size2(picture.size.x, frame.position.y - picture.position.y)), bar);
			monitor->draw_rect(Rect2(Point2(picture.position.x, frame.get_end().y), Size2(picture.size.x, picture.get_end().y - frame.get_end().y)), bar);
		} else {
			frame.size.x = Math::round(picture.size.y * ratio);
			frame.position.x += Math::round((picture.size.x - frame.size.x) * 0.5f);
			monitor->draw_rect(Rect2(picture.position, Size2(frame.position.x - picture.position.x, picture.size.y)), bar);
			monitor->draw_rect(Rect2(Point2(frame.get_end().x, picture.position.y), Size2(picture.get_end().x - frame.get_end().x, picture.size.y)), bar);
		}
	}

	const Color guide = Color(1, 1, 1, 0.35);
	if (guides & GUIDE_THIRDS) {
		for (int i = 1; i < 3; i++) {
			const float x = Math::round(frame.position.x + frame.size.x * i / 3.0f) + 0.5f;
			const float y = Math::round(frame.position.y + frame.size.y * i / 3.0f) + 0.5f;
			monitor->draw_line(Point2(x, frame.position.y), Point2(x, frame.get_end().y), guide);
			monitor->draw_line(Point2(frame.position.x, y), Point2(frame.get_end().x, y), guide);
		}
	}
	if (guides & GUIDE_CENTER) {
		const Point2 center = frame.get_center().round() + Vector2(0.5, 0.5);
		const float arm = 10 * EDSCALE;
		monitor->draw_line(center - Vector2(arm, 0), center + Vector2(arm, 0), Color(1, 1, 1, 0.7));
		monitor->draw_line(center - Vector2(0, arm), center + Vector2(0, arm), Color(1, 1, 1, 0.7));
	}
	if (guides & GUIDE_SAFE_AREAS) {
		// Action safe and title safe, at 93% and 90% of the frame as in SMPTE ST 2046-1.
		monitor->draw_rect(_centered(frame, 0.93f), Color(1, 1, 1, 0.3), false);
		monitor->draw_rect(_centered(frame, 0.90f), Color(1, 0.85, 0.3, 0.4), false);
	}

	const Camera3D *live = get_live_camera();
	const bool on_air = shown == live;
	const float margin = 6 * EDSCALE;
	const float gap = 3 * EDSCALE;
	const Rect2 inner = frame.grow(-margin);
	const float row = _get_label_size("0").y;
	if ((guides & GUIDE_BURN_IN) && inner.size.y >= row) {
		// In a small picture, what doesn't fit is left out, the least needed first, rather than drawn
		// over the rest.
		const String tag = on_air ? TTR("LIVE") : TTR("PREVIEW");
		const String name = _get_camera_label(shown);
		const String lens = _get_lens_text(shown);
		float top_left = _draw_label(monitor, inner.position, HORIZONTAL_ALIGNMENT_LEFT, true, tag, on_air ? PROGRAM_COLOR : PREVIEW_COLOR.darkened(0.2));
		if (top_left + gap + _get_label_size(name).x <= inner.size.x) {
			top_left += gap + _draw_label(monitor, inner.position + Vector2(top_left + gap, 0), HORIZONTAL_ALIGNMENT_LEFT, true, name, LABEL_BACKGROUND);
		}
		if (top_left + gap * 4 + _get_label_size(lens).x <= inner.size.x) {
			_draw_label(monitor, Point2(inner.get_end().x, inner.position.y), HORIZONTAL_ALIGNMENT_RIGHT, true, lens, LABEL_BACKGROUND);
		}

		if (inner.size.y >= row * 3) {
			const Ref<Animation> animation = workspace->_get_animation();
			String time_text;
			String frame_text = _get_aspect_name();
			if (animation.is_valid()) {
				const double time = workspace->_get_playhead();
				time_text = AsosdotAnimationWorkspace::format_time(time);
				if (animation->get_step() > 0.0) {
					frame_text = vformat(TTR("Frame %d"), int(Math::round(time / animation->get_step()))) + "  " + frame_text;
				}
			}
			float bottom_left = 0.0f;
			if (!time_text.is_empty()) {
				bottom_left = _draw_label(monitor, Point2(inner.position.x, inner.get_end().y), HORIZONTAL_ALIGNMENT_LEFT, false, time_text, LABEL_BACKGROUND);
			}
			if (bottom_left + gap * 4 + _get_label_size(frame_text).x <= inner.size.x) {
				_draw_label(monitor, inner.get_end(), HORIZONTAL_ALIGNMENT_RIGHT, false, frame_text, LABEL_BACKGROUND);
			}
		}
	}

	// Tally: red while the monitor shows the live shot, green while it previews another camera.
	monitor->draw_rect(picture.grow(-1 * EDSCALE), on_air ? PROGRAM_COLOR : PREVIEW_COLOR, false, 2 * EDSCALE);
}

void AsosdotCameraMonitor::_draw_tile(int p_index) {
	if (p_index >= int(tiles.size())) {
		return;
	}
	AsosdotCameraView *tile = tiles[p_index];
	Camera3D *camera = tile->get_camera();
	if (!camera) {
		return;
	}
	const Rect2 picture = tile->get_picture_rect();
	const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
	const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));

	// The camera's name on a band in its shot colour, the colour its shots have on the timeline.
	const float band = font->get_height(font_size) + 4 * EDSCALE;
	const Rect2 band_rect = Rect2(picture.position.x, picture.get_end().y - band, picture.size.x, band);
	tile->draw_rect(band_rect, Color(0, 0, 0, 0.6));
	tile->draw_rect(Rect2(band_rect.position.x, band_rect.get_end().y - 3 * EDSCALE, band_rect.size.x, 3 * EDSCALE), AsosdotAnimationTrackEditShot::get_shot_color(camera));
	tile->draw_string(font, band_rect.position + Point2(5 * EDSCALE, 1 * EDSCALE + font->get_ascent(font_size)), _get_camera_label(camera), HORIZONTAL_ALIGNMENT_LEFT, band_rect.size.x - 10 * EDSCALE, font_size, Color(1, 1, 1));
	if (p_index < SHORTCUT_CAMERAS) {
		_draw_label(tile, picture.position + Vector2(3, 3) * EDSCALE, HORIZONTAL_ALIGNMENT_LEFT, true, itos(p_index + 1), LABEL_BACKGROUND);
	}

	// Tally lights, as on a vision mixer: red on air, green in the monitor.
	if (camera == get_live_camera()) {
		tile->draw_rect(picture.grow(-1.5 * EDSCALE), PROGRAM_COLOR, false, 3 * EDSCALE);
	} else if (picked.is_valid() && camera == monitor->get_camera()) {
		tile->draw_rect(picture.grow(-1.5 * EDSCALE), PREVIEW_COLOR, false, 3 * EDSCALE);
	} else if (p_index == hovered_tile) {
		tile->draw_rect(picture.grow(-0.5 * EDSCALE), Color(1, 1, 1, 0.6), false, 1 * EDSCALE);
	}
}

void AsosdotCameraMonitor::shortcut_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (!active || key.is_null() || !key->is_pressed() || key->is_echo()) {
		return;
	}
	for (int i = 0; i < SHORTCUT_CAMERAS; i++) {
		if (ED_IS_SHORTCUT(vformat("animation_workspace/cut_to_camera_%d", i + 1), p_event)) {
			cut_to(i);
			get_viewport()->set_input_as_handled();
			return;
		}
	}
}

// The monitor sits in a workspace panel, which sits in the 3D editor's split beside the viewer.
// While the panel floats in a window of its own there is no split to share.
SplitContainer *AsosdotCameraMonitor::_get_split() const {
	for (Node *node = get_parent(); node; node = node->get_parent()) {
		if (Object::cast_to<Window>(node)) {
			return nullptr;
		}
		if (SplitContainer *split = Object::cast_to<SplitContainer>(node)) {
			return split;
		}
	}
	return nullptr;
}

void AsosdotCameraMonitor::add_header_control(Control *p_control) {
	header->add_child(p_control);
}

void AsosdotCameraMonitor::set_active(bool p_active) {
	ERR_FAIL_NULL(workspace);
	if (active == p_active) {
		return;
	}
	SplitContainer *split = _get_split();
	if (!p_active && split) {
		saved_split = split->get_split_offset();
		saved_split_valid = true;
	}
	active = p_active;
	set_visible(active);
	set_process(active);
	set_process_shortcut_input(active);
	if (active) {
		_update_cameras(true);
		_update_view_menu();
		_apply_aspect();
		if (split && saved_split_valid) {
			split->set_split_offset(saved_split);
		}
	}
}

Dictionary AsosdotCameraMonitor::get_layout() const {
	Dictionary layout;
	Camera3D *camera = ObjectDB::get_instance<Camera3D>(picked);
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	layout["source"] = camera && scene && camera->is_inside_tree() ? String(scene->get_path_to(camera)) : picked_path;
	layout["guides"] = guides;
	layout["letterbox"] = letterbox;
	layout["aspect"] = aspect;
	layout["half_resolution"] = half_resolution;
	layout["multicam"] = multicam_button->is_pressed();
	const SplitContainer *split = _get_split();
	if (split && active) {
		layout["split"] = split->get_split_offset();
	} else if (saved_split_valid) {
		layout["split"] = saved_split;
	}
	return layout;
}

void AsosdotCameraMonitor::set_layout(const Dictionary &p_layout) {
	picked = ObjectID();
	picked_path = p_layout.get("source", String());
	guides = int(p_layout.get("guides", GUIDE_THIRDS | GUIDE_BURN_IN)) & (GUIDE_THIRDS | GUIDE_CENTER | GUIDE_SAFE_AREAS | GUIDE_BURN_IN);
	letterbox = Letterbox(CLAMP(int(p_layout.get("letterbox", LETTERBOX_NONE)), int(LETTERBOX_NONE), int(LETTERBOX_2_39)));
	aspect = Aspect(CLAMP(int(p_layout.get("aspect", ASPECT_GAME)), int(ASPECT_GAME), int(ASPECT_1_1)));
	half_resolution = p_layout.get("half_resolution", false);
	monitor->set_half_resolution(half_resolution);
	multicam_button->set_pressed(p_layout.get("multicam", true));
	if (p_layout.has("split")) {
		saved_split = p_layout["split"];
		saved_split_valid = true;
		SplitContainer *split = _get_split();
		if (split && active) {
			split->set_split_offset(saved_split);
		}
	}
	_update_view_menu();
	_apply_aspect();
	listed_cameras_hash = 0;
}

void AsosdotCameraMonitor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &AsosdotCameraMonitor::_project_settings_changed));
		} break;

		case NOTIFICATION_EXIT_TREE: {
			ProjectSettings::get_singleton()->disconnect("settings_changed", callable_mp(this, &AsosdotCameraMonitor::_project_settings_changed));
		} break;

		case NOTIFICATION_RESIZED: {
			// In a short panel the strip shrinks, and the picture keeps most of the height.
			const float height = CLAMP(Math::round(get_size().y * 0.25f), 44.0f * EDSCALE, TILE_HEIGHT * EDSCALE);
			if (!Math::is_equal_approx(height, tile_height)) {
				tile_height = height;
				_apply_aspect();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			view_menu->set_button_icon(get_editor_theme_icon(SNAME("GuiVisibilityVisible")));
			multicam_button->set_button_icon(get_editor_theme_icon(SNAME("GridLayout")));
			listed_cameras_hash = 0;
		} break;

		case NOTIFICATION_PROCESS: {
			_update_cameras();
			Camera3D *shown = get_shown_camera();
			monitor->set_camera(shown);

			// The burn-in and tally lights follow the playhead and the cuts; redrawn only when they change.
			const Camera3D *live = get_live_camera();
			uint32_t overlay = hash_murmur3_one_64(shown ? uint64_t(shown->get_instance_id()) : 0);
			overlay = hash_murmur3_one_64(live ? uint64_t(live->get_instance_id()) : 0, overlay);
			overlay = hash_murmur3_one_32(int(picked.is_valid()) | (guides << 1) | (letterbox << 8) | (aspect << 12), overlay);
			if (shown && (guides & GUIDE_BURN_IN)) {
				const Ref<Animation> animation = workspace->_get_animation();
				overlay = hash_murmur3_one_double(animation.is_valid() ? workspace->_get_playhead() : -1.0, overlay);
				overlay = hash_murmur3_one_32(_get_lens_text(shown).hash(), overlay);
			}
			overlay = MAX(overlay, 1u);
			if (overlay != overlay_hash) {
				overlay_hash = overlay;
				monitor->queue_redraw();
				for (AsosdotCameraView *tile : tiles) {
					tile->queue_redraw();
				}
			}
			_refresh_strip();
		} break;
	}
}

void AsosdotCameraMonitor::_project_settings_changed() {
	monitor->apply_project_settings();
	_update_view_menu();
	_apply_aspect();
}

void AsosdotCameraMonitor::_bind_methods() {
	// For the editor's own checks; the class is internal.
	ClassDB::bind_static_method("AsosdotCameraMonitor", D_METHOD("get_live_camera"), &AsosdotCameraMonitor::get_live_camera);
	ClassDB::bind_method(D_METHOD("get_shown_camera"), &AsosdotCameraMonitor::get_shown_camera);
	ClassDB::bind_method(D_METHOD("show_camera", "camera"), &AsosdotCameraMonitor::show_camera);
	ClassDB::bind_method(D_METHOD("cut_to", "index"), &AsosdotCameraMonitor::cut_to);
	ClassDB::bind_method(D_METHOD("get_camera_count"), &AsosdotCameraMonitor::get_camera_count);
}

AsosdotCameraMonitor::AsosdotCameraMonitor(AsosdotAnimationWorkspace *p_workspace) {
	workspace = p_workspace;
	tile_height = TILE_HEIGHT * EDSCALE;
	set_name(TTRC("Camera Monitor"));
	set_custom_minimum_size(Size2(220 * EDSCALE, 0));
	set_h_size_flags(SIZE_EXPAND_FILL);

	for (int i = 1; i <= SHORTCUT_CAMERAS; i++) {
		ED_SHORTCUT(vformat("animation_workspace/cut_to_camera_%d", i), vformat(TTR("Cut to Camera %d"), i), Key(int(Key::KEY_0) + i));
	}

	header = memnew(HBoxContainer);
	add_child(header);
	source = memnew(OptionButton);
	source->set_h_size_flags(SIZE_EXPAND_FILL);
	source->set_clip_text(true);
	source->set_tooltip_text(TTR("What the monitor shows: the live shot, which follows the cuts, or any camera."));
	source->connect(SceneStringName(item_selected), callable_mp(this, &AsosdotCameraMonitor::_source_selected));
	header->add_child(source);

	view_menu = memnew(MenuButton);
	view_menu->set_tooltip_text(TTR("Frame guides, letterbox, aspect ratio and resolution."));
	PopupMenu *popup = view_menu->get_popup();
	popup->add_check_item(TTR("Rule of Thirds"), VIEW_THIRDS);
	popup->add_check_item(TTR("Center Mark"), VIEW_CENTER);
	popup->add_check_item(TTR("Safe Areas"), VIEW_SAFE_AREAS);
	popup->set_item_tooltip(popup->get_item_count() - 1, TTR("Action safe (93%) and title safe (90%) frames."));
	popup->add_check_item(TTR("Burn-in"), VIEW_BURN_IN);
	popup->set_item_tooltip(popup->get_item_count() - 1, TTR("The camera, its lens, the timecode and the frame, over the picture."));
	popup->add_separator(TTR("Letterbox"));
	popup->add_radio_check_item(TTR("None"), VIEW_LETTERBOX_NONE);
	popup->add_radio_check_item(TTR("1.85:1 (Flat)"), VIEW_LETTERBOX_1_85);
	popup->add_radio_check_item(TTR("2.39:1 (Scope)"), VIEW_LETTERBOX_2_39);
	popup->add_separator(TTR("Aspect Ratio"));
	popup->add_radio_check_item(TTR("Game"), VIEW_ASPECT_GAME);
	popup->add_radio_check_item("16:9", VIEW_ASPECT_16_9);
	popup->add_radio_check_item("4:3", VIEW_ASPECT_4_3);
	popup->add_radio_check_item("21:9", VIEW_ASPECT_21_9);
	popup->add_radio_check_item(TTR("9:16 (Portrait)"), VIEW_ASPECT_9_16);
	popup->add_radio_check_item("1:1", VIEW_ASPECT_1_1);
	popup->add_separator();
	popup->add_check_item(TTR("Half Resolution"), VIEW_HALF_RESOLUTION);
	popup->set_hide_on_checkable_item_selection(false);
	popup->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotCameraMonitor::_view_menu_option));
	header->add_child(view_menu);

	multicam_button = memnew(Button);
	multicam_button->set_theme_type_variation("FlatButton");
	multicam_button->set_toggle_mode(true);
	multicam_button->set_pressed(true);
	multicam_button->set_tooltip_text(TTR("Multicam: every camera at once, to cut between."));
	multicam_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotCameraMonitor::_multicam_toggled));
	header->add_child(multicam_button);

	monitor = memnew(AsosdotCameraView);
	monitor->set_v_size_flags(SIZE_EXPAND_FILL);
	monitor->set_full_quality(true);
	monitor->set_tooltip_text(TTR("Double-click to select this camera."));
	monitor->connect(SceneStringName(draw), callable_mp(this, &AsosdotCameraMonitor::_draw_monitor));
	monitor->connect(SceneStringName(gui_input), callable_mp(this, &AsosdotCameraMonitor::_monitor_gui_input));
	add_child(monitor);

	strip_scroll = memnew(ScrollContainer);
	strip_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	strip_scroll->set_custom_minimum_size(Size2(0, tile_height + 14 * EDSCALE));
	add_child(strip_scroll);
	strip = memnew(HBoxContainer);
	strip_scroll->add_child(strip);
	strip_hint = memnew(Label);
	strip_hint->set_text(TTR("Every camera in the scene shows here. Click one to cut to it."));
	strip_hint->set_v_size_flags(SIZE_SHRINK_CENTER);
	strip->add_child(strip_hint);

	tile_menu = memnew(PopupMenu);
	tile_menu->add_item(TTR("Cut Here"), TILE_CUT);
	tile_menu->add_item(TTR("Show in Monitor"), TILE_SHOW);
	tile_menu->add_item(TTR("Select Camera"), TILE_SELECT);
	tile_menu->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotCameraMonitor::_tile_menu_option));
	add_child(tile_menu);

	_update_view_menu();
	hide();
}

AsosdotCameraMonitor::~AsosdotCameraMonitor() {
}
