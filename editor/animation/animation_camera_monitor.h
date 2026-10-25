/**************************************************************************/
/*  animation_camera_monitor.h                                            */
/**************************************************************************/
/*  Asosdot: the Animation workspace's camera monitor, beside the viewer: */
/*  any camera's picture or the live shot, with frame guides, and a       */
/*  multicam strip of every camera to cut between, live while playing.    */
/**************************************************************************/

#pragma once

#include "scene/gui/box_container.h"
#include "scene/resources/environment.h"

class AsosdotAnimationWorkspace;
class Button;
class Camera3D;
class HBoxContainer;
class Label;
class MenuButton;
class OptionButton;
class PopupMenu;
class ScrollContainer;
class SubViewport;

// A camera's picture. It's rendered through a stand-in camera that copies the real one, so the scene
// shows as the game sees it, without the editor's gizmos or grid, whatever layers the camera sees.
class AsosdotCameraView : public Control {
	GDCLASS(AsosdotCameraView, Control);

	SubViewport *viewport = nullptr;
	RID proxy;
	ObjectID camera;
	// What the stand-in was last given, so it's only updated when the camera changes.
	uint32_t synced_hash = 0;
	float aspect = 16.0f / 9.0f;
	bool continuous = true;
	bool full_quality = false;
	bool half_resolution = false;

	// A view that isn't full quality sees the scene through a copy of its environment without what
	// costs each viewport the most (global illumination, screen-space effects, volumetric fog),
	// remade when the scene is edited.
	Ref<Environment> light_source;
	Ref<Environment> light_environment;
	uint64_t light_version = 0;

	void _update_viewport_size();
	void _sync_proxy();
	Ref<Environment> _get_light_environment(Camera3D *p_source);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_camera(Camera3D *p_camera);
	Camera3D *get_camera() const;

	// The shape of the picture, width over height; it's fitted in the control with black around it.
	void set_aspect(float p_aspect);
	float get_aspect() const { return aspect; }
	Rect2 get_picture_rect() const;

	// Continuous views render every frame the editor draws; others only when refresh() asks.
	void set_continuous(bool p_continuous);
	void refresh();

	// Full quality uses the project's antialiasing and 3D scaling, as the game would.
	void set_full_quality(bool p_full_quality);
	void set_half_resolution(bool p_half);
	void apply_project_settings();

	AsosdotCameraView();
	~AsosdotCameraView();
};

class AsosdotCameraMonitor : public VBoxContainer {
	GDCLASS(AsosdotCameraMonitor, VBoxContainer);

public:
	enum Guide {
		GUIDE_THIRDS = 1,
		GUIDE_CENTER = 2,
		GUIDE_SAFE_AREAS = 4,
		GUIDE_BURN_IN = 8,
	};

	enum Letterbox {
		LETTERBOX_NONE,
		LETTERBOX_1_85,
		LETTERBOX_2_39,
	};

	enum Aspect {
		ASPECT_GAME,
		ASPECT_16_9,
		ASPECT_4_3,
		ASPECT_21_9,
		ASPECT_9_16,
		ASPECT_1_1,
	};

	static constexpr int SHORTCUT_CAMERAS = 9;

private:
	enum ViewMenu {
		VIEW_THIRDS,
		VIEW_CENTER,
		VIEW_SAFE_AREAS,
		VIEW_BURN_IN,
		VIEW_LETTERBOX_NONE,
		VIEW_LETTERBOX_1_85,
		VIEW_LETTERBOX_2_39,
		VIEW_ASPECT_GAME,
		VIEW_ASPECT_16_9,
		VIEW_ASPECT_4_3,
		VIEW_ASPECT_21_9,
		VIEW_ASPECT_9_16,
		VIEW_ASPECT_1_1,
		VIEW_HALF_RESOLUTION,
	};

	enum TileMenu {
		TILE_CUT,
		TILE_SHOW,
		TILE_SELECT,
	};

	AsosdotAnimationWorkspace *workspace = nullptr;
	bool active = false;

	OptionButton *source = nullptr;
	MenuButton *view_menu = nullptr;
	Button *multicam_button = nullptr;
	AsosdotCameraView *monitor = nullptr;
	ScrollContainer *strip_scroll = nullptr;
	HBoxContainer *strip = nullptr;
	Label *strip_hint = nullptr;
	PopupMenu *tile_menu = nullptr;
	LocalVector<AsosdotCameraView *> tiles;

	// The scene's cameras in scene order, which is also the order of the strip and its number keys,
	// and what they're called: their names, or with their parent's where names repeat.
	LocalVector<ObjectID> cameras;
	LocalVector<String> camera_labels;
	uint32_t listed_cameras_hash = 0;
	float tile_height = 0.0f;

	// The camera picked in the source menu; none means the live shot.
	ObjectID picked;
	// A picked camera remembered by the layout, found once the scene's cameras are listed.
	String picked_path;

	int guides = GUIDE_THIRDS | GUIDE_BURN_IN;
	Letterbox letterbox = LETTERBOX_NONE;
	Aspect aspect = ASPECT_GAME;
	bool half_resolution = false;

	// The strip renders a couple of cameras a frame, and only after something moved, so an idle
	// editor stays idle.
	uint32_t strip_state_hash = 0;
	int tiles_to_refresh = 0;
	int next_tile = 0;
	int hovered_tile = -1;
	int menu_tile = -1;
	uint32_t overlay_hash = 0;
	int saved_split = 0;
	bool saved_split_valid = false;

	void _update_cameras(bool p_force = false);
	void _update_view_menu();
	void _apply_aspect();
	float _get_aspect_ratio() const;
	String _get_aspect_name() const;
	Camera3D *_get_camera(int p_index) const;
	String _get_camera_label(const Camera3D *p_camera) const;

	void _source_selected(int p_index);
	void _view_menu_option(int p_option);
	void _multicam_toggled(bool p_pressed);
	void _monitor_gui_input(const Ref<InputEvent> &p_event);
	void _tile_gui_input(const Ref<InputEvent> &p_event, int p_index);
	void _tile_hovered(int p_index);
	void _tile_menu_option(int p_option);
	void _select_camera(Camera3D *p_camera);
	void _refresh_strip();
	void _project_settings_changed();

	void _draw_monitor();
	void _draw_tile(int p_index);
	// A line of text on a box, from a corner; returns the box's width.
	Size2 _get_label_size(const String &p_text) const;
	float _draw_label(Control *p_canvas, const Point2 &p_corner, HorizontalAlignment p_align, bool p_top, const String &p_text, const Color &p_background) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	virtual void shortcut_input(const Ref<InputEvent> &p_event) override;

public:
	// The camera live at the playhead: the edited scene's current camera, which the cuts switch.
	static Camera3D *get_live_camera();
	// What the monitor shows: the picked camera, or else the live one.
	Camera3D *get_shown_camera() const;
	void show_camera(Camera3D *p_camera);
	// Cuts to the strip's camera at the playhead, as clicking it or pressing its number key does.
	void cut_to(int p_index);
	int get_camera_count() const { return cameras.size(); }

	void set_active(bool p_active);
	Dictionary get_layout() const;
	void set_layout(const Dictionary &p_layout);

	// The workspace is always given; the default lets ClassDB register the class, which needs a
	// constructor without arguments.
	AsosdotCameraMonitor(AsosdotAnimationWorkspace *p_workspace = nullptr);
	~AsosdotCameraMonitor();
};
