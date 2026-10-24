/**************************************************************************/
/*  render_editor_layout.cpp                                              */
/**************************************************************************/
/*  Asosdot: the Render workspace layout.                                 */
/**************************************************************************/

#include "render_editor_plugin.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_string_names.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/aspect_ratio_container.h"
#include "scene/gui/button.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/range.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/subviewport_container.h"
#include "scene/gui/tab_bar.h"
#include "scene/gui/text_edit.h"
#include "scene/main/viewport.h"
#include "scene/resources/font.h"
#include "scene/resources/style_box_flat.h"

namespace {

// Track labels stay in place while the sequence fills the remaining width.
constexpr float TRACK_LABEL_WIDTH = 84.0;
constexpr float RULER_HEIGHT = 22.0;
constexpr float SHOT_HEIGHT = 40.0;
constexpr float KEY_HEIGHT = 18.0;
constexpr float AUDIO_HEIGHT = 24.0;
// How close a click on the key lane must be to a key for it to land on that key.
constexpr float KEY_SNAP_DISTANCE = 6.0;

Button *tool_button(const String &p_name, const String &p_tooltip, bool p_toggle = false) {
	Button *button = memnew(Button);
	button->set_name(p_name);
	button->set_accessibility_name(p_name);
	button->set_tooltip_text(p_tooltip);
	button->set_theme_type_variation(SceneStringName(FlatButton));
	button->set_toggle_mode(p_toggle);
	button->set_focus_mode(Control::FOCUS_ALL);
	return button;
}

Label *section_label(const String &p_text) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	label->set_clip_text(true);
	label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	return label;
}

VBoxContainer *panel_content(PanelContainer *p_panel) {
	VBoxContainer *box = memnew(VBoxContainer);
	box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	box->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	p_panel->add_child(box);
	return box;
}

} // namespace

String AsosdotRenderEditor::_camera_label(const Ref<AsosdotRenderShot> &p_shot) {
	const NodePath camera_path = p_shot->get_value("camera");
	return camera_path.is_empty() ? TTR("Studio Camera") : String(camera_path.get_name(camera_path.get_name_count() - 1));
}

void AsosdotRenderEditor::_build_workspace() {
	add_theme_constant_override("separation", 6 * EDSCALE);
	workspace_toolbar = memnew(HBoxContainer);
	workspace_toolbar->set_name("WorkspaceToolbar");
	add_child(workspace_toolbar);
	Label *title = section_label(TTR("Render Workspace"));
	title->set_name("WorkspaceTitle");
	title->set_h_size_flags(Control::SIZE_SHRINK_BEGIN);
	workspace_toolbar->add_child(title);
	workspace_focus_button = tool_button("FocusWorkspace", TTR("Hide the editor's outside docks while working in Render. Leaving Render restores the previous editor layout."), true);
	workspace_focus_button->set_text(TTR("Focus Workspace"));
	workspace_focus_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotRenderEditor::_set_workspace_focus));
	workspace_toolbar->add_child(workspace_focus_button);
	reset_layout_button = tool_button("ResetLayout", TTR("Restore the Render panels to their default sizes and show the preview, properties and sequence."));
	reset_layout_button->set_text(TTR("Reset Layout"));
	reset_layout_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_reset_workspace_layout));
	workspace_toolbar->add_child(reset_layout_button);
	// The output details and the render actions sit together at the right.
	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	workspace_toolbar->add_child(toolbar_spacer);
	size_label = memnew(Label);
	size_label->set_name("OutputSize");
	size_label->set_modulate(Color(1, 1, 1, 0.7));
	size_label->set_tooltip_text(TTR("The output size and frame rate of the sequence, set under Output / Music."));
	workspace_toolbar->add_child(size_label);
	render_image_button = tool_button("RenderImage", TTR("Render the current frame to a PNG, JPEG or WebP image."));
	render_image_button->set_text(TTR("Render Image"));
	render_image_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_render_image_pressed));
	workspace_toolbar->add_child(render_image_button);
	render_animation_button = memnew(Button);
	render_animation_button->set_name("RenderSequence");
	render_animation_button->set_text(TTR("Render Sequence"));
	render_animation_button->set_tooltip_text(TTR("Render all shots with their soundtrack to AVI or OGV, or to numbered PNG frames."));
	render_animation_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_render_animation_pressed));
	workspace_toolbar->add_child(render_animation_button);

	workspace_split = memnew(VSplitContainer);
	workspace_split->set_name("WorkspaceSplit");
	workspace_split->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(workspace_split);
	preview_split = memnew(HSplitContainer);
	preview_split->set_name("PreviewSplit");
	preview_split->set_v_size_flags(SIZE_EXPAND_FILL);
	workspace_split->add_child(preview_split);

	preview_panel = memnew(PanelContainer);
	preview_panel->set_name("PreviewPanel");
	preview_panel->set_h_size_flags(SIZE_EXPAND_FILL);
	preview_panel->set_custom_minimum_size(Size2(160, 0) * EDSCALE);
	preview_panel->connect(SceneStringName(resized), callable_mp(this, &AsosdotRenderEditor::_update_workspace_layout));
	preview_split->add_child(preview_panel);
	VBoxContainer *view = panel_content(preview_panel);
	HBoxContainer *view_tools = memnew(HBoxContainer);
	view->add_child(view_tools);
	preview_title = section_label(TTR("Camera Preview"));
	view_tools->add_child(preview_title);
	frame_button = tool_button("FrameSubject", TTR("Frame Subject: fit the whole scene in the studio camera."));
	frame_button->set_text(TTR("Frame Subject"));
	frame_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_frame_subject));
	view_tools->add_child(frame_button);
	pose_button = tool_button("Pose", TTR("Pose bones and parts in the preview and key the pose into a clip."), true);
	pose_button->set_text(TTR("Pose"));
	pose_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotRenderEditor::_set_pose_mode));
	view_tools->add_child(pose_button);
	guides_button = tool_button("CompositionGuides", TTR("Show a rule-of-thirds grid and a 10% safe margin in the preview. Guides are excluded from rendered images."), true);
	guides_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotRenderEditor::_set_composition_guides));
	view_tools->add_child(guides_button);
	preview_focus_button = tool_button("MaximizePreview", TTR("Maximize the camera preview. Toggle again to restore the properties and sequence panels."), true);
	preview_focus_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotRenderEditor::_set_preview_focus));
	view_tools->add_child(preview_focus_button);

	// The picture and, while posing, a shelf of pose tools beside it. The shelf
	// is one column of icons, so posing takes width from the preview, not height.
	HBoxContainer *body = memnew(HBoxContainer);
	body->set_name("PreviewBody");
	body->set_v_size_flags(SIZE_EXPAND_FILL);
	view->add_child(body);
	pose_scroll = memnew(ScrollContainer);
	pose_scroll->set_name("PoseToolsScroll");
	pose_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	pose_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
	pose_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	pose_scroll->hide();
	body->add_child(pose_scroll);
	pose_bar = memnew(VBoxContainer);
	pose_bar->set_name("PoseTools");
	pose_scroll->add_child(pose_bar);
	const char *tool_names[POSE_TOOL_MAX] = { TTRC("Rotate"), TTRC("Move"), TTRC("Reach") };
	const char *tool_tips[POSE_TOOL_MAX] = {
		TTRC("Rotate (R). Shift+drag rotates freely; Alt+drag twists about the bone's length."),
		TTRC("Move (G). Drag to slide the selection in the view."),
		TTRC("Reach (E). Drag a hand or foot and its parent bones bend to follow."),
	};
	Ref<ButtonGroup> tool_group;
	tool_group.instantiate();
	for (int i = 0; i < POSE_TOOL_MAX; i++) {
		pose_tool_buttons[i] = tool_button(TTRGET(tool_names[i]), TTRGET(tool_tips[i]), true);
		pose_tool_buttons[i]->set_button_group(tool_group);
		pose_tool_buttons[i]->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_set_pose_tool).bind(i));
		pose_bar->add_child(pose_tool_buttons[i]);
	}
	pose_tool_buttons[POSE_TOOL_ROTATE]->set_pressed(true);
	pose_bar->add_child(memnew(HSeparator));
	parent_button = tool_button("ParentBone", TTR("Select the parent bone or node (Up)."));
	parent_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_select_parent));
	pose_bar->add_child(parent_button);
	reset_pose_button = tool_button("ResetPose", TTR("Reset the selected bone to its rest pose. Shift+click resets the whole skeleton."));
	reset_pose_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_reset_pose_pressed));
	pose_bar->add_child(reset_pose_button);
	pose_bar->add_child(memnew(HSeparator));
	new_clip_button = tool_button("NewAnimation", TTR("Create a new animation clip in the scene."));
	new_clip_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_new_clip_pressed));
	pose_bar->add_child(new_clip_button);
	auto_key_button = tool_button("AutoKey", TTR("Auto Key: record every pose at the current time."), true);
	auto_key_button->connect(SceneStringName(toggled), callable_mp(this, &AsosdotRenderEditor::_set_auto_key));
	pose_bar->add_child(auto_key_button);
	key_button = tool_button("KeyPose", TTR("Key the pose and selection (I)."));
	key_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_key).bind(KEY_POSED));
	pose_bar->add_child(key_button);
	key_menu = memnew(MenuButton);
	key_menu->set_flat(true);
	key_menu->set_tooltip_text(TTR("More keyframe operations: key the whole skeleton (Shift+I) or delete keys here (Alt+I)."));
	key_menu->get_popup()->add_item(TTR("Key Whole Skeleton"), KEY_SKELETON, KeyModifierMask::SHIFT | Key::I);
	key_menu->get_popup()->add_item(TTR("Delete Keys Here"), KEY_DELETE, KeyModifierMask::ALT | Key::I);
	key_menu->get_popup()->connect(SceneStringName(id_pressed), callable_mp(this, &AsosdotRenderEditor::_key));
	pose_bar->add_child(key_menu);

	aspect = memnew(AspectRatioContainer);
	aspect->set_h_size_flags(SIZE_EXPAND_FILL);
	aspect->set_v_size_flags(SIZE_EXPAND_FILL);
	aspect->set_custom_minimum_size(Size2(0, 120) * EDSCALE);
	aspect->set_stretch_mode(AspectRatioContainer::STRETCH_FIT);
	body->add_child(aspect);
	preview_holder = memnew(Control);
	preview_holder->set_clip_contents(true);
	preview_holder->connect(SceneStringName(draw), callable_mp(this, &AsosdotRenderEditor::_draw_checker));
	aspect->add_child(preview_holder);
	preview_container = memnew(SubViewportContainer);
	preview_container->set_stretch(true);
	preview_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_holder->add_child(preview_container);
	preview = memnew(SubViewport);
	preview->set_name("RenderPreview");
	preview->set_disable_input(true);
	preview->set_update_mode(SubViewport::UPDATE_DISABLED);
	preview->set_msaa_3d(Viewport::MSAA_4X);
	preview->set_positional_shadow_atlas_size(4096);
	preview_container->add_child(preview);
	preview_camera = memnew(Camera3D);
	preview_camera->set_name("RenderCamera");
	preview_camera->set_cull_mask((1 << 20) - 1);
	preview->add_child(preview_camera);
	preview_camera->make_current();
	preview_surface = memnew(Control);
	preview_surface->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	preview_surface->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	preview_surface->set_focus_mode(Control::FOCUS_CLICK);
	preview_surface->connect(SceneStringName(gui_input), callable_mp(this, &AsosdotRenderEditor::_preview_input));
	preview_surface->connect(SceneStringName(draw), callable_mp(this, &AsosdotRenderEditor::_draw_composition_guides));
	preview_surface->connect(SceneStringName(draw), callable_mp(this, &AsosdotRenderEditor::_draw_pose_overlay));
	preview_holder->add_child(preview_surface);

	properties_panel = memnew(PanelContainer);
	properties_panel->set_name("PropertiesPanel");
	properties_panel->set_custom_minimum_size(Size2(300, 0) * EDSCALE);
	preview_split->add_child(properties_panel);
	VBoxContainer *properties = panel_content(properties_panel);
	properties_title = section_label(TTR("Shot Properties"));
	properties->add_child(properties_title);
	settings_selector = memnew(TabBar);
	settings_selector->set_name("PropertiesTabs");
	settings_selector->add_tab(TTR("Shot"));
	settings_selector->add_tab(TTR("Output / Music"));
	settings_selector->set_tab_alignment(TabBar::ALIGNMENT_LEFT);
	settings_selector->connect("tab_changed", callable_mp(this, &AsosdotRenderEditor::_settings_selected));
	properties->add_child(settings_selector);
	inspector = memnew(EditorInspector);
	inspector->set_v_size_flags(SIZE_EXPAND_FILL);
	inspector->set_hide_script(true);
	inspector->set_hide_metadata(true);
	inspector->set_mark_unsaved(false);
	inspector->set_use_folding(true);
	properties->add_child(inspector);
	inspector->edit(shot.ptr());

	sequence_split = memnew(HSplitContainer);
	sequence_split->set_name("SequenceSplit");
	sequence_dock = sequence_split;
	workspace_split->add_child(sequence_split);
	shots_panel = memnew(PanelContainer);
	shots_panel->set_name("ShotsPanel");
	shots_panel->set_custom_minimum_size(Size2(190, 0) * EDSCALE);
	sequence_split->add_child(shots_panel);
	VBoxContainer *shot_box = panel_content(shots_panel);
	HBoxContainer *shot_header = memnew(HBoxContainer);
	shot_header->add_child(section_label(TTR("Shots")));
	shot_count_label = memnew(Label);
	shot_count_label->set_modulate(Color(1, 1, 1, 0.6));
	shot_count_label->set_tooltip_text(TTR("The number of shots in the sequence."));
	shot_header->add_child(shot_count_label);
	shot_box->add_child(shot_header);
	HBoxContainer *shot_tools = memnew(HBoxContainer);
	shot_box->add_child(shot_tools);
	add_shot_button = tool_button("AddShot", TTR("Add a shot with default settings and the current studio camera view."));
	add_shot_button->set_text(TTR("Add"));
	add_shot_button->set_h_size_flags(SIZE_EXPAND_FILL);
	add_shot_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_add_shot).bind(false));
	shot_tools->add_child(add_shot_button);
	duplicate_shot_button = tool_button("Duplicate", TTR("Duplicate the selected shot, including its camera, animation and dialogue."));
	duplicate_shot_button->set_text(TTR("Duplicate"));
	duplicate_shot_button->set_h_size_flags(SIZE_EXPAND_FILL);
	duplicate_shot_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_add_shot).bind(true));
	shot_tools->add_child(duplicate_shot_button);
	shot_list = memnew(ItemList);
	shot_list->set_name("ShotList");
	shot_list->set_v_size_flags(SIZE_EXPAND_FILL);
	shot_list->set_custom_minimum_size(Size2(0, 60) * EDSCALE);
	shot_list->connect("item_selected", callable_mp(this, &AsosdotRenderEditor::_select_shot));
	shot_box->add_child(shot_list);
	HBoxContainer *ordering = memnew(HBoxContainer);
	shot_box->add_child(ordering);
	move_shot_up_button = tool_button("Up", TTR("Move the selected shot earlier in the sequence."));
	move_shot_up_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_move_shot).bind(-1));
	ordering->add_child(move_shot_up_button);
	move_shot_down_button = tool_button("Down", TTR("Move the selected shot later in the sequence."));
	move_shot_down_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_move_shot).bind(1));
	ordering->add_child(move_shot_down_button);
	Control *ordering_spacer = memnew(Control);
	ordering_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	ordering->add_child(ordering_spacer);
	remove_shot_button = tool_button("Remove", TTR("Remove the selected shot. The sequence must keep at least one shot."));
	remove_shot_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_remove_shot));
	ordering->add_child(remove_shot_button);

	sequence_panel = memnew(PanelContainer);
	sequence_panel->set_name("SequencePanel");
	sequence_panel->set_h_size_flags(SIZE_EXPAND_FILL);
	sequence_split->add_child(sequence_panel);
	VBoxContainer *sequence_box = panel_content(sequence_panel);
	HBoxContainer *sequence_header = memnew(HBoxContainer);
	sequence_header->add_child(section_label(TTR("Sequence")));
	sequence_summary = memnew(Label);
	sequence_summary->set_h_size_flags(SIZE_EXPAND_FILL);
	sequence_summary->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	sequence_summary->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	sequence_summary->set_modulate(Color(1, 1, 1, 0.7));
	sequence_header->add_child(sequence_summary);
	sequence_box->add_child(sequence_header);
	HBoxContainer *transport = memnew(HBoxContainer);
	sequence_box->add_child(transport);
	sequence_start_button = tool_button("SequenceStart", TTR("Go to the start of the sequence (Home)."));
	sequence_start_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_jump_sequence_boundary).bind(false));
	transport->add_child(sequence_start_button);
	previous_frame_button = tool_button("PreviousFrame", TTR("Step back one output frame (Left). Shift+Left steps back one second."));
	previous_frame_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_step_sequence_frame).bind(-1));
	transport->add_child(previous_frame_button);
	play_button = tool_button("PlaySequence", TTR("Play or pause the complete sequence (Space)."));
	play_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_play_pressed));
	transport->add_child(play_button);
	next_frame_button = tool_button("NextFrame", TTR("Step forward one output frame (Right). Shift+Right steps forward one second."));
	next_frame_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_step_sequence_frame).bind(1));
	transport->add_child(next_frame_button);
	sequence_end_button = tool_button("SequenceEnd", TTR("Go to the last output frame of the sequence (End)."));
	sequence_end_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_jump_sequence_boundary).bind(true));
	transport->add_child(sequence_end_button);
	Control *transport_spacer = memnew(Control);
	transport_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	transport->add_child(transport_spacer);
	previous_key_button = tool_button("PreviousKey", TTR("Go to the previous animation key of the selected shot (Ctrl+Left)."));
	previous_key_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_jump_to_key).bind(false));
	transport->add_child(previous_key_button);
	time_label = memnew(Label);
	time_label->set_name("SequenceTimecode");
	time_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	transport->add_child(time_label);
	next_key_button = tool_button("NextKey", TTR("Go to the next animation key of the selected shot (Ctrl+Right)."));
	next_key_button->connect(SceneStringName(pressed), callable_mp(this, &AsosdotRenderEditor::_jump_to_key).bind(true));
	transport->add_child(next_key_button);

	// One timeline: shots, the animation keys of the selected shot, dialogue and music.
	sequence_strip = memnew(Control);
	sequence_strip->set_name("SequenceTimeline");
	sequence_strip->set_custom_minimum_size(Size2(180, RULER_HEIGHT + SHOT_HEIGHT + KEY_HEIGHT + AUDIO_HEIGHT * 2 + 4) * EDSCALE);
	sequence_strip->set_v_size_flags(SIZE_EXPAND_FILL);
	sequence_strip->set_clip_contents(true);
	sequence_strip->set_mouse_filter(Control::MOUSE_FILTER_STOP);
	sequence_strip->set_tooltip_text(TTR("Click or drag to scrub. Shots, keys, dialogue and music have their own lanes, and stripes mark camera blends. A click on a key lands exactly on it. Space plays, Left and Right step frames, and [ and ] change shot."));
	sequence_strip->connect(SceneStringName(draw), callable_mp(this, &AsosdotRenderEditor::_draw_sequence));
	sequence_strip->connect(SceneStringName(gui_input), callable_mp(this, &AsosdotRenderEditor::_sequence_input));
	sequence_box->add_child(sequence_strip);

	// The status line: the selected bone and the clip its keys go to, while
	// posing, then the hints for the preview.
	status_bar = memnew(HBoxContainer);
	status_bar->set_name("StatusBar");
	status_bar->add_theme_constant_override("separation", 12 * EDSCALE);
	add_child(status_bar);
	// The bone and clip keep their full text. The hints beside them trim instead.
	pose_label = memnew(Label);
	pose_label->set_name("PoseSelection");
	pose_label->set_modulate(Color(1, 1, 1, 0.85));
	pose_label->hide();
	status_bar->add_child(pose_label);
	clip_label = memnew(Label);
	clip_label->set_name("PoseClip");
	clip_label->set_modulate(Color(1, 1, 1, 0.65));
	clip_label->hide();
	status_bar->add_child(clip_label);
	hint_label = section_label(String());
	hint_label->set_modulate(Color(1, 1, 1, 0.65));
	status_bar->add_child(hint_label);
}

void AsosdotRenderEditor::_update_workspace_theme() {
	if (!status_bar) {
		return;
	}
	const Color background = get_theme_color(SNAME("dark_color_1"), EditorStringName(Editor));
	const Color border = get_theme_color(SNAME("dark_color_3"), EditorStringName(Editor));
	for (PanelContainer *panel : { preview_panel, properties_panel, shots_panel, sequence_panel }) {
		Ref<StyleBoxFlat> style;
		style.instantiate();
		style->set_bg_color(background);
		style->set_border_color(border);
		style->set_border_width_all(MAX(1, int(EDSCALE)));
		style->set_content_margin_all(6 * EDSCALE);
		style->set_corner_radius_all(3 * EDSCALE);
		panel->add_theme_style_override(SceneStringName(panel), style);
	}
	workspace_focus_button->set_button_icon(get_editor_theme_icon(SNAME("DistractionFree")));
	preview_focus_button->set_button_icon(get_editor_theme_icon(SNAME("ExpandTree")));
	reset_layout_button->set_button_icon(get_editor_theme_icon(SNAME("ContainerLayout")));
	guides_button->set_button_icon(get_editor_theme_icon(SNAME("Grid")));
	add_shot_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
	duplicate_shot_button->set_button_icon(get_editor_theme_icon(SNAME("Duplicate")));
	move_shot_up_button->set_button_icon(get_editor_theme_icon(SNAME("ArrowUp")));
	move_shot_down_button->set_button_icon(get_editor_theme_icon(SNAME("ArrowDown")));
	remove_shot_button->set_button_icon(get_editor_theme_icon(SNAME("Remove")));
	sequence_start_button->set_button_icon(get_editor_theme_icon(SNAME("BackStart")));
	sequence_end_button->set_button_icon(get_editor_theme_icon(SNAME("ForwardEnd")));
	previous_frame_button->set_button_icon(get_editor_theme_icon(SNAME("Back")));
	next_frame_button->set_button_icon(get_editor_theme_icon(SNAME("Forward")));
	_update_workspace_layout();
	sequence_strip->queue_redraw();
	preview_surface->queue_redraw();
}

void AsosdotRenderEditor::_update_workspace_layout() {
	if (!status_bar) {
		return;
	}
	compact_workspace = get_size().x < 880 * EDSCALE;
	workspace_focus_button->set_text(compact_workspace ? String() : TTR("Focus Workspace"));
	reset_layout_button->set_text(compact_workspace ? String() : TTR("Reset Layout"));
	render_image_button->set_text(compact_workspace ? String() : TTR("Render Image"));
	size_label->set_visible(!compact_workspace);
	const bool compact_preview = preview_panel->get_size().x < 620 * EDSCALE;
	frame_button->set_text(compact_preview ? String() : TTR("Frame Subject"));
	pose_button->set_text(compact_preview ? String() : TTR("Pose"));
}

void AsosdotRenderEditor::_update_workspace_labels() {
	if (!preview_title) {
		return;
	}
	const String name = shot->get_value("shot_name");
	const String camera_name = _camera_label(shot);
	preview_title->set_text(vformat(U"%02d · %s", selected_shot + 1, name));
	preview_title->set_tooltip_text(vformat(TTR("Camera Preview\nShot: %s\nCamera: %s"), name, camera_name));
	properties_title->set_text(settings_selector->get_current_tab() == 0 ? vformat(TTR(U"Shot %d · %s"), selected_shot + 1, name) : TTR("Sequence Output & Music"));
	shot_count_label->set_text(itos(shots.size()));
	sequence_summary->set_text(vformat(TTR(U"%s s · %d FPS"), String::num(_sequence_duration(), 2), sequence_output->get_value("fps")));
	sequence_summary->set_tooltip_text(vformat(TTR(U"%d shots · %d output frames"), shots.size(), int(Math::ceil(_sequence_duration() * int(sequence_output->get_value("fps")) - 0.00001))));
}

void AsosdotRenderEditor::_set_workspace_focus(bool p_enabled) {
	workspace_focus = p_enabled;
	workspace_focus_button->set_pressed_no_signal(p_enabled);
	if (!active) {
		return;
	}
	if (p_enabled && !owns_editor_focus) {
		editor_focus_before = EditorInterface::get_singleton()->is_distraction_free_mode_enabled();
		owns_editor_focus = true;
		EditorInterface::get_singleton()->set_distraction_free_mode(true);
	} else if (!p_enabled && owns_editor_focus) {
		EditorInterface::get_singleton()->set_distraction_free_mode(editor_focus_before);
		owns_editor_focus = false;
	}
}

void AsosdotRenderEditor::_set_preview_focus(bool p_enabled) {
	if (p_enabled == preview_focus) {
		preview_focus_button->set_pressed_no_signal(p_enabled);
		return;
	}
	if (p_enabled) {
		preview_split_before_focus = preview_split->get_split_offset();
		workspace_split_before_focus = workspace_split->get_split_offset();
	}
	preview_focus = p_enabled;
	preview_focus_button->set_pressed_no_signal(p_enabled);
	properties_panel->set_visible(!p_enabled);
	sequence_dock->set_visible(!p_enabled);
	if (!p_enabled) {
		preview_split->set_split_offset(preview_split_before_focus);
		workspace_split->set_split_offset(workspace_split_before_focus);
	}
}

void AsosdotRenderEditor::_set_composition_guides(bool p_enabled) {
	composition_guides = p_enabled;
	guides_button->set_pressed_no_signal(p_enabled);
	preview_surface->queue_redraw();
}

void AsosdotRenderEditor::_draw_composition_guides() {
	if (!composition_guides) {
		return;
	}
	const Size2 size = preview_surface->get_size();
	const Color line(1, 1, 1, 0.32);
	for (int i = 1; i < 3; i++) {
		const float fraction = i / 3.0f;
		preview_surface->draw_line(Point2(size.x * fraction, 0), Point2(size.x * fraction, size.y), line, EDSCALE);
		preview_surface->draw_line(Point2(0, size.y * fraction), Point2(size.x, size.y * fraction), line, EDSCALE);
	}
	preview_surface->draw_rect(Rect2(size * 0.1, size * 0.8), Color(1, 1, 1, 0.22), false, EDSCALE);
}

void AsosdotRenderEditor::_reset_workspace_layout() {
	_set_preview_focus(false);
	_set_workspace_focus(false);
	preview_split->set_split_offset(0);
	sequence_split->set_split_offset(0);
	workspace_split->set_split_offset(0);
	_update_workspace_layout();
}

void AsosdotRenderEditor::_step_sequence_frame(int p_direction) {
	if (job.active) {
		return;
	}
	const int fps = sequence_output->get_value("fps");
	const double frame = Math::round(sequence_time * fps) + p_direction;
	const double last = MAX(0.0, Math::ceil(_sequence_duration() * fps - 0.00001) - 1);
	_seek_sequence(CLAMP(frame, 0.0, last) / fps);
}

void AsosdotRenderEditor::_jump_sequence_boundary(bool p_end) {
	if (!job.active) {
		const int fps = sequence_output->get_value("fps");
		_seek_sequence(p_end ? MAX(0.0, Math::ceil(_sequence_duration() * fps - 0.00001) - 1) / fps : 0.0);
	}
}

bool AsosdotRenderEditor::_handle_transport_key(const Ref<InputEventKey> &p_key) {
	// Alt belongs to the editor. Ctrl (Cmd on macOS) turns the arrows into key jumps.
	if (p_key->is_alt_pressed()) {
		return false;
	}
	// Holding a key repeats frame steps and shot changes, but not play or the
	// jumps, which would otherwise flicker.
	const bool echo = p_key->is_echo();
	const bool shift = p_key->is_shift_pressed();
	const bool command = p_key->is_ctrl_pressed() || p_key->is_meta_pressed();
	const int fps = MAX(int(sequence_output->get_value("fps")), 1);
	const Key code = p_key->get_keycode();
	if (code == Key::LEFT || code == Key::RIGHT) {
		const bool forward = code == Key::RIGHT;
		if (command) {
			if (!echo) {
				_jump_to_key(forward);
			}
		} else {
			const int frames = shift ? fps : 1;
			_step_sequence_frame(forward ? frames : -frames);
		}
		return true;
	}
	// The other transport keys take no Ctrl or Shift, so Shift+[ still types a brace.
	if (command || shift) {
		return false;
	}
	bool handled = true;
	switch (code) {
		case Key::SPACE:
			if (!echo) {
				_play_pressed();
			}
			break;
		case Key::HOME:
			if (!echo) {
				_jump_sequence_boundary(false);
			}
			break;
		case Key::END:
			if (!echo) {
				_jump_sequence_boundary(true);
			}
			break;
		case Key::BRACKETLEFT:
			if (!echo) {
				_select_shot(selected_shot - 1);
			}
			break;
		case Key::BRACKETRIGHT:
			if (!echo) {
				_select_shot(selected_shot + 1);
			}
			break;
		default:
			handled = false;
	}
	return handled;
}

void AsosdotRenderEditor::input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || !active || job.active || pose_drag.kind != DRAG_NONE) {
		return;
	}
	// Text and number fields keep their keys, so typing a shot name or a value
	// never moves the playhead.
	Control *focus = get_viewport() ? get_viewport()->gui_get_focus_owner() : nullptr;
	if (Object::cast_to<LineEdit>(focus) || Object::cast_to<TextEdit>(focus) || Object::cast_to<Range>(focus)) {
		return;
	}
	if (_handle_transport_key(key)) {
		get_viewport()->set_input_as_handled();
	}
}

String AsosdotRenderEditor::_format_timecode(double p_time, int p_fps) {
	const int64_t frame = int64_t(Math::floor(MAX(p_time, 0.0) * p_fps + 0.00001));
	const int64_t seconds = frame / p_fps;
	return vformat("%s:%s:%s:%s", String::num_int64(seconds / 3600).pad_zeros(2), String::num_int64(seconds / 60 % 60).pad_zeros(2), String::num_int64(seconds % 60).pad_zeros(2), String::num_int64(frame % p_fps).pad_zeros(p_fps > 100 ? 3 : 2));
}

bool AsosdotRenderEditor::_key_time_near(float p_x, double &r_time) const {
	const float left = TRACK_LABEL_WIDTH * EDSCALE;
	const float width = MAX(sequence_strip->get_size().x - left, 1.0f);
	const double duration = _sequence_duration();
	const double shot_start = _shot_start(selected_shot);
	const float speed = MAX(float(shot->get_value("animation_speed")), 0.0001f);
	const float shot_duration = MAX(float(shot->get_value("duration")), 0.01f);
	float best = KEY_SNAP_DISTANCE * EDSCALE;
	bool found = false;
	for (double key_time : _get_key_times(false)) {
		const float at = (key_time - double(shot->get_value("animation_start"))) / speed;
		if (at < -0.0001f || at > shot_duration + 0.0001f) {
			continue;
		}
		const float x = left + width * CLAMP((shot_start + at) / duration, 0.0, 1.0);
		if (Math::abs(x - p_x) <= best) {
			best = Math::abs(x - p_x);
			r_time = MIN(shot_start + at, duration);
			found = true;
		}
	}
	return found;
}

void AsosdotRenderEditor::_draw_sequence() {
	const double duration = _sequence_duration();
	const Size2 size = sequence_strip->get_size();
	const float left = TRACK_LABEL_WIDTH * EDSCALE;
	const float width = MAX(size.x - left, 1.0f);
	const float ruler = RULER_HEIGHT * EDSCALE;
	const float shot_height = SHOT_HEIGHT * EDSCALE;
	const float key_height = KEY_HEIGHT * EDSCALE;
	const float audio_height = AUDIO_HEIGHT * EDSCALE;
	const Ref<Font> font = sequence_strip->get_theme_font(SNAME("font"), SNAME("Label"));
	const int font_size = sequence_strip->get_theme_font_size(SNAME("font_size"), SNAME("Label"));
	const int small_font = MAX(10, font_size - int(2 * EDSCALE));
	const Color foreground = get_theme_color(SNAME("font_color"), EditorStringName(Editor));
	const Color background = get_theme_color(SNAME("dark_color_2"), EditorStringName(Editor));
	const Color accent = get_theme_color(SNAME("accent_color"), EditorStringName(Editor));
	const Color border = get_theme_color(SNAME("dark_color_3"), EditorStringName(Editor));
	const float keys_y = ruler + shot_height;
	const float dialogue_y = keys_y + key_height + 2 * EDSCALE;
	const float music_y = dialogue_y + audio_height;
	sequence_strip->draw_rect(Rect2(Point2(), size), background);
	// Each lane's name sits in the left column, beside the lane.
	const char *labels[] = { TTRC("Shots"), TTRC("Keys"), TTRC("Dialogue"), TTRC("Music") };
	const float lane_tops[] = { ruler, keys_y, dialogue_y, music_y };
	const float lane_heights[] = { shot_height, key_height, audio_height, audio_height };
	for (int i = 0; i < 4; i++) {
		sequence_strip->draw_line(Point2(0, lane_tops[i]), Point2(size.x, lane_tops[i]), border);
		font->draw_string(sequence_strip->get_canvas_item(), Point2(7 * EDSCALE, lane_tops[i] + lane_heights[i] * 0.65), TTRGET(labels[i]), HORIZONTAL_ALIGNMENT_LEFT, left - 12 * EDSCALE, small_font, foreground * Color(1, 1, 1, 0.7));
	}
	sequence_strip->draw_line(Point2(left, 0), Point2(left, size.y), border);
	// A 1/2/5 ruler keeps labels readable for both tiny clips and long films.
	const double target_step = duration * (70 * EDSCALE) / width;
	const double magnitude = Math::pow(10.0, Math::floor(Math::log(MAX(target_step, 0.001)) / Math::log(10.0)));
	const double normalized = target_step / magnitude;
	const double step = magnitude * (normalized > 5 ? 10 : normalized > 2 ? 5
													: normalized > 1	  ? 2
																		  : 1);
	for (double at = 0; at < duration + step * 0.001; at += step) {
		const float x = left + at / duration * width;
		sequence_strip->draw_line(Point2(x, ruler - 4 * EDSCALE), Point2(x, music_y + audio_height), border * Color(1, 1, 1, 0.65));
		font->draw_string(sequence_strip->get_canvas_item(), Point2(x + 3 * EDSCALE, ruler - 7 * EDSCALE), String::num(at, step < 1 ? 2 : 0) + "s", HORIZONTAL_ALIGNMENT_LEFT, step / duration * width - 5 * EDSCALE, small_font, foreground * Color(1, 1, 1, 0.65));
	}
	double start = 0.0;
	for (int i = 0; i < shots.size(); i++) {
		const Ref<AsosdotRenderShot> &settings = shots[i].settings;
		const double length = settings->get_value("duration");
		const float x = left + start / duration * width;
		const float shot_width = length / duration * width;
		const Rect2 rect(x + 1, ruler + 2, MAX(shot_width - 2, 0.0f), shot_height - 3);
		sequence_strip->draw_rect(rect, background.lerp(accent, i == selected_shot ? 0.46 : 0.20));
		if (i == selected_shot) {
			sequence_strip->draw_rect(rect, accent, false);
		}
		if (i > 0) {
			const float blend_width = MIN(double(settings->get_value("camera_blend")), length) / duration * width;
			for (float hatch = x + 1; hatch < x + blend_width; hatch += 7 * EDSCALE) {
				sequence_strip->draw_line(Point2(hatch, rect.get_end().y), Point2(MIN(hatch + 6 * EDSCALE, x + blend_width), rect.position.y), Color(1, 1, 1, 0.12));
			}
		}
		if (shot_width > 40 * EDSCALE) {
			font->draw_string(sequence_strip->get_canvas_item(), Point2(x + 6 * EDSCALE, ruler + 17 * EDSCALE), vformat("%02d  %s", i + 1, settings->get_value("shot_name")), HORIZONTAL_ALIGNMENT_LEFT, shot_width - 12 * EDSCALE, font_size, foreground);
			font->draw_string(sequence_strip->get_canvas_item(), Point2(x + 6 * EDSCALE, ruler + 32 * EDSCALE), _camera_label(settings), HORIZONTAL_ALIGNMENT_LEFT, shot_width - 12 * EDSCALE, small_font, foreground * Color(1, 1, 1, 0.65));
		}
		const String dialogue = settings->get_value("dialogue");
		if (!dialogue.is_empty()) {
			const Rect2 audio_rect(x + 1, dialogue_y + 2, MAX(shot_width - 2, 0.0f), audio_height - 4);
			sequence_strip->draw_rect(audio_rect, background.lerp(Color(0.9, 0.6, 0.25), 0.45));
			font->draw_string(sequence_strip->get_canvas_item(), Point2(x + 5 * EDSCALE, dialogue_y + 16 * EDSCALE), dialogue.get_file().get_basename(), HORIZONTAL_ALIGNMENT_LEFT, MAX(shot_width - 10 * EDSCALE, 0.0f), small_font, foreground);
		}
		start += length;
	}
	// The selected shot's animation keys, as diamonds in their own lane. The
	// selected bone's keys are bright and every other key is faint.
	const float shot_duration = MAX(float(shot->get_value("duration")), 0.01f);
	const float speed = MAX(float(shot->get_value("animation_speed")), 0.0001f);
	const float key_y = keys_y + key_height * 0.5f;
	const float r = 3.5f * EDSCALE;
	const Color faint = foreground * Color(1, 1, 1, 0.45);
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1 && !pose_selected.is_set()) {
			break;
		}
		for (double key_time : _get_key_times(pass == 1)) {
			const float at = (key_time - double(shot->get_value("animation_start"))) / speed;
			if (at < -0.0001f || at > shot_duration + 0.0001f) {
				continue;
			}
			const float x = left + width * CLAMP((_shot_start(selected_shot) + at) / duration, 0.0, 1.0);
			const Vector<Point2> diamond = { Point2(x, key_y - r), Point2(x + r, key_y), Point2(x, key_y + r), Point2(x - r, key_y) };
			sequence_strip->draw_colored_polygon(diamond, pass == 1 ? accent : faint);
		}
	}
	const String music = sequence_output->get_value("music");
	if (!music.is_empty()) {
		const float x = left + CLAMP(double(sequence_output->get_value("music_start")) / duration, 0.0, 1.0) * width;
		const float music_width = MAX(size.x - x - 1, 0.0f);
		sequence_strip->draw_rect(Rect2(x, music_y + 2, music_width, audio_height - 4), background.lerp(Color(0.3, 0.75, 0.5), 0.45));
		font->draw_string(sequence_strip->get_canvas_item(), Point2(x + 5 * EDSCALE, music_y + 16 * EDSCALE), music.get_file().get_basename(), HORIZONTAL_ALIGNMENT_LEFT, MAX(music_width - 10 * EDSCALE, 0.0f), small_font, foreground);
	}
	const float playhead = left + CLAMP(sequence_time / duration, 0.0, 1.0) * width;
	sequence_strip->draw_line(Point2(playhead, 0), Point2(playhead, music_y + audio_height), foreground, 1.5 * EDSCALE);
	sequence_strip->draw_colored_polygon({ Point2(playhead - 4 * EDSCALE, 0), Point2(playhead + 4 * EDSCALE, 0), Point2(playhead, 6 * EDSCALE) }, accent);
}

void AsosdotRenderEditor::_sequence_input(const Ref<InputEvent> &p_event) {
	if (job.active) {
		return;
	}
	const Ref<InputEventMouseButton> button = p_event;
	const Ref<InputEventMouseMotion> motion = p_event;
	if ((button.is_valid() && button->is_pressed() && button->get_button_index() == MouseButton::LEFT) || (motion.is_valid() && motion->get_button_mask().has_flag(MouseButtonMask::LEFT))) {
		const Point2 position = button.is_valid() ? button->get_position() : motion->get_position();
		const float left = TRACK_LABEL_WIDTH * EDSCALE;
		if (position.x < left) {
			return;
		}
		const float width = MAX(sequence_strip->get_size().x - left, 1.0f);
		const int fps = sequence_output->get_value("fps");
		const float keys_top = (RULER_HEIGHT + SHOT_HEIGHT) * EDSCALE;
		const float keys_bottom = keys_top + KEY_HEIGHT * EDSCALE;
		double key_time = 0.0;
		if (position.y >= keys_top && position.y < keys_bottom && _key_time_near(position.x, key_time)) {
			_seek_sequence(key_time);
		} else {
			const double at = CLAMP((position.x - left) / width, 0.0f, 1.0f) * _sequence_duration();
			_seek_sequence(MIN(Math::round(at * fps) / fps, _sequence_duration()));
		}
		sequence_strip->accept_event();
	}
}
