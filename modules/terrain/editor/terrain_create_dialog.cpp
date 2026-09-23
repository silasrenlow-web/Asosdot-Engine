/**************************************************************************/
/*  terrain_create_dialog.cpp                                             */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_create_dialog.h"

#include "terrain_brush.h"
#include "terrain_heightmap_io.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/spin_box.h"

namespace {

SpinBox *make_spin(double p_min, double p_max, double p_step, double p_value, const String &p_suffix = String()) {
	SpinBox *spin = memnew(SpinBox);
	spin->set_min(p_min);
	spin->set_max(p_max);
	spin->set_step(p_step);
	spin->set_value(p_value);
	spin->set_allow_greater(true);
	spin->set_suffix(p_suffix);
	spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	return spin;
}

} // namespace

Control *TerrainCreateDialog::_add_row(GridContainer *p_grid, const String &p_label, Control *p_control, LocalVector<Control *> *p_group) {
	Label *label = memnew(Label(p_label));
	p_grid->add_child(label);
	p_grid->add_child(p_control);
	if (p_group) {
		p_group->push_back(label);
		p_group->push_back(p_control);
	}
	return p_control;
}

void TerrainCreateDialog::_source_changed(int p_index) {
	for (Control *c : size_rows) {
		c->set_visible(p_index != SOURCE_FILE);
	}
	for (Control *c : noise_rows) {
		c->set_visible(p_index == SOURCE_NOISE);
	}
	for (Control *c : file_rows) {
		c->set_visible(p_index == SOURCE_FILE);
	}
	reset_size();
}

void TerrainCreateDialog::_browse() {
	file_dialog->popup_file_dialog();
}

void TerrainCreateDialog::_file_selected(const String &p_path) {
	file_path->set_text(p_path);
	TerrainHeightmapIO::Loaded loaded;
	String error;
	if (TerrainHeightmapIO::load(p_path, loaded, error) != OK) {
		file_info->set_text(error);
		return;
	}
	const int w = loaded.image->get_width();
	const int h = loaded.image->get_height();
	if (loaded.normalized) {
		file_info->set_text(vformat(TTR("%d x %d, %d-bit. Values are mapped from 0-1 onto the height range below."), w, h, loaded.bits));
	} else {
		file_info->set_text(vformat(TTR("%d x %d, 32-bit float, already in metres (%.1f to %.1f m). The height range is ignored."), w, h, loaded.range.x, loaded.range.y));
	}
	reset_size();
}

void TerrainCreateDialog::_browse_data() {
	data_dialog->set_current_path(get_data_path());
	data_dialog->popup_file_dialog();
}

void TerrainCreateDialog::_data_path_selected(const String &p_path) {
	data_path->set_text(p_path);
}

String TerrainCreateDialog::get_data_path() const {
	String path = data_path->get_text().strip_edges();
	if (!path.ends_with(".res")) {
		path += ".terrain.res";
	}
	return path;
}

void TerrainCreateDialog::popup_for(Mode p_mode, const Ref<TerrainData> &p_existing, float p_spacing, const String &p_data_path) {
	mode = p_mode;
	data_path->set_text(p_data_path);
	set_title(mode == MODE_NEW ? TTR("New Terrain") : TTR("Import Heightmap"));
	set_ok_button_text(mode == MODE_NEW ? TTR("Create") : TTR("Import"));

	const bool has_data = p_existing.is_valid() && p_existing->get_region_count() > 0;
	keep_layers->set_visible(mode == MODE_NEW && p_existing.is_valid() && p_existing->get_layer_count() > 0);
	// Importing into an existing terrain keeps its layout; only a new one can
	// pick a region size.
	region_size->set_disabled(mode == MODE_IMPORT && has_data);
	if (p_existing.is_valid()) {
		for (int i = 0; i < region_size->get_item_count(); i++) {
			if (region_size->get_item_id(i) == p_existing->get_region_size()) {
				region_size->select(i);
			}
		}
	}
	spacing->set_value(p_spacing);
	if (mode == MODE_IMPORT) {
		source->select(SOURCE_FILE);
		_source_changed(SOURCE_FILE);
	}
	popup_centered(Size2(460, 0) * EDSCALE);
}

Ref<TerrainData> TerrainCreateDialog::build(const Ref<TerrainData> &p_existing, float &r_spacing, String &r_error) const {
	r_spacing = spacing->get_value();
	const int src = source->get_selected();

	Ref<TerrainData> tdata;
	if (mode == MODE_IMPORT && p_existing.is_valid()) {
		// A copy, so undo can swap the untouched original back in.
		tdata = p_existing->duplicate_data();
	} else {
		tdata.instantiate();
		tdata->set_region_size(region_size->get_selected_id());
		if (keep_layers->is_visible() && keep_layers->is_pressed()) {
			for (int i = 0; i < p_existing->get_layer_count(); i++) {
				tdata->add_layer(p_existing->get_layer(i));
			}
		}
	}

	const int rs = tdata->get_region_size();
	Ref<Image> heights;
	Vector2i offset;
	bool normalized = false;
	float lo = 0.0;
	float hi = 0.0;

	if (src == SOURCE_FILE) {
		TerrainHeightmapIO::Loaded loaded;
		if (TerrainHeightmapIO::load(file_path->get_text(), loaded, r_error) != OK) {
			return Ref<TerrainData>();
		}
		heights = loaded.image;
		normalized = loaded.normalized;
		lo = file_min->get_value();
		hi = file_max->get_value();

		// A 2^n + 1 map spends its last row and column closing off the edge,
		// which the terrain does by itself. Keeping them would open a whole
		// ring of regions for one line of vertices.
		const int w = heights->get_width();
		const int h = heights->get_height();
		const int cw = (w > rs && (w - 1) % rs == 0) ? w - 1 : w;
		const int ch = (h > rs && (h - 1) % rs == 0) ? h - 1 : h;
		if (cw != w || ch != h) {
			heights = heights->get_region(Rect2i(0, 0, cw, ch));
		}
		if (centered->is_pressed()) {
			offset = -Vector2i(cw, ch) / 2;
		}

		if (mode == MODE_NEW) {
			// Fill out any region the map only partly covers from its nearest
			// edge, rather than leaving a drop to zero at the border.
			const Vector2i r0(TerrainData::floor_div(offset.x, rs), TerrainData::floor_div(offset.y, rs));
			const Vector2i r1(TerrainData::floor_div(offset.x + cw - 1, rs), TerrainData::floor_div(offset.y + ch - 1, rs));
			const Rect2i aligned(r0 * rs, (r1 - r0 + Vector2i(1, 1)) * rs);
			if (aligned != Rect2i(offset, Size2i(cw, ch))) {
				Vector<uint8_t> bytes;
				bytes.resize(aligned.size.x * aligned.size.y * sizeof(float));
				float *dst = (float *)bytes.ptrw();
				const float *srcp = (const float *)heights->ptr();
				for (int z = 0; z < aligned.size.y; z++) {
					const int sz = CLAMP(aligned.position.y + z - offset.y, 0, ch - 1);
					for (int x = 0; x < aligned.size.x; x++) {
						const int sx = CLAMP(aligned.position.x + x - offset.x, 0, cw - 1);
						dst[z * aligned.size.x + x] = srcp[sz * cw + sx];
					}
				}
				heights = Image::create_from_data(aligned.size.x, aligned.size.y, false, Image::FORMAT_RF, bytes);
				offset = aligned.position;
			}
		}
	} else {
		// Generated terrain always fills whole regions.
		const int regions_x = MAX(1, (int)Math::ceil(width->get_value() / (r_spacing * rs)));
		const int regions_z = MAX(1, (int)Math::ceil(depth->get_value() / (r_spacing * rs)));
		const int vw = regions_x * rs;
		const int vd = regions_z * rs;
		if ((int64_t)vw * vd > 64 * 1024 * 1024) {
			r_error = TTR("That terrain would have more than 64 million vertices. Increase the vertex spacing or make it smaller.");
			return Ref<TerrainData>();
		}
		if (centered->is_pressed()) {
			offset = -Vector2i(regions_x / 2, regions_z / 2) * rs;
		}
		Vector<uint8_t> bytes;
		bytes.resize(vw * vd * sizeof(float));
		float *h = (float *)bytes.ptrw();
		const float base = base_height->get_value();
		if (src == SOURCE_FLAT) {
			for (int i = 0; i < vw * vd; i++) {
				h[i] = base;
			}
		} else {
			const uint32_t seed = (uint32_t)noise_seed->get_value();
			const float feature = MAX((float)noise_size->get_value(), 0.1f);
			const float amplitude = noise_height->get_value();
			const int octaves = (int)noise_octaves->get_value();
			const bool ridged = noise_ridged->is_pressed();
			for (int z = 0; z < vd; z++) {
				for (int x = 0; x < vw; x++) {
					// Sampled in world metres, so the same seed gives the same
					// hills whatever the spacing.
					const float wx = (offset.x + x) * r_spacing;
					const float wz = (offset.y + z) * r_spacing;
					float sum = 0.0;
					float amp = 1.0;
					float freq = 1.0 / feature;
					float norm = 0.0;
					for (int o = 0; o < octaves; o++) {
						float n = TerrainBrush::fractal_noise(wx * freq, wz * freq, seed + o * 7919);
						if (ridged) {
							// Folded noise makes sharp crests: mountain ridges.
							n = 1.0f - Math::abs(n * 2.0f - 1.0f);
							n *= n;
						}
						sum += n * amp;
						norm += amp;
						amp *= 0.5f;
						freq *= 2.0f;
					}
					h[z * vw + x] = base + (sum / norm) * amplitude;
				}
			}
		}
		heights = Image::create_from_data(vw, vd, false, Image::FORMAT_RF, bytes);
	}

	const Error err = tdata->import_height_image(heights, offset, lo, hi, normalized);
	if (err != OK) {
		r_error = TTR("The heights could not be written into the terrain.");
		return Ref<TerrainData>();
	}
	return tdata;
}

TerrainCreateDialog::TerrainCreateDialog() {
	set_title(TTR("New Terrain"));
	VBoxContainer *vbox = memnew(VBoxContainer);
	add_child(vbox);

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	vbox->add_child(grid);

	source = memnew(OptionButton);
	source->add_item(TTR("Flat"), SOURCE_FLAT);
	source->add_item(TTR("Noise"), SOURCE_NOISE);
	source->add_item(TTR("Heightmap File"), SOURCE_FILE);
	source->connect(SceneStringName(item_selected), callable_mp(this, &TerrainCreateDialog::_source_changed));
	_add_row(grid, TTR("Heights:"), source);

	width = make_spin(8, 16384, 1, 512, "m");
	_add_row(grid, TTR("Width:"), width, &size_rows);
	depth = make_spin(8, 16384, 1, 512, "m");
	_add_row(grid, TTR("Depth:"), depth, &size_rows);
	spacing = make_spin(0.05, 64, 0.01, 1.0, "m");
	spacing->set_tooltip_text(TTR("Distance between neighbouring vertices. Smaller is more detailed and uses more memory: memory grows with the square of 1/spacing."));
	_add_row(grid, TTR("Vertex Spacing:"), spacing);
	region_size = memnew(OptionButton);
	for (int rsize = 64; rsize <= 1024; rsize *= 2) {
		region_size->add_item(vformat("%d", rsize), rsize);
	}
	region_size->select(2);
	region_size->set_tooltip_text(TTR("Vertices per region side. The terrain is stored and uploaded a region at a time, and each region has its own collision shape."));
	_add_row(grid, TTR("Region Size:"), region_size);
	base_height = make_spin(-10000, 10000, 0.01, 0.0, "m");
	base_height->set_allow_lesser(true);
	_add_row(grid, TTR("Base Height:"), base_height, &size_rows);

	noise_seed = make_spin(0, 999999, 1, 1);
	_add_row(grid, TTR("Seed:"), noise_seed, &noise_rows);
	noise_size = make_spin(1, 10000, 0.1, 180, "m");
	_add_row(grid, TTR("Feature Size:"), noise_size, &noise_rows);
	noise_height = make_spin(0, 10000, 0.1, 40, "m");
	_add_row(grid, TTR("Height:"), noise_height, &noise_rows);
	noise_octaves = make_spin(1, 8, 1, 4);
	_add_row(grid, TTR("Octaves:"), noise_octaves, &noise_rows);
	noise_ridged = memnew(CheckBox);
	noise_ridged->set_text(TTR("Ridged (mountains)"));
	_add_row(grid, TTR("Shape:"), noise_ridged, &noise_rows);

	HBoxContainer *file_row = memnew(HBoxContainer);
	file_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	file_path = memnew(LineEdit);
	file_path->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	file_path->set_placeholder(TTR("PNG (8 or 16-bit), EXR, HDR or R16"));
	file_path->connect(SceneStringName(text_submitted), callable_mp(this, &TerrainCreateDialog::_file_selected));
	file_row->add_child(file_path);
	Button *browse = memnew(Button);
	browse->set_text("...");
	browse->connect(SceneStringName(pressed), callable_mp(this, &TerrainCreateDialog::_browse));
	file_row->add_child(browse);
	_add_row(grid, TTR("File:"), file_row, &file_rows);
	file_min = make_spin(-10000, 10000, 0.01, 0.0, "m");
	file_min->set_allow_lesser(true);
	_add_row(grid, TTR("Lowest Height:"), file_min, &file_rows);
	file_max = make_spin(-10000, 10000, 0.01, 100.0, "m");
	file_max->set_allow_lesser(true);
	_add_row(grid, TTR("Highest Height:"), file_max, &file_rows);

	HBoxContainer *data_row = memnew(HBoxContainer);
	data_row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	data_path = memnew(LineEdit);
	data_path->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	data_path->set_tooltip_text(TTR("The terrain is saved to this compressed binary file, not inside the scene, so the scene stays small and quick to load."));
	data_row->add_child(data_path);
	Button *data_browse = memnew(Button);
	data_browse->set_text("...");
	data_row->add_child(data_browse);
	_add_row(grid, TTR("Data File:"), data_row);

	centered = memnew(CheckBox);
	centered->set_text(TTR("Centre on the node"));
	centered->set_pressed(true);
	vbox->add_child(centered);
	keep_layers = memnew(CheckBox);
	keep_layers->set_text(TTR("Keep the current layers"));
	keep_layers->set_pressed(true);
	vbox->add_child(keep_layers);

	file_info = memnew(Label);
	file_info->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	file_info->set_custom_minimum_size(Size2(420, 0) * EDSCALE);
	vbox->add_child(file_info);
	file_rows.push_back(file_info);

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	file_dialog->add_filter("*.png", TTR("PNG (8 or 16-bit)"));
	file_dialog->add_filter("*.exr", TTR("OpenEXR"));
	file_dialog->add_filter("*.hdr", TTR("Radiance HDR"));
	file_dialog->add_filter("*.r16,*.raw", TTR("Raw 16-bit"));
	file_dialog->add_filter("*.jpg,*.jpeg,*.webp,*.tga,*.bmp", TTR("8-bit images"));
	file_dialog->connect("file_selected", callable_mp(this, &TerrainCreateDialog::_file_selected));
	add_child(file_dialog);

	data_dialog = memnew(EditorFileDialog);
	data_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	data_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	data_dialog->add_filter("*.res", TTR("Binary Resource"));
	data_dialog->connect("file_selected", callable_mp(this, &TerrainCreateDialog::_data_path_selected));
	add_child(data_dialog);
	data_browse->connect(SceneStringName(pressed), callable_mp(this, &TerrainCreateDialog::_browse_data));

	_source_changed(SOURCE_FLAT);
}
