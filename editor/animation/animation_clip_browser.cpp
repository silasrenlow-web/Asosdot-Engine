/**************************************************************************/
/*  animation_clip_browser.cpp                                            */
/**************************************************************************/
/*  Asosdot: every clip of an AnimationPlayer, searchable and grouped.    */
/**************************************************************************/

#include "animation_clip_browser.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/themes/editor_scale.h"
#include "scene/animation/animation_player.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/tree.h"
#include "scene/resources/animation.h"

namespace {

// Clip names are mostly a word, a separator and the rest: "rifle_fire", "Armature|Run".
int first_word_end(const String &p_name, int p_from) {
	for (int i = p_from; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if (c == '_' || c == '-' || c == ' ' || c == '|' || c == '.') {
			return i;
		}
	}
	return -1;
}

} // namespace

AnimationPlayer *AsosdotAnimationClipBrowser::_get_player() const {
	return ObjectDB::get_instance<AnimationPlayer>(player_id);
}

void AsosdotAnimationClipBrowser::_update_tree() {
	tree->clear();
	group_keys.clear();
	first_match = String();
	AnimationPlayer *player = _get_player();
	if (!player) {
		return;
	}
	const String filter = search->get_text().strip_edges();
	TreeItem *root = tree->create_item();
	TreeItem *current_item = nullptr;
	int shown = 0;
	int total = 0;

	LocalVector<StringName> libraries;
	player->get_animation_library_list(&libraries);
	const bool show_libraries = libraries.size() > 1 || (libraries.size() == 1 && libraries[0] != StringName());
	for (const StringName &library_name : libraries) {
		Ref<AnimationLibrary> library = player->get_animation_library(library_name);
		if (library.is_null()) {
			continue;
		}
		LocalVector<StringName> names;
		library->get_animation_list(&names);
		Vector<String> clips;
		for (const StringName &name : names) {
			clips.push_back(name);
		}
		clips.sort_custom<NaturalNoCaseComparator>();
		total += clips.size();

		// A word every clip starts with groups nothing, so group by the word after it.
		int skip = 0;
		if (clips.size() > 1) {
			const int end = first_word_end(clips[0], 0);
			if (end > 0) {
				const String shared = clips[0].substr(0, end + 1);
				bool everywhere = true;
				for (const String &clip : clips) {
					everywhere = everywhere && clip.begins_with(shared);
				}
				skip = everywhere ? end + 1 : 0;
			}
		}
		HashMap<String, int> word_counts;
		for (const String &clip : clips) {
			const int end = first_word_end(clip, skip);
			if (end > skip) {
				word_counts[clip.substr(skip, end - skip)] += 1;
			}
		}

		TreeItem *library_item = root;
		if (show_libraries) {
			library_item = tree->create_item(root);
			library_item->set_text(0, library_name == StringName() ? TTR("[Global]") : String(library_name));
			library_item->set_icon(0, get_editor_theme_icon(SNAME("AnimationLibrary")));
			library_item->set_selectable(0, false);
			const String key = String(library_name) + "/";
			group_keys.insert(library_item, key);
			library_item->set_collapsed(filter.is_empty() && folded.has(key));
		}
		HashMap<String, TreeItem *> groups;
		for (const String &clip : clips) {
			const String path = library_name == StringName() ? clip : String(library_name) + "/" + clip;
			if (!filter.is_empty() && !path.containsn(filter)) {
				continue;
			}
			TreeItem *group_parent = library_item;
			const int end = first_word_end(clip, skip);
			if (end > skip) {
				const String word = clip.substr(skip, end - skip);
				if (word_counts[word] >= 2) {
					if (!groups.has(word)) {
						TreeItem *group = tree->create_item(library_item);
						group->set_text(0, word);
						group->set_suffix(0, vformat(TTR("%d clips"), word_counts[word]));
						group->set_icon(0, get_editor_theme_icon(SNAME("Folder")));
						group->set_selectable(0, false);
						const String key = String(library_name) + "/" + word;
						group_keys.insert(group, key);
						// A search shows every match, folded or not.
						group->set_collapsed(filter.is_empty() && folded.has(key));
						groups.insert(word, group);
					}
					group_parent = groups[word];
				}
			}

			TreeItem *item = tree->create_item(group_parent);
			item->set_text(0, clip);
			item->set_metadata(0, path);
			Ref<Animation> animation = player->get_animation(path);
			if (animation.is_valid()) {
				String suffix = vformat(TTR("%s s"), String::num(animation->get_length(), 2));
				const bool read_only = EditorNode::get_singleton()->is_resource_read_only(animation);
				if (read_only) {
					suffix += String(U" · ") + TTR("imported");
				}
				if (path == player->get_autoplay()) {
					suffix += String(U" · ") + TTR("autoplay");
				}
				item->set_suffix(0, suffix);
				StringName icon = SNAME("Animation");
				if (animation->get_loop_mode() == Animation::LOOP_LINEAR) {
					icon = SNAME("Loop");
				} else if (animation->get_loop_mode() == Animation::LOOP_PINGPONG) {
					icon = SNAME("PingPongLoop");
				}
				item->set_icon(0, get_editor_theme_icon(icon));
				item->set_tooltip_text(0, read_only ? vformat(TTR("%s\nImported from a scene file, so its loop mode is set in the file's Advanced Import Settings."), path) : path);
			}
			if (first_match.is_empty()) {
				first_match = path;
			}
			if (path == current_clip) {
				current_item = item;
			}
			shown++;
		}
		if (show_libraries && !library_item->get_first_child()) {
			// Nothing in this library matches the search.
			group_keys.erase(library_item);
			memdelete(library_item);
		}
	}
	if (current_item) {
		current_item->select(0);
		tree->scroll_to_item(current_item, true);
	}
	status->set_text(filter.is_empty() ? vformat(TTR("%d clips. Double-click a clip to open it; Ctrl or Shift selects several."), total) : vformat(TTR("%d of %d clips match."), shown, total));
	_selection_changed();
}

void AsosdotAnimationClipBrowser::_choose(TreeItem *p_item) {
	if (!p_item || p_item->get_metadata(0).get_type() != Variant::STRING) {
		return;
	}
	const String path = p_item->get_metadata(0);
	hide();
	emit_signal(SNAME("clip_chosen"), path);
}

void AsosdotAnimationClipBrowser::_item_activated() {
	_choose(tree->get_selected());
}

void AsosdotAnimationClipBrowser::_item_collapsed(TreeItem *p_item) {
	const String *key = group_keys.getptr(p_item);
	if (!key || !search->get_text().strip_edges().is_empty()) {
		return;
	}
	if (p_item->is_collapsed()) {
		folded.insert(*key);
	} else {
		folded.erase(*key);
	}
}

void AsosdotAnimationClipBrowser::_search_changed(const String &p_text) {
	_update_tree();
}

void AsosdotAnimationClipBrowser::_search_submitted(const String &p_text) {
	if (!first_match.is_empty()) {
		hide();
		emit_signal(SNAME("clip_chosen"), first_match);
	}
}

void AsosdotAnimationClipBrowser::_search_input(const Ref<InputEvent> &p_event) {
	// Down moves from the search into the list, onto the first match.
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->get_keycode() != Key::DOWN) {
		return;
	}
	for (TreeItem *item = tree->get_root(); item; item = item->get_next_in_tree()) {
		if (item->get_metadata(0).get_type() == Variant::STRING) {
			tree->deselect_all();
			item->select(0);
			tree->scroll_to_item(item);
			tree->grab_focus();
			search->accept_event();
			return;
		}
	}
}

Vector<String> AsosdotAnimationClipBrowser::_get_selected_clips() const {
	Vector<String> clips;
	for (TreeItem *item = tree->get_next_selected(nullptr); item; item = tree->get_next_selected(item)) {
		if (item->get_metadata(0).get_type() == Variant::STRING) {
			clips.push_back(item->get_metadata(0));
		}
	}
	return clips;
}

void AsosdotAnimationClipBrowser::_selection_changed() {
	const bool any = !_get_selected_clips().is_empty();
	for (Button *button : loop_buttons) {
		button->set_disabled(!any);
	}
}

void AsosdotAnimationClipBrowser::_set_loop_mode(int p_mode) {
	AnimationPlayer *player = _get_player();
	if (!player) {
		return;
	}
	LocalVector<Ref<Animation>> changed;
	int read_only = 0;
	for (const String &clip : _get_selected_clips()) {
		Ref<Animation> animation = player->get_animation(clip);
		if (animation.is_null()) {
			continue;
		}
		if (EditorNode::get_singleton()->is_resource_read_only(animation)) {
			read_only++;
		} else if (animation->get_loop_mode() != p_mode) {
			changed.push_back(animation);
		}
	}
	if (!changed.is_empty()) {
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Set Loop Mode of Clips"));
		for (const Ref<Animation> &animation : changed) {
			undo_redo->add_do_method(animation.ptr(), "set_loop_mode", p_mode);
			undo_redo->add_undo_method(animation.ptr(), "set_loop_mode", animation->get_loop_mode());
		}
		undo_redo->add_do_method(this, "_update_tree");
		undo_redo->add_undo_method(this, "_update_tree");
		undo_redo->commit_action();
	}
	String message = vformat(TTR("Loop mode set on %d clips."), (int)changed.size());
	if (read_only > 0) {
		message += " " + vformat(TTR("%d imported clips were left as they are: set their loop mode in the scene file's Advanced Import Settings."), read_only);
	}
	status->set_text(message);
}

void AsosdotAnimationClipBrowser::popup_for(AnimationPlayer *p_player, const String &p_current_clip, const Rect2i &p_below) {
	player_id = p_player ? p_player->get_instance_id() : ObjectID();
	current_clip = p_current_clip;
	search->clear();
	_update_tree();
	const Size2i popup_size = Size2i(Size2(380, 460) * EDSCALE);
	// Like a dropdown: below the button when it fits, above it otherwise.
	Point2i popup_position = p_below.position + Vector2i(0, p_below.size.y);
	const Rect2i usable = get_usable_parent_rect();
	if (usable.has_area() && popup_position.y + popup_size.y > usable.get_end().y && p_below.position.y - popup_size.y >= usable.position.y) {
		popup_position.y = p_below.position.y - popup_size.y;
	}
	popup(Rect2i(popup_position, popup_size));
	search->call_deferred(SNAME("grab_focus"));
}

void AsosdotAnimationClipBrowser::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		search->set_right_icon(get_editor_theme_icon(SNAME("Search")));
		loop_buttons[Animation::LOOP_LINEAR]->set_button_icon(get_editor_theme_icon(SNAME("Loop")));
		loop_buttons[Animation::LOOP_PINGPONG]->set_button_icon(get_editor_theme_icon(SNAME("PingPongLoop")));
	}
}

void AsosdotAnimationClipBrowser::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_update_tree"), &AsosdotAnimationClipBrowser::_update_tree);
	ADD_SIGNAL(MethodInfo("clip_chosen", PropertyInfo(Variant::STRING, "clip")));
}

AsosdotAnimationClipBrowser::AsosdotAnimationClipBrowser() {
	VBoxContainer *box = memnew(VBoxContainer);
	add_child(box);
	search = memnew(LineEdit);
	search->set_placeholder(TTR("Filter Clips"));
	search->set_clear_button_enabled(true);
	search->set_accessibility_name(TTRC("Filter Clips"));
	search->connect(SceneStringName(text_changed), callable_mp(this, &AsosdotAnimationClipBrowser::_search_changed));
	search->connect(SceneStringName(text_submitted), callable_mp(this, &AsosdotAnimationClipBrowser::_search_submitted));
	search->connect(SceneStringName(gui_input), callable_mp(this, &AsosdotAnimationClipBrowser::_search_input));
	box->add_child(search);

	tree = memnew(Tree);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_MULTI);
	tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree->set_accessibility_name(TTRC("Clips"));
	tree->connect("item_activated", callable_mp(this, &AsosdotAnimationClipBrowser::_item_activated));
	tree->connect("item_collapsed", callable_mp(this, &AsosdotAnimationClipBrowser::_item_collapsed));
	tree->connect("multi_selected", callable_mp(this, &AsosdotAnimationClipBrowser::_selection_changed).unbind(3));
	box->add_child(tree);

	HBoxContainer *loop = memnew(HBoxContainer);
	box->add_child(loop);
	Label *loop_label = memnew(Label);
	loop_label->set_text(TTR("Loop Selected:"));
	loop->add_child(loop_label);
	const char *names[3] = { TTRC("None"), TTRC("Loop"), TTRC("Ping-Pong") };
	const char *tips[3] = {
		TTRC("Play the selected clips once."),
		TTRC("Loop the selected clips from the start."),
		TTRC("Loop the selected clips back and forth."),
	};
	for (int i = 0; i < 3; i++) {
		loop_buttons[i] = memnew(Button);
		loop_buttons[i]->set_text(TTRGET(names[i]));
		loop_buttons[i]->set_tooltip_text(TTRGET(tips[i]));
		loop_buttons[i]->set_theme_type_variation(SceneStringName(FlatButton));
		loop_buttons[i]->connect(SceneStringName(pressed), callable_mp(this, &AsosdotAnimationClipBrowser::_set_loop_mode).bind(i));
		loop->add_child(loop_buttons[i]);
	}

	status = memnew(Label);
	// A wrapping label sizes itself by its width, so it gets one, and a few lines at most.
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status->set_custom_minimum_size(Size2(340, 0) * EDSCALE);
	status->set_max_lines_visible(3);
	status->set_modulate(Color(1, 1, 1, 0.7));
	box->add_child(status);
}
