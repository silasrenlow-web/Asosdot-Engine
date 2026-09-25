/**************************************************************************/
/*  render_editor_plugin.h                                                */
/**************************************************************************/
/*  Asosdot: the Render workspace.                                        */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"

class AnimationPlayer;
class AspectRatioContainer;
class Button;
class Camera3D;
class ConfirmationDialog;
class DirectionalLight3D;
class EditorFileDialog;
class EditorInspector;
class Environment;
class HSlider;
class Label;
class MeshInstance3D;
class MovieWriter;
class ProceduralSkyMaterial;
class ProgressBar;
class SpotLight3D;
class ShaderMaterial;
class StandardMaterial3D;
class SubViewport;
class SubViewportContainer;

// The settings of a render: which camera, how the scene is lit, how big the
// picture is and what moves during an animation. Edited in the Render
// workspace's inspector and remembered with each scene.
class RenderShot : public Resource {
	GDCLASS(RenderShot, Resource);

public:
	enum Studio {
		STUDIO_SCENE, // The scene's own lights and environment.
		STUDIO_STUDIO,
		STUDIO_SOFT_BOX,
		STUDIO_OUTDOOR,
		STUDIO_NIGHT,
		STUDIO_SPOTLIGHT,
	};

	enum Resolution {
		RESOLUTION_720P,
		RESOLUTION_1080P,
		RESOLUTION_1440P,
		RESOLUTION_4K,
		RESOLUTION_SQUARE_1080,
		RESOLUTION_SQUARE_2048,
		RESOLUTION_PORTRAIT_1080,
		RESOLUTION_ICON_512,
		RESOLUTION_CUSTOM,
	};

	enum CameraMotion {
		CAMERA_MOTION_NONE,
		CAMERA_MOTION_TURNTABLE,
		CAMERA_MOTION_HALF_TURN,
		CAMERA_MOTION_SWING,
		CAMERA_MOTION_DOLLY_IN,
	};

private:
	Dictionary values;

protected:
	static void _bind_methods();
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _validate_property(PropertyInfo &p_property) const;
	bool _property_can_revert(const StringName &p_name) const;
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const;

public:
	static Dictionary get_defaults();

	Variant get_value(const StringName &p_name) const;
	void set_value(const StringName &p_name, const Variant &p_value);

	Size2i get_output_size() const;
	Dictionary get_state() const { return values.duplicate(); }
	void set_state(const Dictionary &p_state);

	RenderShot();
};

// The Render workspace: a live view through the render camera, with a studio
// of lights and a backdrop around the edited scene, and the means to render
// it to an image or an animation. Everything it adds to the scene's world
// lives outside the scene and is gone when the workspace is.
class RenderEditor : public VBoxContainer {
	GDCLASS(RenderEditor, VBoxContainer);

	friend class RenderEditorPlugin;

	Ref<RenderShot> shot;

	// Where the studio camera looks from, around what.
	Vector3 orbit_target;
	float orbit_yaw = 0.6;
	float orbit_pitch = -0.2;
	float orbit_distance = 4.0;
	bool framed = false;
	// What the studio lights and frames, found from the scene's geometry.
	AABB subject_bounds;

	float time = 0.0;
	bool playing = false;
	bool active = false;
	bool dragging = false;
	bool drag_moved = false;

	// Controls.
	Button *frame_button = nullptr;
	Button *render_image_button = nullptr;
	Button *render_animation_button = nullptr;
	Button *play_button = nullptr;
	HSlider *timeline = nullptr;
	Label *time_label = nullptr;
	Label *size_label = nullptr;
	AspectRatioContainer *aspect = nullptr;
	Control *preview_holder = nullptr;
	SubViewportContainer *preview_container = nullptr;
	SubViewport *preview = nullptr;
	Camera3D *preview_camera = nullptr;
	Control *preview_surface = nullptr;
	Label *hint_label = nullptr;
	EditorInspector *inspector = nullptr;
	EditorFileDialog *image_dialog = nullptr;
	EditorFileDialog *animation_dialog = nullptr;
	ConfirmationDialog *progress_dialog = nullptr;
	ProgressBar *progress_bar = nullptr;
	Label *progress_label = nullptr;

	// The studio. These are nodes under the preview viewport, which shares
	// the edited scene's world, so they light the scene without being in it.
	Node3D *studio = nullptr;
	DirectionalLight3D *key_light = nullptr;
	DirectionalLight3D *fill_light = nullptr;
	DirectionalLight3D *rim_light = nullptr;
	SpotLight3D *spot_light = nullptr;
	MeshInstance3D *backdrop = nullptr;
	Ref<StandardMaterial3D> backdrop_material;
	// Shows only the shadows falling on it, for transparent renders.
	Ref<ShaderMaterial> shadow_catcher_material;
	Ref<Environment> environment;
	Ref<ProceduralSkyMaterial> sky_material;
	float backdrop_built_size = 0.0;

	// Offline rendering, into a viewport of its own at the output size.
	SubViewport *render_viewport = nullptr;
	Camera3D *render_camera = nullptr;
	struct Job {
		bool active = false;
		bool animation = false;
		bool waiting = false;
		bool cancelled = false;
		String path;
		Size2i size;
		int supersampling = 1;
		int frame = 0;
		int frame_count = 1;
		int warmup = 0;
		float fps = 30.0;
		float time_before = 0.0;
		MovieWriter *writer = nullptr;
		bool low_processor_before = false;
	};
	Job job;

	// What posing an animation for the preview or a render changed, so it
	// can be put back exactly afterwards, whether or not the animation has
	// a RESET track.
	struct SavedValue {
		ObjectID object;
		Vector<StringName> property; // A property, or empty for a transform.
		int bone = -1; // A bone of a Skeleton3D.
		int blend_shape = -1;
		Variant value;
	};
	LocalVector<SavedValue> saved_pose;
	ObjectID posed_player;
	StringName posed_clip;
	StringName posed_assigned;
	bool posed = false;
	// The pose is taken off while the scene saves and put back after.
	bool repose_pending = false;
	void _save_pose(AnimationPlayer *p_player, const StringName &p_clip);

	Node *_get_scene_root() const;
	AABB _get_subject_bounds() const;
	void _frame_subject();
	void _shot_changed();
	void _update_output_label();

	Transform3D _camera_transform_at(float p_time, float &r_fov) const;
	Camera3D *_get_scene_camera() const;
	AnimationPlayer *_get_animation_player() const;
	void _apply_time(float p_time);
	void _restore_pose();
	void _update_studio();
	void _place_studio(const Transform3D &p_camera);
	void _build_backdrop(float p_size);
	void _update_cameras();
	void _update_timeline();
	void _draw_checker();

	void _timeline_changed(double p_value);
	void _play_pressed();
	void _preview_input(const Ref<InputEvent> &p_event);
	void _pick(const Point2 &p_point);

	void _render_image_pressed();
	void _render_animation_pressed();
	void _image_path_selected(const String &p_path);
	void _animation_path_selected(const String &p_path);
	void _start_job(const String &p_path, bool p_animation);
	void _step_job();
	void _finish_job(const String &p_error = String());
	void _cancel_job();
	Ref<Image> _grab_frame();

protected:
	void _notification(int p_what);

public:
	void set_active(bool p_active);
	void scene_changed();
	Dictionary get_state() const;
	void set_state(const Dictionary &p_state);

	RenderEditor();
	~RenderEditor();
};

class RenderEditorPlugin : public EditorPlugin {
	GDCLASS(RenderEditorPlugin, EditorPlugin);

	RenderEditor *render_editor = nullptr;

public:
	virtual String get_plugin_name() const override { return "Render"; }
	bool has_main_screen() const override { return true; }
	virtual void edit(Object *p_object) override {}
	virtual bool handles(Object *p_object) const override { return false; }
	virtual void make_visible(bool p_visible) override;
	virtual Dictionary get_state() const override;
	virtual void set_state(const Dictionary &p_state) override;
	virtual void clear() override;
	virtual void edited_scene_changed() override;
	virtual void apply_changes() override;

	RenderEditorPlugin();
};

VARIANT_ENUM_CAST(RenderShot::Studio);
VARIANT_ENUM_CAST(RenderShot::Resolution);
VARIANT_ENUM_CAST(RenderShot::CameraMotion);
