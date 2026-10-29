/**
 * Raven - tools/ravenc/ravenc.c
 * The XMLC compiler: an interface in XML with its code, in one file, turned
 * into one source file of the language. Nothing is read or interpreted at run
 * time: the interface becomes a table of RXmlCompiledNode.
 *
 *     ravenc main.xmlc -o main.xmlc.c [--name main] [--lang c]
 *
 *   .xmlc    C        CMake: raven_target_xmlc
 *   .xmlc++  C++      (or .xmlcpp) CMake: raven_target_xmlc
 *   .xmlrs   Rust     build.rs: raven_build::compile (bindings/rust-build)
 *   .xmlpy   Python   import raven_xmlpy, then import the module (bindings/python)
 *   .xmlgo   Go       //go:generate ravenc main.xmlgo -o main.xmlgo.go
 *   .xmlcs   C#       Raven.XmlCs.targets (bindings/csharp)
 *   .xmlzig  Zig      build.zig: xmlzig.add (bindings/zig)
 *   .xmllua  LuaJIT   require("raven_xmllua") (bindings/luajit)
 *
 * An .xmlc file:
 *
 *     <script>
 *     #include <stdio.h>
 *     static int clicks = 0;
 *     static void on_ok(Rid sender, const char* action, void* user) {
 *         Rlabel_set_text(ui.status, "Clicked");      // ui: the widgets that have a Name
 *         clicks++;
 *     }
 *     int main(void) {
 *         Rinit(R_BACKEND_VULKAN);
 *         main_create();                              // made by ravenc from the file name
 *         while (Rdraw_frame()) {}
 *         Rshutdown();
 *         return 0;
 *     }
 *     </script>
 *
 *     <Window Title="Hello" Width="400" Height="200">
 *         <VBox>
 *             <Label Name="status">Not clicked</Label>
 *             <Button Text="OK" OnClicked="{on_ok}"/>
 *         </VBox>
 *     </Window>
 *
 * - <script> holds plain code of the language (no XML escaping: a < b and &&
 *   are written as is), at the top level of the file, as many as wanted.
 *   C and C++ get #line; Rust, Go, C# and Zig keep the scripts on the lines
 *   they have in the file: the errors of the compiler point into it either way.
 * - One interface element at the top level: a <Window> (NAME_create returns
 *   the window) or any widget (NAME_create returns the widget).
 * - Action="{f}", OnClicked="{f}", OnChanged="{f}", OnCommitted="{f}",
 *   OnActivated="{f}": the function f of the scripts (a path is allowed:
 *   App.on_ok), checked by the compiler of the language:
 *     C       void f(Rid sender, const char* action, void* user_data)
 *     C++     the same, or void f(Rid, const char*), or anything callable like it
 *     Rust    fn f(sender: Rid, action: &str)
 *     Python  def f(sender, action)
 *     Go      func f(sender raven.ID, action string)
 *     C#      static void F(uint sender, string action)   (Program.F, or a method of NAMEUi)
 *     Zig     fn f(sender: Rid, action: ?[*:0]const u8, user_data: ?*anyopaque) callconv(.C) void
 *     Lua     function f(sender, action)   (a global function, or App.f)
 *   Without braces, an action name as in XML.
 * - Name="x" (an identifier): the widget once NAME_create has run, as
 *   ui.x (C, C++, Python, Go, Zig, Lua), ui().x (Rust), NAMEUi.x (C#).
 * - Everything else is the XML of Raven, built exactly as Rwindow_create_from_xml would.
 *
 * No header is written: another file declares NAME_create itself.
 *
 * Standalone (C99, no dependency): it runs on the build machine.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  Small tools
 * ------------------------------------------------------------------------- */

static const char* g_path = "";

static void die(int line, const char* format, ...) {
	va_list args;
	fprintf(stderr, "%s:%d: error: ", g_path, line);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
	exit(1);
}

static void* xalloc(size_t size) {
	void* p = calloc(1, size ? size : 1);
	if (!p) { fprintf(stderr, "ravenc: out of memory\n"); exit(1); }
	return p;
}

static char* xstrndup(const char* s, size_t n) {
	char* p = (char*)xalloc(n + 1);
	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

typedef struct buffer_t {
	char* data;
	size_t size, cap;
	int lines;
} buffer_t;

static void buf_add(buffer_t* b, const char* s, size_t n) {
	size_t i;
	if (b->size + n + 1 > b->cap) {
		b->cap = (b->size + n + 1) * 2;
		b->data = (char*)realloc(b->data, b->cap);
		if (!b->data) { fprintf(stderr, "ravenc: out of memory\n"); exit(1); }
	}
	memcpy(b->data + b->size, s, n);
	b->size += n;
	b->data[b->size] = 0;
	for (i = 0; i < n; i++) if (s[i] == '\n') b->lines++;
}

static void buf_puts(buffer_t* b, const char* s) { buf_add(b, s, strlen(s)); }

static void buf_printf(buffer_t* b, const char* format, ...) {
	char small[1024];
	va_list args;
	int n;
	va_start(args, format);
	n = vsnprintf(small, sizeof(small), format, args);
	va_end(args);
	if (n < 0) return;
	if ((size_t)n < sizeof(small)) { buf_add(b, small, (size_t)n); return; }
	{
		char* big = (char*)xalloc((size_t)n + 1);
		va_start(args, format);
		vsnprintf(big, (size_t)n + 1, format, args);
		va_end(args);
		buf_add(b, big, (size_t)n);
		free(big);
	}
}

/* A C string literal of any text (UTF-8 kept as is). */
static void buf_cstring(buffer_t* b, const char* s) {
	buf_puts(b, "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { char e[3] = { '\\', (char)c, 0 }; buf_puts(b, e); }
		else if (c == '\n') buf_puts(b, "\\n");
		else if (c == '\r') buf_puts(b, "\\r");
		else if (c == '\t') buf_puts(b, "\\t");
		else if (c == '?' && s[1] == '?') buf_puts(b, "?\"\"");  /* no trigraphs */
		else if (c < 0x20) buf_printf(b, "\\%03o", c);
		else { char e[2] = { (char)c, 0 }; buf_puts(b, e); }
	}
	buf_puts(b, "\"");
}

static int is_identifier(const char* s) {
	if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
	for (s++; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
	return 1;
}

static int iequal(const char* a, const char* b) {
	for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
	return *a == *b;
}

/* ---------------------------------------------------------------------------
 *  The file: scripts and elements
 * ------------------------------------------------------------------------- */

typedef struct attribute_t {
	char* name;
	char* value;
	struct attribute_t* next;
} attribute_t;

typedef struct node_t {
	char* tag;
	char* text;           /* text content, trimmed */
	attribute_t* attributes;
	int attribute_count;
	struct node_t* parent;
	struct node_t* first_child;
	struct node_t* last_child;
	struct node_t* next;
	int line;
	int index;            /* in the table */
} node_t;

typedef struct script_t {
	char* code;
	int line;             /* line of the first character of the code */
} script_t;

typedef struct parser_t {
	const char* p;
	const char* end;
	int line;
	script_t* scripts;
	int script_count;
	node_t* root;
} parser_t;

static void advance(parser_t* ps, size_t n) {
	size_t i;
	for (i = 0; i < n && ps->p < ps->end; i++, ps->p++) if (*ps->p == '\n') ps->line++;
}

static int starts(parser_t* ps, const char* s) {
	size_t n = strlen(s);
	return (size_t)(ps->end - ps->p) >= n && memcmp(ps->p, s, n) == 0;
}

static int starts_i(parser_t* ps, const char* s) {
	size_t n = strlen(s), i;
	if ((size_t)(ps->end - ps->p) < n) return 0;
	for (i = 0; i < n; i++) if (tolower((unsigned char)ps->p[i]) != tolower((unsigned char)s[i])) return 0;
	return 1;
}

static void skip_space(parser_t* ps) {
	while (ps->p < ps->end && isspace((unsigned char)*ps->p)) advance(ps, 1);
}

/* Skips what is not content: comments, <?...?>, <!DOCTYPE ...>. R_TRUE when something was skipped. */
static int skip_misc(parser_t* ps) {
	if (starts(ps, "<!--")) {
		int line = ps->line;
		while (ps->p < ps->end && !starts(ps, "-->")) advance(ps, 1);
		if (ps->p >= ps->end) die(line, "unterminated comment");
		advance(ps, 3);
		return 1;
	}
	if (starts(ps, "<?") || starts(ps, "<!DOCTYPE") || starts(ps, "<!doctype")) {
		int line = ps->line;
		while (ps->p < ps->end && *ps->p != '>') advance(ps, 1);
		if (ps->p >= ps->end) die(line, "unterminated declaration");
		advance(ps, 1);
		return 1;
	}
	return 0;
}

static char* read_name(parser_t* ps) {
	const char* start = ps->p;
	while (ps->p < ps->end && (isalnum((unsigned char)*ps->p) || *ps->p == '_' || *ps->p == '-' || *ps->p == '.' || *ps->p == ':' || (unsigned char)*ps->p >= 0x80))
		advance(ps, 1);
	if (ps->p == start) die(ps->line, "a name is expected here");
	return xstrndup(start, (size_t)(ps->p - start));
}

/* Appends the UTF-8 of a code point. */
static void put_utf8(buffer_t* b, unsigned long c) {
	char s[5] = { 0 };
	if (c < 0x80) s[0] = (char)c;
	else if (c < 0x800) { s[0] = (char)(0xC0 | (c >> 6)); s[1] = (char)(0x80 | (c & 63)); }
	else if (c < 0x10000) { s[0] = (char)(0xE0 | (c >> 12)); s[1] = (char)(0x80 | ((c >> 6) & 63)); s[2] = (char)(0x80 | (c & 63)); }
	else { s[0] = (char)(0xF0 | (c >> 18)); s[1] = (char)(0x80 | ((c >> 12) & 63)); s[2] = (char)(0x80 | ((c >> 6) & 63)); s[3] = (char)(0x80 | (c & 63)); }
	buf_puts(b, s);
}

/* Reads an entity after '&'. */
static void read_entity(parser_t* ps, buffer_t* out) {
	int line = ps->line;
	const char* semi = ps->p;
	char name[16];
	size_t n;
	while (semi < ps->end && *semi != ';' && semi - ps->p < 12) semi++;
	if (semi >= ps->end || *semi != ';') die(line, "'&' must start an entity (&amp; for the character itself)");
	n = (size_t)(semi - ps->p);
	memcpy(name, ps->p, n);
	name[n] = 0;
	if (!strcmp(name, "amp")) buf_puts(out, "&");
	else if (!strcmp(name, "lt")) buf_puts(out, "<");
	else if (!strcmp(name, "gt")) buf_puts(out, ">");
	else if (!strcmp(name, "quot")) buf_puts(out, "\"");
	else if (!strcmp(name, "apos")) buf_puts(out, "'");
	else if (name[0] == '#') {
		char* tail;
		unsigned long c = (name[1] == 'x' || name[1] == 'X') ? strtoul(name + 2, &tail, 16) : strtoul(name + 1, &tail, 10);
		if (*tail || c == 0 || c > 0x10FFFF) die(line, "bad character reference &%s;", name);
		put_utf8(out, c);
	} else die(line, "unknown entity &%s;", name);
	advance(ps, n + 1);
}

static char* trim(char* s) {
	char* e;
	while (*s && isspace((unsigned char)*s)) s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) e--;
	*e = 0;
	return s;
}

static void read_script(parser_t* ps, int line) {
	/* after <script ...>: plain C until </script> */
	const char* start;
	int code_line;
	skip_space(ps);
	if (starts(ps, "/>")) { advance(ps, 2); return; }
	if (*ps->p != '>') die(ps->line, "<script> takes no attributes");
	advance(ps, 1);
	start = ps->p;
	code_line = ps->line;
	while (ps->p < ps->end && !starts_i(ps, "</script")) advance(ps, 1);
	if (ps->p >= ps->end) die(line, "<script> is not closed (</script>)");
	ps->scripts = (script_t*)realloc(ps->scripts, sizeof(script_t) * (size_t)(ps->script_count + 1));
	ps->scripts[ps->script_count].code = xstrndup(start, (size_t)(ps->p - start));
	ps->scripts[ps->script_count].line = code_line;
	ps->script_count++;
	advance(ps, 8);
	skip_space(ps);
	if (*ps->p != '>') die(ps->line, "'>' expected after </script");
	advance(ps, 1);
}

static node_t* read_element(parser_t* ps, node_t* parent);

/* Content of an element until its closing tag. */
static void read_content(parser_t* ps, node_t* node) {
	buffer_t text = { 0 };
	for (;;) {
		if (ps->p >= ps->end) die(node->line, "<%s> is not closed", node->tag);
		if (starts(ps, "</")) {
			char* name;
			advance(ps, 2);
			name = read_name(ps);
			if (strcmp(name, node->tag) != 0) die(ps->line, "</%s> closes <%s>", name, node->tag);
			free(name);
			skip_space(ps);
			if (*ps->p != '>') die(ps->line, "'>' expected");
			advance(ps, 1);
			break;
		}
		if (starts(ps, "<![CDATA[")) {
			const char* start;
			advance(ps, 9);
			start = ps->p;
			while (ps->p < ps->end && !starts(ps, "]]>")) advance(ps, 1);
			if (ps->p >= ps->end) die(node->line, "unterminated CDATA");
			buf_add(&text, start, (size_t)(ps->p - start));
			advance(ps, 3);
			continue;
		}
		if (skip_misc(ps)) continue;
		if (*ps->p == '<') {
			if (starts_i(ps, "<script")) die(ps->line, "<script> goes at the top level of the file, not inside <%s>", node->tag);
			read_element(ps, node);
			continue;
		}
		if (*ps->p == '&') { advance(ps, 1); read_entity(ps, &text); continue; }
		buf_add(&text, ps->p, 1);
		advance(ps, 1);
	}
	node->text = xstrndup(text.data ? trim(text.data) : "", text.data ? strlen(trim(text.data)) : 0);
	free(text.data);
}

static node_t* read_element(parser_t* ps, node_t* parent) {
	node_t* node = (node_t*)xalloc(sizeof(node_t));
	attribute_t* last = NULL;
	node->line = ps->line;
	advance(ps, 1); /* < */
	node->tag = read_name(ps);
	node->parent = parent;
	if (parent) {
		if (parent->last_child) parent->last_child->next = node;
		else parent->first_child = node;
		parent->last_child = node;
	}
	for (;;) {
		skip_space(ps);
		if (ps->p >= ps->end) die(node->line, "<%s> is not closed", node->tag);
		if (starts(ps, "/>")) { advance(ps, 2); node->text = xstrndup("", 0); return node; }
		if (*ps->p == '>') { advance(ps, 1); read_content(ps, node); return node; }
		{
			attribute_t* a = (attribute_t*)xalloc(sizeof(attribute_t));
			buffer_t value = { 0 };
			char quote;
			a->name = read_name(ps);
			skip_space(ps);
			if (*ps->p != '=') die(ps->line, "'=' expected after %s", a->name);
			advance(ps, 1);
			skip_space(ps);
			quote = *ps->p;
			if (quote != '"' && quote != '\'') die(ps->line, "the value of %s must be in quotes", a->name);
			advance(ps, 1);
			while (ps->p < ps->end && *ps->p != quote) {
				if (*ps->p == '&') { advance(ps, 1); read_entity(ps, &value); continue; }
				if (*ps->p == '<') die(ps->line, "'<' in a value: write &lt;");
				buf_add(&value, ps->p, 1);
				advance(ps, 1);
			}
			if (ps->p >= ps->end) die(node->line, "unterminated value of %s", a->name);
			advance(ps, 1);
			a->value = value.data ? value.data : xstrndup("", 0);
			if (last) last->next = a; else node->attributes = a;
			last = a;
			node->attribute_count++;
		}
	}
}

static void parse(parser_t* ps) {
	for (;;) {
		skip_space(ps);
		if (ps->p >= ps->end) break;
		if (skip_misc(ps)) continue;
		if (starts_i(ps, "<script")) {
			int line = ps->line;
			advance(ps, 7);
			if (ps->p < ps->end && !isspace((unsigned char)*ps->p) && *ps->p != '>' && *ps->p != '/') die(line, "unknown element at the top level");
			read_script(ps, line);
			continue;
		}
		if (*ps->p == '<') {
			int line = ps->line;
			node_t* node = read_element(ps, NULL);
			if (ps->root) die(line, "a second interface element <%s>: an .xmlc file has one (<Window> or a widget), and <script> blocks", node->tag);
			ps->root = node;
			continue;
		}
		die(ps->line, "text outside of an element (C code goes in <script>)");
	}
}

/* ---------------------------------------------------------------------------
 *  C
 * ------------------------------------------------------------------------- */

typedef struct binding_t {
	char* function;
	char* action;
	int line;
} binding_t;

static node_t** g_table;
static int g_count;
static char** g_names;
static int g_name_count;
static binding_t* g_bindings;
static int g_binding_count;

static int is_event(const char* name) {
	return iequal(name, "Action") || iequal(name, "OnClicked") || iequal(name, "OnChanged") ||
		iequal(name, "OnCommitted") || iequal(name, "OnActivated");
}

/* What the braces may hold: a name, or a path to one (App.on_ok, crate::ui::on_ok). */
static int is_callable(const char* s) {
	if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
	for (s++; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '.' || *s == ':')) return 0;
	return 1;
}

static int inside_declaration(const node_t* node) {
	/* the body of a <Widget> declaration is a template: its names belong to each use */
	for (node = node->parent; node; node = node->parent) if (iequal(node->tag, "Widget")) return 1;
	return 0;
}

/* {Text} in the body of <Widget Text="..."> is a parameter of the declaration, not a function */
static int is_template_parameter(const node_t* node, const char* name) {
	const attribute_t* a;
	for (node = node->parent; node; node = node->parent) {
		if (!iequal(node->tag, "Widget")) continue;
		for (a = node->attributes; a; a = a->next) if (!strcmp(a->name, name)) return 1;
	}
	return 0;
}

static void collect(node_t* node, const char* unit) {
	attribute_t* a;
	node_t* child;
	node->index = g_count;
	g_table = (node_t**)realloc(g_table, sizeof(node_t*) * (size_t)(g_count + 1));
	g_table[g_count++] = node;
	for (a = node->attributes; a; a = a->next) {
		size_t n = strlen(a->value);
		if (is_event(a->name) && n >= 2 && a->value[0] == '{' && a->value[n - 1] == '}') {
			char* function = trim(xstrndup(a->value + 1, n - 2));
			char action[512];
			int i, found = -1;
			if (is_template_parameter(node, function)) { free(function); continue; }
			if (!is_callable(function)) die(node->line, "%s=\"%s\": the braces hold the name of a function", a->name, a->value);
			for (i = 0; i < g_binding_count; i++) if (!strcmp(g_bindings[i].function, function)) found = i;
			if (found >= 0) free(function);
			else {
				snprintf(action, sizeof(action), "ravenc.%s.%s", unit, function);
				g_bindings = (binding_t*)realloc(g_bindings, sizeof(binding_t) * (size_t)(g_binding_count + 1));
				g_bindings[g_binding_count].function = function;
				g_bindings[g_binding_count].action = xstrndup(action, strlen(action));
				g_bindings[g_binding_count].line = node->line;
				found = g_binding_count++;
			}
			free(a->value);
			a->value = xstrndup(g_bindings[found].action, strlen(g_bindings[found].action));
		}
		if (iequal(a->name, "Name") && is_identifier(a->value) && !inside_declaration(node) && !iequal(node->tag, "Widget")) {
			int i, seen = 0;
			for (i = 0; i < g_name_count; i++) if (!strcmp(g_names[i], a->value)) seen = 1;
			if (!seen) {
				g_names = (char**)realloc(g_names, sizeof(char*) * (size_t)(g_name_count + 1));
				g_names[g_name_count++] = a->value;
			}
		}
	}
	for (child = node->first_child; child; child = child->next) collect(child, unit);
}

/* Everything the parser and collect() allocated (ravenc exits right after, but
 * the tests run it under LeakSanitizer). */
static void free_node(node_t* node) {
	while (node) {
		node_t* next = node->next;
		attribute_t* a = node->attributes;
		while (a) {
			attribute_t* an = a->next;
			free(a->name);
			free(a->value);
			free(a);
			a = an;
		}
		free_node(node->first_child);
		free(node->tag);
		free(node->text);
		free(node);
		node = next;
	}
}

static void free_all(parser_t* ps) {
	int i;
	free_node(ps->root);
	for (i = 0; i < ps->script_count; i++) free(ps->scripts[i].code);
	free(ps->scripts);
	for (i = 0; i < g_binding_count; i++) {
		free(g_bindings[i].function);
		free(g_bindings[i].action);
	}
	free(g_bindings);
	free(g_table);
	free(g_names);         /* (the names belong to the attributes) */
	g_bindings = NULL; g_table = NULL; g_names = NULL;
	g_binding_count = g_count = g_name_count = 0;
}

static void line_directive(buffer_t* out, int line, const char* path) {
	buf_printf(out, "#line %d ", line);
	buf_cstring(out, path);
	buf_puts(out, "\n");
}

static char* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	char* data;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (char*)xalloc((size_t)n + 1);
	if (n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(data); return NULL; }
	fclose(f);
	*size = (size_t)n;
	return data;
}

static char* forward_slashes(const char* s) {
	char* r = xstrndup(s, strlen(s)), *p;
	for (p = r; *p; p++) if (*p == '\\') *p = '/';
	return r;
}

static const char* base_name(const char* path) {
	const char* base = path;
	const char* p;
	for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
	return base;
}

/* "dir/my-app.xmlc" -> "my_app" */
static char* unit_name(const char* path) {
	const char* base = base_name(path);
	char* name;
	size_t n, i;
	n = strcspn(base, ".");
	name = xstrndup(base, n ? n : 1);
	if (!n) name[0] = 'x';
	for (i = 0; name[i]; i++) if (!isalnum((unsigned char)name[i])) name[i] = '_';
	if (isdigit((unsigned char)name[0])) { char* r = (char*)xalloc(strlen(name) + 2); r[0] = '_'; strcpy(r + 1, name); free(name); name = r; }
	return name;
}

/* ---------------------------------------------------------------------------
 *  The languages
 * ------------------------------------------------------------------------- */

typedef enum { LANG_C, LANG_CPP, LANG_RUST, LANG_PYTHON, LANG_GO, LANG_CSHARP, LANG_ZIG, LANG_LUA } lang_t;

static const struct { const char* name; const char* extension; lang_t lang; } g_languages[] = {
	{ "c", "xmlc", LANG_C }, { "cpp", "xmlc++", LANG_CPP }, { "c++", "xmlcpp", LANG_CPP }, { "rust", "xmlrs", LANG_RUST },
	{ "python", "xmlpy", LANG_PYTHON }, { "go", "xmlgo", LANG_GO }, { "csharp", "xmlcs", LANG_CSHARP }, { "zig", "xmlzig", LANG_ZIG },
	{ "lua", "xmllua", LANG_LUA }
};

static int language_named(const char* name, lang_t* lang) {
	size_t i;
	for (i = 0; i < sizeof(g_languages) / sizeof(g_languages[0]); i++) {
		if (iequal(name, g_languages[i].name) || iequal(name, g_languages[i].extension)) {
			*lang = g_languages[i].lang;
			return 1;
		}
	}
	if (iequal(name, "rs")) { *lang = LANG_RUST; return 1; }
	if (iequal(name, "py")) { *lang = LANG_PYTHON; return 1; }
	if (iequal(name, "cs") || iequal(name, "c#")) { *lang = LANG_CSHARP; return 1; }
	return 0;
}

static int language_of_file(const char* path, lang_t* lang) {
	const char* dot = strrchr(base_name(path), '.');
	return dot && language_named(dot + 1, lang);
}

/*
 * A string literal: C and C++ (octal escapes, no trigraphs), and the others
 * with \xHH (C#: \u00HH; Rust byte strings: every byte above 127 escaped too).
 */
static void buf_literal(buffer_t* b, const char* s, lang_t lang) {
	if (lang == LANG_C || lang == LANG_CPP) { buf_cstring(b, s); return; }
	buf_puts(b, lang == LANG_RUST ? "b\"" : "\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { char e[3] = { '\\', (char)c, 0 }; buf_puts(b, e); }
		else if (c == '\n') buf_puts(b, "\\n");
		else if (c == '\r') buf_puts(b, "\\r");
		else if (c == '\t') buf_puts(b, "\\t");
		else if (c < 0x20 || c == 0x7F || (lang == LANG_RUST && c >= 0x80)) buf_printf(b, lang == LANG_CSHARP ? "\\u%04X" : "\\x%02X", c);
		else { char e[2] = { (char)c, 0 }; buf_puts(b, e); }
	}
	buf_puts(b, lang == LANG_RUST ? "\\0\"" : "\"");
}

/* A Rust literal is a pointer: b"...\0".as_ptr() as *const c_char */
static void buf_rust_ptr(buffer_t* b, const char* s) {
	buf_literal(b, s, LANG_RUST);
	buf_puts(b, ".as_ptr() as *const ::std::os::raw::c_char");
}

/*
 * The scripts on the lines they have in the source, the other lines left
 * empty: what the compiler of the language reports is at the right line of
 * the .xml file. `first_free` is where a line of our own may go (before the
 * first script), or 0.
 */
static void emit_scripts_in_place(buffer_t* out, const parser_t* ps, const char* first_line) {
	int i;
	if (first_line) {
		buf_puts(out, first_line);
		/* a script that starts on the first line: ours shares it, the lines stay those of the file */
		buf_puts(out, ps->script_count && ps->scripts[0].line <= 1 ? "; " : "\n");
	}
	for (i = 0; i < ps->script_count; i++) {
		while (out->lines + 1 < ps->scripts[i].line) buf_puts(out, "\n");
		buf_puts(out, ps->scripts[i].code);
	}
	buf_puts(out, "\n");
}

/* ---- C and C++ ---- */

static void emit_c_like(buffer_t* out, const parser_t* ps, lang_t lang, const char* unit, const char* source_path, const char* out_path, int is_window) {
	int i;
	buf_printf(out, "/* Generated by ravenc from %s: edit it, not this file. */\n", source_path);
	buf_puts(out, "#include <Raven.h>\n\n");
	if (lang == LANG_CPP) {
		buf_puts(out, "#include <map>\n#include <memory>\n#include <string>\n\n");
		/* {f} takes void f(Rid, const char*, void*), void f(Rid, const char*), or anything callable like it */
		buf_puts(out,
			"namespace ravenc_detail {\n"
			"inline void bind(const char* name, void (*f)(Rid, const char*, void*)) { Raction_bind(name, f, nullptr); }\n"
			"inline void bind(const char* name, void (*f)(Rid, const char*)) {\n"
			"\tRaction_bind(name, [](Rid s, const char* a, void* u) { reinterpret_cast<void (*)(Rid, const char*)>(u)(s, a); }, reinterpret_cast<void*>(f));\n"
			"}\n"
			"template <class F> inline void bind(const char* name, F f) {\n"
			"\t/* one copy per action, freed when the program ends (or when the action is bound again) */\n"
			"\tstatic std::map<std::string, std::unique_ptr<F>> kept;\n"
			"\tstd::unique_ptr<F>& slot = kept[name];\n"
			"\tslot.reset(new F(f));\n"
			"\tRaction_bind(name, [](Rid s, const char* a, void* u) { (*static_cast<F*>(u))(s, a); }, slot.get());\n"
			"}\n"
			"}\n\n");
	}
	if (g_name_count) {
		buf_puts(out, "/* the widgets that have a Name, filled by the create function */\n");
		buf_puts(out, "static struct {\n");
		for (i = 0; i < g_name_count; i++) buf_printf(out, "\tRid %s;\n", g_names[i]);
		buf_puts(out, "} ui;\n\n");
	}
	if (ps->root) buf_printf(out, "Rid %s_create(void);\n\n", unit);

	for (i = 0; i < ps->script_count; i++) {
		line_directive(out, ps->scripts[i].line, source_path);
		buf_puts(out, ps->scripts[i].code);
		buf_puts(out, "\n");
		line_directive(out, out->lines + 2, out_path);
	}
	if (!ps->root) return;

	buf_puts(out, "\n/* ---- the interface ---- */\n\n");
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		attribute_t* a;
		if (!n->attribute_count) continue;
		buf_printf(out, "static const char* const ravenc_a%d[] = { ", i);
		for (a = n->attributes; a; a = a->next) {
			buf_cstring(out, a->name);
			buf_puts(out, ", ");
			buf_cstring(out, a->value);
			buf_puts(out, a->next ? ", " : " ");
		}
		buf_puts(out, "};\n");
	}
	buf_printf(out, "\nstatic const RXmlCompiledNode ravenc_nodes[%d] = {\n", g_count);
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		buf_puts(out, "\t{ ");
		buf_cstring(out, n->tag);
		buf_puts(out, ", ");
		buf_cstring(out, n->text);
		if (n->attribute_count) buf_printf(out, ", ravenc_a%d, %d", i, n->attribute_count);
		else buf_puts(out, ", 0, 0");
		buf_printf(out, ", %d, %d },\n", n->parent ? n->parent->index : -1, n->line);
	}
	buf_puts(out, "};\n\n");

	buf_printf(out, "Rid %s_create(void) {\n\tRid root;\n", unit);
	for (i = 0; i < g_binding_count; i++) {
		/* (a missing or mistyped function is reported on the line of its element) */
		line_directive(out, g_bindings[i].line, source_path);
		buf_puts(out, lang == LANG_CPP ? "\travenc_detail::bind(" : "\tRaction_bind(");
		buf_cstring(out, g_bindings[i].action);
		buf_printf(out, lang == LANG_CPP ? ", %s);\n" : ", %s, NULL);\n", g_bindings[i].function);
	}
	if (g_binding_count) line_directive(out, out->lines + 2, out_path);
	buf_printf(out, "\troot = %s(ravenc_nodes, %d, ", is_window ? "Rwindow_create_from_compiled" : "Rwidget_create_from_compiled", g_count);
	buf_cstring(out, source_path);
	buf_puts(out, ");\n");
	for (i = 0; i < g_name_count; i++) {
		buf_printf(out, "\tui.%s = root != R_INVALID_ID ? Rwidget_find(root, ", g_names[i]);
		buf_cstring(out, g_names[i]);
		buf_puts(out, ") : R_INVALID_ID;\n");
	}
	buf_puts(out, "\treturn root;\n}\n");
}

/* ---- Rust ---- */

static void emit_rust(buffer_t* out, const parser_t* ps, const char* unit, const char* source_path, int is_window) {
	int i;
	emit_scripts_in_place(out, ps, NULL);
	buf_printf(out, "\n// ---- generated by ravenc from %s: edit it, not this file ----\n\n", source_path);
	buf_puts(out, "/// The widgets that have a Name (filled by the create function).\n#[allow(dead_code)]\n#[derive(Clone, Copy, Debug)]\npub struct RavencUi {\n");
	for (i = 0; i < g_name_count; i++) buf_printf(out, "    pub %s: ::raven::Rid,\n", g_names[i]);
	buf_puts(out, "}\n\nstatic RAVENC_UI: ::std::sync::Mutex<RavencUi> = ::std::sync::Mutex::new(RavencUi {");
	for (i = 0; i < g_name_count; i++) buf_printf(out, "%s %s: ::raven::R_INVALID_ID", i ? "," : "", g_names[i]);
	buf_puts(out, " });\n\n/// The widgets of the interface: `ui().status`.\n#[allow(dead_code)]\npub fn ui() -> RavencUi {\n    *RAVENC_UI.lock().unwrap()\n}\n\n");
	if (!ps->root) return;
	buf_printf(out, "/// Builds the interface of %s (no XML is read: the table below is it).\n#[allow(dead_code, unused_unsafe)]\npub fn %s_create() -> ::raven::Rid {\n", base_name(source_path), unit);
	for (i = 0; i < g_binding_count; i++) {
		buf_puts(out, "    ::raven::action_bind(");
		buf_literal(out, g_bindings[i].action, LANG_C);
		buf_printf(out, ", |sender, action| %s(sender, action));\n", g_bindings[i].function);
	}
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		attribute_t* a;
		if (!n->attribute_count) continue;
		buf_printf(out, "    let a%d: [*const ::std::os::raw::c_char; %d] = [", i, n->attribute_count * 2);
		for (a = n->attributes; a; a = a->next) {
			buf_rust_ptr(out, a->name);
			buf_puts(out, ", ");
			buf_rust_ptr(out, a->value);
			if (a->next) buf_puts(out, ", ");
		}
		buf_puts(out, "];\n");
	}
	buf_printf(out, "    let nodes: [::raven::RXmlCompiledNode; %d] = [\n", g_count);
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		buf_puts(out, "        ::raven::RXmlCompiledNode { tag: ");
		buf_rust_ptr(out, n->tag);
		buf_puts(out, ", text: ");
		buf_rust_ptr(out, n->text);
		if (n->attribute_count) buf_printf(out, ", attributes: a%d.as_ptr(), attribute_count: %d", i, n->attribute_count);
		else buf_puts(out, ", attributes: ::std::ptr::null(), attribute_count: 0");
		buf_printf(out, ", parent: %d, line: %d },\n", n->parent ? n->parent->index : -1, n->line);
	}
	buf_printf(out, "    ];\n    let root = unsafe { ::raven::%s(nodes.as_ptr(), %d, ", is_window ? "Rwindow_create_from_compiled" : "Rwidget_create_from_compiled", g_count);
	buf_rust_ptr(out, source_path);
	buf_puts(out, ") };\n");
	if (g_name_count) {
		buf_puts(out, "    if root != ::raven::R_INVALID_ID {\n        let mut ui = RAVENC_UI.lock().unwrap();\n");
		for (i = 0; i < g_name_count; i++) buf_printf(out, "        ui.%s = ::raven::widget_find(root, \"%s\");\n", g_names[i], g_names[i]);
		buf_puts(out, "    }\n");
	}
	buf_puts(out, "    root\n}\n");
}

/* ---- Python ---- */

static void emit_python(buffer_t* out, const parser_t* ps, const char* unit, const char* source_path, int is_window) {
	int i;
	/* (the functions of the scripts are found when create runs: it can come first) */
	buf_printf(out, "# -*- coding: utf-8 -*-\n# Generated by ravenc from %s: edit it, not this file.\n", source_path);
	buf_puts(out, "import ctypes as _ravenc_ctypes\nimport raven as _ravenc_raven\n\n\nclass _RavencUi:\n    \"\"\"The widgets that have a Name (filled by the create function).\"\"\"\n");
	for (i = 0; i < g_name_count; i++) buf_printf(out, "    %s = _ravenc_raven.R_INVALID_ID\n", g_names[i]);
	if (!g_name_count) buf_puts(out, "    pass\n");
	buf_puts(out, "\n\nui = _RavencUi()\n\n\n");
	if (ps->root) {
		buf_printf(out, "def %s_create():\n    \"\"\"Builds the interface of %s.\"\"\"\n", unit, base_name(source_path));
		buf_puts(out, "    R = _ravenc_raven\n    keep = []\n\n"
			"    def s(text):\n        data = text.encode(\"utf-8\")\n        keep.append(data)\n        return data\n\n"
			"    def a(*texts):\n        array = (_ravenc_ctypes.c_char_p * len(texts))(*[s(t) for t in texts])\n        keep.append(array)\n"
			"        return _ravenc_ctypes.cast(array, _ravenc_ctypes.POINTER(_ravenc_ctypes.c_char_p))\n\n");
		for (i = 0; i < g_binding_count; i++) {
			buf_puts(out, "    R.Raction_bind(");
			buf_literal(out, g_bindings[i].action, LANG_PYTHON);
			buf_printf(out, ", lambda sender, action, user_data: %s(sender, action))\n", g_bindings[i].function);
		}
		buf_printf(out, "    nodes = (R.RXmlCompiledNode * %d)()\n", g_count);
		for (i = 0; i < g_count; i++) {
			node_t* n = g_table[i];
			attribute_t* a;
			buf_printf(out, "    nodes[%d] = R.RXmlCompiledNode(s(", i);
			buf_literal(out, n->tag, LANG_PYTHON);
			buf_puts(out, "), s(");
			buf_literal(out, n->text, LANG_PYTHON);
			buf_puts(out, "), ");
			if (n->attribute_count) {
				buf_puts(out, "a(");
				for (a = n->attributes; a; a = a->next) {
					buf_literal(out, a->name, LANG_PYTHON);
					buf_puts(out, ", ");
					buf_literal(out, a->value, LANG_PYTHON);
					if (a->next) buf_puts(out, ", ");
				}
				buf_puts(out, ")");
			} else {
				buf_puts(out, "None");
			}
			buf_printf(out, ", %d, %d, %d)\n", n->attribute_count, n->parent ? n->parent->index : -1, n->line);
		}
		buf_printf(out, "    root = R.%s(nodes, %d, ", is_window ? "Rwindow_create_from_compiled" : "Rwidget_create_from_compiled", g_count);
		buf_literal(out, source_path, LANG_PYTHON);
		buf_puts(out, ")\n    if root != R.R_INVALID_ID:\n");
		for (i = 0; i < g_name_count; i++) buf_printf(out, "        ui.%s = R.Rwidget_find(root, \"%s\")\n", g_names[i], g_names[i]);
		buf_puts(out, "        pass\n    return root\n\n\n");
	}
	buf_puts(out, "# ---- the scripts ----\n");
	for (i = 0; i < ps->script_count; i++) {
		buf_printf(out, "# %s, line %d\n", base_name(source_path), ps->scripts[i].line);
		buf_puts(out, ps->scripts[i].code);
		buf_puts(out, "\n");
	}
}

/* ---- Go ---- */

static void emit_go(buffer_t* out, const parser_t* ps, const char* unit, const char* source_path, int is_window) {
	static const char import_line[] = "import ravenc_raven \"raven/raven\"";
	int i, has_package = 0;
	/* a script that names its package: the import goes on the same line, after it */
	if (ps->script_count) {
		const char* p = ps->scripts[0].code;
		while (*p && (isspace((unsigned char)*p))) p++;
		has_package = !strncmp(p, "package ", 8);
	}
	if (has_package) {
		parser_t copy = *ps;
		script_t first = ps->scripts[0];
		const char* p = first.code;
		const char* end;
		buffer_t code = { 0 };
		while (*p && isspace((unsigned char)*p)) p++;
		end = strchr(p, '\n');
		if (!end) end = p + strlen(p);
		buf_add(&code, first.code, (size_t)(end - first.code));
		buf_puts(&code, "; ");
		buf_puts(&code, import_line);
		buf_puts(&code, end);
		copy.scripts = (script_t*)xalloc(sizeof(script_t) * (size_t)ps->script_count);
		memcpy(copy.scripts, ps->scripts, sizeof(script_t) * (size_t)ps->script_count);
		copy.scripts[0].code = code.data;
		emit_scripts_in_place(out, &copy, NULL);
		free(copy.scripts);
		free(code.data);
	} else {
		char first[160];
		snprintf(first, sizeof(first), "package main; %s", import_line);
		/* (on the first line when the scripts leave it free: their lines stay those of the file) */
		emit_scripts_in_place(out, ps, first);
	}
	buf_printf(out, "\n// ---- generated by ravenc from %s: edit it, not this file ----\n\n", source_path);
	buf_puts(out, "// ui holds the widgets that have a Name (filled by the create function).\nvar ui struct {\n");
	for (i = 0; i < g_name_count; i++) buf_printf(out, "\t%s ravenc_raven.ID\n", g_names[i]);
	buf_puts(out, "}\n\n");
	if (!ps->root) return;
	buf_printf(out, "// %s_create builds the interface of %s.\nfunc %s_create() ravenc_raven.ID {\n", unit, base_name(source_path), unit);
	for (i = 0; i < g_binding_count; i++) {
		buf_puts(out, "\travenc_raven.ActionBind(");
		buf_literal(out, g_bindings[i].action, LANG_GO);
		buf_printf(out, ", %s)\n", g_bindings[i].function);
	}
	buf_puts(out, "\tnodes := []ravenc_raven.CompiledNode{\n");
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		attribute_t* a;
		buf_puts(out, "\t\t{Tag: ");
		buf_literal(out, n->tag, LANG_GO);
		buf_puts(out, ", Text: ");
		buf_literal(out, n->text, LANG_GO);
		if (n->attribute_count) {
			buf_puts(out, ", Attributes: []string{");
			for (a = n->attributes; a; a = a->next) {
				buf_literal(out, a->name, LANG_GO);
				buf_puts(out, ", ");
				buf_literal(out, a->value, LANG_GO);
				if (a->next) buf_puts(out, ", ");
			}
			buf_puts(out, "}");
		}
		buf_printf(out, ", Parent: %d, Line: %d},\n", n->parent ? n->parent->index : -1, n->line);
	}
	buf_printf(out, "\t}\n\troot := ravenc_raven.CreateCompiled(nodes, %s, ", is_window ? "true" : "false");
	buf_literal(out, source_path, LANG_GO);
	buf_puts(out, ")\n");
	if (g_name_count) {
		buf_puts(out, "\tif root != ravenc_raven.R_INVALID_ID {\n");
		for (i = 0; i < g_name_count; i++) buf_printf(out, "\t\tui.%s = ravenc_raven.WidgetFind(root, \"%s\")\n", g_names[i], g_names[i]);
		buf_puts(out, "\t}\n");
	}
	buf_puts(out, "\treturn root\n}\n");
}

/* ---- C# ---- */

static void pascal(char* out, size_t size, const char* unit) {
	size_t n = 0;
	int up = 1;
	for (; *unit && n + 1 < size; unit++) {
		if (*unit == '_') { up = 1; continue; }
		out[n++] = up ? (char)toupper((unsigned char)*unit) : *unit;
		up = 0;
	}
	out[n] = 0;
}

static void emit_csharp(buffer_t* out, const parser_t* ps, const char* unit, const char* source_path, int is_window) {
	char cls[160];
	int i;
	pascal(cls, sizeof(cls) - 3, unit);
	strcat(cls, "Ui");
	emit_scripts_in_place(out, ps, NULL);
	buf_printf(out, "\n// ---- generated by ravenc from %s: edit it, not this file ----\n\n", source_path);
	buf_printf(out, "/// <summary>The interface of %s: Create() builds it; the widgets that have a Name are its fields.</summary>\n", base_name(source_path));
	buf_printf(out, "public static partial class %s\n{\n", cls);
	for (i = 0; i < g_name_count; i++) buf_printf(out, "    public static uint %s = 0xFFFFFFFF;\n", g_names[i]);
	if (ps->root) {
		buf_puts(out, "\n    public static uint Create()\n    {\n");
		for (i = 0; i < g_binding_count; i++) {
			const char* f = g_bindings[i].function;
			buf_puts(out, "        global::Raven.R.Raction_bind(");
			buf_literal(out, g_bindings[i].action, LANG_CSHARP);
			buf_printf(out, ", global::Raven.R.Keep<global::Raven.RActionCallback>((sender, action, user) => %s(sender, "
				"global::System.Runtime.InteropServices.Marshal.PtrToStringUTF8(action) ?? \"\")), global::System.IntPtr.Zero);\n", f);
		}
		buf_puts(out, "        var memory = new global::System.Collections.Generic.List<global::System.IntPtr>();\n"
			"        global::System.IntPtr S(string text) { var p = global::System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(text); memory.Add(p); return p; }\n"
			"        global::System.IntPtr A(params string[] texts)\n        {\n"
			"            var p = global::System.Runtime.InteropServices.Marshal.AllocCoTaskMem(global::System.IntPtr.Size * texts.Length);\n"
			"            memory.Add(p);\n"
			"            for (int k = 0; k < texts.Length; k++) global::System.Runtime.InteropServices.Marshal.WriteIntPtr(p, k * global::System.IntPtr.Size, S(texts[k]));\n"
			"            return p;\n        }\n");
		buf_printf(out, "        var nodes = new global::Raven.RXmlCompiledNode[%d];\n", g_count);
		for (i = 0; i < g_count; i++) {
			node_t* n = g_table[i];
			attribute_t* a;
			buf_printf(out, "        nodes[%d].tag = S(", i);
			buf_literal(out, n->tag, LANG_CSHARP);
			buf_printf(out, "); nodes[%d].text = S(", i);
			buf_literal(out, n->text, LANG_CSHARP);
			buf_puts(out, ");");
			if (n->attribute_count) {
				buf_printf(out, " nodes[%d].attributes = A(", i);
				for (a = n->attributes; a; a = a->next) {
					buf_literal(out, a->name, LANG_CSHARP);
					buf_puts(out, ", ");
					buf_literal(out, a->value, LANG_CSHARP);
					if (a->next) buf_puts(out, ", ");
				}
				buf_puts(out, ");");
			}
			buf_printf(out, " nodes[%d].attribute_count = %d; nodes[%d].parent = %d; nodes[%d].line = %d;\n", i, n->attribute_count, i,
				n->parent ? n->parent->index : -1, i, n->line);
		}
		buf_printf(out, "        uint root = global::Raven.R.%s(ref nodes[0], %d, ", is_window ? "Rwindow_create_from_compiled" : "Rwidget_create_from_compiled", g_count);
		buf_literal(out, source_path, LANG_CSHARP);
		buf_puts(out, ");\n        foreach (var p in memory) global::System.Runtime.InteropServices.Marshal.FreeCoTaskMem(p);\n");
		if (g_name_count) {
			buf_puts(out, "        if (root != 0xFFFFFFFF)\n        {\n");
			for (i = 0; i < g_name_count; i++) buf_printf(out, "            %s = global::Raven.R.Rwidget_find(root, \"%s\");\n", g_names[i], g_names[i]);
			buf_puts(out, "        }\n");
		}
		buf_puts(out, "        return root;\n    }\n");
	}
	buf_puts(out, "}\n");
}

/* ---- Zig ---- */

static void emit_zig(buffer_t* out, const parser_t* ps, const char* unit, const char* source_path, int is_window) {
	int i;
	emit_scripts_in_place(out, ps, NULL);
	buf_printf(out, "\n// ---- generated by ravenc from %s: edit it, not this file ----\n\n", source_path);
	buf_puts(out, "const ravenc_r = @import(\"raven\");\n\n/// The widgets that have a Name (filled by the create function).\npub const RavencUi = struct {\n");
	for (i = 0; i < g_name_count; i++) buf_printf(out, "    %s: ravenc_r.Rid = ravenc_r.R_INVALID_ID,\n", g_names[i]);
	buf_puts(out, "};\npub var ui: RavencUi = .{};\n\n");
	if (!ps->root) return;
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		attribute_t* a;
		if (!n->attribute_count) continue;
		buf_printf(out, "const ravenc_a%d = [_]?[*:0]const u8{ ", i);
		for (a = n->attributes; a; a = a->next) {
			buf_literal(out, a->name, LANG_ZIG);
			buf_puts(out, ", ");
			buf_literal(out, a->value, LANG_ZIG);
			if (a->next) buf_puts(out, ", ");
		}
		buf_puts(out, " };\n");
	}
	buf_puts(out, "const ravenc_nodes = [_]ravenc_r.RXmlCompiledNode{\n");
	for (i = 0; i < g_count; i++) {
		node_t* n = g_table[i];
		buf_puts(out, "    .{ .tag = ");
		buf_literal(out, n->tag, LANG_ZIG);
		buf_puts(out, ", .text = ");
		buf_literal(out, n->text, LANG_ZIG);
		if (n->attribute_count) buf_printf(out, ", .attributes = &ravenc_a%d, .attribute_count = %d", i, n->attribute_count);
		buf_printf(out, ", .parent = %d, .line = %d },\n", n->parent ? n->parent->index : -1, n->line);
	}
	buf_printf(out, "};\n\n/// Builds the interface of %s.\npub fn %s_create() ravenc_r.Rid {\n", base_name(source_path), unit);
	for (i = 0; i < g_binding_count; i++) {
		buf_puts(out, "    ravenc_r.Raction_bind(");
		buf_literal(out, g_bindings[i].action, LANG_ZIG);
		buf_printf(out, ", &%s, null);\n", g_bindings[i].function);
	}
	buf_printf(out, "    const root = ravenc_r.%s(&ravenc_nodes[0], ravenc_nodes.len, ", is_window ? "Rwindow_create_from_compiled" : "Rwidget_create_from_compiled");
	buf_literal(out, source_path, LANG_ZIG);
	buf_puts(out, ");\n");
	if (g_name_count) {
		buf_puts(out, "    if (root != ravenc_r.R_INVALID_ID) {\n");
		for (i = 0; i < g_name_count; i++) buf_printf(out, "        ui.%s = ravenc_r.Rwidget_find(root, \"%s\");\n", g_names[i], g_names[i]);
		buf_puts(out, "    }\n");
	}
	buf_puts(out, "    return root;\n}\n");
}

/* ---- Lua (LuaJIT) ---- */

static void emit_lua(buffer_t* out, const parser_t* ps, const char* unit, const char* source_path, int is_window) {
	int i;
	buf_printf(out, "-- Generated by ravenc from %s: edit it, not this file.\n", source_path);
	buf_puts(out, "local ravenc_raven = require(\"raven\")\nlocal ravenc_ffi = ravenc_raven.ffi\nlocal ravenc_kept = {}\n\n");
	buf_puts(out, "-- the widgets that have a Name (filled by the create function)\nui = {\n");
	for (i = 0; i < g_name_count; i++) buf_printf(out, "\t%s = ravenc_raven.R_INVALID_ID,\n", g_names[i]);
	buf_puts(out, "}\n\n");
	if (ps->root) {
		/* (the functions named in {} are looked for when create runs: global functions, or fields of a global table) */
		buf_printf(out, "-- builds the interface of %s\nfunction %s_create()\n\tlocal R, ffi = ravenc_raven, ravenc_ffi\n", base_name(source_path), unit);
		buf_puts(out, "\tlocal keep = {}\n\tlocal function a(t)\n\t\tlocal array = ffi.new(\"const char*[?]\", #t, t)\n\t\tkeep[#keep + 1] = array\n\t\treturn array\n\tend\n");
		for (i = 0; i < g_binding_count; i++) {
			buf_printf(out, "\tlocal callback%d = ffi.cast(\"RActionCallback\", function(sender, action, user_data) %s(sender, ffi.string(action)) end)\n", i,
				g_bindings[i].function);
			buf_printf(out, "\travenc_kept[#ravenc_kept + 1] = callback%d\n\tR.Raction_bind(", i);
			buf_literal(out, g_bindings[i].action, LANG_LUA);
			buf_printf(out, ", callback%d, nil)\n", i);
		}
		buf_printf(out, "\tlocal nodes = ffi.new(\"RXmlCompiledNode[?]\", %d)\n", g_count);
		for (i = 0; i < g_count; i++) {
			node_t* n = g_table[i];
			attribute_t* a;
			buf_printf(out, "\tnodes[%d].tag = ", i);
			buf_literal(out, n->tag, LANG_LUA);
			buf_printf(out, "; nodes[%d].text = ", i);
			buf_literal(out, n->text, LANG_LUA);
			if (n->attribute_count) {
				buf_printf(out, "; nodes[%d].attributes = a({ ", i);
				for (a = n->attributes; a; a = a->next) {
					buf_literal(out, a->name, LANG_LUA);
					buf_puts(out, ", ");
					buf_literal(out, a->value, LANG_LUA);
					if (a->next) buf_puts(out, ", ");
				}
				buf_puts(out, " })");
			}
			buf_printf(out, "; nodes[%d].attribute_count = %d; nodes[%d].parent = %d; nodes[%d].line = %d\n", i, n->attribute_count, i,
				n->parent ? n->parent->index : -1, i, n->line);
		}
		buf_printf(out, "\tlocal root = R.%s(nodes, %d, ", is_window ? "Rwindow_create_from_compiled" : "Rwidget_create_from_compiled", g_count);
		buf_literal(out, source_path, LANG_LUA);
		buf_puts(out, ")\n\tif root ~= R.R_INVALID_ID then\n");
		for (i = 0; i < g_name_count; i++) buf_printf(out, "\t\tui.%s = R.Rwidget_find(root, \"%s\")\n", g_names[i], g_names[i]);
		buf_puts(out, "\tend\n\treturn root\nend\n\n");
	}
	buf_puts(out, "-- ---- the scripts ----\n");
	for (i = 0; i < ps->script_count; i++) {
		buf_printf(out, "-- %s, line %d\n", base_name(source_path), ps->scripts[i].line);
		buf_puts(out, ps->scripts[i].code);
		buf_puts(out, "\n");
	}
}

static int usage(void) {
	fprintf(stderr,
		"ravenc: compiles an interface written in Raven XML with its code into one source file\n"
		"usage: ravenc <file> -o <output> [--lang LANGUAGE] [--name NAME]\n"
		"  the language comes from the extension of the file:\n"
		"    .xmlc (C)  .xmlc++ .xmlcpp (C++)  .xmlrs (Rust)  .xmlpy (Python)\n"
		"    .xmlgo (Go)  .xmlcs (C#)  .xmlzig (Zig)  .xmllua (LuaJIT)\n"
		"  --lang LANGUAGE   c, cpp, rust, python, go, csharp, zig or lua, whatever the extension\n"
		"  --name NAME       the function is NAME_create (default: the file name)\n");
	return 2;
}

int main(int argc, char** argv) {
	const char *input = NULL, *output = NULL, *name = NULL, *lang_name = NULL;
	parser_t ps;
	buffer_t out = { 0 };
	char* source;
	char* source_path;
	char* out_path;
	char* unit;
	size_t size = 0;
	int i, is_window, result;
	lang_t lang = LANG_C;
	FILE* f;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
		else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "--lang") && i + 1 < argc) lang_name = argv[++i];
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return usage();
		else if (argv[i][0] == '-') return usage();
		else if (!input) input = argv[i];
		else return usage();
	}
	if (!input || !output) return usage();
	if (lang_name ? !language_named(lang_name, &lang) : !language_of_file(input, &lang)) {
		if (lang_name) { fprintf(stderr, "ravenc: unknown language %s\n", lang_name); return 2; }
		lang = LANG_C;
	}
	g_path = input;
	source = read_file(input, &size);
	if (!source) { fprintf(stderr, "ravenc: cannot read %s\n", input); return 1; }
	unit = name ? xstrndup(name, strlen(name)) : unit_name(input);
	if (!is_identifier(unit)) { fprintf(stderr, "ravenc: --name must be an identifier\n"); return 2; }

	memset(&ps, 0, sizeof(ps));
	ps.p = source;
	ps.end = source + size;
	ps.line = 1;
	if (size >= 3 && (unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) ps.p += 3;
	parse(&ps);
	if (ps.root) collect(ps.root, unit);
	is_window = ps.root && iequal(ps.root->tag, "Window");

	source_path = forward_slashes(input);
	out_path = forward_slashes(output);
	switch (lang) {
		case LANG_C:
		case LANG_CPP: emit_c_like(&out, &ps, lang, unit, source_path, out_path, is_window); break;
		case LANG_RUST: emit_rust(&out, &ps, unit, source_path, is_window); break;
		case LANG_PYTHON: emit_python(&out, &ps, unit, source_path, is_window); break;
		case LANG_GO: emit_go(&out, &ps, unit, source_path, is_window); break;
		case LANG_CSHARP: emit_csharp(&out, &ps, unit, source_path, is_window); break;
		case LANG_ZIG: emit_zig(&out, &ps, unit, source_path, is_window); break;
		case LANG_LUA: emit_lua(&out, &ps, unit, source_path, is_window); break;
	}

	f = fopen(output, "wb");
	result = 0;
	if (!f || fwrite(out.data, 1, out.size, f) != out.size) result = 1;
	if (f && fclose(f) != 0) result = 1;
	if (result) fprintf(stderr, "ravenc: cannot write %s\n", output);

	free_all(&ps);
	free(out.data);
	free(source);
	free(source_path);
	free(out_path);
	free(unit);
	return result;
}
