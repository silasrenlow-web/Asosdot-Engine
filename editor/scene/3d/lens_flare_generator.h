/**************************************************************************/
/*  lens_flare_generator.h                                                */
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

#pragma once

#include "scene/gui/dialogs.h"

class CheckBox;
class ColorPickerButton;
class EditorSpinSlider;
class Label;
class Light3D;
class OptionButton;

// Builds a LensFlare3D on a selected 3D light. The light is what makes this
// worth a button: its type already says how the flare should behave, so a
// DirectionalLight3D gets a flare at infinity, a SpotLight3D one that only shows
// from inside its cone, and both inherit the light's colour and rotation without
// the user matching anything by hand.
class AsosdotLensFlareGeneratorDialog : public ConfirmationDialog {
	GDCLASS(AsosdotLensFlareGeneratorDialog, ConfirmationDialog);

public:
	enum Preset {
		PRESET_BUNDLED,
		PRESET_PROCEDURAL,
		PRESET_EMPTY,
	};

private:
	ObjectID light_id;

	Label *target_label = nullptr;
	OptionButton *preset_option = nullptr;
	CheckBox *match_light_color = nullptr;
	ColorPickerButton *color_picker = nullptr;
	EditorSpinSlider *intensity_slider = nullptr;
	EditorSpinSlider *scale_slider = nullptr;
	CheckBox *occlusion_check = nullptr;

	Light3D *_get_light() const;
	void _match_light_color_toggled(bool p_pressed);
	void _sync_to_light();

protected:
	void _notification(int p_what);

public:
	// Returns the one selected 3D light, or null when the selection is anything
	// else. Shared with Node3DEditor so the button's enabled state and the
	// dialog's target can never disagree.
	static Light3D *get_selected_light();

	void popup_for_selection();
	virtual void ok_pressed() override;

	AsosdotLensFlareGeneratorDialog();
};
