/**
 * Raven Designer - editors.c
 * Code editors in tabs: any text file of the project, the interfaces
 * (.xml, .xmlc...) included, whose preview is the island (island.c) until
 * the visual editor is asked for; and the <script> blocks of an .xmlc. Completion of the Raven API, of the
 * widgets of the interface (ui.name), of tags, attributes and @keys.
 */

#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RAVEN_IDE_INCLUDE_DIR
#define RAVEN_IDE_INCLUDE_DIR ""
#endif

typedef struct editor_t {
	Rid widget;
	char* path;      /* file shown, or NULL */
	char* key;       /* what finds it again: the path, or "file.xmlc#script1" */
	char* title;
	int modified;
	/* counts the changes of its text (the island rebuilds its preview when it moves) */
	unsigned revision;
	ide_save_callback save;
	void* user;
} editor_t;

static struct {
	editor_t* list;
	int count;
	int loading;
	/* the Raven API, read from its headers */
	char** api;
	char** api_detail;
	int api_count;
	int api_loaded;
	/* the code editor the keyboard was in last */
	Rid last;
} g_e;

/* ---------------------------------------------------------------------------
 *  Editors
 * ------------------------------------------------------------------------- */

static editor_t* by_widget(Rid widget) {
	int i;
	for (i = 0; i < g_e.count; i++) if (g_e.list[i].widget == widget) return &g_e.list[i];
	return NULL;
}

static void update_tab(editor_t* e) {
	char title[320];
	snprintf(title, sizeof(title), "%s%s", e->title, e->modified ? " *" : "");
	Rdock_area_set_panel_title(g_ide_dock, e->widget, title);
}

void editors_set_modified(Rid widget, int modified) {
	editor_t* e = by_widget(widget);
	if (!e || e->modified == modified) return;
	e->modified = modified;
	update_tab(e);
}

static void on_changed(Rid widget, void* user_data) {
	editor_t* e = by_widget(widget);
	(void)user_data;
	if (e) e->revision++;
	if (!g_e.loading) editors_set_modified(widget, R_TRUE);
}

static int save_editor(editor_t* e) {
	if (e->save) return e->save(e->widget, e->user);
	if (e->path) {
		const char* text = Rtext_editor_get_text(e->widget);
		if (!Rfile_write(e->path, text, strlen(text))) {
			ide_status("Cannot write %s", e->path);
			return R_FALSE;
		}
		editors_set_modified(e->widget, R_FALSE);
		ide_status("Saved %s", e->path);
		/* a table of translations, a CMakeLists.txt: what depends on it follows */
		if (ide_iequal(ide_extension(e->path), "csv")) strings_reload();
		return R_TRUE;
	}
	return R_FALSE;
}

static void remove_editor(editor_t* e) {
	int index = (int)(e - g_e.list);
	free(e->path);
	free(e->key);
	free(e->title);
	memmove(&g_e.list[index], &g_e.list[index + 1], sizeof(editor_t) * (size_t)(g_e.count - index - 1));
	g_e.count--;
}

static int on_close(Rid area, Rid panel, void* user_data) {
	editor_t* e = by_widget(panel);
	(void)area;
	(void)user_data;
	if (!e) return R_TRUE;
	if (e->modified) {
		char text[400];
		RMessageResult answer;
		snprintf(text, sizeof(text), "%s has changes that were not saved. Save them?", e->title);
		answer = Rmessage_box(g_ide_window, "Unsaved changes", text, R_MESSAGE_YES_NO_CANCEL);
		if (answer == R_MESSAGE_RESULT_CANCEL) return R_FALSE;
		if (answer == R_MESSAGE_RESULT_YES && !save_editor(e)) return R_FALSE;
	}
	remove_editor(e);
	return R_TRUE;
}

/*
 * Where the code editors go: in the tabs of the one shown last, else in those
 * of the preview, else (the preview is hidden: no interface in the visual
 * editor) above the Output.
 */
Rid editors_anchor(void) {
	int i;
	editors_forget_closed();
	if (g_e.last != R_INVALID_ID && Rwidget_is_valid(g_e.last) && by_widget(g_e.last) && Rdock_area_is_panel_open(g_ide_dock, g_e.last)) return g_e.last;
	for (i = g_e.count - 1; i >= 0; i--) {
		if (Rdock_area_is_panel_open(g_ide_dock, g_e.list[i].widget)) return g_e.list[i].widget;
	}
	if (g_ide_center != R_INVALID_ID && Rdock_area_is_panel_open(g_ide_dock, g_ide_center)) return g_ide_center;
	return R_INVALID_ID;
}

static void dock_editor(Rid widget, const char* title, Rid anchor) {
	if (anchor != R_INVALID_ID) Rdock_area_add_panel(g_ide_dock, widget, title, R_DOCK_TAB, anchor, 0.0f);
	else Rdock_area_add_panel(g_ide_dock, widget, title, R_DOCK_TOP, Rwidget_find(g_ide_window, "output"), 0.7f);
}

void editors_regroup(void) {
	Rid anchor = g_ide_center != R_INVALID_ID && Rdock_area_is_panel_open(g_ide_dock, g_ide_center) ? g_ide_center : R_INVALID_ID;
	int i;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) {
		Rid widget = g_e.list[i].widget;
		if (!Rdock_area_is_panel_open(g_ide_dock, widget)) continue;
		if (anchor == R_INVALID_ID) {
			Rdock_area_move_panel(g_ide_dock, widget, R_DOCK_TOP, Rwidget_find(g_ide_window, "output"), 0.7f);
			anchor = widget;
		} else {
			Rdock_area_move_panel(g_ide_dock, widget, R_DOCK_TAB, anchor, 0.0f);
		}
	}
	if (g_e.last != R_INVALID_ID && by_widget(g_e.last) && Rdock_area_is_panel_open(g_ide_dock, g_e.last)) Rdock_area_show_panel(g_ide_dock, g_e.last);
}

/* An interface (.xml, .xmlc...): XML, its scripts colored in their language, and the completion of tags and attributes. */
static int is_interface(const char* path) {
	return path && (ide_interface_language(path) || ide_iequal(ide_extension(path), "xml"));
}

static editor_t* create(const char* title, const char* key, const char* path, const char* text, const char* language) {
	editor_t* e;
	/* (where it goes, before it is one of the editors) */
	Rid anchor = editors_anchor();
	Rid widget = Rtext_editor_create(NULL);
	g_e.loading++;
	Rtext_editor_set_text(widget, text ? text : "");
	g_e.loading--;
	if (language && language[0]) Rtext_editor_set_language(widget, language);
	Rtext_editor_set_line_numbers(widget, R_TRUE);
	Rtext_editor_set_tab_size(widget, 4);
	Rtext_editor_set_changed_callback(widget, on_changed, NULL);
	completion_attach(widget, (language && ide_iequal(language, "XML")) || is_interface(path));
	editors_zoom_attach(widget);
	g_e.list = (editor_t*)realloc(g_e.list, sizeof(editor_t) * (size_t)(g_e.count + 1));
	e = &g_e.list[g_e.count++];
	memset(e, 0, sizeof(*e));
	e->widget = widget;
	e->path = path ? ide_strdup(path) : NULL;
	e->key = ide_strdup(key);
	e->title = ide_strdup(title);
	dock_editor(widget, title, anchor);
	Rdock_area_show_panel(g_ide_dock, widget);
	g_e.last = widget;
	return e;
}

/* ---------------------------------------------------------------------------
 *  Zoom: Ctrl + wheel (Ctrl + '+', '-', 0) in any editor zooms them all, and
 *  the next start comes back to it (ide.ini, editor_zoom).
 * ------------------------------------------------------------------------- */

static struct {
	float zoom;
	int loaded;
	/* the editors that are not code editors: the XML panel, the Output */
	Rid others[8];
	int other_count;
} g_z;

static float zoom_now(void) {
	if (!g_z.loaded) {
		float saved = (float)atof(prefs_get("editor_zoom"));
		g_z.zoom = saved >= 0.5f && saved <= 4.0f ? saved : 1.0f;
		g_z.loaded = R_TRUE;
	}
	return g_z.zoom;
}

/* Every editor of the IDE at one zoom (`from` already is: the user zoomed it). */
static void zoom_all(float zoom, Rid from) {
	int i;
	char text[16];
	g_z.zoom = zoom;
	g_z.loaded = R_TRUE;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) if (g_e.list[i].widget != from) Rtext_editor_set_zoom(g_e.list[i].widget, zoom);
	for (i = 0; i < g_z.other_count; i++) {
		if (g_z.others[i] != from && Rwidget_is_valid(g_z.others[i])) Rtext_editor_set_zoom(g_z.others[i], zoom);
	}
	snprintf(text, sizeof(text), "%.2f", zoom);
	prefs_set("editor_zoom", text);
}

static void on_zoom(Rid editor, float zoom, void* user_data) {
	(void)user_data;
	zoom_all(zoom, editor);
	ide_status("Text zoom: %d %%  (Ctrl + wheel, Ctrl + '+' / '-', Ctrl + 0: 100 %%)", (int)(zoom * 100.0f + 0.5f));
}

void editors_zoom_attach(Rid editor) {
	int i, code = by_widget(editor) != NULL;
	if (editor == R_INVALID_ID || !Rwidget_is_valid(editor)) return;
	Rtext_editor_set_zoom(editor, zoom_now());
	Rtext_editor_set_zoom_callback(editor, on_zoom, NULL);
	if (code) return;
	for (i = 0; i < g_z.other_count; i++) if (g_z.others[i] == editor) return;
	if (g_z.other_count < (int)(sizeof(g_z.others) / sizeof(g_z.others[0]))) g_z.others[g_z.other_count++] = editor;
}

void editors_reset_zoom(void) {
	zoom_all(1.0f, R_INVALID_ID);
	ide_status("Text zoom: 100 %%");
}

/*
 * A dock area closes a panel without asking when the program does it
 * (Rdock_area_close_panel: the scripts of an .xmlc that is closed): the
 * editors whose widget is gone are forgotten here, before anyone looks.
 */
void editors_forget_closed(void) {
	int i;
	for (i = g_e.count - 1; i >= 0; i--) {
		if (!Rwidget_is_valid(g_e.list[i].widget)) remove_editor(&g_e.list[i]);
	}
}

Rid editors_find(const char* key) {
	int i;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) if (ide_same_path(g_e.list[i].key, key)) return g_e.list[i].widget;
	return R_INVALID_ID;
}

/* A tab whose title contains a text (the scripts of the tests). */
Rid editors_find_title(const char* text) {
	int i;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) if (strstr(g_e.list[i].title, text)) return g_e.list[i].widget;
	return R_INVALID_ID;
}

void editors_close_all(void) {
	editors_forget_closed();
	while (g_e.count > 0) {
		Rid widget = g_e.list[g_e.count - 1].widget;
		remove_editor(&g_e.list[g_e.count - 1]);
		if (Rwidget_is_valid(widget)) Rdock_area_close_panel(g_ide_dock, widget);
	}
}

static void go_to_line(Rid widget, int line) {
	if (line <= 0) return;
	Rtext_editor_set_cursor(widget, line - 1, 0);
	Rtext_editor_scroll_to_line(widget, line - 1);
	Rwidget_focus(widget);
}

static const char* language_of(const char* path) {
	const char* name = ide_basename(path);
	const char* ext = ide_extension(path);
	if (ide_iequal(name, "CMakeLists.txt") || ide_iequal(ext, "cmake")) return "CMake";
	/* (an .xmlc, .xmlrs...: its scripts colored in their language, as in the XML panel) */
	if (ide_interface_language(path)) return Rlanguage_for_file(path) ? Rlanguage_for_file(path) : "XML";
	return Rlanguage_for_file(path);
}

Rid editors_open(const char* path, int line) {
	Rid found = editors_find(path);
	size_t size = 0;
	char* text;
	editor_t* e;
	if (found != R_INVALID_ID) {
		Rdock_area_open_panel(g_ide_dock, found);
		go_to_line(found, line);
		return found;
	}
	text = Rfile_read(path, &size);
	if (!text) {
		ide_status("Cannot read %s", path);
		return R_INVALID_ID;
	}
	e = create(ide_basename(path), path, path, text, language_of(path));
	Rmemory_free(text);
	go_to_line(e->widget, line);
	return e->widget;
}

Rid editors_open_text(const char* path, const char* text, int modified) {
	Rid found = editors_find(path);
	editor_t* e;
	if (found == R_INVALID_ID) {
		e = create(ide_basename(path), path, path, text, language_of(path));
		found = e->widget;
	} else {
		g_e.loading++;
		Rtext_editor_set_text(found, text);
		g_e.loading--;
		Rdock_area_open_panel(g_ide_dock, found);
	}
	editors_set_modified(found, modified);
	return found;
}

int editors_is_modified(Rid widget) {
	editor_t* e = by_widget(widget);
	return e && e->modified;
}

void editors_close_file(const char* path) {
	Rid widget = editors_find(path);
	editor_t* e = widget != R_INVALID_ID ? by_widget(widget) : NULL;
	if (!e) return;
	remove_editor(e);
	if (g_e.last == widget) g_e.last = R_INVALID_ID;
	if (Rwidget_is_valid(widget)) Rdock_area_close_panel(g_ide_dock, widget);
}

Rid editors_open_virtual(const char* title, const char* key, const char* text, const char* language, ide_save_callback save, void* user_data) {
	Rid found = editors_find(key);
	editor_t* e;
	if (found != R_INVALID_ID) {
		/* opened again: what the file says now */
		g_e.loading++;
		Rtext_editor_set_text(found, text);
		g_e.loading--;
		editors_set_modified(found, R_FALSE);
		e = by_widget(found);
		if (e) {
			e->save = save;
			e->user = user_data;
		}
		return found;
	}
	e = create(title, key, NULL, text, language);
	e->save = save;
	e->user = user_data;
	return e->widget;
}

int editors_save_all(void) {
	int i, ok = R_TRUE;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) {
		if (g_e.list[i].modified && !save_editor(&g_e.list[i])) ok = R_FALSE;
	}
	return ok;
}

int editors_any_modified(void) {
	int i;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) if (g_e.list[i].modified) return R_TRUE;
	return R_FALSE;
}

/* The editor the keyboard is in, else the one it was in last if its tab is in front, else one in front, for File > Save. */
static editor_t* focused_editor(void) {
	Rid focus = Rwindow_get_focused_widget(g_ide_window);
	editor_t* last;
	int i;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) {
		Rid walk;
		for (walk = focus; walk != R_INVALID_ID; walk = Rwidget_get_parent(walk)) {
			if (walk == g_e.list[i].widget) {
				g_e.last = walk;
				return &g_e.list[i];
			}
		}
	}
	last = g_e.last != R_INVALID_ID ? by_widget(g_e.last) : NULL;
	if (last && Rdock_area_is_panel_shown(g_ide_dock, last->widget)) return last;
	for (i = 0; i < g_e.count; i++) if (Rdock_area_is_panel_shown(g_ide_dock, g_e.list[i].widget)) return &g_e.list[i];
	return NULL;
}

const char* editors_current_path(void) {
	editor_t* e = focused_editor();
	return e ? e->path : NULL;
}

int editors_current_interface(const char** path, const char** text, unsigned* revision) {
	editor_t* e = focused_editor();
	if (!e || !e->path || !is_interface(e->path)) return R_FALSE;
	if (path) *path = e->path;
	if (text) *text = Rtext_editor_get_text(e->widget);
	if (revision) *revision = e->revision;
	return R_TRUE;
}

int editors_save_focused(void);
int editors_save_focused(void) {
	editor_t* e = focused_editor();
	if (!e) return -1;
	return save_editor(e);
}

void editors_clear_markers(void) {
	int i;
	editors_forget_closed();
	for (i = 0; i < g_e.count; i++) Rtext_editor_clear_markers(g_e.list[i].widget);
}

void editors_add_marker(const char* path, int line, int column, int error, const char* message) {
	Rid widget = editors_find(path);
	if (widget == R_INVALID_ID && xmlc_is_open() && ide_same_path(path, xmlc_path())) {
		/* an .xmlc is whole in the XML panel */
		designer_add_marker(line, column, error, message);
		return;
	}
	if (widget == R_INVALID_ID || line <= 0) return;
	Rtext_editor_add_marker(widget, line - 1, column > 0 ? column - 1 : 0, -1, error ? R_MARKER_ERROR : R_MARKER_WARNING, message);
}

void editors_init(void) {
	static RLanguageDesc cmake;
	g_e.last = R_INVALID_ID;
	memset(&cmake, 0, sizeof(cmake));
	cmake.name = "CMake";
	cmake.extensions = "cmake";
	cmake.keywords = "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro return break continue";
	cmake.types = "cmake_minimum_required project add_executable add_library target_link_libraries target_sources target_include_directories "
		"target_compile_definitions target_compile_options set unset option find_package include add_subdirectory install message list string "
		"file get_filename_component set_target_properties configure_file add_custom_command add_custom_target add_test enable_testing "
		"raven_target_xmlc";
	cmake.constants = "ON OFF TRUE FALSE PRIVATE PUBLIC INTERFACE REQUIRED";
	cmake.line_comment = "#";
	cmake.string_quotes = "\"";
	Rlanguage_register(&cmake);
	/* the languages of the scripts that Raven does not color by itself */
	{
		static RLanguageDesc go, zig, lua;
		memset(&go, 0, sizeof(go));
		go.name = "Go";
		go.extensions = "go";
		go.keywords = "break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var";
		go.types = "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any "
			"append cap close copy delete len make new panic print println recover";
		go.constants = "true false nil iota";
		go.line_comment = "//";
		go.block_comment_start = "/*";
		go.block_comment_end = "*/";
		go.string_quotes = "\"'";
		go.multiline_strings = "`";
		Rlanguage_register(&go);
		memset(&zig, 0, sizeof(zig));
		zig.name = "Zig";
		zig.extensions = "zig";
		zig.keywords = "const var fn pub return if else while for switch break continue defer errdefer try catch orelse struct enum union error test comptime inline export extern callconv usingnamespace and or unreachable";
		zig.types = "bool void u8 u16 u32 u64 usize i8 i16 i32 i64 isize f32 f64 c_int c_uint anyopaque anytype type noreturn";
		zig.constants = "true false null undefined";
		zig.line_comment = "//";
		zig.string_quotes = "\"'";
		zig.annotation = "@";
		Rlanguage_register(&zig);
		memset(&lua, 0, sizeof(lua));
		lua.name = "Lua";
		lua.extensions = "lua";
		lua.keywords = "and break do else elseif end for function goto if in local not or repeat return then until while";
		lua.types = "print pairs ipairs require setmetatable getmetatable tostring tonumber type select error assert pcall string table math os io";
		lua.constants = "true false nil";
		lua.line_comment = "--";
		lua.block_comment_start = "--[[";
		lua.block_comment_end = "]]";
		lua.string_quotes = "\"'";
		Rlanguage_register(&lua);
	}
	Rdock_area_set_close_callback(g_ide_dock, on_close, NULL);
}

void editors_shutdown(void) {
	int i;
	for (i = 0; i < g_e.count; i++) {
		free(g_e.list[i].path);
		free(g_e.list[i].key);
		free(g_e.list[i].title);
	}
	free(g_e.list);
	for (i = 0; i < g_e.api_count; i++) {
		free(g_e.api[i]);
		free(g_e.api_detail[i]);
	}
	free(g_e.api);
	free(g_e.api_detail);
	memset(&g_e, 0, sizeof(g_e));
	g_e.last = R_INVALID_ID;
}

/* ---------------------------------------------------------------------------
 *  Completion
 * ------------------------------------------------------------------------- */

static void api_add(const char* name, size_t length, const char* detail) {
	int i;
	char* word = (char*)malloc(length + 1);
	memcpy(word, name, length);
	word[length] = 0;
	for (i = 0; i < g_e.api_count; i++) {
		if (!strcmp(g_e.api[i], word)) {
			free(word);
			return;
		}
	}
	g_e.api = (char**)realloc(g_e.api, sizeof(char*) * (size_t)(g_e.api_count + 1));
	g_e.api_detail = (char**)realloc(g_e.api_detail, sizeof(char*) * (size_t)(g_e.api_count + 1));
	g_e.api[g_e.api_count] = word;
	g_e.api_detail[g_e.api_count] = ide_strdup(detail);
	g_e.api_count++;
}

static int is_word(char c) {
	return isalnum((unsigned char)c) || c == '_';
}

/* The functions (RAVEN_API ... Rname(), the constants (R_NAME) and the types (Rname) of a header. */
static void read_header(const char* path) {
	size_t size = 0;
	char* text = Rfile_read(path, &size);
	char* line;
	if (!text) return;
	for (line = text; line && *line;) {
		char* end = strchr(line, '\n');
		char* api;
		if (end) *end = 0;
		api = strstr(line, "RAVEN_API ");
		if (api) {
			char* paren = strchr(api, '(');
			if (paren) {
				char* name_end = paren;
				char* name;
				char detail[160];
				while (name_end > api && isspace((unsigned char)name_end[-1])) name_end--;
				name = name_end;
				while (name > api && is_word(name[-1])) name--;
				/* the return type, as a hint */
				snprintf(detail, sizeof(detail), "%.*s", (int)(name - api - 10 > 0 ? name - api - 10 : 0), api + 10);
				if (name < name_end) api_add(name, (size_t)(name_end - name), detail);
			}
		} else {
			char* p = line;
			while (*p == ' ' || *p == '\t') p++;
			/* an enumerator: R_SOMETHING = ..., or R_SOMETHING, */
			if (p[0] == 'R' && p[1] == '_') {
				char* q = p;
				while (is_word(*q)) q++;
				if (*q == ',' || *q == ' ' || *q == '=' || *q == 0) api_add(p, (size_t)(q - p), "constant");
			} else if (!strncmp(p, "} R", 3) || !strncmp(p, "}R", 2)) {
				char* q = strchr(p, 'R');
				char* e = q;
				while (is_word(*e)) e++;
				api_add(q, (size_t)(e - q), "type");
			}
		}
		line = end ? end + 1 : NULL;
	}
	Rmemory_free(text);
}

void completion_load_raven_api(void) {
	const char* candidates[3];
	char* tried[3];
	int i, n = 0;
	if (g_e.api_loaded) return;
	g_e.api_loaded = R_TRUE;
	/* next to the program (a package: bin/ and include/), or the sources it was built from */
	tried[0] = ide_join(ide_exe_dir(), "../include/raven");
	tried[1] = ide_join(ide_exe_dir(), "../../include/raven");
	tried[2] = ide_join(RAVEN_IDE_INCLUDE_DIR[0] ? RAVEN_IDE_INCLUDE_DIR : ".", "raven");
	candidates[0] = tried[0];
	candidates[1] = tried[1];
	candidates[2] = tried[2];
	for (i = 0; i < 3 && !n; i++) {
		ide_entry_t* entries;
		int count = ide_list_dir(candidates[i], &entries), j;
		for (j = 0; j < count; j++) {
			if (!entries[j].is_dir && ide_iequal(ide_extension(entries[j].name), "h")) {
				char* full = ide_join(candidates[i], entries[j].name);
				read_header(full);
				free(full);
				n++;
			}
		}
		free(entries);
	}
	for (i = 0; i < 3; i++) free(tried[i]);
	/* the macros of the headers are useful too */
	api_add("Rid", 3, "type");
	api_add("R_INVALID_ID", 12, "constant");
	api_add("R_TRUE", 6, "constant");
	api_add("R_FALSE", 7, "constant");
}

/* The text of the line before the cursor. */
static int before_cursor(Rid editor, char* out, size_t size) {
	int line, column;
	const char* text;
	Rtext_editor_get_cursor(editor, &line, &column);
	text = Rtext_editor_get_line(editor, line);
	if (!text) return 0;
	if ((size_t)column >= size) column = (int)size - 1;
	if ((int)strlen(text) < column) column = (int)strlen(text);
	memcpy(out, text, (size_t)column);
	out[column] = 0;
	return 1;
}

static void complete_keys(Rid editor) {
	int i, count = strings_key_count();
	for (i = 0; i < count; i++) Rtext_editor_add_completion(editor, strings_key(i), "translation");
}

static void complete_c(Rid editor, const char* prefix, const char* before) {
	size_t lp = strlen(prefix), lb = strlen(before);
	const char* word_start = before + lb - lp;
	int i;
	/* ui.<name>: the widgets of the interface */
	if (word_start - before >= 3 && !strncmp(word_start - 3, "ui.", 3)) {
		const char* names[256];
		int count = designer_get_names(names, 256);
		for (i = 0; i < count; i++) Rtext_editor_add_completion(editor, names[i], "Rid");
		return;
	}
	/* Rtr("key"), Rtr_plural("key", ... */
	if (word_start > before && word_start[-1] == '"' && strstr(before, "Rtr")) {
		complete_keys(editor);
		return;
	}
	completion_load_raven_api();
	for (i = 0; i < g_e.api_count; i++) Rtext_editor_add_completion(editor, g_e.api[i], g_e.api_detail[i]);
	if (xmlc_is_open()) Rtext_editor_add_completion(editor, "ui", "the widgets that have a Name");
}

static void complete_xml(Rid editor, const char* prefix, const char* before) {
	size_t lp = strlen(prefix), lb = strlen(before);
	const char* word_start = before + lb - lp;
	const char* open = strrchr(before, '<');
	const char* close = strrchr(before, '>');
	int i, quotes = 0;
	const char* p;
	/* a value that starts with @: a key of the tables */
	if (word_start > before && word_start[-1] == '@') {
		complete_keys(editor);
		return;
	}
	if (!open || (close && close > open)) return;
	/* the name of an element */
	if (word_start == open + 1 || (word_start == open + 2 && open[1] == '/')) {
		for (i = 0; i < Rxml_get_tag_count(); i++) Rtext_editor_add_completion(editor, Rxml_get_tag_name(i), "widget");
		for (i = 0; i < Rxml_get_registered_widget_count(); i++) Rtext_editor_add_completion(editor, Rxml_get_registered_widget_name(i), "declared");
		Rtext_editor_add_completion(editor, "Item", "row");
		Rtext_editor_add_completion(editor, "Shortcut", "keys");
		Rtext_editor_add_completion(editor, "Strings", "translations");
		Rtext_editor_add_completion(editor, "script", "code of the language of the file");
		return;
	}
	/* inside a value: nothing to propose */
	for (p = open; p < word_start; p++) if (*p == '"') quotes++;
	if (quotes % 2) return;
	/* an attribute of that element; "Slot.Fi" proposes "Fill" after the dot */
	{
		char tag[96], group[64] = "";
		const char* t = open + 1;
		const char* g;
		size_t n = 0;
		while (*t && is_word(*t) && n + 1 < sizeof(tag)) tag[n++] = *t++;
		tag[n] = 0;
		for (g = word_start; g > open && (is_word(g[-1]) || g[-1] == '.'); g--) {}
		if (g < word_start && word_start[-1] == '.') snprintf(group, sizeof(group), "%.*s", (int)(word_start - g), g);
		for (i = 0; i < Rxml_get_attribute_count(tag); i++) {
			RXmlAttributeInfo info;
			if (!Rxml_get_attribute_info(tag, i, &info)) continue;
			if (group[0]) {
				if (!strncmp(info.name, group, strlen(group))) Rtext_editor_add_completion(editor, info.name + strlen(group), info.group);
			} else {
				Rtext_editor_add_completion(editor, info.name, info.group);
			}
		}
	}
}

/* In the XML panel of an .xmlc: the cursor is between <script> and </script>. */
static int in_script(Rid editor) {
	const char* text = Rtext_editor_get_text(editor);
	const char* at = text;
	const char* open = NULL;
	const char* p;
	int line, column, l = 0;
	Rtext_editor_get_cursor(editor, &line, &column);
	while (*at && l < line) if (*at++ == '\n') l++;
	for (p = at; *p && *p != '\n' && p - at < column; p++) {}
	/* the last <script before the cursor, and no </script after it */
	for (at = text; at < p; at++) {
		if (*at != '<') continue;
		if (!strncmp(at, "<script", 7) && (at[7] == '>' || at[7] == ' ' || at[7] == '\t' || at[7] == '\n' || at[7] == '\r')) open = at;
		else if (!strncmp(at, "</script", 8)) open = NULL;
	}
	if (!open) return 0;
	/* (past the end of the opening tag) */
	at = strchr(open, '>');
	return at && at < p;
}

static void on_complete(Rid editor, const char* prefix, void* user_data) {
	char before[1024];
	if (!before_cursor(editor, before, sizeof(before))) return;
	/* an .xmlc is whole in the XML panel: in its scripts, the code of the language */
	if (user_data && xmlc_is_open() && in_script(editor)) {
		/* the words of the language of the scripts too (the panel itself is in XMLC, XMLRS...) */
		const ide_language_t* language = ide_interface_language(xmlc_path());
		RLanguageDesc desc;
		if (language && Rlanguage_get(language->editor, &desc)) {
			const char* lists[2];
			int k;
			lists[0] = desc.keywords;
			lists[1] = desc.types;
			for (k = 0; k < 2; k++) {
				const char* w = lists[k];
				while (w && *w) {
					char word[64];
					size_t n = 0;
					while (*w == ' ') w++;
					while (*w && *w != ' ' && n + 1 < sizeof(word)) word[n++] = *w++;
					while (*w && *w != ' ') w++;
					word[n] = '\0';
					if (n && !strncmp(word, prefix, strlen(prefix))) Rtext_editor_add_completion(editor, word, k ? "type" : "keyword");
				}
			}
		}
		complete_c(editor, prefix, before);
	}
	else if (user_data) complete_xml(editor, prefix, before);
	else complete_c(editor, prefix, before);
}

void completion_attach(Rid editor, int xml) {
	Rtext_editor_set_completion_callback(editor, on_complete, xml ? (void*)1 : NULL);
}
