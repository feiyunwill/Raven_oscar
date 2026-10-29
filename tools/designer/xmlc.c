/**
 * Raven Designer - xmlc.c
 * An interface with its code in one file (.xmlc, .xmlrs, .xmlpy...): the
 * whole file is in one editor, scripts and interface together, as it is
 * written. The visual editor shows the interface element of the file (the
 * first element that is not a <script>); what it changes is written back in
 * its place, the rest of the file untouched.
 */

#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
	char* path;
} g_x;

static int line_of(const char* text, const char* at) {
	int line = 1;
	for (; text < at; text++) if (*text == '\n') line++;
	return line;
}

static int starts_i(const char* p, const char* end, const char* s) {
	size_t n = strlen(s), i;
	if ((size_t)(end - p) < n) return 0;
	for (i = 0; i < n; i++) if (tolower((unsigned char)p[i]) != tolower((unsigned char)s[i])) return 0;
	return 1;
}

/* After the end of the element that starts at p ('<'), or NULL. */
static const char* skip_element(const char* p, const char* end) {
	int depth = 0;
	while (p < end) {
		if (starts_i(p, end, "<!--")) {
			const char* close = strstr(p + 4, "-->");
			if (!close) return NULL;
			p = close + 3;
		} else if (starts_i(p, end, "<![CDATA[")) {
			const char* close = strstr(p + 9, "]]>");
			if (!close) return NULL;
			p = close + 3;
		} else if (*p == '<' && p + 1 < end && p[1] == '/') {
			const char* close = memchr(p, '>', (size_t)(end - p));
			if (!close) return NULL;
			p = close + 1;
			if (--depth == 0) return p;
		} else if (*p == '<') {
			/* an opening tag: its attributes may hold '>' in quotes */
			char quote = 0;
			const char* q = p + 1;
			for (; q < end; q++) {
				if (quote) { if (*q == quote) quote = 0; }
				else if (*q == '"' || *q == '\'') quote = *q;
				else if (*q == '>') break;
			}
			if (q >= end) return NULL;
			if (q[-1] != '/') depth++;
			p = q + 1;
			if (depth == 0) return p;
		} else {
			p++;
		}
	}
	return NULL;
}

int xmlc_find_interface(const char* text, size_t* start, size_t* end, int* line) {
	const char* p = text;
	const char* stop = text + strlen(text);
	while (p < stop) {
		if (starts_i(p, stop, "<!--")) {
			const char* close = strstr(p + 4, "-->");
			p = close ? close + 3 : stop;
		} else if (starts_i(p, stop, "<?") || starts_i(p, stop, "<!")) {
			const char* close = memchr(p, '>', (size_t)(stop - p));
			p = close ? close + 1 : stop;
		} else if (starts_i(p, stop, "<script") && (p + 7 >= stop || isspace((unsigned char)p[7]) || p[7] == '>' || p[7] == '/')) {
			/* a script: its code may hold anything, '<' included */
			const char* open_end = memchr(p, '>', (size_t)(stop - p));
			const char* close;
			if (!open_end) return -1;
			if (open_end[-1] == '/') {
				p = open_end + 1;
				continue;
			}
			for (close = open_end + 1; close < stop && !starts_i(close, stop, "</script"); close++) {}
			if (close >= stop) return -1;
			close = memchr(close, '>', (size_t)(stop - close));
			if (!close) return -1;
			p = close + 1;
		} else if (*p == '<') {
			const char* element_end = skip_element(p, stop);
			if (!element_end) return -1;
			*start = (size_t)(p - text);
			*end = (size_t)(element_end - text);
			if (line) *line = line_of(text, p);
			return R_TRUE;
		} else {
			p++;
		}
	}
	return R_FALSE;
}

int xmlc_is_open(void) {
	return g_x.path != NULL;
}

const char* xmlc_path(void) {
	return g_x.path;
}

void xmlc_close(void) {
	free(g_x.path);
	g_x.path = NULL;
}

int xmlc_open(const char* path) {
	return xmlc_open_text(path, NULL);
}

int xmlc_open_text(const char* path, const char* given) {
	size_t size = 0, start, end;
	char* text = given ? NULL : Rfile_read(path, &size);
	if (!given && !text) {
		ide_status("Cannot read %s", path);
		return R_FALSE;
	}
	if (given) {
		/* (the text of a code editor: a copy, freed as the file would be) */
		size = strlen(given);
		text = (char*)Rmemory_alloc(size + 1);
		memcpy(text, given, size + 1);
	}
	if (xmlc_is_open()) xmlc_close();
	g_x.path = ide_strdup(path);
	if (xmlc_find_interface(text, &start, &end, NULL) <= 0) {
		/* a file with only scripts: an interface to start from, at its end (not when its elements are not closed: that shows in the Problems panel) */
		ide_buffer_t out = { 0 };
		static const char start_with[] = "\n<Window Title=\"Window\" Width=\"480\" Height=\"320\">\n\t<VBox/>\n</Window>\n";
		int closed = xmlc_find_interface(text, &start, &end, NULL) == 0;
		ide_buffer_add(&out, text, strlen(text));
		if (closed) ide_buffer_add(&out, start_with, sizeof(start_with) - 1);
		designer_load_interface(out.data ? out.data : "", path, R_TRUE);
		ide_buffer_free(&out);
	} else {
		designer_load_interface(text, path, R_TRUE);
	}
	Rmemory_free(text);
	return R_TRUE;
}

int xmlc_save(void) {
	const char* text = designer_get_interface_text();
	int ok;
	if (!g_x.path) return R_FALSE;
	ok = Rfile_write(g_x.path, text, strlen(text));
	if (ok) {
		designer_set_modified(R_FALSE);
		ide_status("Saved %s", g_x.path);
	} else {
		ide_status("Cannot write %s", g_x.path);
	}
	return ok;
}

int xmlc_interface_line(void) {
	size_t start, end;
	int line = 1;
	if (xmlc_find_interface(designer_get_interface_text(), &start, &end, &line) <= 0) return 1;
	return line;
}
