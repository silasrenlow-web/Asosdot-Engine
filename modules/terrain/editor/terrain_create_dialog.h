/**************************************************************************/
/*  terrain_create_dialog.h                                               */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "../terrain_data.h"

#include "scene/gui/dialogs.h"

class CheckBox;
class EditorFileDialog;
class GridContainer;
class Label;
class LineEdit;
class OptionButton;
class SpinBox;

// New Terrain and Import Heightmap share one dialog: a size, a spacing and a
// source for the heights, which is flat ground, generated noise or a file.
class AsosdotTerrainCreateDialog : public ConfirmationDialog {
	GDCLASS(AsosdotTerrainCreateDialog, ConfirmationDialog);

public:
	enum Mode {
		MODE_NEW,
		MODE_IMPORT,
	};

	enum Source {
		SOURCE_FLAT,
		SOURCE_NOISE,
		SOURCE_FILE,
	};

private:
	Mode mode = MODE_NEW;

	OptionButton *source = nullptr;
	SpinBox *width = nullptr;
	SpinBox *depth = nullptr;
	SpinBox *spacing = nullptr;
	OptionButton *region_size = nullptr;
	SpinBox *base_height = nullptr;
	CheckBox *centered = nullptr;
	CheckBox *keep_layers = nullptr;

	SpinBox *noise_seed = nullptr;
	SpinBox *noise_size = nullptr;
	SpinBox *noise_height = nullptr;
	SpinBox *noise_octaves = nullptr;
	CheckBox *noise_ridged = nullptr;

	LineEdit *file_path = nullptr;
	SpinBox *file_min = nullptr;
	SpinBox *file_max = nullptr;
	Label *file_info = nullptr;
	EditorFileDialog *file_dialog = nullptr;
	LineEdit *data_path = nullptr;
	EditorFileDialog *data_dialog = nullptr;

	LocalVector<Control *> size_rows;
	LocalVector<Control *> noise_rows;
	LocalVector<Control *> file_rows;

	Control *_add_row(GridContainer *p_grid, const String &p_label, Control *p_control, LocalVector<Control *> *p_group = nullptr);
	void _source_changed(int p_index);
	void _browse();
	void _file_selected(const String &p_path);
	void _data_path_selected(const String &p_path);
	void _browse_data();

public:
	void popup_for(Mode p_mode, const Ref<TerrainData> &p_existing, float p_spacing, const String &p_data_path);
	String get_data_path() const;

	// Builds the heights the dialog describes. For MODE_NEW that is a fresh
	// TerrainData; for MODE_IMPORT it is a copy of p_existing with the heights
	// written in, so the edit can be undone by swapping the old one back.
	Ref<TerrainData> build(const Ref<TerrainData> &p_existing, float &r_spacing, String &r_error) const;

	AsosdotTerrainCreateDialog();
};
