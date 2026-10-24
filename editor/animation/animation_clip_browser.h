/**************************************************************************/
/*  animation_clip_browser.h                                              */
/**************************************************************************/
/*  Asosdot: every clip of an AnimationPlayer, searchable and grouped.    */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/gui/popup.h"

class AnimationPlayer;
class Button;
class Label;
class LineEdit;
class Tree;
class TreeItem;

// Every clip of an AnimationPlayer in one searchable list. Clips whose names start
// with the same word ("rifle_fire", "rifle_reload") are grouped, after any word that
// every clip starts with, such as Blender's "Armature|". Selected clips can be given
// a loop mode together, with one undo.
class AsosdotAnimationClipBrowser : public PopupPanel {
	GDCLASS(AsosdotAnimationClipBrowser, PopupPanel);

	ObjectID player_id;
	String current_clip;
	String first_match;
	LineEdit *search = nullptr;
	Tree *tree = nullptr;
	Label *status = nullptr;
	Button *loop_buttons[3] = {};
	// Groups the user folded, by library and word. Kept while the editor runs.
	HashSet<String> folded;
	HashMap<TreeItem *, String> group_keys;

	AnimationPlayer *_get_player() const;
	void _update_tree();
	void _choose(TreeItem *p_item);
	void _item_activated();
	void _item_collapsed(TreeItem *p_item);
	void _search_changed(const String &p_text);
	void _search_submitted(const String &p_text);
	void _search_input(const Ref<InputEvent> &p_event);
	Vector<String> _get_selected_clips() const;
	void _selection_changed();
	void _set_loop_mode(int p_mode);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void popup_for(AnimationPlayer *p_player, const String &p_current_clip, const Rect2i &p_below);

	AsosdotAnimationClipBrowser();
};
