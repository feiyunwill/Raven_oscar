/*
 * Raven IDE - island.c
 * The island at the top of the window (raven_shell.h): what is going on,
 * at a glance, and what to do about it.
 *
 * Folded, a capsule hanging from the top edge: a dot (gray: nothing going
 * on, blue: building or running, green: the last build went well, orange:
 * with warnings, red: it failed), the project and the file, and the state.
 * Unfolded (the pointer over it, or a click), the project and its folder,
 * the file and whether it has unsaved changes, the build (under way for how
 * long, or how the last one ended), Build / Run / Stop, the problems, the
 * content drawer, Save all. When a build ends, the island unfolds by itself
 * for a few seconds to say how.
 *
 * When the code editor in front holds an interface (.xml, .xmlc, .xmlpy...),
 * the island is its preview: unfolded, it shows the interface built from the
 * text being typed (rebuilt as it changes, in the language of the preview),
 * and opens it in the visual editor when asked. The visual editor itself is
 * only there when asked for (the Preview of the content drawer).
 */

#include "ide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest the preview of the island grows; a bigger interface scrolls in it. */
#define PREVIEW_MAX_WIDTH 720.0f
#define PREVIEW_MAX_HEIGHT 440.0f

static const char ISLAND_XML[] =
"<Island Name=\"island\" Attached=\"true\">"
"  <HBox Spacing=\"8\" Slot.Padding=\"14,5\">"
"    <Border Name=\"island.dot\" Width=\"8\" Height=\"8\" CornerRadius=\"4\" Background=\"#8E8E93\" Slot.VAlign=\"Center\" AccessibleHidden=\"true\"/>"
"    <Label Name=\"island.title\" Text=\"Raven IDE\" Slot.VAlign=\"Center\"/>"
"    <Label Name=\"island.state\" Opacity=\"0.6\" Slot.VAlign=\"Center\" Visibility=\"Collapsed\"/>"
"  </HBox>"
"  <VBox Name=\"island.body\" Spacing=\"10\" Slot.Padding=\"18,14\" Width=\"420\">"
"    <HBox Spacing=\"10\">"
"      <Border Name=\"island.dot.big\" Width=\"10\" Height=\"10\" CornerRadius=\"5\" Background=\"#8E8E93\" Slot.VAlign=\"Center\" AccessibleHidden=\"true\"/>"
"      <VBox Slot.Fill=\"1\" Spacing=\"2\">"
"        <Label Name=\"island.project\" TextSize=\"16\"/>"
"        <Label Name=\"island.where\" Opacity=\"0.55\" TextSize=\"11\"/>"
"      </VBox>"
"    </HBox>"
"    <Label Name=\"island.file\" Opacity=\"0.85\" Wrap=\"true\"/>"
"    <VBox Name=\"island.preview\" Spacing=\"6\" Visibility=\"Collapsed\">"
"      <HBox Spacing=\"6\">"
"        <Label Name=\"island.preview.title\" Text=\"Preview\" Opacity=\"0.6\" Slot.Fill=\"1\" Slot.VAlign=\"Center\"/>"
"        <Button Text=\"Open in the visual editor\" Style=\"Flat\" OnClicked=\"view.preview\" Tooltip=\"Ctrl+Shift+P\"/>"
"      </HBox>"
"      <ScrollBox Name=\"island.preview.scroll\" ScrollBoth=\"true\">"
"        <Border Name=\"island.preview.frame\" CornerRadius=\"8\" BorderWidth=\"1\" Slot.HAlign=\"Center\"/>"
"      </ScrollBox>"
"      <Label Name=\"island.preview.error\" Wrap=\"true\" Opacity=\"0.8\" Visibility=\"Collapsed\"/>"
"    </VBox>"
"    <Separator/>"
"    <Label Name=\"island.build\" Wrap=\"true\"/>"
"    <HBox Spacing=\"6\">"
"      <Button Text=\"Build\" OnClicked=\"build.build\" Tooltip=\"Ctrl+B\"/>"
"      <Button Text=\"Run\" Style=\"Primary\" OnClicked=\"build.run\" Tooltip=\"F5\"/>"
"      <Button Name=\"island.stop\" Text=\"Stop\" OnClicked=\"build.stop\" Tooltip=\"Ctrl+F5\"/>"
"      <Spacer Slot.Fill=\"1\"/>"
"      <Button Name=\"island.problems\" Text=\"No problem\" Style=\"Flat\" OnClicked=\"island.problems\" Tooltip=\"The problems of the interface, then those of the last build\"/>"
"    </HBox>"
"    <HBox Spacing=\"6\">"
"      <Button Text=\"Content drawer  (Ctrl+Space)\" Style=\"Flat\" OnClicked=\"drawer.toggle\" Slot.Fill=\"1\"/>"
"      <Button Text=\"Save all\" Style=\"Flat\" OnClicked=\"file.save_all\" Tooltip=\"Ctrl+Alt+S\"/>"
"    </HBox>"
"  </VBox>"
"</Island>";

static struct {
	Rid island, timer;
	int was_running;
	char problems_text[48];
	/* the preview: what it was built from (the file, the change of its text), what it built */
	Rid preview;
	char preview_path[1024];
	unsigned preview_revision;
	int preview_stale;
} g_i = { R_INVALID_ID, R_INVALID_ID, 0, "", R_INVALID_ID, "", 0, 1 };

static Rid find(const char* name) {
	return Rwidget_find(g_i.island, name);
}

/* (a text is set only when it changes: no layout for nothing, four times a second) */
static void set_text(const char* name, const char* text) {
	Rid label = find(name);
	const char* now = Rlabel_get_text(label);
	if (!now || strcmp(now, text) != 0) Rlabel_set_text(label, text);
}

static void set_dot(const char* color) {
	Rwidget_set_attribute(find("island.dot"), "Background", color);
	Rwidget_set_attribute(find("island.dot.big"), "Background", color);
}

/* ---------------------------------------------------------------------------
 *  The preview of the interface in the code editor in front
 * ------------------------------------------------------------------------- */

/* The paths written in an interface (images, includes) start from its folder. */
static void base_path_of(const char* path) {
	char* folder = ide_strdup(path);
	char* slash = strrchr(folder, '/');
	char* back = strrchr(folder, '\\');
	if (back && (!slash || back > slash)) slash = back;
	if (slash) {
		*slash = '\0';
		Rxml_set_base_path(folder);
	}
	free(folder);
}

static void drop_preview(void) {
	if (g_i.preview != R_INVALID_ID && Rwidget_is_valid(g_i.preview)) Rwidget_destroy(g_i.preview);
	g_i.preview = R_INVALID_ID;
}

static void preview_error(const char* text) {
	Rid label = find("island.preview.error");
	Rlabel_set_text(label, text ? text : "");
	Rwidget_set_visibility(label, text && text[0] ? R_VISIBLE : R_COLLAPSED);
	Rwidget_set_visibility(find("island.preview.scroll"), text && text[0] ? R_COLLAPSED : R_VISIBLE);
}

/* Builds the interface of a text (an .xml, or the interface element of an .xmlc...) into the frame of the island. */
static void build_preview(const char* path, const char* text) {
	Rid frame = find("island.preview.frame"), scroll = find("island.preview.scroll"), document, root;
	size_t start = 0, end = strlen(text);
	int first_line = 1;
	float width = 520.0f, height = 300.0f;
	RXmlError error;
	char message[400];

	drop_preview();
	if (ide_interface_language(path)) {
		int found = xmlc_find_interface(text, &start, &end, &first_line);
		if (found <= 0) {
			preview_error(found == 0 ? "No interface in the file yet: write its element after the <script> blocks." : "An element of the file is not closed.");
			return;
		}
	}
	memset(&error, 0, sizeof(error));
	document = Rxml_document_parse(text + start, end - start, &error);
	if (document == R_INVALID_ID) {
		snprintf(message, sizeof(message), "Line %d: %s", error.line + first_line - 1, error.message);
		preview_error(message);
		return;
	}
	/* (the paths of the file start from its folder; then those of the visual editor again) */
	base_path_of(path);
	root = Rxml_document_get_root(document);
	if (ide_iequal(Rxml_node_get_tag(root), "Window")) {
		const char* w = Rxml_node_get_attribute(root, "Width");
		const char* h = Rxml_node_get_attribute(root, "Height");
		width = w && atoi(w) > 0 ? (float)atoi(w) : 800.0f;
		height = h && atoi(h) > 0 ? (float)atoi(h) : 600.0f;
		Rwidget_set_size(frame, width, height);
	} else {
		/* a piece of interface: the size it asks for */
		Rwidget_set_size(frame, -1.0f, -1.0f);
	}
	Rwidget_set_direction(frame, Rlocale_get_language_count() > 0 && Rlocale_is_rtl() ? R_DIRECTION_RTL : R_DIRECTION_LTR);
	g_i.preview = Rxml_document_create_widget(document);
	Rxml_document_destroy(document);
	if (designer_get_path()) base_path_of(designer_get_path());
	if (g_i.preview == R_INVALID_ID) {
		preview_error("This interface shows nothing (yet).");
		return;
	}
	Rwidget_add_child(frame, g_i.preview);
	preview_error(NULL);
	/* the island grows with the interface, up to a point: past it, the preview scrolls */
	width = (width < PREVIEW_MAX_WIDTH ? width : PREVIEW_MAX_WIDTH) + 2.0f;
	height = (height < PREVIEW_MAX_HEIGHT ? height : PREVIEW_MAX_HEIGHT) + 2.0f;
	Rwidget_set_size(scroll, width, height);
	Rwidget_set_size(find("island.body"), width > 420.0f ? width : 420.0f, -1.0f);
}

/* Shows (and builds when its text changed) the preview of the interface in the code editor in front; `force`: even folded. */
static void update_preview(int force) {
	const char* path = NULL;
	const char* text = NULL;
	unsigned revision = 0;
	int has = editors_current_interface(&path, &text, &revision);
	char title[400];
	/* (the interface the visual editor shows is previewed there) */
	if (has && designer_has_document() && designer_get_path() && ide_same_path(designer_get_path(), path)) has = R_FALSE;
	if (!has) {
		if (g_i.preview_path[0]) {
			drop_preview();
			g_i.preview_path[0] = '\0';
			Rwidget_set_visibility(find("island.preview"), R_COLLAPSED);
			Rwidget_set_size(find("island.body"), 420.0f, -1.0f);
		}
		return;
	}
	if (!force && !Risland_is_expanded(g_i.island)) return;
	Rwidget_set_visibility(find("island.preview"), R_VISIBLE);
	if (!g_i.preview_stale && !strcmp(g_i.preview_path, path) && g_i.preview_revision == revision) return;
	snprintf(g_i.preview_path, sizeof(g_i.preview_path), "%s", path);
	g_i.preview_revision = revision;
	g_i.preview_stale = R_FALSE;
	snprintf(title, sizeof(title), "Preview  \xC2\xB7  %s", ide_basename(path));
	set_text("island.preview.title", title);
	build_preview(path, text);
}

static void refresh(void) {
	build_summary_t build;
	/* the file: the one of the visual editor, else the one of the code editor in front */
	const char* path = designer_has_document() && designer_get_path() ? designer_get_path() : editors_current_path();
	const char* file = path ? ide_basename(path) : NULL;
	int unsaved = designer_is_modified() || editors_any_modified();
	int problems = designer_problem_count();
	double now = Rget_time();
	char title[300], state[96], text[400];

	build_get_summary(&build);

	/* folded: the project and the file */
	if (g_project_dir && file) snprintf(title, sizeof(title), "%s  \xC2\xB7  %s%s", ide_basename(g_project_dir), file, unsaved ? " \xE2\x80\xA2" : "");
	else if (g_project_dir) snprintf(title, sizeof(title), "%s", ide_basename(g_project_dir));
	else if (file) snprintf(title, sizeof(title), "%s%s", file, unsaved ? " \xE2\x80\xA2" : "");
	else snprintf(title, sizeof(title), "Raven IDE");
	set_text("island.title", title);

	/* the state, and its color */
	state[0] = '\0';
	if (build.running && build.program) {
		snprintf(state, sizeof(state), "running");
		set_dot("#0A84FF");
	} else if (build.running) {
		snprintf(state, sizeof(state), "building %.0f s", now - build.started);
		set_dot("#0A84FF");
	} else if (build.has_result && !build.ok) {
		snprintf(state, sizeof(state), "%d error%s", build.errors, build.errors == 1 ? "" : "s");
		set_dot("#FF453A");
	} else if (build.has_result && build.warnings) {
		snprintf(state, sizeof(state), "%d warning%s", build.warnings, build.warnings == 1 ? "" : "s");
		set_dot("#FF9F0A");
	} else if (build.has_result) {
		snprintf(state, sizeof(state), "built");
		set_dot("#30D158");
	} else if (problems) {
		snprintf(state, sizeof(state), "%d problem%s", problems, problems == 1 ? "" : "s");
		set_dot("#FF9F0A");
	} else {
		set_dot("#8E8E93");
	}
	set_text("island.state", state);
	Rwidget_set_visibility(find("island.state"), state[0] ? R_VISIBLE : R_COLLAPSED);

	/* a build just ended: the island says how, for a few seconds */
	if (g_i.was_running && !build.running && build.has_result && !Risland_is_expanded(g_i.island)) Risland_expand_for(g_i.island, 4.0);

	/* unfolded */
	set_text("island.project", g_project_dir ? ide_basename(g_project_dir) : "No project");
	if (g_project_dir) {
		char pretty[600];
		ide_pretty_path(g_project_dir, pretty, sizeof(pretty));
		set_text("island.where", pretty);
	} else {
		set_text("island.where", "Project > Open folder, or New project");
	}
	if (file) snprintf(text, sizeof(text), "%s%s", file, unsaved ? "  \xC2\xB7  unsaved changes" : "  \xC2\xB7  saved");
	else snprintf(text, sizeof(text), unsaved ? "Unsaved changes in the code editors" : "No file open");
	set_text("island.file", text);

	update_preview(R_FALSE);

	/* (what follows is only seen unfolded: "12 s ago" does not redraw the window every second for nobody) */
	if (!Risland_is_expanded(g_i.island)) {
		g_i.was_running = build.running;
		return;
	}
	if (build.running && build.program) {
		snprintf(text, sizeof(text), "The program runs (for %.0f s). Stop ends it.", now - build.started);
	} else if (build.running) {
		snprintf(text, sizeof(text), "Building for %.0f s...", now - build.started);
	} else if (build.has_result) {
		double ago = now - build.finished;
		char when[48];
		if (ago < 60.0) snprintf(when, sizeof(when), "%.0f s ago", ago);
		else if (ago < 3600.0) snprintf(when, sizeof(when), "%.0f min ago", ago / 60.0);
		else snprintf(when, sizeof(when), "%.0f h ago", ago / 3600.0);
		snprintf(text, sizeof(text), "%s %s: %d error%s, %d warning%s.", build.ok ? "Built" : "Failed", when,
			build.errors, build.errors == 1 ? "" : "s", build.warnings, build.warnings == 1 ? "" : "s");
	} else {
		snprintf(text, sizeof(text), g_project_dir ? "Not built yet: Build, or Run." : "Open a project to build it.");
	}
	set_text("island.build", text);
	Rwidget_set_enabled(find("island.stop"), build.running);

	if (problems || build.errors || build.warnings) {
		snprintf(text, sizeof(text), "Problems (%d)", problems + build.errors + build.warnings);
	} else {
		snprintf(text, sizeof(text), "No problem");
	}
	if (strcmp(g_i.problems_text, text) != 0) {
		snprintf(g_i.problems_text, sizeof(g_i.problems_text), "%.47s", text);
		Rbutton_set_text(find("island.problems"), text);
	}

	g_i.was_running = build.running;
}

static void on_timer(Rid timer, void* user_data) {
	(void)timer;
	(void)user_data;
	if (g_i.island != R_INVALID_ID && Rwidget_is_valid(g_i.island)) refresh();
}

static void on_changed(Rid island, int expanded, void* user_data) {
	(void)island;
	(void)user_data;
	if (expanded) refresh();
}

static void action_island(Rid sender, const char* action, void* user_data) {
	Rid problems = Rwidget_find(g_ide_window, "problems");
	Rid build_problems = Rwidget_find(g_ide_window, "build.problems");
	build_summary_t build;
	(void)sender;
	(void)user_data;
	(void)action;
	build_get_summary(&build);
	/* the problems of the interface first; else those of the build */
	/* (the Problems panel belongs to the visual editor: hidden with it) */
	if (designer_has_document() && (designer_problem_count() || !(build.errors || build.warnings))) Rdock_area_open_panel(g_ide_dock, problems);
	else Rdock_area_open_panel(g_ide_dock, build_problems);
	Risland_set_expanded(g_i.island, R_FALSE);
}

void island_init(void) {
	g_i.island = Rwidget_create_from_xml(ISLAND_XML, sizeof(ISLAND_XML) - 1);
	if (g_i.island == R_INVALID_ID) return;
	Rwindow_set_island(g_ide_window, g_i.island);
	Rwidget_set_clipping(find("island.preview.frame"), R_TRUE);
	Raction_bind("island.problems", action_island, NULL);
	Risland_set_changed_callback(g_i.island, on_changed, NULL);
	refresh();
	g_i.timer = Rtimer_create(0.25, R_TRUE, on_timer, NULL);
}

void island_refresh(void) {
	if (g_i.island != R_INVALID_ID && Rwidget_is_valid(g_i.island)) refresh();
}

void island_preview_changed(void) {
	g_i.preview_stale = R_TRUE;
}

void island_show_preview(void) {
	const char* path = NULL;
	if (g_i.island == R_INVALID_ID || !Rwidget_is_valid(g_i.island)) return;
	if (!editors_current_interface(&path, NULL, NULL)) return;
	g_i.preview_stale = R_TRUE;
	/* an interface was opened as code: the island shows that it is its preview */
	if (!Risland_is_expanded(g_i.island)) Risland_expand_for(g_i.island, 3.0);
	update_preview(R_TRUE);
}

Rid island_preview_widget(void) {
	if (g_i.island == R_INVALID_ID || !Rwidget_is_valid(g_i.island)) return R_INVALID_ID;
	update_preview(R_TRUE);
	return g_i.preview_path[0] && g_i.preview != R_INVALID_ID && Rwidget_is_valid(g_i.preview) ? g_i.preview : R_INVALID_ID;
}

void island_shutdown(void) {
	drop_preview();
	if (g_i.timer != R_INVALID_ID && Rtimer_is_valid(g_i.timer)) Rtimer_destroy(g_i.timer);
	g_i.timer = R_INVALID_ID;
}
