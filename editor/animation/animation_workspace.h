/**************************************************************************/
/*  animation_workspace.h                                                 */
/**************************************************************************/
/*  Asosdot: the Animation workspace, a main screen laid out for          */
/*  animating cutscenes and rigs: the 3D editor as the viewer, a library  */
/*  of animations, shots, sounds and poses beside it, the Inspector, and  */
/*  a tall timeline below, with recording, camera cuts and a pose library.*/
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/resources/animation.h"

class AnimationLibrary;
class AnimationPlayer;
class AnimationTrackEditor;
class AsosdotCameraMonitor;
class AudioStreamPlayer;
class Button;
class Camera3D;
class ConfirmationDialog;
class EditorFileDialog;
class HSlider;
class ItemList;
class Label;
class LineEdit;
class MenuButton;
class Node3D;
class SceneTreeEditor;
class ScreenSelect;
class Skeleton3D;
class TabBar;
class TabContainer;
class TextureRect;
class Tree;
class TreeItem;
class VSplitContainer;
class WindowWrapper;

// A part of the workspace under a header of its own. Its float button moves it into a window, to
// put on another screen, and closing that window docks it back. While it floats it takes no room
// in the workspace, and it leaves and returns with the workspace.
class AsosdotWorkspacePanel : public VBoxContainer {
	GDCLASS(AsosdotWorkspacePanel, VBoxContainer);

	String title;
	StringName icon_name;
	HBoxContainer *header = nullptr;
	TextureRect *icon = nullptr;
	Label *title_label = nullptr;
	LocalVector<ScreenSelect *> float_buttons;
	WindowWrapper *wrapper = nullptr;

	bool shown = true;
	bool floating = false;
	bool suspended = true;
	bool changing = false;
	Rect2i window_rect;
	int window_screen = -1;
	Rect2i window_screen_rect;

	void _float_requested(int p_screen);
	void _window_visibility_changed(bool p_visible);
	void _store_window();
	void _apply();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	HBoxContainer *get_header() const { return header; }
	// Another float button, for a part that draws its own header.
	Control *make_float_button();

	bool can_float() const;
	void set_shown(bool p_shown);
	bool is_shown() const { return shown; }
	void set_floating(bool p_floating, int p_screen = -1);
	bool is_floating() const { return floating; }
	bool is_window_open() const;
	// Suspended while the workspace is closed: a floating part's window closes until it opens again.
	void set_suspended(bool p_suspended);

	Dictionary get_layout();
	void set_layout(const Dictionary &p_layout);

	AsosdotWorkspacePanel(const String &p_title, const StringName &p_icon, Control *p_content, bool p_with_header = true);
};

class AsosdotAnimationWorkspace : public VBoxContainer {
	GDCLASS(AsosdotAnimationWorkspace, VBoxContainer);

	friend class AsosdotAnimationWorkspacePlugin;
	friend class AsosdotCameraMonitor;

public:
	struct CameraCut {
		double time = 0.0;
		Camera3D *camera = nullptr;
	};

private:
	enum LibraryTab {
		TAB_ANIMATIONS,
		TAB_SHOTS,
		TAB_SOUNDS,
		TAB_POSES,
	};

	enum PanelsMenu {
		PANELS_SCENE,
		PANELS_LIBRARY,
		PANELS_MONITOR,
		PANELS_DOCKS,
		PANELS_FLOAT_SCENE,
		PANELS_FLOAT_LIBRARY,
		PANELS_FLOAT_MONITOR,
		PANELS_FLOAT_TIMELINE,
		PANELS_FLOAT_INSPECTOR,
		PANELS_DOCK_ALL,
	};

	bool active = false;

	// The toolbar sits in the 3D editor's menu bar while the workspace is open.
	HBoxContainer *toolbar = nullptr;
	Button *record_button = nullptr;
	Button *shot_button = nullptr;
	Button *graph_button = nullptr;
	Button *cut_button = nullptr;
	// What is shown and what floats is chosen in one menu, not a button each.
	MenuButton *panels_menu = nullptr;
	bool monitor_enabled = true;
	bool docks_enabled = false;
	Label *timecode = nullptr;

	// The parts beside the viewer. Each can float in a window of its own.
	VSplitContainer *left_split = nullptr;
	AsosdotWorkspacePanel *scene_panel = nullptr;
	AsosdotWorkspacePanel *library_panel = nullptr;
	AsosdotWorkspacePanel *monitor_panel = nullptr;

	// The scene's nodes, to pick what to animate without leaving the workspace. It shares the
	// editor's selection, and marks the nodes the current animation has tracks for.
	SceneTreeEditor *hierarchy = nullptr;
	LineEdit *hierarchy_filter = nullptr;
	Button *hierarchy_expand = nullptr;
	Button *hierarchy_collapse = nullptr;
	Button *hierarchy_status = nullptr; // How many nodes have tracks; pressing it selects them.
	uint32_t marked_hash = 0;

	// Beside the viewer, in the 3D editor's right panel.
	AsosdotCameraMonitor *monitor = nullptr;

	// The edited scene's cameras in scene order, found once a frame for the Shots list and the monitor.
	LocalVector<ObjectID> scene_cameras;

	TabContainer *tabs = nullptr;
	TabBar *library_tab_bar = nullptr;

	// Animations.
	LineEdit *animation_search = nullptr;
	Tree *animation_tree = nullptr;
	LocalVector<Button *> animation_tool_buttons;
	uint32_t listed_animations_hash = 0;
	String listed_current;

	// Shots.
	Tree *camera_tree = nullptr;
	Tree *cut_tree = nullptr;
	uint32_t listed_cuts_hash = 0;
	uint32_t listed_cameras_hash = 0;

	// Sounds.
	LineEdit *sound_search = nullptr;
	Tree *sound_tree = nullptr;
	Label *sound_target = nullptr;
	AudioStreamPlayer *sound_preview = nullptr;
	String previewing_sound;

	// Poses.
	Label *pose_library_label = nullptr;
	ItemList *pose_list = nullptr;
	HSlider *pose_mix = nullptr;
	Label *pose_hint = nullptr;
	EditorFileDialog *pose_file_dialog = nullptr;
	ConfirmationDialog *pose_name_dialog = nullptr;
	LineEdit *pose_name = nullptr;
	Ref<AnimationLibrary> pose_library;
	String pose_library_path;

	// Recording: what the selection looked like after the last edit, playhead move or key, to tell
	// what an edit changed.
	struct RecordedNode {
		Vector3 position;
		Quaternion rotation;
		Vector3 scale;
		LocalVector<Vector3> bone_positions;
		LocalVector<Quaternion> bone_rotations;
		LocalVector<Vector3> bone_scales;
	};
	HashMap<ObjectID, RecordedNode> recorded;
	HashSet<Pair<ObjectID, StringName>> recorded_properties;
	uint64_t recorded_version = 0;
	bool record_flush_queued = false;
	bool record_inserting = false;
	ObjectID watched_player;

	// The editor around the workspace, put back when it closes.
	int saved_center_split = 0;
	bool saved_center_split_valid = false;

	AnimationTrackEditor *_get_track_editor() const;
	AnimationPlayer *_get_player() const;
	Ref<Animation> _get_animation() const;
	Node *_get_animation_root() const;
	double _get_playhead() const;
	void _set_playhead(double p_time);

	void _notification(int p_what);
	void _update_timecode();
	void _watch_player();

	// Animations.
	void _update_animations(bool p_force = false);
	void _animation_selected();
	void _animation_activated();
	void _animation_tool(int p_tool);

	// Shots.
	void _update_scene_cameras();
	void _update_shots(bool p_force = false);
	void _camera_activated();
	void _camera_button(TreeItem *p_item, int p_column, int p_id, MouseButton p_button);
	void _cut_selected();
	void _cut_button(TreeItem *p_item, int p_column, int p_id, MouseButton p_button);
	void _cut_to_selected_camera();

	// Sounds.
	void _update_sounds();
	void _sound_activated();
	void _sound_button(TreeItem *p_item, int p_column, int p_id, MouseButton p_button);
	Variant _sound_drag_data(const Point2 &p_point);
	void _add_sound_at_playhead(const String &p_path);
	Node *_get_sound_player(bool p_selected_only) const;
	void _update_sound_target();

	// Poses.
	void _pose_library_open();
	void _pose_library_new();
	void _pose_library_chosen(const String &p_path);
	void _save_pose_requested();
	void _save_pose_confirmed();
	void _apply_pose(bool p_selected_bone_only);
	void _delete_pose();
	void _update_poses();
	Skeleton3D *_get_selected_skeleton() const;

	// Recording.
	void _record_toggled(bool p_pressed);
	void _record_snapshot();
	void _take_record_snapshot();
	void _record_version_changed();
	void _record_property_edited(const String &p_property);
	void _record_flush();

	void _shot_toggled(bool p_pressed);
	void _graph_toggled(bool p_pressed);
	void _graph_editor_toggled(bool p_shown);
	void _apply_docks();
	void _apply_monitor();

	void _panels_menu_about_to_popup();
	void _panels_menu_pressed(int p_id);
	void _panel_state_changed();
	void _update_left_column();

	// Hierarchy.
	void _hierarchy_filter_changed(const String &p_filter);
	void _hierarchy_fold_all(bool p_collapsed);
	void _update_hierarchy_marks(bool p_force = false);
	void _select_animated_nodes();
	void _library_tab_selected(int p_tab);
	void _enlarge_timeline(float p_share);

protected:
	static void _bind_methods();

public:
	// Camera cuts are `current` tracks on each Camera3D: true where its shot starts, false where it
	// ends, so they play the same in Godot without Asosdot.
	static Vector<CameraCut> get_camera_cuts(const Ref<Animation> &p_animation, Node *p_root);
	static void cut_to_camera(const Ref<Animation> &p_animation, Node *p_root, Camera3D *p_camera, double p_time);
	static void remove_cut(const Ref<Animation> &p_animation, Node *p_root, double p_time);
	// Cuts the edited animation to the camera at the playhead; while playing, on the frame being shown.
	void cut_to_camera_at_playhead(Camera3D *p_camera);

	static String format_time(double p_time);
	static uint64_t get_scene_history_version();

	// A pose is an Animation holding one key per bone track; it's applied by bone name, so it fits
	// any rig with the same bones.
	static Ref<Animation> make_pose(Skeleton3D *p_skeleton, const Vector<int> &p_bones);
	static void apply_pose(Skeleton3D *p_skeleton, const Ref<Animation> &p_pose, float p_mix, int p_only_bone = -1);

	void set_active(bool p_active);
	bool is_active() const { return active; }
	bool is_recording() const;
	AsosdotCameraMonitor *get_monitor() const { return monitor; }
	AsosdotWorkspacePanel *get_monitor_panel() const { return monitor_panel; }

	Dictionary get_layout() const;
	void set_layout(const Dictionary &p_layout);

	AsosdotAnimationWorkspace();
};

class AsosdotAnimationWorkspacePlugin : public EditorPlugin {
	GDCLASS(AsosdotAnimationWorkspacePlugin, EditorPlugin);

	static AsosdotAnimationWorkspacePlugin *singleton;

	AsosdotAnimationWorkspace *workspace = nullptr;
	EditorPlugin *node_3d_plugin = nullptr;
	mutable EditorPlugin *canvas_plugin = nullptr;

	EditorPlugin *_get_node_3d_plugin();

public:
	static AsosdotAnimationWorkspacePlugin *get_singleton() { return singleton; }

	virtual String get_plugin_name() const override { return "Animation"; }
	virtual bool has_main_screen() const override { return true; }
	virtual bool hosts_main_screen_of(const EditorPlugin *p_plugin) const override;
	virtual void make_visible(bool p_visible) override;
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override { return false; }
	virtual void set_window_layout(Ref<ConfigFile> p_layout) override;
	virtual void get_window_layout(Ref<ConfigFile> p_layout) override;

	AsosdotAnimationWorkspace *get_workspace() const { return workspace; }

	AsosdotAnimationWorkspacePlugin();
	~AsosdotAnimationWorkspacePlugin();
};
