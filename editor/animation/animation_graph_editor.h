/**************************************************************************/
/*  animation_graph_editor.h                                              */
/**************************************************************************/
/*  Asosdot: the Graph Editor, a view of the timeline that draws every    */
/*  animated number as a curve, as Blender's does: positions, rotations,  */
/*  scales, blend shapes, property values and Bezier tracks, edited with  */
/*  Blender's keys and menus.                                             */
/**************************************************************************/

#pragma once

#include "core/templates/rb_set.h"
#include "scene/gui/box_container.h"
#include "scene/resources/animation.h"

class AnimationTimelineEdit;
class AnimationTrackEditor;
class Button;
class EditorUndoRedoManager;
class Label;
class MenuButton;
class OptionButton;
class PanelContainer;
class PopupMenu;
class SpinBox;

class AsosdotGraphEditor : public VBoxContainer {
	GDCLASS(AsosdotGraphEditor, VBoxContainer);

public:
	enum MenuOption {
		// View.
		VIEW_FRAME_ALL,
		VIEW_FRAME_SELECTED,
		VIEW_SHOW_HANDLES,
		VIEW_NORMALIZE,
		VIEW_ONLY_SELECTED,
		VIEW_SIDEBAR,
		VIEW_DOPE_SHEET,
		// Select.
		SELECT_ALL,
		SELECT_NONE,
		SELECT_INVERT,
		SELECT_BOX,
		SELECT_COLUMNS,
		SELECT_BEFORE_CURRENT,
		SELECT_AFTER_CURRENT,
		// Channel.
		CHANNEL_HIDE,
		CHANNEL_HIDE_UNSELECTED,
		CHANNEL_REVEAL,
		CHANNEL_LOCK,
		CHANNEL_UNLOCK,
		CHANNEL_EXPAND_ALL,
		CHANNEL_COLLAPSE_ALL,
		CHANNEL_CONVERT_TO_BEZIER,
		// Key.
		KEY_INSERT,
		KEY_DELETE,
		KEY_DUPLICATE,
		KEY_COPY,
		KEY_PASTE,
		KEY_GRAB,
		KEY_SCALE,
		KEY_SNAP_TO_CURRENT,
		KEY_SNAP_TO_FRAME,
		KEY_CURRENT_TO_SELECTED,
		KEY_JUMP_NEXT,
		KEY_JUMP_PREVIOUS,
		// Interpolation, easing and handles, which T, Ctrl+E and V open under the mouse.
		INTERPOLATION_CONSTANT,
		INTERPOLATION_LINEAR,
		INTERPOLATION_BEZIER,
		EASING_LINEAR,
		EASING_IN,
		EASING_OUT,
		EASING_IN_OUT,
		EASING_OUT_IN,
		HANDLE_AUTO_CLAMPED,
		HANDLE_VECTOR,
		HANDLE_ALIGNED,
		HANDLE_FREE,
		HANDLE_MIRRORED,
	};

	// A whole track's keys, for putting a track back as it was, or as an edit leaves it.
	struct TrackKey {
		double time = 0.0;
		Variant value;
		real_t transition = 1.0;
		Vector2 in_handle;
		Vector2 out_handle;
		int handle_mode = 0;
	};

private:
	// One curve: a number of a track, such as the X of a position track.
	struct Channel {
		int track = -1;
		int component = 0;
		int group = -1;
		String label;
		// Path, track type and number: what hiding and locking a curve are kept by.
		String id;
		Color color;
		bool rotation = false; // Shown as Euler degrees made from the track's quaternions.
		EulerOrder euler_order = EulerOrder::YXZ;
	};

	// The curves of one node, or of one bone.
	struct Group {
		String key;
		String label;
		Ref<Texture2D> icon;
		LocalVector<int> channels;
	};

	// A row of the channel list: a group's header, or one of its curves.
	struct Row {
		int group = -1;
		int channel = -1;
		float y = 0.0f;
	};

	// A track's curves sampled across the view, all its numbers at once, kept until the track, the
	// zoom or the scroll changes; drawing then only maps them to pixels.
	struct TrackSamples {
		LocalVector<double> times;
		LocalVector<double> values[4];
		bool valid = false;
	};

	// What a track's curves share: how many numbers it has, and for rotations their Euler order.
	struct TrackInfo {
		int components = 0;
		bool rotation = false;
		EulerOrder euler_order = EulerOrder::YXZ;
	};

	// A key on a curve as drawn, with the handles of a Bezier track's key.
	struct CurveKey {
		int index = 0;
		double time = 0.0;
		double value = 0.0;
		Vector2 in_handle;
		Vector2 out_handle;
	};

	// A key of a curve, kept by its time since keys change places as they move.
	struct Selected {
		int track = 0;
		int component = 0;
		double time = 0.0;

		bool operator<(const Selected &p_other) const {
			if (track != p_other.track) {
				return track < p_other.track;
			}
			if (component != p_other.component) {
				return component < p_other.component;
			}
			return time < p_other.time;
		}
		bool operator==(const Selected &p_other) const {
			return track == p_other.track && component == p_other.component && Math::is_equal_approx(time, p_other.time);
		}
	};

	// A key being moved or scaled, and where it was.
	struct EditItem {
		Selected key;
		int channel = -1;
		int key_index = -1;
		double shown = 0.0;
	};

	struct ClipboardKey {
		int track = 0;
		Animation::TrackType type = Animation::TYPE_VALUE;
		double time = 0.0;
		TrackKey key;
	};

	enum Mode {
		MODE_NONE,
		MODE_BOX_SELECT,
		MODE_GRAB,
		MODE_SCALE,
		MODE_HANDLE,
		MODE_PAN,
	};

	enum Axis {
		AXIS_BOTH,
		AXIS_TIME,
		AXIS_VALUE,
	};

	enum SidebarField {
		FIELD_TIME,
		FIELD_VALUE,
		FIELD_TRANSITION,
		FIELD_IN_TIME,
		FIELD_IN_VALUE,
		FIELD_OUT_TIME,
		FIELD_OUT_VALUE,
		FIELD_INTERPOLATION,
		FIELD_EASING,
		FIELD_HANDLE_MODE,
	};

	AnimationTrackEditor *track_editor = nullptr;
	AnimationTimelineEdit *timeline = nullptr;
	Ref<Animation> animation;
	Node *root = nullptr;
	bool read_only = false;
	float play_position = 0.0f;

	HBoxContainer *header = nullptr;
	MenuButton *view_menu = nullptr;
	MenuButton *select_menu = nullptr;
	MenuButton *channel_menu = nullptr;
	MenuButton *key_menu = nullptr;
	Button *only_selected_button = nullptr;
	Button *normalize_button = nullptr;
	Button *handles_button = nullptr;
	Button *sidebar_button = nullptr;
	Label *status = nullptr;
	String status_message;
	uint64_t status_message_until = 0;
	PopupMenu *interpolation_menu = nullptr;
	PopupMenu *easing_menu = nullptr;
	PopupMenu *handle_menu = nullptr;
	PopupMenu *snap_menu = nullptr;

	Control *view = nullptr;
	// The playhead, drawn on its own so playing doesn't redraw the curves.
	Control *playhead_overlay = nullptr;
	Ref<Texture2D> key_texture;
	Ref<Texture2D> _get_key_texture();

	// The N sidebar: the active keyframe.
	PanelContainer *sidebar = nullptr;
	Label *sidebar_channel = nullptr;
	Control *sidebar_fields = nullptr;
	SpinBox *sidebar_time = nullptr;
	SpinBox *sidebar_value = nullptr;
	Control *sidebar_curve = nullptr;
	OptionButton *sidebar_interpolation = nullptr;
	OptionButton *sidebar_easing = nullptr;
	SpinBox *sidebar_transition = nullptr;
	Control *sidebar_bezier = nullptr;
	SpinBox *sidebar_in_time = nullptr;
	SpinBox *sidebar_in_value = nullptr;
	SpinBox *sidebar_out_time = nullptr;
	SpinBox *sidebar_out_value = nullptr;
	OptionButton *sidebar_handle_mode = nullptr;
	bool updating_sidebar = false;

	LocalVector<Channel> channels;
	LocalVector<Group> groups;
	LocalVector<Row> rows;
	uint32_t channels_hash = 0;
	HashSet<String> collapsed_groups;
	// By path, track type and number, so they survive tracks being added and removed.
	HashSet<String> hidden_channels;
	HashSet<String> locked_channels;
	HashSet<int> selected_channels;
	float channel_scroll = 0.0f;
	float channel_scroll_max = 0.0f;

	// What the view shows: the value at its middle, and value units per pixel.
	double value_center = 0.0;
	double value_per_pixel = 0.01;
	bool framed_once = false;

	RBSet<Selected> selection;
	Selected active;
	bool has_active = false;

	// Keys as drawn, by curve, worked out again only when the animation changes.
	LocalVector<LocalVector<CurveKey>> curve_keys;
	// Each curve's value range, for Normalize.
	LocalVector<Vector2> channel_ranges;
	bool keys_dirty = true;
	HashMap<int, TrackInfo> track_info;
	HashMap<int, TrackSamples> track_samples;
	uint32_t samples_view_hash = 0;
	// While this editor writes a track itself, the animation's change signal leaves the other
	// tracks' samples be.
	int writing_track = -1;

	Mode mode = MODE_NONE;
	Axis axis = AXIS_BOTH;
	bool box_pending = false;
	bool press_on_key = false;
	bool press_on_empty = false;
	bool dragging_with_button = false;
	Vector2 drag_from;
	Vector2 drag_to;
	Vector2 scale_pivot;
	// The handle being dragged: its key, and -1 for the in handle, 1 for the out one.
	Selected handle_key;
	int handle_side = 0;
	// Tracks as they were when the edit began, the keys it moves, and where they end up.
	HashMap<int, LocalVector<TrackKey>> edit_original;
	LocalVector<EditItem> edit_items;
	LocalVector<Selected> edit_new_selection;
	bool edit_changed = false;
	bool edit_duplicating = false;
	// A move by exact amounts, for move_selection(), rather than by the mouse.
	bool edit_exact = false;
	Vector2 edit_exact_offset;

	LocalVector<ClipboardKey> clipboard;

	// Layout.
	float _row_height() const;
	float _list_width() const;
	float _time_to_x(double p_time) const;
	double _x_to_time(float p_x) const;
	float _value_to_y(int p_channel, double p_value) const;
	double _y_to_value(int p_channel, float p_y) const;
	double _value_per_pixel_for(int p_channel) const;
	String _channel_id(int p_channel) const;
	bool _is_channel_shown(int p_channel) const;
	bool _is_channel_locked(int p_channel) const;
	int _channel_of(int p_track, int p_component) const;

	// Channels.
	void _update_channels(bool p_force = false);
	static int _component_count(const Variant &p_value);
	static double _get_component(const Variant &p_value, int p_component);
	static Variant _set_component(const Variant &p_value, int p_component, double p_component_value);
	EulerOrder _get_euler_order(int p_track) const;

	// Values.
	bool _evaluate(int p_channel, double p_time, double &r_value) const;
	void _evaluate_track(int p_track, double p_time, double r_values[4]) const;
	void _sample_track(int p_track, TrackSamples &r_samples);
	const TrackSamples &_get_samples(int p_track);
	void _on_animation_changed();
	void _cache_curves();
	double _key_raw_value(int p_channel, int p_key) const;
	Variant _key_with_value(int p_channel, const Variant &p_key_value, double p_delta) const;
	int _find_key(const Selected &p_selected) const;
	bool _value_at_playhead(int p_track, Variant &r_value) const;

	// Editing.
	LocalVector<TrackKey> _read_track(int p_track) const;
	void _write_track(int p_track, const LocalVector<TrackKey> &p_keys);
	void _set_track_keys(const Ref<Animation> &p_animation, int p_track, const Array &p_keys);
	Array _track_keys_to_array(const LocalVector<TrackKey> &p_keys) const;
	void _add_track_ops(EditorUndoRedoManager *p_undo_redo, const HashMap<int, LocalVector<TrackKey>> &p_before, const HashMap<int, LocalVector<TrackKey>> &p_after);
	void _commit_tracks(const String &p_action, const HashMap<int, LocalVector<TrackKey>> &p_before, const HashMap<int, LocalVector<TrackKey>> &p_after);
	static void _auto_handles(LocalVector<TrackKey> &r_keys, int p_index);
	static void _vector_handles(LocalVector<TrackKey> &r_keys, int p_index);
	void _begin_edit(Mode p_mode);
	void _update_edit(const Vector2 &p_mouse);
	void _finish_edit(bool p_confirm);
	void _drag_handle(const Vector2 &p_mouse);
	void _delete_selection();
	void _duplicate_selection();
	void _copy_selection();
	void _paste();
	void _insert_keys();
	void _insert_key_at(int p_channel, double p_time, double p_value);
	HashSet<int> _selected_tracks() const;
	void _set_interpolation(int p_option);
	void _set_easing(int p_option);
	void _set_handle_mode(int p_option);
	void _snap(int p_option);
	void _jump(bool p_next);
	void _convert_to_bezier();

	// Selection.
	void _layout_rows();
	bool _key_at(const Vector2 &p_pos, Selected &r_key) const;
	bool _handle_at(const Vector2 &p_pos, Selected &r_key, int &r_side) const;
	int _channel_at(const Vector2 &p_pos) const;
	int _curve_at(const Vector2 &p_pos) const;
	void _select_all(bool p_select);
	void _select_invert();
	void _select_columns();
	void _select_side(bool p_after);
	void _box_select(const Rect2 &p_rect, bool p_add, bool p_remove);
	void _set_active(const Selected &p_key);
	void _prune_selection();

	// View.
	void _frame(bool p_selected_only, bool p_fit_time);
	void _menu_option(int p_option);
	void _popup_at_mouse(PopupMenu *p_menu);
	void _set_status_message(const String &p_message);
	void _update_status();
	void _update_menus();
	void _toggle_sidebar(bool p_shown);
	void _update_sidebar();
	void _sidebar_changed(double p_value, int p_field);
	void _sidebar_option_changed(int p_index, int p_field);

	void _draw_view();
	void _draw_playhead();
	void _draw_channel_list();
	void _view_gui_input(const Ref<InputEvent> &p_event);
	bool _handle_shortcut(const Ref<InputEvent> &p_event);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_track_editor(AnimationTrackEditor *p_editor);
	void set_timeline(AnimationTimelineEdit *p_timeline);
	void set_animation(const Ref<Animation> &p_animation, bool p_read_only);
	void set_root(Node *p_root);
	void set_play_position(float p_position);
	void update_view();
	void animation_changed();
	void frame_all();
	void grab_view_focus();

	// For the timeline's Edit menu, and the editor's own checks.
	void delete_selection();
	void duplicate_selection();
	void copy_selection(bool p_cut);
	void paste();
	PackedStringArray get_channel_labels() const;
	int get_selected_key_count() const;
	void select_channel_keys(int p_channel);
	void move_selection(double p_time_offset, double p_value_offset);
	void apply_menu_option(int p_option);
	Vector2 get_key_point(int p_channel, int p_key);
	void send_view_input(const Ref<InputEvent> &p_event);

	AsosdotGraphEditor();
};
