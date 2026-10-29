/**
 * Raven Designer - build.c
 * Builds the project with the CMake and the compiler installed on the
 * machine: configure, build, run, stop. What they write goes to the Output
 * panel; the errors and warnings (GCC, Clang, MSVC, CMake, ravenc) go to the
 * Build panel, and on the lines of the editors. A double click opens the
 * file at the line, the .xmlc included (the compiler sees its lines).
 */

#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS 4
#define MAX_ARGS 24
#define MAX_ISSUES 400

typedef struct step_t {
	char* argv[MAX_ARGS + 1];
	char* directory;
	int run;     /* the program itself: its output is not read for errors */
	int find;    /* argv[0] is the program CMake makes: looked for when the step starts */
} step_t;

/* How a project is built: from the files it has. */
typedef enum { KIND_NONE, KIND_CMAKE, KIND_CARGO, KIND_GO, KIND_DOTNET, KIND_ZIG, KIND_PYTHON, KIND_LUA } kind_t;

typedef struct issue_t {
	char* file;
	int line, column, error;
	char* message;
} issue_t;

static struct {
	Rid output, issues_list, timer;
	ide_process_t* process;
	step_t steps[MAX_STEPS];
	int step_count, step_at;
	ide_buffer_t pending;      /* the end of a line not complete yet */
	issue_t issues[MAX_ISSUES];
	int issue_count, error_count, warning_count;
	double started;
	char configured_with[2048];
	kind_t kind;
	/* rustc: "error[E0308]: ..." comes before the line that says where */
	char pending_message[512];
	int pending_error;
	/* said in the Output once it is cleared, when the build starts */
	char note[512];
	/* CMake said it could not find the package of Raven */
	int package_missing;
	/* the last build that ended (the island shows it) */
	int has_result, last_ok;
	double finished;
} g_b;

int build_is_running(void) {
	return g_b.process != NULL || g_b.step_at < g_b.step_count;
}

void build_get_summary(build_summary_t* summary) {
	memset(summary, 0, sizeof(*summary));
	summary->running = build_is_running();
	/* the step under way is the program itself: it runs, it is not building */
	summary->program = g_b.process && g_b.step_at < g_b.step_count && g_b.steps[g_b.step_at].run;
	summary->started = g_b.started;
	summary->has_result = g_b.has_result;
	summary->ok = g_b.last_ok;
	summary->finished = g_b.finished;
	summary->errors = g_b.error_count;
	summary->warnings = g_b.warning_count;
}

/* ---------------------------------------------------------------------------
 *  Output
 * ------------------------------------------------------------------------- */

static void output_append(const char* text, size_t length) {
	int lines;
	char* copy;
	if (!length) return;
	copy = (char*)malloc(length + 1);
	memcpy(copy, text, length);
	copy[length] = 0;
	Rtext_editor_set_read_only(g_b.output, R_FALSE);
	lines = Rtext_editor_get_line_count(g_b.output);
	{
		const char* last = Rtext_editor_get_line(g_b.output, lines - 1);
		Rtext_editor_set_cursor(g_b.output, lines - 1, last ? (int)strlen(last) : 0);
	}
	Rtext_editor_insert_text(g_b.output, copy);
	Rtext_editor_set_read_only(g_b.output, R_TRUE);
	Rtext_editor_scroll_to_line(g_b.output, Rtext_editor_get_line_count(g_b.output) - 1);
	free(copy);
}

static void output_printf(const char* format, ...) {
	char text[2048];
	va_list args;
	int n;
	va_start(args, format);
	n = vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (n > 0) output_append(text, (size_t)(n < (int)sizeof(text) ? n : (int)sizeof(text) - 1));
}

static void output_clear(void) {
	Rtext_editor_set_read_only(g_b.output, R_FALSE);
	Rtext_editor_set_text(g_b.output, "");
	Rtext_editor_set_read_only(g_b.output, R_TRUE);
}

/* ---------------------------------------------------------------------------
 *  Problems in what the tools write
 * ------------------------------------------------------------------------- */

static void clear_issues(void) {
	int i;
	for (i = 0; i < g_b.issue_count; i++) {
		free(g_b.issues[i].file);
		free(g_b.issues[i].message);
	}
	g_b.issue_count = g_b.error_count = g_b.warning_count = 0;
	Rlist_view_clear_items(g_b.issues_list);
	editors_clear_markers();
}

static char* find_in(const char* folder, const char* name, int depth) {
	ide_entry_t* entries;
	int count = ide_list_dir(folder, &entries), i;
	char* found = NULL;
	for (i = 0; i < count && !found; i++) if (!entries[i].is_dir && !strcmp(entries[i].name, name)) found = ide_join(folder, name);
	for (i = 0; i < count && !found && depth > 0; i++) {
		if (entries[i].is_dir && entries[i].name[0] != '.' && strcmp(entries[i].name, "build") && strcmp(entries[i].name, "target")
			&& strcmp(entries[i].name, "obj") && strcmp(entries[i].name, "bin") && strcmp(entries[i].name, "zig-out") && strcmp(entries[i].name, "node_modules")) {
			char* sub = ide_join(folder, entries[i].name);
			found = find_in(sub, name, depth - 1);
			free(sub);
		}
	}
	free(entries);
	return found;
}

/*
 * A file ravenc made (main.xmlrs.rs, main.xmlgo.go, main.xmlcs.cs...) is the
 * .xmlrs of the project: the scripts keep their lines in it.
 */
static char* source_of_generated(const char* file) {
	char name[300];
	const char* base = ide_basename(file);
	char* dot;
	snprintf(name, sizeof(name), "%s", base);
	dot = strrchr(name, '.');
	if (!dot || !g_project_dir) return NULL;
	*dot = 0;
	if (!ide_interface_language(name)) return NULL;
	return find_in(g_project_dir, name, 4);
}

/* A path the tool wrote, as a file that exists (they are often relative to the build folder). */
static char* resolve_file(const char* file) {
	char* candidate;
	char* source = source_of_generated(file);
	if (source) {
		char* n = ide_normalize(source);
		free(source);
		return n;
	}
	if (ide_file_exists(file)) return ide_normalize(file);
	if (g_project_dir) {
		char* build = ide_join(g_project_dir, "build");
		candidate = ide_join(build, file);
		free(build);
		if (ide_file_exists(candidate)) {
			char* n = ide_normalize(candidate);
			free(candidate);
			return n;
		}
		free(candidate);
		candidate = ide_join(g_project_dir, file);
		if (ide_file_exists(candidate)) {
			char* n = ide_normalize(candidate);
			free(candidate);
			return n;
		}
		free(candidate);
	}
	return ide_normalize(file);
}

static int read_number(const char** p) {
	int n = 0;
	if (!isdigit((unsigned char)**p)) return -1;
	while (isdigit((unsigned char)**p)) n = n * 10 + (*(*p)++ - '0');
	return n;
}

static int kind_at(const char* p, int* error, const char** message) {
	while (*p == ' ') p++;
	if (!strncmp(p, "fatal error", 11)) { *error = 1; p += 11; }
	else if (!strncmp(p, "error", 5)) { *error = 1; p += 5; }
	else if (!strncmp(p, "warning", 7)) { *error = 0; p += 7; }
	else return 0;
	/* MSVC: "error C2065: ..."; GCC: "error: ..." */
	while (*p && *p != ':') p++;
	if (*p == ':') p++;
	while (*p == ' ') p++;
	*message = p;
	return 1;
}

static void add_issue(const char* file, size_t file_length, int line, int column, int error, const char* message) {
	issue_t* issue;
	char* raw;
	char row[700];
	if (g_b.issue_count >= MAX_ISSUES || file_length == 0) return;
	raw = (char*)malloc(file_length + 1);
	memcpy(raw, file, file_length);
	raw[file_length] = 0;
	issue = &g_b.issues[g_b.issue_count++];
	issue->file = resolve_file(raw);
	free(raw);
	issue->line = line;
	issue->column = column;
	issue->error = error;
	issue->message = ide_strdup(message);
	if (error) g_b.error_count++;
	else g_b.warning_count++;
	snprintf(row, sizeof(row), "%s  %s:%d  %s", error ? "error" : "warning", ide_basename(issue->file), line, message);
	Rlist_view_add_item(g_b.issues_list, row);
	editors_add_marker(issue->file, line, column, error, message);
}

static void parse_line(const char* line) {
	const char* p;
	const char* message;
	int error;
	/* rustc: "error[E0425]: cannot find value", then " --> src/main.rs:12:5" */
	if (g_b.kind == KIND_CARGO) {
		if (!strncmp(line, "error", 5) || !strncmp(line, "warning", 7)) {
			const char* colon = strchr(line, ':');
			g_b.pending_error = line[0] == 'e';
			snprintf(g_b.pending_message, sizeof(g_b.pending_message), "%s", colon ? colon + 1 + (colon[1] == ' ') : line);
			/* (the summary lines have no place) */
			if (strstr(line, "could not compile") || strstr(line, "generated ") || strstr(line, "aborting")) g_b.pending_message[0] = 0;
			return;
		}
		p = strstr(line, "--> ");
		if (p && g_b.pending_message[0]) {
			const char* file = p + 4;
			const char* q;
			for (q = file + 1; (q = strchr(q, ':')) != NULL; q++) {
				const char* r = q + 1;
				int n = read_number(&r), column = 0;
				if (n <= 0) continue;
				if (*r == ':') {
					r++;
					column = read_number(&r);
				}
				add_issue(file, (size_t)(q - file), n, column > 0 ? column : 0, g_b.pending_error, g_b.pending_message);
				break;
			}
			g_b.pending_message[0] = 0;
			return;
		}
	}
	/* go: "./main.xmlgo.go:12:5: undefined: x" (no word for the kind: an error) */
	if (g_b.kind == KIND_GO && line[0] != '#') {
		for (p = line + 1; (p = strchr(p, ':')) != NULL; p++) {
			const char* q = p + 1;
			int n = read_number(&q), column = 0;
			if (n <= 0 || *q != ':') continue;
			q++;
			{
				const char* r = q;
				int c = read_number(&r);
				if (c > 0 && *r == ':') {
					column = c;
					q = r + 1;
				}
			}
			while (*q == ' ') q++;
			if (column > 0) {
				add_issue(line, (size_t)(p - line), n, column, 1, q);
				return;
			}
			break;
		}
	}
	/* find_package(raven) failed: the IDE says what to do about it at the end */
	if (strstr(line, "ravenConfig.cmake") || strstr(line, "raven-config.cmake")) g_b.package_missing = R_TRUE;
	/* CMake Error at CMakeLists.txt:12 (add_executable): */
	if (!strncmp(line, "CMake Error at ", 15) || !strncmp(line, "CMake Warning at ", 17)) {
		const char* file = line + (line[6] == 'E' ? 15 : 17);
		const char* colon = strrchr(file, ':');
		if (colon) {
			const char* q = colon + 1;
			int n = read_number(&q);
			if (n > 0) add_issue(file, (size_t)(colon - file), n, 0, line[6] == 'E', line[6] == 'E' ? "CMake error (see the Output)" : "CMake warning (see the Output)");
		}
		return;
	}
	/* MSVC: file(12,5): error C2065: ... or file(12): warning C4100: ... */
	p = strchr(line, '(');
	while (p) {
		const char* q = p + 1;
		int n = read_number(&q), column = 0;
		if (n > 0) {
			if (*q == ',') {
				q++;
				column = read_number(&q);
			}
			if (*q == ')' && q[1] == ':' && kind_at(q + 2, &error, &message)) {
				const char* start = line;
				while (*start == ' ') start++;
				add_issue(start, (size_t)(p - start), n, column > 0 ? column : 0, error, message);
				return;
			}
		}
		p = strchr(p + 1, '(');
	}
	/* GCC, Clang, ravenc: file:12:5: error: ... or file:12: error: ... (C:\ is a drive, not a line) */
	for (p = line + 1; (p = strchr(p, ':')) != NULL; p++) {
		const char* q = p + 1;
		int n = read_number(&q), column = 0;
		if (n <= 0 || *q != ':') continue;
		q++;
		{
			const char* r = q;
			int c = read_number(&r);
			if (c > 0 && *r == ':') {
				column = c;
				q = r + 1;
			}
		}
		if (kind_at(q, &error, &message)) {
			const char* start = line;
			while (*start == ' ') start++;
			add_issue(start, (size_t)(p - start), n, column, error, message);
		}
		return;
	}
}

static void take_output(const char* text, size_t length) {
	size_t i, start = 0;
	output_append(text, length);
	ide_buffer_add(&g_b.pending, text, length);
	for (i = 0; i < g_b.pending.size; i++) {
		if (g_b.pending.data[i] == '\n') {
			g_b.pending.data[i] = 0;
			if (i > start && g_b.pending.data[i - 1] == '\r') g_b.pending.data[i - 1] = 0;
			if (!g_b.steps[g_b.step_at < g_b.step_count ? g_b.step_at : 0].run) parse_line(g_b.pending.data + start);
			start = i + 1;
		}
	}
	if (start) {
		memmove(g_b.pending.data, g_b.pending.data + start, g_b.pending.size - start);
		g_b.pending.size -= start;
		g_b.pending.data[g_b.pending.size] = 0;
	}
}

static void on_issue_activated(Rid list, int index, void* user_data) {
	(void)list;
	(void)user_data;
	if (index < 0 || index >= g_b.issue_count) return;
	ide_open_file(g_b.issues[index].file, g_b.issues[index].line);
}

/* ---------------------------------------------------------------------------
 *  Steps
 * ------------------------------------------------------------------------- */

static void free_steps(void) {
	int i, j;
	for (i = 0; i < g_b.step_count; i++) {
		for (j = 0; g_b.steps[i].argv[j]; j++) free(g_b.steps[i].argv[j]);
		free(g_b.steps[i].directory);
	}
	memset(g_b.steps, 0, sizeof(g_b.steps));
	g_b.step_count = g_b.step_at = 0;
}

static step_t* add_step(const char* directory) {
	step_t* step;
	if (g_b.step_count >= MAX_STEPS) return NULL;
	step = &g_b.steps[g_b.step_count++];
	memset(step, 0, sizeof(*step));
	step->directory = ide_strdup(directory);
	return step;
}

static void add_arg(step_t* step, const char* format, ...) {
	char text[2048];
	va_list args;
	int i;
	for (i = 0; i < MAX_ARGS && step->argv[i]; i++) {}
	if (i >= MAX_ARGS) return;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	step->argv[i] = ide_strdup(text);
}

static void finish(int ok) {
	double seconds = Rget_time() - g_b.started;
	g_b.has_result = R_TRUE;
	g_b.last_ok = ok;
	g_b.finished = Rget_time();
	if (g_b.timer != R_INVALID_ID && Rtimer_is_valid(g_b.timer)) Rtimer_destroy(g_b.timer);
	g_b.timer = R_INVALID_ID;
	free_steps();
	if (g_b.error_count || g_b.warning_count) {
		char title[64];
		snprintf(title, sizeof(title), "Build (%d)", g_b.error_count + g_b.warning_count);
		Rdock_area_set_panel_title(g_ide_dock, g_b.issues_list, title);
	} else {
		Rdock_area_set_panel_title(g_ide_dock, g_b.issues_list, "Build");
	}
	if (g_b.error_count) Rdock_area_open_panel(g_ide_dock, g_b.issues_list);
	if (!ok && g_b.package_missing) {
		output_printf("\n[CMake did not find the Raven package. Choose its folder (the one with bin/, include/ and lib/) in Project > Build settings > "
			"Raven package, or on the start page (File > Welcome page).]\n");
		ide_status("The Raven package was not found: Project > Build settings > Raven package.");
		return;
	}
	ide_status(ok ? "Done in %.1f s (%d warning%s)." : "Failed after %.1f s: %d error%s, see the Build panel.", seconds,
		ok ? g_b.warning_count : g_b.error_count, (ok ? g_b.warning_count : g_b.error_count) == 1 ? "" : "s");
}

static void start_step(void);
static void refresh_run_step(void);

static void on_poll(Rid timer, void* user_data) {
	char buffer[8192];
	int n, code = 0, guard = 0;
	(void)timer;
	(void)user_data;
	if (!g_b.process) return;
	while ((n = ide_process_read(g_b.process, buffer, sizeof(buffer))) > 0 && guard++ < 64) take_output(buffer, (size_t)n);
	if (ide_process_done(g_b.process, &code)) {
		while ((n = ide_process_read(g_b.process, buffer, sizeof(buffer))) > 0) take_output(buffer, (size_t)n);
		if (g_b.pending.size) {
			take_output("\n", 1);
		}
		ide_process_free(g_b.process);
		g_b.process = NULL;
		if (g_b.steps[g_b.step_at].run) output_printf("\n[the program ended with code %d]\n", code);
		else if (code != 0) output_printf("\n[%s failed with code %d]\n", g_b.steps[g_b.step_at].argv[0], code);
		if (code != 0 && !g_b.steps[g_b.step_at].run) {
			finish(R_FALSE);
			return;
		}
		g_b.step_at++;
		if (g_b.step_at < g_b.step_count) start_step();
		else finish(R_TRUE);
	}
}

static void start_step(void) {
	step_t* step;
	int i;
	refresh_run_step();
	step = &g_b.steps[g_b.step_at];
	output_printf("%s>", step->run ? "\n" : "");
	for (i = 0; step->argv[i]; i++) output_printf(" %s", step->argv[i]);
	output_printf("\n");
	g_b.process = ide_process_start((const char* const*)step->argv, step->directory);
	if (!g_b.process) {
		output_printf("cannot start %s (is it installed and in the PATH?)\n", step->argv[0]);
		finish(R_FALSE);
		return;
	}
	if (g_b.timer == R_INVALID_ID) g_b.timer = Rtimer_create(0.05, R_TRUE, on_poll, NULL);
}

static void begin(void) {
	g_b.started = Rget_time();
	g_b.step_at = 0;
	g_b.pending.size = 0;
	g_b.package_missing = R_FALSE;
	clear_issues();
	output_clear();
	if (g_b.note[0]) {
		output_printf("%s\n", g_b.note);
		g_b.note[0] = 0;
	}
	Rdock_area_open_panel(g_ide_dock, g_b.output);
	start_step();
}

void build_reset(void) {
	if (g_b.process) {
		ide_process_kill(g_b.process);
		ide_process_free(g_b.process);
		g_b.process = NULL;
	}
	if (g_b.timer != R_INVALID_ID && Rtimer_is_valid(g_b.timer)) Rtimer_destroy(g_b.timer);
	g_b.timer = R_INVALID_ID;
	free_steps();
	clear_issues();
	output_clear();
	g_b.configured_with[0] = 0;
	g_b.note[0] = 0;
	Rdock_area_set_panel_title(g_ide_dock, g_b.issues_list, "Build");
}

/* ---------------------------------------------------------------------------
 *  What the commands do
 * ------------------------------------------------------------------------- */

static int has_file(const char* name) {
	char* path = ide_join(g_project_dir, name);
	int ok = ide_file_exists(path);
	free(path);
	return ok;
}

/* The first file of the project with that extension (its top folder), or NULL. */
static char* first_with_extension(const char* extension) {
	ide_entry_t* entries;
	int count = ide_list_dir(g_project_dir, &entries), i;
	char* found = NULL;
	for (i = 0; i < count && !found; i++) {
		if (!entries[i].is_dir && ide_iequal(ide_extension(entries[i].name), extension)) found = ide_strdup(entries[i].name);
	}
	free(entries);
	return found;
}

static kind_t project_kind(void) {
	char* file;
	if (!g_project_dir) return KIND_NONE;
	if (has_file("Cargo.toml")) return KIND_CARGO;
	if (has_file("go.mod")) return KIND_GO;
	if ((file = first_with_extension("csproj")) != NULL) {
		free(file);
		return KIND_DOTNET;
	}
	if (has_file("build.zig")) return KIND_ZIG;
	if (has_file("CMakeLists.txt")) return KIND_CMAKE;
	if ((file = first_with_extension("xmlpy")) != NULL) {
		free(file);
		return KIND_PYTHON;
	}
	if ((file = first_with_extension("xmllua")) != NULL) {
		free(file);
		return KIND_LUA;
	}
	/* native projects: a program in the language alone */
	if ((file = first_with_extension("py")) != NULL) {
		free(file);
		return KIND_PYTHON;
	}
	if ((file = first_with_extension("lua")) != NULL) {
		free(file);
		return KIND_LUA;
	}
	return KIND_NONE;
}

static int have_project(void) {
	if (!g_project_dir) {
		ide_status("Open a project first (Project > Open folder, or New project).");
		return R_FALSE;
	}
	g_b.kind = project_kind();
	if (g_b.kind == KIND_NONE) {
		ide_status("Nothing to build: the project has no CMakeLists.txt, Cargo.toml, go.mod, .csproj, build.zig, .xmlpy, .xmllua, .py or .lua.");
		return R_FALSE;
	}
	return R_TRUE;
}

/*
 * What the tools of the languages need to find Raven, from the folder of the
 * Raven package: ravenc in the PATH, the library for the linker and for the
 * program, the bindings of Python and Lua.
 */
static void set_environment(void) {
	char *bin, *lib, *bindings, *value;
	const char* prefix = g_settings.raven_prefix;
	if (!prefix[0]) return;
	bin = ide_join(prefix, "bin");
	lib = ide_join(prefix, "lib");
	bindings = ide_join(prefix, "share/raven/bindings");
	ide_prepend_path(bin);
#if defined(_WIN32)
	value = ide_join(bin, "ravenc.exe");
#else
	value = ide_join(bin, "ravenc");
#endif
	ide_set_env("RAVENC", value);
	free(value);
	ide_set_env("RAVEN_LIB_DIR", lib);
#if defined(_WIN32)
	value = ide_join(bin, "raven.dll");
#elif defined(__APPLE__)
	value = ide_join(lib, "libraven.dylib");
	ide_set_env("DYLD_LIBRARY_PATH", lib);
#else
	value = ide_join(lib, "libraven.so");
	{
		const char* old = getenv("LD_LIBRARY_PATH");
		char* both = (char*)malloc(strlen(lib) + (old ? strlen(old) : 0) + 2);
		sprintf(both, "%s%s%s", lib, old && old[0] ? ":" : "", old ? old : "");
		ide_set_env("LD_LIBRARY_PATH", both);
		free(both);
	}
#endif
	ide_set_env("RAVEN_LIBRARY", value);
	free(value);
	{
		char* include = ide_join(prefix, "include");
		char flags[1200];
		snprintf(flags, sizeof(flags), "-I%s", include);
		ide_set_env("CGO_CFLAGS", flags);
		snprintf(flags, sizeof(flags), "-L%s", lib);
		ide_set_env("CGO_LDFLAGS", flags);
		free(include);
	}
	value = ide_join(bindings, "python");
	ide_set_env("PYTHONPATH", value);
	free(value);
	{
		char lua[1200];
		snprintf(lua, sizeof(lua), "%s/luajit/?.lua;./?.lua;;", bindings);
		ide_set_env("LUA_PATH", lua);
	}
	free(bin);
	free(lib);
	free(bindings);
}

static const char* python_program(void) {
	char found[1024];
	if (ide_find_program("python3", found, sizeof(found))) return "python3";
	return "python";
}

static int is_release(void) {
	return g_settings.build_type[0] && !ide_iequal(g_settings.build_type, "Debug");
}

static void add_run_arguments(step_t* step) {
	if (g_settings.run_arguments[0]) {
		char arguments[512];
		char* token;
		snprintf(arguments, sizeof(arguments), "%s", g_settings.run_arguments);
		for (token = strtok(arguments, " "); token; token = strtok(NULL, " ")) add_arg(step, "%s", token);
	}
}

/* The steps of a project that is not built with CMake. R_FALSE: the command does not apply. */
static int add_language_steps(const char* action) {
	int build = !strcmp(action, "build.build") || !strcmp(action, "build.rebuild"), run = !strcmp(action, "build.run");
	int clean = !strcmp(action, "build.clean") || !strcmp(action, "build.rebuild");
	step_t* step;
	char* main_file = NULL;
	if (!strcmp(action, "build.configure")) {
		ide_status("Only a CMake project is configured; this one builds as it is.");
		return R_FALSE;
	}
	switch (g_b.kind) {
		case KIND_CARGO:
			if (clean) { step = add_step(g_project_dir); add_arg(step, "cargo"); add_arg(step, "clean"); }
			if (build || run) {
				step = add_step(g_project_dir);
				add_arg(step, "cargo");
				add_arg(step, run ? "run" : "build");
				if (is_release()) add_arg(step, "--release");
				if (run && g_settings.run_arguments[0]) { add_arg(step, "--"); add_run_arguments(step); }
			}
			break;
		case KIND_GO:
			if (clean) { step = add_step(g_project_dir); add_arg(step, "go"); add_arg(step, "clean"); }
			if (build || run) {
				/* ravenc writes main.xmlgo.go (//go:generate) */
				step = add_step(g_project_dir);
				add_arg(step, "go");
				add_arg(step, "generate");
				add_arg(step, "./...");
				step = add_step(g_project_dir);
				add_arg(step, "go");
				add_arg(step, run ? "run" : "build");
				add_arg(step, ".");
				if (run) add_run_arguments(step);
			}
			break;
		case KIND_DOTNET:
			if (clean) { step = add_step(g_project_dir); add_arg(step, "dotnet"); add_arg(step, "clean"); }
			if (build || run) {
				step = add_step(g_project_dir);
				add_arg(step, "dotnet");
				add_arg(step, run ? "run" : "build");
				add_arg(step, "-c");
				add_arg(step, "%s", is_release() ? "Release" : "Debug");
				if (run && g_settings.run_arguments[0]) { add_arg(step, "--"); add_run_arguments(step); }
			}
			break;
		case KIND_ZIG:
			if (build || run) {
				step = add_step(g_project_dir);
				add_arg(step, "zig");
				add_arg(step, "build");
				if (run) add_arg(step, "run");
				if (is_release()) add_arg(step, "-Doptimize=ReleaseFast");
				if (run && g_settings.run_arguments[0]) { add_arg(step, "--"); add_run_arguments(step); }
			}
			break;
		case KIND_PYTHON:
			main_file = has_file("main.xmlpy") ? ide_strdup("main.xmlpy") : first_with_extension("xmlpy");
			if (!main_file) {
				/* a native project: python and its file, nothing of Raven */
				main_file = has_file("main.py") ? ide_strdup("main.py") : first_with_extension("py");
				step = add_step(g_project_dir);
				add_arg(step, "%s", python_program());
				if (run) {
					step->run = R_TRUE;
					add_arg(step, "%s", main_file);
					add_run_arguments(step);
				} else {
					add_arg(step, "-m");
					add_arg(step, "py_compile");
					add_arg(step, "%s", main_file);
				}
				free(main_file);
				break;
			}
			/* building: ravenc reads it (its mistakes are reported); running: python with raven_xmlpy */
			step = add_step(g_project_dir);
			if (run) {
				step->run = R_TRUE;
				add_arg(step, "%s", python_program());
				add_arg(step, "-m");
				add_arg(step, "raven_xmlpy");
				add_arg(step, "%s", main_file);
				add_run_arguments(step);
			} else {
				add_arg(step, "%s", python_program());
				add_arg(step, "-c");
				add_arg(step, "import raven_xmlpy, py_compile, sys; py_compile.compile(raven_xmlpy.compile_file(sys.argv[1]), doraise=True)");
				add_arg(step, "%s", main_file);
			}
			free(main_file);
			break;
		case KIND_LUA:
			main_file = has_file("main.xmllua") ? ide_strdup("main.xmllua") : first_with_extension("xmllua");
			if (!main_file) {
				/* a native project: LuaJIT (or Lua) and its file */
				char found[1024], check[700];
				const char* lua = ide_find_program("luajit", found, sizeof(found)) || !ide_find_program("lua", found, sizeof(found)) ? "luajit" : "lua";
				main_file = has_file("main.lua") ? ide_strdup("main.lua") : first_with_extension("lua");
				step = add_step(g_project_dir);
				add_arg(step, "%s", lua);
				if (run) {
					step->run = R_TRUE;
					add_arg(step, "%s", main_file);
					add_run_arguments(step);
				} else {
					/* building: the file is read, its mistakes are reported, nothing runs */
					snprintf(check, sizeof(check), "assert(loadfile('%s'))", main_file);
					add_arg(step, "-e");
					add_arg(step, "%s", check);
				}
				free(main_file);
				break;
			}
			step = add_step(g_project_dir);
			if (run) {
				char script[1200];
				step->run = R_TRUE;
				add_arg(step, "luajit");
				snprintf(script, sizeof(script), "%s/share/raven/bindings/luajit/raven_xmllua.lua", g_settings.raven_prefix[0] ? g_settings.raven_prefix : ".");
				add_arg(step, "%s", script);
				add_arg(step, "%s", main_file);
				add_run_arguments(step);
			} else {
				char output[600];
				snprintf(output, sizeof(output), "%s.lua", main_file);
				add_arg(step, "ravenc");
				add_arg(step, "%s", main_file);
				add_arg(step, "-o");
				add_arg(step, "%s", output);
			}
			free(main_file);
			break;
		default:
			return R_FALSE;
	}
	return R_TRUE;
}

static char* build_dir(void) {
	return ide_join(g_project_dir, "build");
}

/* What the configuration depends on: when it changes, CMake is run again. */
static void configuration_key(char* out, size_t size) {
	snprintf(out, size, "%s|%s|%s|%s", g_project_dir, g_settings.build_type, g_settings.raven_prefix, g_settings.generator);
}

/* The generator the build folder was made with ("" when it was not configured yet). */
static void cached_generator(const char* build, char* out, size_t size) {
	char* cache = ide_join(build, "CMakeCache.txt");
	size_t length = 0;
	char* text = Rfile_read(cache, &length);
	const char* at;
	out[0] = 0;
	free(cache);
	if (!text) return;
	at = strstr(text, "CMAKE_GENERATOR:INTERNAL=");
	if (at) {
		size_t n;
		at += 25;
		n = strcspn(at, "\r\n");
		snprintf(out, size, "%.*s", (int)n, at);
	}
	Rmemory_free(text);
}

/*
 * The generator: the one of the settings; else the one the build folder
 * already has; else Ninja when it is installed, else on Windows MinGW
 * Makefiles when mingw32-make is there (the default of CMake would be Visual
 * Studio, which the Raven of a MinGW package cannot link with).
 */
static void choose_generator(const char* build, char* out, size_t size) {
	char found[1024], cached[256];
	cached_generator(build, cached, sizeof(cached));
	out[0] = 0;
	if (g_settings.generator[0]) {
		/* another generator than the one the folder was made with: CMake refuses, so the cache goes */
		if (cached[0] && strcmp(cached, g_settings.generator) != 0) {
			char* cache = ide_join(build, "CMakeCache.txt");
			ide_remove_file(cache);
			free(cache);
			snprintf(g_b.note, sizeof(g_b.note), "[the build folder was made with %s: configured again with %s]", cached, g_settings.generator);
		}
		snprintf(out, size, "%s", g_settings.generator);
		return;
	}
	if (cached[0]) return;
	if (ide_find_program("ninja", found, sizeof(found))) snprintf(out, size, "Ninja");
#if defined(_WIN32)
	else if (ide_find_program("mingw32-make", found, sizeof(found))) snprintf(out, size, "MinGW Makefiles");
#endif
}

static void add_configure(void) {
	char* build = build_dir();
	char generator[256];
	step_t* step = add_step(g_project_dir);
	choose_generator(build, generator, sizeof(generator));
	add_arg(step, "cmake");
	add_arg(step, "-S");
	add_arg(step, "%s", g_project_dir);
	add_arg(step, "-B");
	add_arg(step, "%s", build);
	if (generator[0]) {
		add_arg(step, "-G");
		add_arg(step, "%s", generator);
	}
	add_arg(step, "-DCMAKE_BUILD_TYPE=%s", g_settings.build_type[0] ? g_settings.build_type : "Debug");
	add_arg(step, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
	if (g_settings.raven_prefix[0]) add_arg(step, "-DCMAKE_PREFIX_PATH=%s", g_settings.raven_prefix);
	configuration_key(g_b.configured_with, sizeof(g_b.configured_with));
	free(build);
}

static void add_build(const char* target) {
	char* build = build_dir();
	step_t* step = add_step(g_project_dir);
	add_arg(step, "cmake");
	add_arg(step, "--build");
	add_arg(step, "%s", build);
	add_arg(step, "--config");
	add_arg(step, "%s", g_settings.build_type[0] ? g_settings.build_type : "Debug");
	add_arg(step, "--parallel");
	if (target) {
		add_arg(step, "--target");
		add_arg(step, "%s", target);
	}
	free(build);
}

static int needs_configure(void) {
	char* build = build_dir();
	char* cache = ide_join(build, "CMakeCache.txt");
	char key[2048];
	int needs = !ide_file_exists(cache);
	configuration_key(key, sizeof(key));
	if (strcmp(key, g_b.configured_with) != 0) needs = R_TRUE;
	free(cache);
	free(build);
	return needs;
}

/* The program the project makes, in the build folder (wherever the generator put it). */
static char* find_program_file(const char* folder, const char* name, int depth) {
	ide_entry_t* entries;
	int count, i;
	char* found = NULL;
	char exe[300];
#if defined(_WIN32)
	snprintf(exe, sizeof(exe), "%s.exe", name);
#else
	snprintf(exe, sizeof(exe), "%s", name);
#endif
	count = ide_list_dir(folder, &entries);
	for (i = 0; i < count && !found; i++) {
		if (!entries[i].is_dir && ide_iequal(entries[i].name, exe)) found = ide_join(folder, entries[i].name);
	}
	for (i = 0; i < count && !found && depth > 0; i++) {
		if (entries[i].is_dir && strcmp(entries[i].name, "CMakeFiles") != 0 && entries[i].name[0] != '.') {
			char* sub = ide_join(folder, entries[i].name);
			found = find_program_file(sub, name, depth - 1);
			free(sub);
		}
	}
	free(entries);
	return found;
}

static void add_run(void) {
	char name[256];
	char* build;
	char* program;
	step_t* step;
	if (!project_target_name(name, sizeof(name))) {
		snprintf(g_b.note, sizeof(g_b.note), "[the CMakeLists.txt has no add_executable: it is built, there is nothing to run]");
		return;
	}
	build = build_dir();
	program = find_program_file(build, name, 4);
	free(build);
	step = add_step(g_project_dir);
	step->run = R_TRUE;
	step->find = R_TRUE;
	/* (found once it is built: looked for again when the step starts) */
	add_arg(step, "%s", program ? program : name);
	free(program);
	add_run_arguments(step);
}

static int save_everything(void) {
	int ok = R_TRUE;
	if (designer_has_document() && designer_is_modified() && designer_get_path()) ok = designer_save() && ok;
	if (editors_any_modified()) ok = editors_save_all() && ok;
	if (strings_any_modified()) ok = strings_save() && ok;
	return ok;
}

static void action_build(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	if (!strcmp(action, "build.stop")) {
		if (g_b.process) {
			ide_process_kill(g_b.process);
			output_printf("\n[stopped]\n");
		}
		return;
	}
	if (build_is_running()) {
		ide_status("A build or the program is still running (Build > Stop).");
		return;
	}
	/* no project: Run opens the interface in a window, as the designer did */
	if (!g_project_dir && !strcmp(action, "build.run")) {
		Raction_fire("view.run", sender);
		return;
	}
	if (!have_project()) return;
	save_everything();
	free_steps();
	set_environment();
	g_b.pending_message[0] = 0;
	if (g_b.kind != KIND_CMAKE) {
		if (!add_language_steps(action)) return;
	} else if (!strcmp(action, "build.configure")) {
		add_configure();
	} else if (!strcmp(action, "build.build")) {
		if (needs_configure()) add_configure();
		add_build(NULL);
	} else if (!strcmp(action, "build.run")) {
		if (needs_configure()) add_configure();
		add_build(NULL);
		add_run();
	} else if (!strcmp(action, "build.clean")) {
		add_build("clean");
	} else if (!strcmp(action, "build.rebuild")) {
		add_configure();
		add_build(NULL);
		{
			step_t* step = &g_b.steps[g_b.step_count - 1];
			add_arg(step, "--clean-first");
		}
	}
	if (g_b.step_count) {
		ide_status("Building %s...", ide_basename(g_project_dir));
		begin();
	}
}

/* (the program is looked for again once it exists: the first build makes it) */
static void refresh_run_step(void) {
	int i;
	for (i = g_b.step_at; i < g_b.step_count; i++) {
		if (g_b.steps[i].find && !ide_file_exists(g_b.steps[i].argv[0])) {
			char name[256];
			char* build = build_dir();
			char* program = project_target_name(name, sizeof(name)) ? find_program_file(build, name, 4) : NULL;
			free(build);
			if (program) {
				free(g_b.steps[i].argv[0]);
				g_b.steps[i].argv[0] = program;
			}
		}
	}
}

void build_init(void) {
	g_b.output = Rwidget_find(g_ide_window, "output");
	g_b.issues_list = Rwidget_find(g_ide_window, "build.problems");
	g_b.timer = R_INVALID_ID;
	Rtext_editor_set_read_only(g_b.output, R_TRUE);
	Rlist_view_set_activated_callback(g_b.issues_list, on_issue_activated, NULL);
	Raction_bind("build.configure", action_build, NULL);
	Raction_bind("build.build", action_build, NULL);
	Raction_bind("build.run", action_build, NULL);
	Raction_bind("build.stop", action_build, NULL);
	Raction_bind("build.clean", action_build, NULL);
	Raction_bind("build.rebuild", action_build, NULL);
}

void build_shutdown(void) {
	if (g_b.process) {
		ide_process_kill(g_b.process);
		ide_process_free(g_b.process);
		g_b.process = NULL;
	}
	free_steps();
	clear_issues();
	ide_buffer_free(&g_b.pending);
}
