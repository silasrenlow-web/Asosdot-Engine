/**************************************************************************/
/*  render_editor_plugin.h                                                */
/**************************************************************************/
/*  Asosdot: the Render workspace.                                        */
/**************************************************************************/

#pragma once

#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/list.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "servers/audio/audio_stream.h"

class Animation;
class AnimationPlayer;
class AudioStreamPlayer;
class AspectRatioContainer;
class Button;
class Camera3D;
class ConfirmationDialog;
class DirectionalLight3D;
class EditorFileDialog;
class EditorInspector;
class EditorUndoRedoManager;
class Environment;
class GeometryInstance3D;
class HBoxContainer;
class InputEventKey;
class Label;
class ItemList;
class TabBar;
class SplitContainer;
class PanelContainer;
class VSplitContainer;
class MenuButton;
class MeshInstance3D;
class MovieWriter;
class Node3D;
class ScrollContainer;
class Skeleton3D;
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
class AsosdotRenderShot : public Resource {
	GDCLASS(AsosdotRenderShot, Resource);

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
	bool sequence_output = false;

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
	void set_sequence_output(bool p_enabled) { sequence_output = p_enabled; }

	AsosdotRenderShot();
};

// The Render workspace: a live view through the render camera, with a studio
// of lights and a backdrop around the edited scene, and the means to render
// it to an image or an animation. Everything it adds to the scene's world
// lives outside the scene and is gone when the workspace is.
class AsosdotRenderEditor : public VBoxContainer {
	GDCLASS(AsosdotRenderEditor, VBoxContainer);

	friend class AsosdotRenderEditorPlugin;

public:
	enum PoseTool {
		POSE_TOOL_ROTATE,
		POSE_TOOL_MOVE,
		POSE_TOOL_REACH, // Pulls a bone's joint, bending the two bones above it.
		POSE_TOOL_MAX,
	};

	enum KeyScope {
		KEY_POSED, // What was posed since the last key, and the selection.
		KEY_SKELETON, // Every bone of the selection's skeleton.
		KEY_DELETE, // Take the selection's keys at this time away.
	};

private:
	Ref<AsosdotRenderShot> shot;
	Ref<AsosdotRenderShot> sequence_output;
	struct SequenceShot {
		Ref<AsosdotRenderShot> settings;
		String id;
		Vector3 target;
		float yaw = 0.6;
		float pitch = -0.2;
		float distance = 4.0;
		bool framed = false;
	};
	Vector<SequenceShot> shots;
	// Keep settings objects stable across sequence undo, so Inspector undo
	// actions still refer to the live shot after adding/removing/reordering.
	HashMap<String, Ref<AsosdotRenderShot>> shot_resources;
	String _shot_resource_key(const String &p_id) const;
	int selected_shot = 0;
	double sequence_time = 0.0;
	bool updating_sequence = false;
	bool evaluating_transition = false;
	bool camera_blend_active = false;
	Transform3D camera_blend_from;
	float camera_blend_fov = 35.0;
	float camera_blend_size = 1.0;
	int camera_blend_projection = 0;
	int camera_blend_aspect = 1;
	float camera_blend_weight = 1.0;
	ItemList *shot_list = nullptr;
	Control *sequence_strip = nullptr;
	TabBar *settings_selector = nullptr;
	Button *add_shot_button = nullptr;
	Button *duplicate_shot_button = nullptr;
	Button *remove_shot_button = nullptr;
	Button *move_shot_up_button = nullptr;
	Button *move_shot_down_button = nullptr;
	AudioStreamPlayer *music_player = nullptr;
	AudioStreamPlayer *dialogue_player = nullptr;
	String preview_music_path;
	String preview_dialogue_path;
	int preview_dialogue_shot = -1;

	double _sequence_duration() const;
	double _shot_start(int p_index) const;
	static String _camera_label(const Ref<AsosdotRenderShot> &p_shot);
	void _store_shot_view();
	void _activate_shot(int p_index);
	void _apply_sequence_time(double p_time);
	void _update_sequence_ui();
	void _settings_selected(int p_index);
	void _sequence_output_changed();
	void _draw_sequence();
	void _sequence_input(const Ref<InputEvent> &p_event);
	void _select_shot(int p_index);
	void _add_shot(bool p_duplicate);
	void _remove_shot();
	void _move_shot(int p_direction);
	void _commit_sequence_edit(const Dictionary &p_before, const String &p_name);
	void _set_sequence_editing(bool p_enabled);
	void _stop_audio();
	void _update_preview_audio(bool p_seek = false);
	void _prepare_render_audio();
	Vector<int32_t> _mix_render_audio();
	void _render_sequence(const String &p_path);

	// Preview above, a full-width sequence dock below, and properties beside
	// the preview. Layout state belongs to the workspace, not the scene.
	VSplitContainer *workspace_split = nullptr;
	SplitContainer *preview_split = nullptr;
	SplitContainer *sequence_split = nullptr;
	PanelContainer *preview_panel = nullptr;
	PanelContainer *properties_panel = nullptr;
	PanelContainer *shots_panel = nullptr;
	PanelContainer *sequence_panel = nullptr;
	HBoxContainer *workspace_toolbar = nullptr;
	Control *sequence_dock = nullptr;
	Label *preview_title = nullptr;
	Label *sequence_summary = nullptr;
	Label *shot_count_label = nullptr;
	Label *properties_title = nullptr;
	Button *workspace_focus_button = nullptr;
	Button *preview_focus_button = nullptr;
	Button *guides_button = nullptr;
	Button *reset_layout_button = nullptr;
	Button *sequence_start_button = nullptr;
	Button *sequence_end_button = nullptr;
	Button *previous_frame_button = nullptr;
	Button *next_frame_button = nullptr;
	bool workspace_focus = false;
	bool preview_focus = false;
	int preview_split_before_focus = 0;
	int workspace_split_before_focus = 0;
	bool composition_guides = false;
	bool owns_editor_focus = false;
	bool editor_focus_before = false;
	bool compact_workspace = false;
	// The status line under the workspace: the selected bone and its clip while
	// posing, then the controls' hints.
	HBoxContainer *status_bar = nullptr;
	void _build_workspace();
	void _update_workspace_theme();
	void _update_workspace_layout();
	void _update_workspace_labels();
	void _set_workspace_focus(bool p_enabled);
	void _set_preview_focus(bool p_enabled);
	void _set_composition_guides(bool p_enabled);
	void _draw_composition_guides();
	void _reset_workspace_layout();
	void _step_sequence_frame(int p_direction);
	void _jump_sequence_boundary(bool p_end);
	bool _handle_transport_key(const Ref<InputEventKey> &p_key);
	bool _key_time_near(float p_x, double &r_time) const;
	static String _format_timecode(double p_time, int p_fps);

	// Where the studio camera looks from, around what.
	Vector3 orbit_target;
	float orbit_yaw = 0.6;
	float orbit_pitch = -0.2;
	float orbit_distance = 4.0;
	bool framed = false;
	// What the studio lights and frames, found from the scene's geometry.
	AABB subject_bounds;

	float time = 0.0;
	// Where the clip was put for time, and where keys set now land.
	double clip_time = 0.0;
	// The player, clip and speed the pose was last taken from, so changing
	// anything else leaves a pose that isn't keyed yet alone.
	String applied_animation;
	bool playing = false;
	bool active = false;
	bool dragging = false;
	bool drag_moved = false;

	// Posing: bones of any Skeleton3D, or any other Node3D, turned and moved
	// by hand in the preview, and keyed into a clip.
	struct PoseTarget {
		ObjectID node; // The Skeleton3D for a bone, or the node itself.
		int bone = -1;
		bool operator==(const PoseTarget &p_other) const { return node == p_other.node && bone == p_other.bone; }
		bool is_set() const { return node.is_valid(); }
	};
	struct PoseValue {
		PoseTarget target;
		// A bone's pose.
		Vector3 position;
		Quaternion rotation;
		Vector3 scale = Vector3(1, 1, 1);
		// A node's local transform.
		Transform3D transform;
	};
	enum DragKind {
		DRAG_NONE,
		DRAG_VIEW_ROTATE, // About the line of sight, through the joint.
		DRAG_TRACKBALL,
		DRAG_TWIST, // About the bone's own length.
		DRAG_MOVE,
		DRAG_REACH,
	};
	struct PoseDrag {
		DragKind kind = DRAG_NONE;
		PoseTarget target;
		Point2 start_mouse;
		Point2 pivot_screen;
		Vector3 pivot; // In the world, where the target turns about.
		Vector3 axis; // In the world, what a twist turns about.
		Vector3 grab; // In the world, the point under the cursor at the start.
		LocalVector<PoseValue> before; // Everything the drag may change.
		bool moved = false;
	};
	bool pose_mode = false;
	PoseTool pose_tool = POSE_TOOL_ROTATE;
	bool auto_key = false;
	PoseTarget pose_selected;
	PoseTarget pose_hovered;
	// Posed since the last key, so a key takes all of them.
	LocalVector<PoseTarget> pose_touched;
	PoseDrag pose_drag;

	// Where renders went last, so the dialogs open there again.
	String last_output_dir;
	String reveal_path;

	// Controls.
	Button *frame_button = nullptr;
	Button *pose_button = nullptr;
	// The pose shelf scrolls, so a short preview never has to grow to fit it.
	ScrollContainer *pose_scroll = nullptr;
	VBoxContainer *pose_bar = nullptr;
	Button *pose_tool_buttons[POSE_TOOL_MAX] = {};
	Label *pose_label = nullptr;
	Button *parent_button = nullptr;
	Button *reset_pose_button = nullptr;
	Label *clip_label = nullptr;
	Button *new_clip_button = nullptr;
	Button *auto_key_button = nullptr;
	Button *key_button = nullptr;
	MenuButton *key_menu = nullptr;
	Button *previous_key_button = nullptr;
	Button *next_key_button = nullptr;
	Button *render_image_button = nullptr;
	Button *render_animation_button = nullptr;
	Button *play_button = nullptr;
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
		double time_before = 0.0;
		int shot_before = 0;
		int rendered_shot = -1;
		MovieWriter *writer = nullptr;
		bool low_processor_before = false;
	};
	Job job;

	// Frames are encoded and written off the main thread, which only renders and reads back.
	// Image files go to worker threads in parallel; video frames go in order to one writer thread.
	int max_frames_in_flight = 2;
	SafeFlag frame_write_failed;
	LocalVector<WorkerThreadPool::TaskID> frame_write_tasks;
	MovieWriter *frame_writer_movie = nullptr;
	Thread frame_writer_thread;
	Mutex frame_queue_mutex;
	Semaphore frame_queue_semaphore; // Posted for each queued video frame, and to stop the writer.
	Semaphore frame_slots; // Free places in the video queue, so rendering can't run far ahead of encoding.
	struct MovieFrame {
		Ref<Image> image;
		Vector<int32_t> audio;
	};
	List<MovieFrame> frame_queue;
	bool frame_writer_exit = false;
	struct AudioCue {
		Ref<AudioStreamPlayback> playback;
		double start = 0.0;
		double end = 0.0;
		double offset = 0.0;
		float gain = 1.0;
		bool started = false;
	};
	Vector<AudioCue> render_audio;
	uint64_t rendered_audio_samples = 0;

	static void _frame_writer_thread_func(void *p_userdata);
	void _queue_image_file(const Ref<Image> &p_image, const String &p_path);
	void _queue_movie_frame(const Ref<Image> &p_image);
	void _flush_frame_writes();

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
	bool _saved_value_for_track(const Ref<Animation> &p_clip, int p_track, Node *p_animation_root, SavedValue &r_saved) const;
	static String _saved_value_key(const SavedValue &p_saved);
	static void _put_back(const SavedValue &p_saved);
	void _sync_saved_pose();

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

	void _seek_sequence(double p_time);
	void _play_pressed();
	void _preview_input(const Ref<InputEvent> &p_event);
	GeometryInstance3D *_raycast_geometry(const Point2 &p_point) const;
	void _pick(const Point2 &p_point);

	// Posing (render_editor_pose.cpp).
	void _set_pose_mode(bool p_enabled);
	void _set_pose_tool(int p_tool);
	void _set_auto_key(bool p_enabled);
	void _update_pose_bar();
	void _draw_pose_overlay();
	LocalVector<Skeleton3D *> _get_skeletons() const;
	Vector3 _get_target_origin(const PoseTarget &p_target) const;
	Vector3 _get_bone_axis(Skeleton3D *p_skeleton, int p_bone) const;
	PoseTarget _pick_bone(const Point2 &p_point, Skeleton3D *p_only = nullptr, float p_max_distance = -1.0) const;
	void _pose_pick(const Point2 &p_point);
	void _select_pose_target(const PoseTarget &p_target);
	void _select_parent();
	PoseValue _get_pose_value(const PoseTarget &p_target) const;
	static void _set_pose_value(const PoseValue &p_value);
	static void _add_pose_undo(EditorUndoRedoManager *p_undo_redo, const PoseValue &p_before, const PoseValue &p_after);
	bool _begin_pose_drag(const Point2 &p_point, bool p_shift, bool p_alt);
	void _update_pose_drag(const Point2 &p_point);
	void _end_pose_drag();
	void _cancel_pose_drag();
	bool _intersect_view_plane(const Point2 &p_point, const Vector3 &p_through, Vector3 &r_hit) const;
	void _turn_target(const PoseTarget &p_target, const Quaternion &p_turn);
	void _move_target(const PoseTarget &p_target, const Vector3 &p_offset);
	void _reach(const Vector3 &p_target);
	void _reset_pose_pressed();
	void _touch(const PoseTarget &p_target);

	// Keyframing (render_editor_pose.cpp).
	double _get_key_time(const Ref<Animation> &p_clip) const;
	bool _is_clip_editable(AnimationPlayer *p_player, const Ref<Animation> &p_clip) const;
	Ref<Animation> _get_key_clip(EditorUndoRedoManager *p_undo_redo, bool p_new_clip, bool p_create, bool &r_created, Node *&r_animation_root);
	static Ref<Animation> _snapshot_clip(const Ref<Animation> &p_clip);
	void _set_clip_state(const Ref<Animation> &p_clip, const Ref<Animation> &p_state);
	void _clip_changed();
	LocalVector<PoseTarget> _get_key_targets(KeyScope p_scope) const;
	NodePath _get_track_path(const PoseTarget &p_target, Node *p_animation_root) const;
	void _write_keys(const Ref<Animation> &p_clip, const LocalVector<PoseTarget> &p_targets, Node *p_animation_root, double p_time, bool p_delete) const;
	bool _add_key_ops(EditorUndoRedoManager *p_undo_redo, const LocalVector<PoseTarget> &p_targets, bool p_delete);
	void _key(int p_scope);
	void _new_clip_pressed();
	Vector<double> _get_key_times(bool p_selected_only) const;
	void _jump_to_key(bool p_next);

	String _default_output_path(const String &p_extension) const;
	String _chosen_output_path(const String &p_path);
	void _reveal_output();
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
	static void _bind_methods();
	void _notification(int p_what);
	// Transport keys come before the focused control, so Space and the arrows
	// step the sequence even after a button in the workspace was pressed.
	void input(const Ref<InputEvent> &p_event) override;

public:
	void set_active(bool p_active);
	bool is_rendering() const { return job.active; }
	void scene_changed();
	Dictionary get_state() const;
	void set_state(const Dictionary &p_state);

	AsosdotRenderEditor();
	~AsosdotRenderEditor();
};

class AsosdotRenderEditorPlugin : public EditorPlugin {
	GDCLASS(AsosdotRenderEditorPlugin, EditorPlugin);

	AsosdotRenderEditor *render_editor = nullptr;

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

	AsosdotRenderEditorPlugin();
};

VARIANT_ENUM_CAST(AsosdotRenderShot::Studio);
VARIANT_ENUM_CAST(AsosdotRenderShot::Resolution);
VARIANT_ENUM_CAST(AsosdotRenderShot::CameraMotion);
