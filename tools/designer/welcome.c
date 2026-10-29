/**
 * Raven Designer - welcome.c
 * The start page: what the IDE shows when nothing is open. New project, open
 * a folder or a file, and the recent projects (the last opened first, with
 * their language; a click opens one, the cross takes it out of the list).
 *
 * Also the preferences of the IDE, kept between two runs in ide_config_dir():
 *   ide.ini               key=value (theme, the folder of new projects, the Raven package)
 *   recent-projects.txt   one project per line: "<seconds since 1970>\t<folder>"
 */

#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_RECENT 30
#define MAX_PREFS 64
#define ROW_HEIGHT 62.0f
#define ROW_GAP 4.0f

typedef struct recent_t {
	char* path;
	char* name;
	const char* language;   /* "C", "Rust"... or "CMake", "Folder" */
	double opened;          /* seconds since 1970 */
	int missing;            /* the folder is not there any more */
} recent_t;

static struct {
	Rid page, list, search, empty, back, package;
	recent_t recent[MAX_RECENT];
	int count;
	/* the rows shown (the search filters them): indices into recent */
	int shown[MAX_RECENT];
	int shown_count;
	int hovered, hovered_remove, selected;
	char* keys[MAX_PREFS];
	char* values[MAX_PREFS];
	int pref_count;
	int prefs_loaded;
} g_w;

/* ---------------------------------------------------------------------------
 *  Preferences
 * ------------------------------------------------------------------------- */

static char* config_file(const char* name) {
	return ide_join(ide_config_dir(), name);
}

static void prefs_load(void) {
	char* path;
	char* text;
	size_t size = 0;
	if (g_w.prefs_loaded) return;
	g_w.prefs_loaded = R_TRUE;
	path = config_file("ide.ini");
	text = Rfile_read(path, &size);
	free(path);
	if (!text) return;
	{
		char* line = text;
		while (line && *line && g_w.pref_count < MAX_PREFS) {
			char* end = strchr(line, '\n');
			char* equal;
			if (end) *end = 0;
			if (end && end > line && end[-1] == '\r') end[-1] = 0;
			equal = strchr(line, '=');
			if (equal && equal > line) {
				*equal = 0;
				g_w.keys[g_w.pref_count] = ide_strdup(line);
				g_w.values[g_w.pref_count] = ide_strdup(equal + 1);
				g_w.pref_count++;
			}
			line = end ? end + 1 : NULL;
		}
	}
	Rmemory_free(text);
}

const char* prefs_get(const char* key) {
	int i;
	prefs_load();
	for (i = 0; i < g_w.pref_count; i++) if (!strcmp(g_w.keys[i], key)) return g_w.values[i];
	return "";
}

void prefs_set(const char* key, const char* value) {
	ide_buffer_t out = { 0 };
	char* path;
	int i;
	prefs_load();
	for (i = 0; i < g_w.pref_count; i++) if (!strcmp(g_w.keys[i], key)) break;
	if (i < g_w.pref_count) {
		if (!strcmp(g_w.values[i], value ? value : "")) return;
		free(g_w.values[i]);
		g_w.values[i] = ide_strdup(value);
	} else if (g_w.pref_count < MAX_PREFS) {
		g_w.keys[g_w.pref_count] = ide_strdup(key);
		g_w.values[g_w.pref_count] = ide_strdup(value);
		g_w.pref_count++;
	}
	for (i = 0; i < g_w.pref_count; i++) ide_buffer_printf(&out, "%s=%s\n", g_w.keys[i], g_w.values[i]);
	path = config_file("ide.ini");
	Rfile_write(path, out.data ? out.data : "", out.size);
	free(path);
	ide_buffer_free(&out);
}

/* ---------------------------------------------------------------------------
 *  Recent projects
 * ------------------------------------------------------------------------- */

/* What a project is written in: its interface file, else how it is built. */
static const char* detect_language(const char* folder) {
	static const char* const subfolders[] = { "", "src" };
	int f, i;
	for (f = 0; f < 2; f++) {
		for (i = 0; i < ide_language_count(); i++) {
			char name[80];
			char* path;
			int found;
			snprintf(name, sizeof(name), "%s%smain.%s", subfolders[f], f ? "/" : "", ide_language_at(i)->extension);
			path = ide_join(folder, name);
			found = ide_file_exists(path);
			free(path);
			if (found) return ide_language_at(i)->name;
		}
	}
	/* a native project: the file its tools read, or its program */
	{
		static const struct { const char* file; const char* language; } natives[] = {
			{ "main.c", "C" }, { "main.cpp", "C++" }, { "Cargo.toml", "Rust" }, { "go.mod", "Go" }, { "Program.cs", "C#" },
			{ "build.zig", "Zig" }, { "main.py", "Python" }, { "main.lua", "Lua" }
		};
		for (i = 0; i < (int)(sizeof(natives) / sizeof(natives[0])); i++) {
			char* path = ide_join(folder, natives[i].file);
			int found = ide_file_exists(path);
			free(path);
			if (found) return natives[i].language;
		}
	}
	{
		char* cmake = ide_join(folder, "CMakeLists.txt");
		int found = ide_file_exists(cmake);
		free(cmake);
		if (found) return "CMake";
	}
	return "Folder";
}

static void recent_free(recent_t* r) {
	free(r->path);
	free(r->name);
	memset(r, 0, sizeof(*r));
}

static void recent_fill(recent_t* r, const char* path, double opened) {
	r->path = ide_normalize(path);
	{
		size_t n = strlen(r->path);
		while (n > 1 && r->path[n - 1] == '/') r->path[--n] = 0;
	}
	r->name = ide_strdup(ide_basename(r->path));
	r->opened = opened;
	r->missing = !ide_is_dir(r->path);
	r->language = r->missing ? "Folder" : detect_language(r->path);
}

static void recent_save(void) {
	ide_buffer_t out = { 0 };
	char* path = config_file("recent-projects.txt");
	int i;
	for (i = 0; i < g_w.count; i++) ide_buffer_printf(&out, "%.0f\t%s\n", g_w.recent[i].opened, g_w.recent[i].path);
	Rfile_write(path, out.data ? out.data : "", out.size);
	free(path);
	ide_buffer_free(&out);
}

static void recent_load(void) {
	char* path = config_file("recent-projects.txt");
	size_t size = 0;
	char* text = Rfile_read(path, &size);
	free(path);
	if (!text) return;
	{
		char* line = text;
		while (line && *line && g_w.count < MAX_RECENT) {
			char* end = strchr(line, '\n');
			char* tab;
			if (end) *end = 0;
			if (end && end > line && end[-1] == '\r') end[-1] = 0;
			tab = strchr(line, '\t');
			if (tab && tab[1]) {
				int i, duplicate = 0;
				for (i = 0; i < g_w.count; i++) if (ide_same_path(g_w.recent[i].path, tab + 1)) duplicate = 1;
				if (!duplicate) recent_fill(&g_w.recent[g_w.count++], tab + 1, atof(line));
			}
			line = end ? end + 1 : NULL;
		}
	}
	Rmemory_free(text);
}

static int recent_find(const char* folder) {
	int i;
	char* normalized = ide_normalize(folder);
	size_t n = strlen(normalized);
	while (n > 1 && normalized[n - 1] == '/') normalized[--n] = 0;
	for (i = 0; i < g_w.count; i++) {
		if (ide_same_path(g_w.recent[i].path, normalized)) break;
	}
	free(normalized);
	return i < g_w.count ? i : -1;
}

static void recent_remove_at(int index) {
	if (index < 0 || index >= g_w.count) return;
	recent_free(&g_w.recent[index]);
	memmove(&g_w.recent[index], &g_w.recent[index + 1], sizeof(recent_t) * (size_t)(g_w.count - index - 1));
	g_w.count--;
	memset(&g_w.recent[g_w.count], 0, sizeof(recent_t));
	recent_save();
}

static void refresh_list(void);

void recent_add(const char* folder) {
	recent_t entry;
	int at = recent_find(folder);
	if (at >= 0) recent_remove_at(at);
	if (g_w.count == MAX_RECENT) recent_free(&g_w.recent[--g_w.count]);
	memset(&entry, 0, sizeof(entry));
	recent_fill(&entry, folder, (double)time(NULL));
	memmove(&g_w.recent[1], &g_w.recent[0], sizeof(recent_t) * (size_t)g_w.count);
	g_w.recent[0] = entry;
	g_w.count++;
	recent_save();
	refresh_list();
}

/* ---------------------------------------------------------------------------
 *  The list
 * ------------------------------------------------------------------------- */

static void language_color(const char* language, float* r, float* gr, float* b) {
	static const struct { const char* name; float r, g, b; } colors[] = {
		{ "C", 0.33f, 0.47f, 0.80f }, { "C++", 0.20f, 0.42f, 0.70f }, { "Rust", 0.80f, 0.42f, 0.24f }, { "Python", 0.22f, 0.55f, 0.75f },
		{ "Go", 0.16f, 0.66f, 0.75f }, { "C#", 0.45f, 0.33f, 0.75f }, { "Zig", 0.93f, 0.62f, 0.15f }, { "Lua", 0.20f, 0.25f, 0.65f },
		{ "CMake", 0.55f, 0.55f, 0.60f }
	};
	size_t i;
	for (i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
		if (!strcmp(colors[i].name, language)) {
			*r = colors[i].r;
			*gr = colors[i].g;
			*b = colors[i].b;
			return;
		}
	}
	*r = *gr = *b = 0.5f;
}

static const char* language_badge(const char* language) {
	if (!strcmp(language, "Rust")) return "Rs";
	if (!strcmp(language, "Python")) return "Py";
	if (!strcmp(language, "Folder")) return "...";
	if (!strcmp(language, "CMake")) return "CM";
	return language;
}

static void time_ago(double then, char* out, size_t size) {
	double seconds = (double)time(NULL) - then;
	if (then <= 0.0) { snprintf(out, size, "%s", ""); return; }
	if (seconds < 60.0) snprintf(out, size, "just now");
	else if (seconds < 3600.0) snprintf(out, size, "%d min ago", (int)(seconds / 60.0));
	else if (seconds < 86400.0) snprintf(out, size, "%d h ago", (int)(seconds / 3600.0));
	else if (seconds < 86400.0 * 2) snprintf(out, size, "yesterday");
	else if (seconds < 86400.0 * 31) snprintf(out, size, "%d days ago", (int)(seconds / 86400.0));
	else {
		time_t t = (time_t)then;
		struct tm* tm = localtime(&t);
		if (tm) strftime(out, size, "%d %b %Y", tm);
		else out[0] = 0;
	}
}

/* The text cut on the left ("...ts/Raven Projects/demo") to fit a width. */
static void fit_left(const char* text, float size, float width, char* out, size_t out_size) {
	float w = 0.0f, h = 0.0f;
	size_t length = strlen(text), cut;
	Rfont_measure_text(R_INVALID_ID, size, text, &w, &h);
	if (w <= width) {
		snprintf(out, out_size, "%s", text);
		return;
	}
	for (cut = 1; cut < length; cut++) {
		/* (not inside a character of several bytes) */
		if (((unsigned char)text[cut] & 0xC0) == 0x80) continue;
		snprintf(out, out_size, "...%s", text + cut);
		Rfont_measure_text(R_INVALID_ID, size, out, &w, &h);
		if (w <= width) return;
	}
	snprintf(out, out_size, "...");
}

static int matches(const recent_t* r, const char* filter) {
	size_t n = strlen(filter), i, j;
	const char* texts[2];
	int t;
	if (!n) return 1;
	texts[0] = r->name;
	texts[1] = r->path;
	for (t = 0; t < 2; t++) {
		const char* text = texts[t];
		for (i = 0; text[i]; i++) {
			for (j = 0; j < n && text[i + j]; j++) if (tolower((unsigned char)text[i + j]) != tolower((unsigned char)filter[j])) break;
			if (j == n) return 1;
		}
	}
	return 0;
}

static void refresh_list(void) {
	const char* filter;
	int i;
	if (g_w.list == R_INVALID_ID || !Rwidget_is_valid(g_w.list)) return;
	filter = Rtext_input_get_text(g_w.search);
	g_w.shown_count = 0;
	for (i = 0; i < g_w.count; i++) if (matches(&g_w.recent[i], filter)) g_w.shown[g_w.shown_count++] = i;
	if (g_w.selected >= g_w.shown_count) g_w.selected = g_w.shown_count - 1;
	g_w.hovered = -1;
	Rwidget_set_size(g_w.list, -1.0f, g_w.shown_count ? g_w.shown_count * (ROW_HEIGHT + ROW_GAP) : 1.0f);
	Rlabel_set_text(g_w.empty, g_w.count == 0 ? "No recent projects yet. Create one, or open a folder: it will be listed here."
		: g_w.shown_count == 0 ? "No recent project matches the search." : "");
	Rwidget_set_visibility(g_w.empty, g_w.shown_count == 0 ? R_VISIBLE : R_COLLAPSED);
	Rwidget_invalidate(g_w.list);
}

static void row_rect(int row, float width, float* x, float* y, float* w, float* h) {
	*x = 0.0f;
	*y = row * (ROW_HEIGHT + ROW_GAP);
	*w = width;
	*h = ROW_HEIGHT;
}

/* The little cross that takes a project out of the list, at the right of a row. */
static void remove_rect(float width, float row_y, float* x, float* y, float* size) {
	*size = 26.0f;
	*x = width - 14.0f - *size;
	*y = row_y + (ROW_HEIGHT - *size) * 0.5f;
}

static void list_paint(Rid widget, RPainter* painter, float width, float height, void* user_data) {
	float tr, tg, tb, ta, hr, hg, hb, ha, sr, sg, sb, sa, dr, dg, db, da;
	Rid bold = Rfont_get_variant(Rfont_get_default(), R_FONT_BOLD);
	int row;
	(void)widget;
	(void)height;
	(void)user_data;
	Rtheme_get_color(R_COLOR_TEXT, &tr, &tg, &tb, &ta);
	Rtheme_get_color(R_COLOR_ROW_HOVERED, &hr, &hg, &hb, &ha);
	Rtheme_get_color(R_COLOR_SELECTION, &sr, &sg, &sb, &sa);
	Rtheme_get_color(R_COLOR_DANGER, &dr, &dg, &db, &da);
	for (row = 0; row < g_w.shown_count; row++) {
		const recent_t* r = &g_w.recent[g_w.shown[row]];
		float x, y, w, h, br, bg, bb, tw = 0.0f, th = 0.0f, text_x, right;
		char text[1200], when[64];
		const char* badge = language_badge(r->language);
		row_rect(row, width, &x, &y, &w, &h);
		if (row == g_w.hovered) Rdraw_rect(painter, x, y, w, h, 12.0f, hr, hg, hb, ha);
		else if (row == g_w.selected) Rdraw_rect(painter, x, y, w, h, 12.0f, sr, sg, sb, sa * 0.6f);
		/* the badge of the language */
		language_color(r->language, &br, &bg, &bb);
		Rdraw_rect(painter, x + 12.0f, y + 11.0f, 40.0f, 40.0f, 10.0f, br, bg, bb, r->missing ? 0.35f : 1.0f);
		Rfont_measure_text(bold, 14.0f, badge, &tw, &th);
		Rdraw_text(painter, bold, 14.0f, x + 32.0f - tw * 0.5f, y + 31.0f - th * 0.5f, badge, 1.0f, 1.0f, 1.0f, 1.0f);
		/* the time, or the cross under the pointer */
		right = w - 16.0f;
		if (row == g_w.hovered) {
			float cx, cy, size;
			remove_rect(width, y, &cx, &cy, &size);
			if (g_w.hovered_remove) Rdraw_rect(painter, cx, cy, size, size, size * 0.5f, tr, tg, tb, 0.12f);
			Rdraw_line(painter, cx + 9.0f, cy + 9.0f, cx + size - 9.0f, cy + size - 9.0f, 1.6f, tr, tg, tb, 0.75f);
			Rdraw_line(painter, cx + size - 9.0f, cy + 9.0f, cx + 9.0f, cy + size - 9.0f, 1.6f, tr, tg, tb, 0.75f);
			right = cx - 10.0f;
		} else {
			time_ago(r->opened, when, sizeof(when));
			Rfont_measure_text(R_INVALID_ID, 12.0f, when, &tw, &th);
			Rdraw_text(painter, R_INVALID_ID, 12.0f, right - tw, y + 23.0f, when, tr, tg, tb, 0.5f);
			right -= tw + 16.0f;
		}
		/* the name, and where it is */
		text_x = x + 66.0f;
		fit_left(r->name, 15.0f, right - text_x, text, sizeof(text));
		Rdraw_text(painter, bold, 15.0f, text_x, y + 12.0f, text, tr, tg, tb, r->missing ? 0.5f : 1.0f);
		if (r->missing) {
			fit_left("The folder is not there any more: the cross takes it out of the list", 12.0f, right - text_x, text, sizeof(text));
			Rdraw_text(painter, R_INVALID_ID, 12.0f, text_x, y + 35.0f, text, dr, dg, db, 0.9f);
		} else {
			char pretty[1100], line[1200];
			ide_pretty_path(r->path, pretty, sizeof(pretty));
			snprintf(line, sizeof(line), "%s  ·  %s", r->language, pretty);
			fit_left(line, 12.0f, right - text_x, text, sizeof(text));
			Rdraw_text(painter, R_INVALID_ID, 12.0f, text_x, y + 35.0f, text, tr, tg, tb, 0.55f);
		}
	}
}

static int row_at(float y) {
	int row = (int)(y / (ROW_HEIGHT + ROW_GAP));
	if (y < 0.0f || row >= g_w.shown_count) return -1;
	if (y - row * (ROW_HEIGHT + ROW_GAP) > ROW_HEIGHT) return -1;
	return row;
}

static void open_row(int row) {
	char* path;
	if (row < 0 || row >= g_w.shown_count) return;
	path = ide_strdup(g_w.recent[g_w.shown[row]].path);
	if (!ide_is_dir(path)) {
		g_w.recent[g_w.shown[row]].missing = R_TRUE;
		Rwidget_invalidate(g_w.list);
		ide_status("%s is not there any more.", path);
	} else {
		project_open(path);
	}
	free(path);
}

static int list_event(Rid widget, const REvent* event, void* user_data) {
	float x, y, w, h;
	int row;
	(void)user_data;
	Rwidget_get_rect(widget, &x, &y, &w, &h);
	switch (event->type) {
		case R_EVENT_MOUSE_MOVE: {
			int on_remove = 0;
			row = row_at(event->local_y);
			if (row >= 0) {
				float cx, cy, size;
				remove_rect(w, row * (ROW_HEIGHT + ROW_GAP), &cx, &cy, &size);
				on_remove = event->local_x >= cx && event->local_x <= cx + size && event->local_y >= cy && event->local_y <= cy + size;
			}
			if (row != g_w.hovered || on_remove != g_w.hovered_remove) {
				g_w.hovered = row;
				g_w.hovered_remove = on_remove;
				Rwidget_invalidate(widget);
			}
			Rwidget_set_cursor(widget, row >= 0 ? R_CURSOR_HAND : R_CURSOR_ARROW);
			return R_TRUE;
		}
		case R_EVENT_MOUSE_LEAVE:
			g_w.hovered = -1;
			g_w.hovered_remove = 0;
			Rwidget_invalidate(widget);
			return R_FALSE;
		case R_EVENT_MOUSE_DOWN:
			Rwidget_focus(widget);
			if (event->button != R_MOUSE_LEFT) return R_TRUE;
			row = row_at(event->local_y);
			if (row < 0) return R_TRUE;
			g_w.selected = row;
			if (g_w.hovered_remove && row == g_w.hovered) {
				recent_remove_at(g_w.shown[row]);
				refresh_list();
			} else {
				open_row(row);
			}
			return R_TRUE;
		case R_EVENT_KEY_DOWN:
			if (event->key == R_KEY_DOWN && g_w.shown_count) g_w.selected = g_w.selected + 1 < g_w.shown_count ? g_w.selected + 1 : g_w.shown_count - 1;
			else if (event->key == R_KEY_UP && g_w.shown_count) g_w.selected = g_w.selected > 0 ? g_w.selected - 1 : 0;
			else if (event->key == R_KEY_ENTER) open_row(g_w.selected);
			else if (event->key == R_KEY_DELETE && g_w.selected >= 0 && g_w.selected < g_w.shown_count) {
				recent_remove_at(g_w.shown[g_w.selected]);
				refresh_list();
			} else return R_FALSE;
			Rwidget_invalidate(widget);
			return R_TRUE;
		default:
			return R_FALSE;
	}
}

static void on_search(Rid input, const char* text, void* user_data) {
	(void)input;
	(void)text;
	(void)user_data;
	g_w.selected = 0;
	refresh_list();
}

/* Enter in the search box: the first project that matches. */
static void on_search_committed(Rid input, const char* text, void* user_data) {
	(void)input;
	(void)text;
	(void)user_data;
	if (g_w.shown_count) open_row(g_w.selected >= 0 ? g_w.selected : 0);
}

/* ---------------------------------------------------------------------------
 *  The page
 * ------------------------------------------------------------------------- */

static const char WELCOME_XML[] =
"<ScrollBox Slot.Fill=\"1\">"
"  <HBox Spacing=\"56\" Slot.Padding=\"64,48\">"
"    <VBox Spacing=\"10\" Width=\"290\">"
"      <Label Text=\"Raven IDE\" TextSize=\"34\"/>"
"      <Label Name=\"welcome.version\" Opacity=\"0.55\"/>"
"      <Spacer Height=\"22\"/>"
"      <Button Text=\"New project...\" Style=\"Primary\" OnClicked=\"project.new\" Tooltip=\"In the language you choose: with a Raven interface, or native (Ctrl+Shift+N)\"/>"
"      <Button Text=\"Open a folder...\" OnClicked=\"project.open\" Tooltip=\"A project folder (Ctrl+Shift+O)\"/>"
"      <Button Text=\"Open a file...\" OnClicked=\"file.open\" Tooltip=\"An interface: .xml, .xmlc, .xmlrs... (Ctrl+O)\"/>"
"      <Button Name=\"welcome.back\" Style=\"Flat\" OnClicked=\"welcome.back\" Visibility=\"Collapsed\"/>"
"      <Spacer Height=\"22\"/>"
"      <Label Text=\"Without a project\" Opacity=\"0.55\"/>"
"      <Button Text=\"A new window\" Style=\"Flat\" OnClicked=\"file.new.window\" Slot.HAlign=\"Left\"/>"
"      <Button Text=\"A form with its own tags\" Style=\"Flat\" OnClicked=\"file.new.form\" Slot.HAlign=\"Left\"/>"
"      <Spacer Height=\"22\"/>"
"      <Label Text=\"The Raven package\" Opacity=\"0.55\"/>"
"      <Label Name=\"welcome.package\" Wrap=\"true\"/>"
"      <Button Text=\"Choose the Raven package...\" Style=\"Flat\" OnClicked=\"welcome.package\" Slot.HAlign=\"Left\""
"        Tooltip=\"The folder with bin/, include/ and lib/: what the projects are built with\"/>"
"    </VBox>"
"    <VBox Spacing=\"14\" Slot.Fill=\"1\">"
"      <HBox Spacing=\"12\">"
"        <Label Text=\"Recent projects\" TextSize=\"20\" Slot.Fill=\"1\" Slot.VAlign=\"Center\"/>"
"        <TextInput Name=\"welcome.search\" Placeholder=\"Search\" Width=\"240\"/>"
"      </HBox>"
"      <VBox Name=\"welcome.rows\"/>"
"      <Label Name=\"welcome.empty\" Opacity=\"0.55\" Wrap=\"true\" Slot.Padding=\"4,8\"/>"
"    </VBox>"
"  </HBox>"
"</ScrollBox>";

static void update_package(void) {
	char text[1200], pretty[1100];
	if (g_settings.raven_prefix[0]) {
		ide_pretty_path(g_settings.raven_prefix, pretty, sizeof(pretty));
		snprintf(text, sizeof(text), "%s", pretty);
		Rlabel_set_text(g_w.package, text);
		Rwidget_set_opacity(g_w.package, 0.8f);
	} else {
		Rlabel_set_text(g_w.package, "Not found. C and C++ projects need it to build (Rust, Go, C#, Zig, Python and Lua need its bindings).");
		Rwidget_set_opacity(g_w.package, 1.0f);
	}
}

static void action_welcome(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	if (!strcmp(action, "welcome.back")) {
		welcome_hide();
		ide_status("Back to %s.", g_project_dir ? ide_basename(g_project_dir) : "the IDE");
	} else if (!strcmp(action, "welcome.package")) {
		RFileDialogDesc desc;
		const char* chosen;
		memset(&desc, 0, sizeof(desc));
		desc.parent = g_ide_window;
		desc.title = "The folder of the Raven package (bin/, include/, lib/)";
		chosen = Rfile_dialog_select_folder(&desc);
		if (chosen) {
			snprintf(g_settings.raven_prefix, sizeof(g_settings.raven_prefix), "%s", chosen);
			prefs_set("raven_prefix", chosen);
			if (g_project_dir) project_save_settings();
			update_package();
		}
	}
}

void welcome_init(void) {
	Rid content, rows;
	g_w.page = Rwidget_find(g_ide_window, "page.welcome");
	g_w.list = R_INVALID_ID;
	g_w.hovered = -1;
	g_w.selected = 0;
	recent_load();
	content = Rwidget_create_from_xml(WELCOME_XML, sizeof(WELCOME_XML) - 1);
	if (content == R_INVALID_ID) return;
	Rslot_set_fill(content, 1.0f);
	Rwidget_add_child(g_w.page, content);
	g_w.search = Rwidget_find(content, "welcome.search");
	g_w.empty = Rwidget_find(content, "welcome.empty");
	g_w.back = Rwidget_find(content, "welcome.back");
	g_w.package = Rwidget_find(content, "welcome.package");
	{
		char version[96];
		snprintf(version, sizeof(version), "Version %s", Rget_version());
		Rlabel_set_text(Rwidget_find(content, "welcome.version"), version);
	}
	rows = Rwidget_find(content, "welcome.rows");
	g_w.list = Rwidget_create(R_WIDGET_CUSTOM);
	Rcustom_set_paint_callback(g_w.list, list_paint, NULL);
	Rwidget_set_event_callback(g_w.list, list_event, NULL);
	Rwidget_set_focusable(g_w.list, R_TRUE);
	Rwidget_add_child(rows, g_w.list);
	Rtext_input_set_changed_callback(g_w.search, on_search, NULL);
	Rtext_input_set_committed_callback(g_w.search, on_search_committed, NULL);
	Raction_bind("welcome.back", action_welcome, NULL);
	Raction_bind("welcome.package", action_welcome, NULL);
	refresh_list();
	update_package();
}

void welcome_shutdown(void) {
	int i;
	for (i = 0; i < g_w.count; i++) recent_free(&g_w.recent[i]);
	g_w.count = 0;
	for (i = 0; i < g_w.pref_count; i++) {
		free(g_w.keys[i]);
		free(g_w.values[i]);
	}
	g_w.pref_count = 0;
}

void welcome_show(void) {
	Rid pages = Rwidget_find(g_ide_window, "pages");
	int i;
	/* folders may have come or gone since */
	for (i = 0; i < g_w.count; i++) {
		g_w.recent[i].missing = !ide_is_dir(g_w.recent[i].path);
		if (!g_w.recent[i].missing) g_w.recent[i].language = detect_language(g_w.recent[i].path);
	}
	Rswitcher_set_index(pages, 0);
	Rwidget_set_visibility(Rwidget_find(g_ide_window, "header.build"), R_COLLAPSED);
	if (g_project_dir) {
		char text[400];
		snprintf(text, sizeof(text), "Back to %s", ide_basename(g_project_dir));
		Rbutton_set_text(g_w.back, text);
		Rwidget_set_visibility(g_w.back, R_VISIBLE);
	} else if (designer_has_document()) {
		Rbutton_set_text(g_w.back, "Back to the interface");
		Rwidget_set_visibility(g_w.back, R_VISIBLE);
	} else {
		Rwidget_set_visibility(g_w.back, R_COLLAPSED);
	}
	update_package();
	refresh_list();
	Rwidget_focus(g_w.list);
}

void welcome_hide(void) {
	Rid pages = Rwidget_find(g_ide_window, "pages");
	if (pages == R_INVALID_ID || Rswitcher_get_index(pages) == 1) return;
	Rswitcher_set_index(pages, 1);
	Rwidget_set_visibility(Rwidget_find(g_ide_window, "header.build"), R_VISIBLE);
}

int welcome_is_shown(void) {
	Rid pages = g_ide_window != R_INVALID_ID ? Rwidget_find(g_ide_window, "pages") : R_INVALID_ID;
	return pages != R_INVALID_ID && Rswitcher_get_index(pages) == 0;
}
