/**************************************************************************/
/*  lens_flare_generator.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "lens_flare_generator.h"

#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/lens_flare_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/check_box.h"
#include "scene/gui/color_picker.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"

Light3D *AsosdotLensFlareGeneratorDialog::get_selected_light() {
	// Called while Node3DEditor is being built to set the button's initial state,
	// which happens from EditorNode's own constructor, before its singleton is
	// assigned. Nothing is selected that early anyway.
	EditorNode *editor = EditorNode::get_singleton();
	if (!editor) {
		return nullptr;
	}

	EditorSelection *selection = editor->get_editor_selection();
	if (!selection) {
		return nullptr;
	}

	// One light exactly. Generating onto several at once would have to guess
	// whether they should share a LensFlare resource or get one each, and
	// getting that wrong is worse than making the user pick them off one by one.
	List<Node *> nodes = selection->get_top_selected_node_list();
	if (nodes.size() != 1) {
		return nullptr;
	}

	return Object::cast_to<Light3D>(nodes.front()->get());
}

Light3D *AsosdotLensFlareGeneratorDialog::_get_light() const {
	return Object::cast_to<Light3D>(ObjectDB::get_instance(light_id));
}

void AsosdotLensFlareGeneratorDialog::_match_light_color_toggled(bool p_pressed) {
	color_picker->set_disabled(p_pressed);
	if (p_pressed) {
		_sync_to_light();
	}
}

void AsosdotLensFlareGeneratorDialog::_sync_to_light() {
	Light3D *light = _get_light();
	if (light && match_light_color->is_pressed()) {
		Color color = light->get_color();
		color.a = 1.0;
		color_picker->set_pick_color(color);
	}
}

void AsosdotLensFlareGeneratorDialog::popup_for_selection() {
	Light3D *light = get_selected_light();
	light_id = light ? light->get_instance_id() : ObjectID();

	if (light) {
		target_label->set_text(vformat(TTR("Generating on %s (%s)."), light->get_name(), light->get_class()));
		get_ok_button()->set_disabled(false);
	} else {
		target_label->set_text(TTR("Select a single DirectionalLight3D, OmniLight3D or SpotLight3D first."));
		get_ok_button()->set_disabled(true);
	}

	_sync_to_light();
	// The target label's text changes length between openings, so shrink back to
	// the content before centring rather than keeping whatever size the last
	// popup settled on.
	reset_size();
	popup_centered_clamped(Size2(460, 0) * EDSCALE, 0.8);
}

void AsosdotLensFlareGeneratorDialog::ok_pressed() {
	Light3D *light = _get_light();
	ERR_FAIL_NULL(light);

	Node *owner = EditorNode::get_singleton()->get_edited_scene();
	ERR_FAIL_NULL(owner);

	LensFlare3D *flare = memnew(LensFlare3D);
	flare->set_name("LensFlare3D");

	switch ((Preset)preset_option->get_selected_id()) {
		case PRESET_PROCEDURAL: {
			flare->set_flare(LensFlare::create_procedural());
		} break;
		case PRESET_EMPTY: {
			Ref<LensFlare> empty;
			empty.instantiate();
			flare->set_flare(empty);
		} break;
		case PRESET_BUNDLED: {
			// Left null on purpose: a LensFlare3D with no resource already draws
			// the bundled default, so this keeps the scene file free of eight
			// sub-resources the user has not edited yet.
		} break;
	}

	Color color = color_picker->get_pick_color();
	if (match_light_color->is_pressed()) {
		color = light->get_color();
		color.a = 1.0;
	}
	flare->set_color(color);
	flare->set_intensity(intensity_slider->get_value());
	flare->set_flare_scale(scale_slider->get_value());
	flare->set_occlusion_enabled(occlusion_check->is_pressed());

	// The light's type is the whole reason this is worth automating.
	if (Object::cast_to<DirectionalLight3D>(light)) {
		// A sun. Sits at infinity and takes its direction from the parent's
		// rotation, so an identity local transform aims it correctly.
		flare->set_directional(true);
	} else if (SpotLight3D *spot = Object::cast_to<SpotLight3D>(light)) {
		// Only flare from inside the cone the spot actually lights, with a
		// little margin so the flare is not clipped exactly at the edge.
		const float half_angle = Math::deg_to_rad(spot->get_param(Light3D::PARAM_SPOT_ANGLE));
		flare->set_angle_limit(MIN(half_angle * 1.1f, (float)Math::PI));
		flare->set_fade_distance_begin(spot->get_param(Light3D::PARAM_RANGE) * 0.6);
		flare->set_fade_distance_end(spot->get_param(Light3D::PARAM_RANGE));
	} else if (OmniLight3D *omni = Object::cast_to<OmniLight3D>(light)) {
		// Fade out over the light's own reach; past it there is nothing bright
		// enough to be throwing a flare.
		flare->set_fade_distance_begin(omni->get_param(Light3D::PARAM_RANGE) * 0.6);
		flare->set_fade_distance_end(omni->get_param(Light3D::PARAM_RANGE));
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Generate Lens Flare"), UndoRedo::MERGE_DISABLE, light);
	ur->add_do_method(light, "add_child", flare, true);
	ur->add_do_method(flare, "set_owner", owner);
	ur->add_do_method(Node3DEditor::get_singleton(), SceneStringName(_request_gizmo), flare);
	ur->add_do_reference(flare);
	ur->add_undo_method(light, "remove_child", flare);
	ur->commit_action();
}

void AsosdotLensFlareGeneratorDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			target_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("readonly_color"), EditorStringName(Editor)));
		} break;
	}
}

AsosdotLensFlareGeneratorDialog::AsosdotLensFlareGeneratorDialog() {
	set_title(TTRC("Generate Lens Flare"));
	set_ok_button_text(TTRC("Generate"));

	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	target_label = memnew(Label);
	target_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	// An autowrapping label with no width to wrap against reports its minimum
	// height as one word per line, which drags the whole dialog to full screen
	// height. Giving it a width to wrap at is what keeps the dialog compact.
	target_label->set_custom_minimum_size(Size2(420, 0) * EDSCALE);
	vb->add_child(target_label);

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	vb->add_child(grid);

	grid->add_child(memnew(Label(TTRC("Elements"))));
	preset_option = memnew(OptionButton);
	preset_option->add_item(TTRC("Default (bundled textures)"), PRESET_BUNDLED);
	preset_option->add_item(TTRC("Procedural shapes"), PRESET_PROCEDURAL);
	preset_option->add_item(TTRC("Empty"), PRESET_EMPTY);
	preset_option->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	preset_option->set_tooltip_text(TTRC("Which elements the new flare starts with. Procedural shapes cost no texture memory and stay sharp at any resolution; the bundled textures look photographed."));
	grid->add_child(preset_option);

	grid->add_child(memnew(Label(TTRC("Colour"))));
	HBoxContainer *color_hb = memnew(HBoxContainer);
	color_hb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	grid->add_child(color_hb);

	color_picker = memnew(ColorPickerButton);
	color_picker->set_edit_alpha(false);
	color_picker->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	color_picker->set_custom_minimum_size(Size2(64, 0) * EDSCALE);
	color_hb->add_child(color_picker);

	match_light_color = memnew(CheckBox);
	match_light_color->set_text(TTRC("From light"));
	match_light_color->set_pressed(true);
	match_light_color->set_tooltip_text(TTRC("Take the colour from the light being generated on."));
	match_light_color->connect(SceneStringName(toggled), callable_mp(this, &AsosdotLensFlareGeneratorDialog::_match_light_color_toggled));
	color_hb->add_child(match_light_color);
	color_picker->set_disabled(true);

	grid->add_child(memnew(Label(TTRC("Intensity"))));
	intensity_slider = memnew(EditorSpinSlider);
	intensity_slider->set_min(0.0);
	intensity_slider->set_max(8.0);
	intensity_slider->set_step(0.01);
	intensity_slider->set_allow_greater(true);
	intensity_slider->set_value(1.0);
	intensity_slider->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	intensity_slider->set_tooltip_text(TTRC("Above 1.0 the flare is bright enough for glow to pick it up."));
	grid->add_child(intensity_slider);

	grid->add_child(memnew(Label(TTRC("Scale"))));
	scale_slider = memnew(EditorSpinSlider);
	scale_slider->set_min(0.0);
	scale_slider->set_max(4.0);
	scale_slider->set_step(0.01);
	scale_slider->set_allow_greater(true);
	scale_slider->set_value(1.0);
	scale_slider->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	grid->add_child(scale_slider);

	occlusion_check = memnew(CheckBox);
	occlusion_check->set_text(TTRC("Fade behind geometry"));
	occlusion_check->set_pressed(true);
	occlusion_check->set_tooltip_text(TTRC("Sample the depth buffer so the flare fades out when something moves in front of the light."));
	vb->add_child(occlusion_check);
}
