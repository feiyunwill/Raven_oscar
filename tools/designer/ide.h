/**
 * Raven Designer - ide.h
 * What the parts of the designer share. main.c is the visual editor of an
 * interface; the other files make it an IDE around a CMake project:
 *
 *   os.c        folders, files and programs (CMake, the compiler, the program)
 *   project.c   the project: its folder, New project (with a Raven interface, or native), settings
 *   editors.c   code editors (C, CMake, any text, the interfaces too) in tabs, and completion
 *   xmlc.c      .xmlc files: their <script> blocks and their interface
 *   build.c     configure, build, run, the Output panel and the build problems
 *   strings.c   the Translations panel (CSV tables) and the language of the preview
 *   welcome.c   the start page (recent projects) and the preferences of the IDE
 *   island.c    the island at the top: the state of the build, and the preview of the interface being typed
 *   drawer.c    the content drawer (Ctrl+Space); a right click on an interface: Preview (the visual editor)
 */

#ifndef RAVEN_IDE_H
#define RAVEN_IDE_H

#include <Raven.h>

#include <stdarg.h>
#include <stddef.h>

/* ---- os.c ---- */

typedef struct ide_entry_t {
	char name[256];
	int is_dir;
} ide_entry_t;

/** The entries of a folder, folders first then by name; free with free(). Returns the count, -1 when it cannot be read. */
int  ide_list_dir(const char* path, ide_entry_t** entries);
int  ide_is_dir(const char* path);
int  ide_file_exists(const char* path);
/** Makes a folder and the folders above it. */
int  ide_make_dirs(const char* path);
/** Seconds since 1970 of the last change of a file, 0 when missing. */
double ide_file_time(const char* path);
/** The folder of the program itself, with no slash at the end. */
const char* ide_exe_dir(void);
/** "a/b" + "c" -> "a/b/c" (malloc). */
char* ide_join(const char* a, const char* b);
/** The part after the last slash. */
const char* ide_basename(const char* path);
/** The extension, lowercase, without the dot ("" when none). */
const char* ide_extension(const char* path);
/** Same file names, whatever the slashes (and the case on Windows). */
int  ide_same_path(const char* a, const char* b);
/** A copy with forward slashes. */
char* ide_normalize(const char* path);

typedef struct ide_process_t ide_process_t;
/** Starts a program, its standard output and errors merged in one pipe. NULL when it cannot start. */
ide_process_t* ide_process_start(const char* const* argv, const char* directory);
/** What the program wrote since the last call (never waits). Returns the bytes read, 0 when nothing, -1 at the end. */
int  ide_process_read(ide_process_t* process, char* buffer, int size);
/** R_TRUE once the program has ended; its exit code then. */
int  ide_process_done(ide_process_t* process, int* exit_code);
void ide_process_kill(ide_process_t* process);
void ide_process_free(ide_process_t* process);
/** Puts a folder first in the PATH of the programs started from now on. */
void ide_prepend_path(const char* folder);
/** R_TRUE when a program of that name is found in the PATH. */
int  ide_find_program(const char* name, char* out, size_t size);
/** The folder of the user (forward slashes). */
const char* ide_home_dir(void);
/** Where the IDE keeps its preferences and its recent projects (made when missing). */
const char* ide_config_dir(void);
/** Where New project proposes to put projects the first time: Documents/Raven Projects. */
const char* ide_default_projects_dir(void);
/** A path for the eyes: the folder of the user becomes "~". */
void ide_pretty_path(const char* path, char* out, size_t size);
int  ide_remove_file(const char* path);

/* ---- main.c (the visual editor) ---- */

extern Rid g_ide_window;
extern Rid g_ide_dock;
/** The content of the dialog the IDE runs now (the scripts of the tests look in it), or R_INVALID_ID. */
extern Rid g_ide_dialog;
/** The dock panel the code editors share tabs with. */
extern Rid g_ide_center;
void ide_status(const char* format, ...);
/** Opens an interface in the visual editor: the text of a .xml, or of the interface part of a .xmlc. */
void designer_load_interface(const char* text, const char* path, int is_xmlc);
/** The text the visual editor holds now (what is written in its XML panel). */
const char* designer_get_interface_text(void);
/** The file the visual editor shows, or NULL. */
const char* designer_get_path(void);
int  designer_is_modified(void);
void designer_set_modified(int modified);
/** The Name attributes of the interface (for ui.<name> in the scripts). */
int  designer_get_names(const char** names, int max);
/** Opens a .xml file in the visual editor; goes to a line of its XML panel. */
void designer_open_file(const char* path);
void designer_go_to_line(int line);
/** A mark of the compiler on a line of the XML panel (an .xmlc: a line of the file). */
void designer_add_marker(int line, int column, int error, const char* message);
/** Builds the preview again (after the translations or the language changed). */
void designer_refresh_preview(void);
/** The frame of the preview: the strings panel sets its direction. */
Rid  designer_get_frame(void);
/** Saves what the visual editor holds into its file (for .xmlc: with its scripts). */
int  designer_save(void);
/** The title of the window, after what it names changed (the project, the page). */
void designer_update_title(void);
/** No interface is shown any more: the visual editor says how to open one. */
void designer_close(void);
/** R_TRUE when the visual editor shows an interface. */
int  designer_has_document(void);
/** The problems of the interface (the Problems panel). */
int  designer_problem_count(void);
/** Closes the visual editor (its panels go): its file goes back to a code editor, unsaved changes included. R_FALSE when the user cancels. */
int  designer_close_preview(void);

/* ---- island.c: the island at the top of the window (project, file, build, problems) ---- */

void island_init(void);
void island_shutdown(void);
/** Shows what changed now (else it follows four times a second). */
void island_refresh(void);
/** The preview of the island is built again (the translations or the language of the preview changed). */
void island_preview_changed(void);
/** An interface was opened as code: the island unfolds a moment on its preview. */
void island_show_preview(void);
/** What the preview of the island shows (scripts of the tests), or R_INVALID_ID. */
Rid  island_preview_widget(void);

/* ---- drawer.c: the content drawer (Ctrl+Space), the files of the project as tiles ---- */

/** Puts the drawer in `host` (an Overlay over the pages of the IDE). */
void drawer_init(Rid host);
void drawer_shutdown(void);
void drawer_open(void);
void drawer_close(void);
void drawer_toggle(void);
int  drawer_is_open(void);

/* ---- project.c ---- */

typedef struct ide_settings_t {
	char build_type[32];      /* Debug, Release... */
	char raven_prefix[1024];  /* where find_package(raven) looks: the Raven package */
	char generator[128];      /* "" : the default of CMake */
	char run_arguments[512];
} ide_settings_t;

extern char* g_project_dir;    /* NULL when no project is open */
extern ide_settings_t g_settings;

void project_init(void);
void project_shutdown(void);
int  project_open(const char* folder);
/**
 * Creates a project in a language ("C", "C++", "Rust", "Python", "Go", "C#", "Zig", "Lua") and opens it.
 * with_interface: R_TRUE, an interface with its code (main.xmlc, main.xmlrs...) and strings.csv;
 * R_FALSE, a native project: the language alone and a classic Hello world.
 */
int  project_create(const char* parent_folder, const char* name, const char* language, int with_interface);
void project_refresh_explorer(void);
void project_save_settings(void);
/**
 * Opens any file of the project where it belongs: a code editor (an interface too: the island is its
 * preview; the visual editor when it already shows it) or the Translations panel.
 */
void ide_open_file(const char* path, int line);
/** Opens an interface (.xml, .xmlc...) in the visual editor: the Preview of the content drawer. */
void ide_preview_file(const char* path);
/** The name of the first add_executable of the CMakeLists.txt of the project. */
int  project_target_name(char* out, size_t size);
void project_new(void);
void project_settings_dialog(void);
/** Closes the project (asks about unsaved changes first): R_FALSE when the user cancels. */
int  project_close(void);
/** Unsaved changes anywhere: asks to save them. R_FALSE when the user cancels. */
int  ide_confirm_unsaved(const char* why);

/* ---- editors.c ---- */

void editors_init(void);
void editors_shutdown(void);
/** Opens a file in a code editor tab (or shows it), and goes to a line (1-based, 0: stays). */
Rid  editors_open(const char* path, int line);
/** Opens a file in a code editor with a text of its own (what the visual editor held), marked modified or not. */
Rid  editors_open_text(const char* path, const char* text, int modified);
/** Closes the code editor of a file, without asking. */
void editors_close_file(const char* path);
int  editors_is_modified(Rid editor);
/** The file of the code editor in front (the keyboard is in it, or it was last), or NULL. */
const char* editors_current_path(void);
/** The same when it is an interface (.xml, .xmlc...): its text now, and a number that changes with it. */
int  editors_current_interface(const char** path, const char** text, unsigned* revision);
/** Where a new code editor goes (an editor, the preview), or R_INVALID_ID: above the Output. */
Rid  editors_anchor(void);
/** The code editors go back together, in the tabs of the preview (after the panels were reset). */
void editors_regroup(void);
/** An editor follows the zoom of the IDE (Ctrl + wheel in one zooms them all; kept in ide.ini). The code editors do by themselves. */
void editors_zoom_attach(Rid editor);
/** Every editor back to 100 %. */
void editors_reset_zoom(void);
/** Opens a piece of text with a name (a script of a .xmlc), written back through `save`. */
typedef int (*ide_save_callback)(Rid editor, void* user_data);
Rid  editors_open_virtual(const char* title, const char* key, const char* text, const char* language, ide_save_callback save, void* user_data);
Rid  editors_find(const char* key);
/** A tab whose title contains `text` (scripts of the tests). */
Rid  editors_find_title(const char* text);
int  editors_save_all(void);
/** Closes every code editor (without asking: ask before). */
void editors_close_all(void);
/** An editor was closed by its dock area without asking: forget it. */
void editors_forget_closed(void);
/** Saves the editor the keyboard is in: R_TRUE / R_FALSE, -1 when it is not in a code editor. */
int  editors_save_focused(void);
int  editors_any_modified(void);
/** Puts the build problems on the editors that show their files. */
void editors_clear_markers(void);
void editors_add_marker(const char* path, int line, int column, int error, const char* message);
void editors_set_modified(Rid editor, int modified);
/** Completion shared with the XML panel of the visual editor. */
void completion_attach(Rid editor, int xml);
void completion_load_raven_api(void);

/* ---- xmlc.c ---- */

/** Opens a .xmlc (or .xmlrs, .xmlpy...) whole in the XML panel; the visual editor shows its interface element. */
int  xmlc_open(const char* path);
/** The same with a text instead of the file (NULL: the file). */
int  xmlc_open_text(const char* path, const char* text);
/** Writes the .xmlc back: the text of the XML panel, scripts and interface together. */
int  xmlc_save(void);
/** Where the interface element of the text of an .xmlc is (offsets, and its first line): 1, 0 when there is none, -1 when an element is not closed. */
int  xmlc_find_interface(const char* text, size_t* start, size_t* end, int* line);
int  xmlc_is_open(void);
const char* xmlc_path(void);
void xmlc_close(void);
/** The line of the interface element in the file. */
int  xmlc_interface_line(void);

/* ---- build.c ---- */

void build_init(void);
void build_shutdown(void);
int  build_is_running(void);
typedef struct build_summary_t {
	int running;      /* a build (or the program) is under way */
	int program;      /* what runs is the program itself */
	double started;   /* Rget_time() when it started */
	int has_result;   /* a build ended since the IDE started */
	int ok;           /* how the last one ended */
	double finished;
	int errors, warnings;
} build_summary_t;
/** What the island shows: the build under way, or how the last one ended. */
void build_get_summary(build_summary_t* summary);
/** Stops what runs, empties the Output and the Build panel (another project). */
void build_reset(void);

/* ---- strings.c ---- */

void strings_init(void);
void strings_shutdown(void);
/** Reads the CSV tables of the project (after it was opened, or a table saved). */
void strings_reload(void);
/** Opens a table in the Translations panel. */
void strings_open(const char* path);
int  strings_any_modified(void);
int  strings_save(void);
/** The language of the preview ("" : the first column). */
void strings_set_preview_language(const char* code);
/** Writes a cell (scripts of the tests). */
int  strings_set_cell(const char* key, int column, const char* text);
/** The keys of the tables (completion of @keys). */
int  strings_key_count(void);
const char* strings_key(int index);

/* ---- welcome.c ---- */

void welcome_init(void);
void welcome_shutdown(void);
/** The start page: New project, Open, the recent projects. */
void welcome_show(void);
/** The panels of the IDE (a project or a file is open). */
void welcome_hide(void);
int  welcome_is_shown(void);
/** A project was opened: first in the list of recent projects. */
void recent_add(const char* folder);
/** A preference of the IDE (ide.ini in ide_config_dir), "" when not set. */
const char* prefs_get(const char* key);
void prefs_set(const char* key, const char* value);

/* ---- accessibility.c ---- */

void accessibility_init(void);
void accessibility_shutdown(void);
/** The settings kept from the last time (ide.ini, a11y.*), applied at start. */
void accessibility_apply_saved(void);
/** View > Accessibility: every setting, live. */
void accessibility_window(void);
/** The menu follows the settings (Reduce motion). */
void accessibility_sync_menu(void);

/* ---- the languages of the interfaces with code (.xmlc, .xmlrs...) ---- */

typedef struct ide_language_t {
	const char* extension;   /* "xmlrs" */
	const char* name;        /* "Rust": what New project shows */
	const char* editor;      /* the language of the script tabs */
} ide_language_t;
/** The language of an interface file with code, or NULL for any other file (a plain .xml included). */
const ide_language_t* ide_interface_language(const char* path);
const ide_language_t* ide_language_at(int index);
int  ide_language_count(void);
/** Sets a variable of the environment of the programs started from now on. */
void ide_set_env(const char* name, const char* value);

/* ---- small things ---- */

char* ide_strdup(const char* text);
int   ide_iequal(const char* a, const char* b);
/** Appends to a growing buffer. */
typedef struct ide_buffer_t {
	char* data;
	size_t size, cap;
} ide_buffer_t;
void ide_buffer_add(ide_buffer_t* buffer, const char* text, size_t length);
void ide_buffer_printf(ide_buffer_t* buffer, const char* format, ...);
void ide_buffer_free(ide_buffer_t* buffer);

#endif
