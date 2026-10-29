/*
 * Raven IDE - drawer.c
 * The content drawer: the files of the project as tiles, in a panel that
 * slides in from the left over the IDE. Ctrl+Space opens and closes it, from
 * anywhere (also from the code editors); Escape, a click beside it, or a
 * file opened closes it.
 *
 *   left     the folders of the project (a click shows one)
 *   right    the tiles of the folder: folders first, then the files, an
 *            icon by kind (interface, XML, code, build, translations,
 *            pictures, text) and the extension in the corner
 *   search   the whole project, by name; each tile says where the file is
 *
 * A double click (or Enter) on a folder goes into it, on a file opens it as
 * the IDE does for any file (ide_open_file: an interface opens as code, the
 * island shows it). A right click on an interface (.xml, .xmlc, .xmlpy...)
 * offers its Preview: the visual editor. Up goes to the folder above, never
 * above the project.
 */

#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TILES 2000
#define ICON_SIZE 96

typedef enum {
	KIND_FOLDER, KIND_INTERFACE, KIND_XML, KIND_CODE, KIND_BUILD, KIND_STRINGS, KIND_IMAGE, KIND_TEXT, KIND_COUNT
} kind_t;

typedef struct tile_t {
	char* name;
	char* path;
	char* detail;
	char badge[12];
	kind_t kind;
} tile_t;

static const char DRAWER_XML[] =
"<Overlay Name=\"drawer.root\" Visibility=\"Collapsed\">"
"  <Border Name=\"drawer.veil\" Background=\"#00000066\"/>"
"  <Border Name=\"drawer\" Slot.HAlign=\"Left\" Width=\"780\" Shadow=\"30,8,0\" CornerRadius=\"0,14,14,0\" AccessibleRole=\"Dialog\" AccessibleName=\"Content drawer\">"
"    <VBox>"
"      <HBox Spacing=\"8\" Slot.Padding=\"16,12,12,10\">"
"        <Label Text=\"Content\" TextSize=\"18\" Slot.VAlign=\"Center\" AccessibleRole=\"Heading\"/>"
"        <Button Name=\"drawer.up\" Text=\"Up\" Style=\"Flat\" Tooltip=\"The folder above\" Slot.VAlign=\"Center\"/>"
"        <Label Name=\"drawer.where\" Opacity=\"0.6\" Slot.VAlign=\"Center\" Slot.Fill=\"1\"/>"
"        <TextInput Name=\"drawer.search\" Placeholder=\"Search the project\" Width=\"230\" AccessibleName=\"Search the project\"/>"
"        <Button Name=\"drawer.close\" Text=\"Close\" Style=\"Flat\" Tooltip=\"Escape, or Ctrl+Space\"/>"
"      </HBox>"
"      <Separator/>"
"      <HBox Slot.Fill=\"1\">"
"        <TreeView Name=\"drawer.folders\" Width=\"220\" AccessibleName=\"Folders\"/>"
"        <TileView Name=\"drawer.tiles\" Slot.Fill=\"1\" TileWidth=\"108\" TileHeight=\"112\" IconSize=\"60\" Spacing=\"6\" AccessibleName=\"Files\"/>"
"      </HBox>"
"      <Separator/>"
"      <HBox Spacing=\"12\" Slot.Padding=\"16,7\">"
"        <Label Name=\"drawer.count\" Opacity=\"0.7\" Slot.Fill=\"1\"/>"
"        <Label Text=\"Double click opens  \xC2\xB7  Right click: Preview  \xC2\xB7  Esc or Ctrl+Space closes\" Opacity=\"0.5\"/>"
"      </HBox>"
"    </VBox>"
"  </Border>"
"</Overlay>";

static struct {
	Rid root, panel, tiles, folders, search, where, count;
	Rid icons[KIND_COUNT];
	tile_t tiles_data[MAX_TILES];
	int tile_count, truncated;
	char* folder;        /* the folder shown (when not searching) */
	char** folder_paths; /* the paths behind the items of the folder tree */
	int folder_path_count;
	int syncing;
	/* the menu of a right click on a tile, and the tile it is for */
	Rid menu, menu_open, menu_preview;
	char* menu_path;
	int menu_is_folder;
} g_d = { R_INVALID_ID };

/* ---------------------------------------------------------------------------
 *  Icons, drawn once
 * ------------------------------------------------------------------------- */

static const unsigned g_kind_colors[KIND_COUNT] = {
	0xF5B83D, /* folder */
	0x0A84FF, /* interface with code (.xmlc, .xmlrs...) */
	0x30B0A0, /* XML */
	0x34C759, /* code */
	0xAF52DE, /* build (CMakeLists.txt, Cargo.toml...) */
	0xFF9F0A, /* translations (.csv) */
	0xFF6482, /* pictures */
	0x8E8E93  /* the rest */
};

/* What the icon is at a point: 0 nothing, 1 the body, 2 a lighter part, 3 a line on the body. */
static int icon_part(kind_t kind, float x, float y) {
	if (kind == KIND_FOLDER) {
		int tab = x >= 14.0f && x <= 46.0f && y >= 18.0f && y <= 32.0f;
		int body = x >= 12.0f && x <= 84.0f && y >= 26.0f && y <= 80.0f;
		/* rounded corners of the body */
		float cx = x < 18.0f ? 18.0f : (x > 78.0f ? 78.0f : x), cy = y < 32.0f ? 32.0f : (y > 74.0f ? 74.0f : y);
		if (body && (x - cx) * (x - cx) + (y - cy) * (y - cy) > 36.0f) body = 0;
		if (body) return y < 36.0f ? 2 : 1;
		return tab ? 1 : 0;
	} else {
		float left = 22.0f, right = 74.0f, top = 10.0f, bottom = 86.0f, fold = 18.0f;
		float cx, cy;
		if (x < left || x > right || y < top || y > bottom) return 0;
		/* the folded corner, top right */
		if (x > right - fold && y < top + fold) return (x - (right - fold)) > (y - top) ? 0 : 2;
		cx = x < left + 6.0f ? left + 6.0f : (x > right - 6.0f ? right - 6.0f : x);
		cy = y < top + 6.0f ? top + 6.0f : (y > bottom - 6.0f ? bottom - 6.0f : y);
		if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > 36.0f) return 0;
		/* lines of text */
		if (x >= 32.0f && x <= 64.0f) {
			int i;
			for (i = 0; i < 4; i++) {
				float line = 42.0f + (float)i * 10.0f;
				if (y >= line - 1.6f && y <= line + 1.6f && (i < 3 || x <= 52.0f)) return 3;
			}
		}
		return 1;
	}
}

static Rid make_icon(kind_t kind) {
	unsigned char* pixels = (unsigned char*)calloc((size_t)ICON_SIZE * ICON_SIZE, 4);
	float base[3], light[3];
	int x, y, sx, sy;
	Rid texture;
	base[0] = (float)((g_kind_colors[kind] >> 16) & 255) / 255.0f;
	base[1] = (float)((g_kind_colors[kind] >> 8) & 255) / 255.0f;
	base[2] = (float)(g_kind_colors[kind] & 255) / 255.0f;
	for (x = 0; x < 3; x++) light[x] = base[x] + (1.0f - base[x]) * 0.45f;
	for (y = 0; y < ICON_SIZE; y++) {
		for (x = 0; x < ICON_SIZE; x++) {
			/* four by four samples per pixel: smooth edges */
			float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
			for (sy = 0; sy < 4; sy++) {
				for (sx = 0; sx < 4; sx++) {
					int part = icon_part(kind, (float)x + ((float)sx + 0.5f) / 4.0f, (float)y + ((float)sy + 0.5f) / 4.0f);
					const float* c = part == 2 ? light : base;
					float k = part == 3 ? 0.55f : 1.0f;
					if (!part) continue;
					/* (a line is the body, lightened) */
					r += part == 3 ? c[0] + (1.0f - c[0]) * k : c[0];
					g += part == 3 ? c[1] + (1.0f - c[1]) * k : c[1];
					b += part == 3 ? c[2] + (1.0f - c[2]) * k : c[2];
					a += 1.0f;
				}
			}
			if (a > 0.0f) {
				unsigned char* p = pixels + ((size_t)y * ICON_SIZE + (size_t)x) * 4;
				p[0] = (unsigned char)(r / a * 255.0f + 0.5f);
				p[1] = (unsigned char)(g / a * 255.0f + 0.5f);
				p[2] = (unsigned char)(b / a * 255.0f + 0.5f);
				p[3] = (unsigned char)(a / 16.0f * 255.0f + 0.5f);
			}
		}
	}
	texture = Rtexture_create_from_pixels(pixels, ICON_SIZE, ICON_SIZE);
	free(pixels);
	return texture;
}

static kind_t kind_of(const char* name, int is_dir) {
	const char* ext = ide_extension(name);
	if (is_dir) return KIND_FOLDER;
	if (ide_interface_language(name)) return KIND_INTERFACE;
	if (!strcmp(ext, "xml")) return KIND_XML;
	if (!strcmp(ext, "csv")) return KIND_STRINGS;
	if (ide_iequal(name, "CMakeLists.txt") || ide_iequal(name, "Cargo.toml") || ide_iequal(name, "go.mod") || ide_iequal(name, "build.zig")
		|| !strcmp(ext, "cmake") || !strcmp(ext, "csproj") || !strcmp(ext, "sln")) return KIND_BUILD;
	if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "bmp") || !strcmp(ext, "gif")
		|| !strcmp(ext, "svg") || !strcmp(ext, "ico") || !strcmp(ext, "tga")) return KIND_IMAGE;
	if (!strcmp(ext, "c") || !strcmp(ext, "h") || !strcmp(ext, "cpp") || !strcmp(ext, "hpp") || !strcmp(ext, "cc") || !strcmp(ext, "rs")
		|| !strcmp(ext, "py") || !strcmp(ext, "go") || !strcmp(ext, "cs") || !strcmp(ext, "zig") || !strcmp(ext, "lua")
		|| !strcmp(ext, "js") || !strcmp(ext, "ts") || !strcmp(ext, "glsl") || !strcmp(ext, "frag") || !strcmp(ext, "vert")) return KIND_CODE;
	return KIND_TEXT;
}

/* ---------------------------------------------------------------------------
 *  The tiles
 * ------------------------------------------------------------------------- */

static int skipped(const char* name, int is_dir) {
	if (name[0] == '.') return R_TRUE;
	return is_dir && (ide_iequal(name, "build") || !strncmp(name, "build-", 6) || ide_iequal(name, "out") || ide_iequal(name, "node_modules")
		|| ide_iequal(name, "CMakeFiles") || ide_iequal(name, "target") || ide_iequal(name, "zig-out") || ide_iequal(name, "zig-cache"));
}

static void clear_tiles(void) {
	int i;
	for (i = 0; i < g_d.tile_count; i++) {
		free(g_d.tiles_data[i].name);
		free(g_d.tiles_data[i].path);
		free(g_d.tiles_data[i].detail);
	}
	g_d.tile_count = 0;
	g_d.truncated = R_FALSE;
}

static void add_tile(const char* folder, const char* name, int is_dir, const char* detail) {
	tile_t* tile;
	const char* ext = ide_extension(name);
	size_t i;
	if (g_d.tile_count >= MAX_TILES) {
		g_d.truncated = R_TRUE;
		return;
	}
	tile = &g_d.tiles_data[g_d.tile_count++];
	tile->name = ide_strdup(name);
	tile->path = ide_join(folder, name);
	tile->detail = detail ? ide_strdup(detail) : NULL;
	tile->kind = kind_of(name, is_dir);
	tile->badge[0] = '\0';
	if (!is_dir && ext[0]) {
		for (i = 0; ext[i] && i + 1 < sizeof(tile->badge); i++) tile->badge[i] = (char)toupper((unsigned char)ext[i]);
		tile->badge[i] = '\0';
	}
}

static void on_tile(Rid tile_view, int index, RTileItem* item, void* user_data) {
	const tile_t* tile;
	(void)tile_view;
	(void)user_data;
	if (index < 0 || index >= g_d.tile_count) return;
	tile = &g_d.tiles_data[index];
	item->text = tile->name;
	item->detail = tile->detail;
	item->badge = tile->badge[0] ? tile->badge : NULL;
	item->icon = g_d.icons[tile->kind];
}

/* Where the drawer is, said the short way: the project, then the folders below. */
static void show_where(const char* text) {
	Rlabel_set_text(g_d.where, text);
}

static void show_count(void) {
	char text[96];
	if (!g_project_dir) text[0] = '\0';
	else snprintf(text, sizeof(text), "%d item%s%s", g_d.tile_count, g_d.tile_count == 1 ? "" : "s", g_d.truncated ? " (the first ones)" : "");
	Rlabel_set_text(g_d.count, text);
}

static void relative(const char* path, char* out, size_t size) {
	size_t root = g_project_dir ? strlen(g_project_dir) : 0;
	if (g_project_dir && !strncmp(path, g_project_dir, root) && (path[root] == '/' || path[root] == '\\' || !path[root])) {
		snprintf(out, size, "%s%s", ide_basename(g_project_dir), path + root);
	} else {
		snprintf(out, size, "%s", path);
	}
}

static void show_folder(const char* folder) {
	ide_entry_t* entries = NULL;
	int count, i;
	char where[600];
	clear_tiles();
	if (folder != g_d.folder) {
		free(g_d.folder);
		g_d.folder = ide_strdup(folder);
	}
	count = ide_list_dir(g_d.folder, &entries);
	for (i = 0; i < count; i++) {
		if (!skipped(entries[i].name, entries[i].is_dir)) add_tile(g_d.folder, entries[i].name, entries[i].is_dir, NULL);
	}
	free(entries);
	Rtile_view_set_items(g_d.tiles, g_d.tile_count, on_tile, NULL);
	Rtile_view_set_empty_text(g_d.tiles, count < 0 ? "This folder cannot be read." : "This folder is empty.");
	relative(g_d.folder, where, sizeof(where));
	show_where(where);
	Rwidget_set_enabled(Rwidget_find(g_d.root, "drawer.up"), g_project_dir && !ide_same_path(g_d.folder, g_project_dir));
	show_count();
}

static void search_in(const char* folder, const char* needle, int depth) {
	ide_entry_t* entries = NULL;
	int count, i;
	if (depth > 10 || g_d.truncated) return;
	count = ide_list_dir(folder, &entries);
	for (i = 0; i < count && !g_d.truncated; i++) {
		const char* name = entries[i].name;
		size_t n = strlen(needle), k;
		int found = 0;
		if (skipped(name, entries[i].is_dir)) continue;
		/* (the name contains the text, whatever the case) */
		for (k = 0; !found && name[k]; k++) {
			size_t j;
			for (j = 0; j < n && name[k + j] && tolower((unsigned char)name[k + j]) == tolower((unsigned char)needle[j]); j++) {}
			found = j == n;
		}
		if (found) {
			char where[600];
			relative(folder, where, sizeof(where));
			add_tile(folder, name, entries[i].is_dir, where);
		}
		if (entries[i].is_dir) {
			char* sub = ide_join(folder, name);
			search_in(sub, needle, depth + 1);
			free(sub);
		}
	}
	free(entries);
}

static void show_search(const char* needle) {
	char text[300];
	clear_tiles();
	if (g_project_dir) search_in(g_project_dir, needle, 0);
	Rtile_view_set_items(g_d.tiles, g_d.tile_count, on_tile, NULL);
	Rtile_view_set_empty_text(g_d.tiles, "Nothing of that name in the project.");
	snprintf(text, sizeof(text), "Search: \"%s\" in the whole project", needle);
	show_where(text);
	show_count();
}

/* ---------------------------------------------------------------------------
 *  The folders
 * ------------------------------------------------------------------------- */

static void forget_folders(void) {
	int i;
	for (i = 0; i < g_d.folder_path_count; i++) free(g_d.folder_paths[i]);
	free(g_d.folder_paths);
	g_d.folder_paths = NULL;
	g_d.folder_path_count = 0;
}

static Rid add_folder(Rid parent, const char* path, const char* name) {
	Rid item = Rtree_view_add_item(g_d.folders, parent, name);
	char* copy = ide_strdup(path);
	g_d.folder_paths = (char**)realloc(g_d.folder_paths, sizeof(char*) * (size_t)(g_d.folder_path_count + 1));
	g_d.folder_paths[g_d.folder_path_count++] = copy;
	Rtree_item_set_user_handle(item, copy);
	Rtree_item_set_text_color(item, 0.96f, 0.72f, 0.24f, 1.0f);
	return item;
}

static void fill_folders(Rid parent, const char* folder, int depth) {
	ide_entry_t* entries = NULL;
	int count, i;
	if (depth > 6 || g_d.folder_path_count > 800) return;
	count = ide_list_dir(folder, &entries);
	for (i = 0; i < count; i++) {
		char* path;
		Rid item;
		if (!entries[i].is_dir || skipped(entries[i].name, 1)) continue;
		path = ide_join(folder, entries[i].name);
		item = add_folder(parent, path, entries[i].name);
		Rtree_item_set_expanded(item, depth < 1);
		fill_folders(item, path, depth + 1);
		free(path);
	}
	free(entries);
}

static void refresh_folders(void) {
	Rid root;
	Rtree_view_clear_items(g_d.folders);
	forget_folders();
	if (!g_project_dir) return;
	root = add_folder(R_INVALID_ID, g_project_dir, ide_basename(g_project_dir));
	Rtree_item_set_expanded(root, R_TRUE);
	fill_folders(root, g_project_dir, 0);
}

static void select_folder_item(const char* path) {
	int i;
	(void)i;
	/* (the tree follows the tiles: the item of the folder shown, when there is one) */
	g_d.syncing++;
	Rtree_view_set_selected_item(g_d.folders, R_INVALID_ID);
	{
		/* walk the items: they keep their paths */
		Rid stack[256];
		int top = 0, n;
		for (n = Rtree_view_get_item_count(g_d.folders) - 1; n >= 0 && top < 256; n--) stack[top++] = Rtree_view_get_item(g_d.folders, n);
		while (top > 0) {
			Rid item = stack[--top];
			const char* item_path = (const char*)Rtree_item_get_user_handle(item);
			if (item_path && ide_same_path(item_path, path)) {
				Rtree_view_set_selected_item(g_d.folders, item);
				Rtree_view_scroll_to_item(g_d.folders, item);
				break;
			}
			for (n = Rtree_item_get_child_count(item) - 1; n >= 0 && top < 256; n--) stack[top++] = Rtree_item_get_child(item, n);
		}
	}
	g_d.syncing--;
}

static void go_to(const char* folder) {
	if (Rtext_input_get_text(g_d.search)[0]) {
		g_d.syncing++;
		Rtext_input_set_text(g_d.search, "");
		g_d.syncing--;
	}
	show_folder(folder);
	select_folder_item(folder);
}

/* ---------------------------------------------------------------------------
 *  Opening and closing
 * ------------------------------------------------------------------------- */

int drawer_is_open(void) {
	return g_d.root != R_INVALID_ID && Rwidget_get_visibility(g_d.root) == R_VISIBLE;
}

void drawer_close(void) {
	if (!drawer_is_open()) return;
	Rwidget_set_visibility(g_d.root, R_COLLAPSED);
}

void drawer_open(void) {
	float r, gr, b, a;
	char color[16];
	if (g_d.root == R_INVALID_ID) return;
	/* the surface of the window, whatever the theme is now */
	Rtheme_get_color(R_COLOR_WINDOW, &r, &gr, &b, &a);
	snprintf(color, sizeof(color), "#%02X%02X%02X", (int)(r * 255.0f + 0.5f), (int)(gr * 255.0f + 0.5f), (int)(b * 255.0f + 0.5f));
	Rwidget_set_attribute(g_d.panel, "Background", color);

	refresh_folders();
	if (!g_project_dir) {
		clear_tiles();
		Rtile_view_set_items(g_d.tiles, 0, on_tile, NULL);
		Rtile_view_set_empty_text(g_d.tiles, "No project is open: Project > Open folder, or New project.");
		show_where("");
		show_count();
		Rwidget_set_enabled(Rwidget_find(g_d.root, "drawer.up"), R_FALSE);
	} else {
		/* back where it was, when that is still in the project */
		const char* folder = g_d.folder && ide_is_dir(g_d.folder) && !strncmp(g_d.folder, g_project_dir, strlen(g_project_dir)) ? g_d.folder : g_project_dir;
		char* copy = ide_strdup(folder);
		go_to(copy);
		free(copy);
	}
	Rwidget_set_visibility(g_d.root, R_VISIBLE);
	Rwidget_play_transition(g_d.panel, R_TRANSITION_FLY_RIGHT, 0.0f);
	Rwidget_focus(g_d.search);
}

void drawer_toggle(void) {
	if (drawer_is_open()) drawer_close();
	else drawer_open();
}

static void on_activated(Rid tile_view, int index, void* user_data) {
	char* path;
	(void)tile_view;
	(void)user_data;
	if (index < 0 || index >= g_d.tile_count) return;
	path = ide_strdup(g_d.tiles_data[index].path);
	if (g_d.tiles_data[index].kind == KIND_FOLDER) {
		go_to(path);
	} else {
		drawer_close();
		ide_open_file(path, 0);
	}
	free(path);
}

/* A right click on a tile: Open, and Preview for an interface. */
static void on_tile_context(Rid tile_view, int index, float x, float y, void* user_data) {
	const tile_t* tile;
	int interface;
	(void)tile_view;
	(void)user_data;
	if (index < 0 || index >= g_d.tile_count || g_d.menu == R_INVALID_ID) return;
	tile = &g_d.tiles_data[index];
	free(g_d.menu_path);
	g_d.menu_path = ide_strdup(tile->path);
	g_d.menu_is_folder = tile->kind == KIND_FOLDER;
	interface = tile->kind == KIND_INTERFACE || tile->kind == KIND_XML;
	Rwidget_set_enabled(g_d.menu_preview, interface);
	Rwidget_set_visibility(g_d.menu_preview, g_d.menu_is_folder ? R_COLLAPSED : R_VISIBLE);
	Rmenu_open(g_d.menu, g_ide_window, x, y);
}

static void on_menu(Rid item, void* user_data) {
	char* path;
	(void)item;
	if (!g_d.menu_path) return;
	path = ide_strdup(g_d.menu_path);
	if (g_d.menu_is_folder) {
		go_to(path);
	} else {
		drawer_close();
		if (!strcmp((const char*)user_data, "preview")) ide_preview_file(path);
		else ide_open_file(path, 0);
	}
	free(path);
}

static void on_folder(Rid tree, Rid item, void* user_data) {
	const char* path;
	(void)tree;
	(void)user_data;
	if (g_d.syncing || item == R_INVALID_ID) return;
	path = (const char*)Rtree_item_get_user_handle(item);
	if (path) {
		char* copy = ide_strdup(path);
		if (Rtext_input_get_text(g_d.search)[0]) {
			g_d.syncing++;
			Rtext_input_set_text(g_d.search, "");
			g_d.syncing--;
		}
		show_folder(copy);
		free(copy);
	}
}

static void on_search(Rid input, const char* text, void* user_data) {
	(void)input;
	(void)user_data;
	if (g_d.syncing || !g_project_dir) return;
	if (text && text[0]) show_search(text);
	else if (g_d.folder) show_folder(g_d.folder);
}

static void on_button(Rid button, void* user_data) {
	const char* which = (const char*)user_data;
	(void)button;
	if (!strcmp(which, "close")) {
		drawer_close();
	} else if (!strcmp(which, "up") && g_d.folder && g_project_dir && !ide_same_path(g_d.folder, g_project_dir)) {
		char* up = ide_strdup(g_d.folder);
		char* slash = strrchr(up, '/');
		char* back = strrchr(up, '\\');
		if (back > slash) slash = back;
		if (slash) {
			*slash = '\0';
			go_to(up);
		}
		free(up);
	}
}

/* A click beside the drawer closes it. */
static int on_veil(Rid widget, const REvent* event, void* user_data) {
	(void)widget;
	(void)user_data;
	if (event->type == R_EVENT_MOUSE_DOWN) {
		drawer_close();
		return R_TRUE;
	}
	return R_FALSE;
}

/* Ctrl+Space from anywhere (before the widgets: the code editors take it for their proposals), Escape while it is open. */
static int on_window_event(Rid window, const REvent* event, void* user_data) {
	(void)window;
	(void)user_data;
	if (event->type == R_EVENT_KEY_DOWN && event->key == R_KEY_SPACE && (event->modifiers & R_MOD_CTRL) && !(event->modifiers & (R_MOD_ALT | R_MOD_SHIFT))) {
		if (!event->repeat && g_ide_dialog == R_INVALID_ID) drawer_toggle();
		return R_TRUE;
	}
	/* (the space that comes with Ctrl+Space is not typed) */
	if (event->type == R_EVENT_TEXT && event->codepoint == ' ' && (event->modifiers & R_MOD_CTRL)) return R_TRUE;
	if (event->type == R_EVENT_KEY_DOWN && event->key == R_KEY_ESCAPE && drawer_is_open()) {
		drawer_close();
		return R_TRUE;
	}
	return R_FALSE;
}

static void action_drawer(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	(void)action;
	drawer_toggle();
}

void drawer_init(Rid host) {
	int i;
	memset(&g_d, 0, sizeof(g_d));
	for (i = 0; i < KIND_COUNT; i++) g_d.icons[i] = R_INVALID_ID;
	g_d.menu = g_d.menu_open = g_d.menu_preview = R_INVALID_ID;
	g_d.root = Rwidget_create_from_xml(DRAWER_XML, sizeof(DRAWER_XML) - 1);
	if (g_d.root == R_INVALID_ID) return;
	Rwidget_add_child(host, g_d.root);
	g_d.panel = Rwidget_find(g_d.root, "drawer");
	g_d.tiles = Rwidget_find(g_d.root, "drawer.tiles");
	g_d.folders = Rwidget_find(g_d.root, "drawer.folders");
	g_d.search = Rwidget_find(g_d.root, "drawer.search");
	g_d.where = Rwidget_find(g_d.root, "drawer.where");
	g_d.count = Rwidget_find(g_d.root, "drawer.count");
	for (i = 0; i < KIND_COUNT; i++) g_d.icons[i] = make_icon((kind_t)i);
	Rtile_view_set_activated_callback(g_d.tiles, on_activated, NULL);
	Rtile_view_set_context_callback(g_d.tiles, on_tile_context, NULL);
	g_d.menu = Rwidget_create(R_WIDGET_MENU);
	g_d.menu_open = Rmenu_add_item(g_d.menu, "Open");
	g_d.menu_preview = Rmenu_add_item(g_d.menu, "Preview");
	Rmenu_item_set_clicked_callback(g_d.menu_open, on_menu, (void*)"open");
	Rmenu_item_set_clicked_callback(g_d.menu_preview, on_menu, (void*)"preview");
	Rtree_view_set_selection_changed_callback(g_d.folders, on_folder, NULL);
	Rtext_input_set_changed_callback(g_d.search, on_search, NULL);
	Rbutton_set_clicked_callback(Rwidget_find(g_d.root, "drawer.close"), on_button, (void*)"close");
	Rbutton_set_clicked_callback(Rwidget_find(g_d.root, "drawer.up"), on_button, (void*)"up");
	Rwidget_set_event_callback(Rwidget_find(g_d.root, "drawer.veil"), on_veil, NULL);
	Rwindow_set_event_hook(g_ide_window, on_window_event, NULL);
	Raction_bind("drawer.toggle", action_drawer, NULL);
}

void drawer_shutdown(void) {
	int i;
	clear_tiles();
	forget_folders();
	free(g_d.folder);
	g_d.folder = NULL;
	free(g_d.menu_path);
	g_d.menu_path = NULL;
	for (i = 0; i < KIND_COUNT; i++) {
		if (g_d.icons[i] != R_INVALID_ID) Rtexture_destroy(g_d.icons[i]);
		g_d.icons[i] = R_INVALID_ID;
	}
}
