/**
 * Raven Designer - os.c
 * Folders, files and programs, for Windows and for POSIX systems. Paths are
 * UTF-8; on Windows they go through the wide functions.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "ide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

/* ---------------------------------------------------------------------------
 *  Small things
 * ------------------------------------------------------------------------- */

char* ide_strdup(const char* text) {
	size_t length = text ? strlen(text) : 0;
	char* copy = (char*)malloc(length + 1);
	if (length) memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}

int ide_iequal(const char* a, const char* b) {
	if (!a || !b) return a == b;
	for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
	return *a == *b;
}

void ide_buffer_add(ide_buffer_t* buffer, const char* text, size_t length) {
	if (buffer->size + length + 1 > buffer->cap) {
		size_t cap = buffer->cap ? buffer->cap * 2 : 1024;
		while (cap < buffer->size + length + 1) cap *= 2;
		buffer->data = (char*)realloc(buffer->data, cap);
		buffer->cap = cap;
	}
	if (length) memcpy(buffer->data + buffer->size, text, length);
	buffer->size += length;
	buffer->data[buffer->size] = '\0';
}

void ide_buffer_printf(ide_buffer_t* buffer, const char* format, ...) {
	char small[2048];
	va_list args;
	int n;
	va_start(args, format);
	n = vsnprintf(small, sizeof(small), format, args);
	va_end(args);
	if (n < 0) return;
	if ((size_t)n < sizeof(small)) {
		ide_buffer_add(buffer, small, (size_t)n);
	} else {
		char* big = (char*)malloc((size_t)n + 1);
		va_start(args, format);
		vsnprintf(big, (size_t)n + 1, format, args);
		va_end(args);
		ide_buffer_add(buffer, big, (size_t)n);
		free(big);
	}
}

void ide_buffer_free(ide_buffer_t* buffer) {
	free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

char* ide_normalize(const char* path) {
	char* copy = ide_strdup(path);
	char* p;
	for (p = copy; *p; p++) if (*p == '\\') *p = '/';
	return copy;
}

char* ide_join(const char* a, const char* b) {
	size_t la = strlen(a), lb = strlen(b);
	char* out = (char*)malloc(la + lb + 2);
	memcpy(out, a, la);
	if (la && a[la - 1] != '/' && a[la - 1] != '\\') out[la++] = '/';
	memcpy(out + la, b, lb + 1);
	return out;
}

const char* ide_basename(const char* path) {
	const char* base = path;
	const char* p;
	for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
	return base;
}

const char* ide_extension(const char* path) {
	static char ext[16];
	const char* base = ide_basename(path);
	const char* dot = strrchr(base, '.');
	size_t i;
	if (!dot || dot == base) return "";
	for (i = 0; dot[1 + i] && i + 1 < sizeof(ext); i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
	ext[i] = '\0';
	return ext;
}

int ide_same_path(const char* a, const char* b) {
	if (!a || !b) return 0;
	for (; *a && *b; a++, b++) {
		char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
#if defined(_WIN32)
		x = (char)tolower((unsigned char)x);
		y = (char)tolower((unsigned char)y);
#endif
		if (x != y) return 0;
	}
	return *a == *b;
}

static int compare_entries(const void* a, const void* b) {
	const ide_entry_t* x = (const ide_entry_t*)a;
	const ide_entry_t* y = (const ide_entry_t*)b;
	const char *p, *q;
	if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
	for (p = x->name, q = y->name; *p && *q; p++, q++) {
		int c = tolower((unsigned char)*p) - tolower((unsigned char)*q);
		if (c) return c;
	}
	return (int)(unsigned char)*p - (int)(unsigned char)*q;
}

/* ---------------------------------------------------------------------------
 *  Windows
 * ------------------------------------------------------------------------- */

#if defined(_WIN32)

static wchar_t* widen(const char* text) {
	int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
	if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, n)) wide[0] = 0;
	return wide;
}

static void narrow(const wchar_t* wide, char* out, size_t size) {
	if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL)) out[0] = 0;
}

int ide_list_dir(const char* path, ide_entry_t** entries) {
	WIN32_FIND_DATAW data;
	char* pattern = ide_join(path, "*");
	wchar_t* wide = widen(pattern);
	HANDLE find = FindFirstFileW(wide, &data);
	int count = 0, cap = 0;
	free(pattern);
	free(wide);
	*entries = NULL;
	if (find == INVALID_HANDLE_VALUE) return -1;
	do {
		ide_entry_t entry;
		narrow(data.cFileName, entry.name, sizeof(entry.name));
		if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
		entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (count == cap) {
			cap = cap ? cap * 2 : 32;
			*entries = (ide_entry_t*)realloc(*entries, sizeof(ide_entry_t) * (size_t)cap);
		}
		(*entries)[count++] = entry;
	} while (FindNextFileW(find, &data));
	FindClose(find);
	if (count) qsort(*entries, (size_t)count, sizeof(ide_entry_t), compare_entries);
	return count;
}

int ide_is_dir(const char* path) {
	wchar_t* wide = widen(path);
	DWORD attributes = GetFileAttributesW(wide);
	free(wide);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int ide_file_exists(const char* path) {
	wchar_t* wide = widen(path);
	DWORD attributes = GetFileAttributesW(wide);
	free(wide);
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int ide_make_dirs(const char* path) {
	char* copy = ide_strdup(path);
	char* p;
	int ok;
	for (p = copy; *p; p++) {
		if ((*p == '/' || *p == '\\') && p > copy && p[-1] != ':') {
			char keep = *p;
			wchar_t* wide;
			*p = 0;
			wide = widen(copy);
			CreateDirectoryW(wide, NULL);
			free(wide);
			*p = keep;
		}
	}
	{
		wchar_t* wide = widen(copy);
		CreateDirectoryW(wide, NULL);
		free(wide);
	}
	ok = ide_is_dir(copy);
	free(copy);
	return ok;
}

double ide_file_time(const char* path) {
	WIN32_FILE_ATTRIBUTE_DATA data;
	wchar_t* wide = widen(path);
	BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
	ULARGE_INTEGER t;
	free(wide);
	if (!ok) return 0.0;
	t.LowPart = data.ftLastWriteTime.dwLowDateTime;
	t.HighPart = data.ftLastWriteTime.dwHighDateTime;
	return (double)(t.QuadPart / 10000000ULL) - 11644473600.0;
}

const char* ide_exe_dir(void) {
	static char dir[1024];
	if (!dir[0]) {
		wchar_t wide[1024];
		char* slash;
		GetModuleFileNameW(NULL, wide, 1024);
		narrow(wide, dir, sizeof(dir));
		slash = strrchr(dir, '\\');
		if (!slash) slash = strrchr(dir, '/');
		if (slash) *slash = 0;
	}
	return dir;
}

struct ide_process_t {
	HANDLE process;
	HANDLE read;
	int ended;
	int exit_code;
};

/* One argument of a command line, quoted the way the C runtime reads it back. */
static void append_argument(ide_buffer_t* line, const char* argument) {
	const char* p;
	int needs = !argument[0] || strpbrk(argument, " \t\"") != NULL;
	if (line->size) ide_buffer_add(line, " ", 1);
	if (!needs) {
		ide_buffer_add(line, argument, strlen(argument));
		return;
	}
	ide_buffer_add(line, "\"", 1);
	for (p = argument;; p++) {
		int backslashes = 0;
		while (*p == '\\') { p++; backslashes++; }
		if (!*p) {
			int i;
			for (i = 0; i < backslashes * 2; i++) ide_buffer_add(line, "\\", 1);
			break;
		}
		if (*p == '"') {
			int i;
			for (i = 0; i < backslashes * 2 + 1; i++) ide_buffer_add(line, "\\", 1);
		} else {
			int i;
			for (i = 0; i < backslashes; i++) ide_buffer_add(line, "\\", 1);
		}
		ide_buffer_add(line, p, 1);
	}
	ide_buffer_add(line, "\"", 1);
}

ide_process_t* ide_process_start(const char* const* argv, const char* directory) {
	SECURITY_ATTRIBUTES security;
	STARTUPINFOW startup;
	PROCESS_INFORMATION info;
	HANDLE read = NULL, write = NULL, null_input;
	ide_buffer_t line = { 0 };
	wchar_t *command, *folder = NULL;
	ide_process_t* process;
	BOOL ok;
	int i;

	memset(&security, 0, sizeof(security));
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	if (!CreatePipe(&read, &write, &security, 0)) return NULL;
	SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
	null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);

	for (i = 0; argv[i]; i++) append_argument(&line, argv[i]);
	command = widen(line.data ? line.data : "");
	ide_buffer_free(&line);
	if (directory && directory[0]) folder = widen(directory);

	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = null_input;
	startup.hStdOutput = write;
	startup.hStdError = write;
	memset(&info, 0, sizeof(info));
	ok = CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, folder, &startup, &info);
	free(command);
	free(folder);
	CloseHandle(write);
	if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
	if (!ok) {
		CloseHandle(read);
		return NULL;
	}
	CloseHandle(info.hThread);
	process = (ide_process_t*)calloc(1, sizeof(ide_process_t));
	process->process = info.hProcess;
	process->read = read;
	return process;
}

int ide_process_read(ide_process_t* process, char* buffer, int size) {
	DWORD available = 0, got = 0;
	if (!process->read) return -1;
	if (!PeekNamedPipe(process->read, NULL, 0, NULL, &available, NULL)) {
		CloseHandle(process->read);
		process->read = NULL;
		return -1;
	}
	if (available == 0) return 0;
	if (available > (DWORD)size) available = (DWORD)size;
	if (!ReadFile(process->read, buffer, available, &got, NULL)) return -1;
	return (int)got;
}

int ide_process_done(ide_process_t* process, int* exit_code) {
	DWORD code;
	if (!process->ended && WaitForSingleObject(process->process, 0) == WAIT_OBJECT_0) {
		GetExitCodeProcess(process->process, &code);
		process->exit_code = (int)code;
		process->ended = 1;
	}
	if (process->ended && exit_code) *exit_code = process->exit_code;
	return process->ended;
}

void ide_process_kill(ide_process_t* process) {
	if (!process->ended) TerminateProcess(process->process, 1);
}

void ide_process_free(ide_process_t* process) {
	if (!process) return;
	if (process->read) CloseHandle(process->read);
	CloseHandle(process->process);
	free(process);
}

void ide_set_env(const char* name, const char* value) {
	wchar_t* wide_name = widen(name);
	wchar_t* wide_value = widen(value ? value : "");
	SetEnvironmentVariableW(wide_name, value ? wide_value : NULL);
	free(wide_name);
	free(wide_value);
}

void ide_prepend_path(const char* folder) {
	wchar_t* wide = widen(folder);
	DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
	wchar_t* old = (wchar_t*)malloc(sizeof(wchar_t) * (n + 1));
	wchar_t* both;
	size_t length;
	old[0] = 0;
	if (n) GetEnvironmentVariableW(L"PATH", old, n + 1);
	if (wcsstr(old, wide)) {
		free(old);
		free(wide);
		return;
	}
	length = wcslen(wide) + wcslen(old) + 2;
	both = (wchar_t*)malloc(sizeof(wchar_t) * length);
	swprintf(both, length, L"%ls;%ls", wide, old);
	SetEnvironmentVariableW(L"PATH", both);
	free(both);
	free(old);
	free(wide);
}

int ide_find_program(const char* name, char* out, size_t size) {
	wchar_t wide_name[260], found[1024];
	MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 260);
	if (!SearchPathW(NULL, wide_name, L".exe", 1024, found, NULL)) return 0;
	narrow(found, out, size);
	return 1;
}

static void env_folder(const wchar_t* name, char* out, size_t size) {
	wchar_t value[1024];
	DWORD n = GetEnvironmentVariableW(name, value, 1024);
	out[0] = 0;
	if (n > 0 && n < 1024) narrow(value, out, size);
}

const char* ide_home_dir(void) {
	static char dir[1024];
	if (!dir[0]) {
		char* n;
		env_folder(L"USERPROFILE", dir, sizeof(dir));
		if (!dir[0]) snprintf(dir, sizeof(dir), "C:/");
		n = ide_normalize(dir);
		snprintf(dir, sizeof(dir), "%s", n);
		free(n);
	}
	return dir;
}

const char* ide_config_dir(void) {
	static char dir[1100];
	if (!dir[0]) {
		char base[1024];
		char* n;
		env_folder(L"APPDATA", base, sizeof(base));
		if (!base[0]) snprintf(base, sizeof(base), "%s/AppData/Roaming", ide_home_dir());
		n = ide_normalize(base);
		snprintf(dir, sizeof(dir), "%s/Raven", n);
		free(n);
		ide_make_dirs(dir);
	}
	return dir;
}

int ide_remove_file(const char* path) {
	wchar_t* wide = widen(path);
	BOOL ok = DeleteFileW(wide);
	free(wide);
	return ok != 0;
}

/* ---------------------------------------------------------------------------
 *  POSIX
 * ------------------------------------------------------------------------- */

#else

int ide_list_dir(const char* path, ide_entry_t** entries) {
	DIR* dir = opendir(path);
	struct dirent* item;
	int count = 0, cap = 0;
	*entries = NULL;
	if (!dir) return -1;
	while ((item = readdir(dir)) != NULL) {
		ide_entry_t entry;
		char* full;
		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
		snprintf(entry.name, sizeof(entry.name), "%s", item->d_name);
		full = ide_join(path, item->d_name);
		entry.is_dir = ide_is_dir(full);
		free(full);
		if (count == cap) {
			cap = cap ? cap * 2 : 32;
			*entries = (ide_entry_t*)realloc(*entries, sizeof(ide_entry_t) * (size_t)cap);
		}
		(*entries)[count++] = entry;
	}
	closedir(dir);
	if (count) qsort(*entries, (size_t)count, sizeof(ide_entry_t), compare_entries);
	return count;
}

int ide_is_dir(const char* path) {
	struct stat s;
	return stat(path, &s) == 0 && S_ISDIR(s.st_mode);
}

int ide_file_exists(const char* path) {
	struct stat s;
	return stat(path, &s) == 0 && S_ISREG(s.st_mode);
}

int ide_make_dirs(const char* path) {
	char* copy = ide_strdup(path);
	char* p;
	int ok;
	for (p = copy + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(copy, 0755);
			*p = '/';
		}
	}
	mkdir(copy, 0755);
	ok = ide_is_dir(copy);
	free(copy);
	return ok;
}

double ide_file_time(const char* path) {
	struct stat s;
	if (stat(path, &s) != 0) return 0.0;
	return (double)s.st_mtime;
}

const char* ide_exe_dir(void) {
	static char dir[1024];
	if (!dir[0]) {
		char* slash;
#if defined(__APPLE__)
		uint32_t size = sizeof(dir);
		if (_NSGetExecutablePath(dir, &size) != 0) dir[0] = 0;
#else
		ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
		dir[n > 0 ? n : 0] = 0;
#endif
		slash = strrchr(dir, '/');
		if (slash) *slash = 0;
		if (!dir[0]) strcpy(dir, ".");
	}
	return dir;
}

struct ide_process_t {
	pid_t pid;
	int read;
	int ended;
	int exit_code;
};

ide_process_t* ide_process_start(const char* const* argv, const char* directory) {
	int fds[2];
	pid_t pid;
	ide_process_t* process;
	if (pipe(fds) != 0) return NULL;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}
	if (pid == 0) {
		int null_input = open("/dev/null", O_RDONLY);
		if (null_input >= 0) dup2(null_input, 0);
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		/* its own group: Stop ends what it started too */
		setpgid(0, 0);
		if (directory && directory[0] && chdir(directory) != 0) _exit(126);
		execvp(argv[0], (char* const*)argv);
		fprintf(stderr, "cannot start %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	process = (ide_process_t*)calloc(1, sizeof(ide_process_t));
	process->pid = pid;
	process->read = fds[0];
	return process;
}

int ide_process_read(ide_process_t* process, char* buffer, int size) {
	ssize_t n;
	if (process->read < 0) return -1;
	n = read(process->read, buffer, (size_t)size);
	if (n > 0) return (int)n;
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
	close(process->read);
	process->read = -1;
	return -1;
}

int ide_process_done(ide_process_t* process, int* exit_code) {
	if (!process->ended) {
		int status = 0;
		if (waitpid(process->pid, &status, WNOHANG) == process->pid) {
			process->ended = 1;
			process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		}
	}
	if (process->ended && exit_code) *exit_code = process->exit_code;
	return process->ended;
}

void ide_process_kill(ide_process_t* process) {
	if (!process->ended) kill(-process->pid, SIGTERM);
}

void ide_process_free(ide_process_t* process) {
	if (!process) return;
	if (process->read >= 0) close(process->read);
	if (!process->ended) {
		int status;
		kill(-process->pid, SIGKILL);
		waitpid(process->pid, &status, 0);
	}
	free(process);
}

void ide_set_env(const char* name, const char* value) {
	if (value) setenv(name, value, 1);
	else unsetenv(name);
}

void ide_prepend_path(const char* folder) {
	const char* old = getenv("PATH");
	char* both;
	if (old && strstr(old, folder)) return;
	both = (char*)malloc(strlen(folder) + (old ? strlen(old) : 0) + 2);
	sprintf(both, "%s%s%s", folder, old ? ":" : "", old ? old : "");
	setenv("PATH", both, 1);
	free(both);
}

int ide_find_program(const char* name, char* out, size_t size) {
	const char* path = getenv("PATH");
	while (path && *path) {
		size_t length = strcspn(path, ":");
		char candidate[1024];
		snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, path, name);
		if (access(candidate, X_OK) == 0 && ide_file_exists(candidate)) {
			snprintf(out, size, "%s", candidate);
			return 1;
		}
		path += length;
		if (*path == ':') path++;
	}
	return 0;
}

const char* ide_home_dir(void) {
	static char dir[1024];
	if (!dir[0]) {
		const char* home = getenv("HOME");
		snprintf(dir, sizeof(dir), "%s", home && home[0] ? home : "/tmp");
	}
	return dir;
}

const char* ide_config_dir(void) {
	static char dir[1100];
	if (!dir[0]) {
#if defined(__APPLE__)
		snprintf(dir, sizeof(dir), "%s/Library/Application Support/Raven", ide_home_dir());
#else
		const char* xdg = getenv("XDG_CONFIG_HOME");
		if (xdg && xdg[0] == '/') snprintf(dir, sizeof(dir), "%s/raven", xdg);
		else snprintf(dir, sizeof(dir), "%s/.config/raven", ide_home_dir());
#endif
		ide_make_dirs(dir);
	}
	return dir;
}

int ide_remove_file(const char* path) {
	return unlink(path) == 0;
}

#endif

/* Where new projects go by default: Documents/Raven Projects (the Documents folder when there is one). */
const char* ide_default_projects_dir(void) {
	static char dir[1200];
	if (!dir[0]) {
		char* documents = ide_join(ide_home_dir(), "Documents");
		snprintf(dir, sizeof(dir), "%s/Raven Projects", ide_is_dir(documents) ? documents : ide_home_dir());
		free(documents);
	}
	return dir;
}

/* "C:/Users/ada/Documents/x" -> "~/Documents/x": shorter, for the eyes only. */
void ide_pretty_path(const char* path, char* out, size_t size) {
	char* normalized = ide_normalize(path);
	char* home = ide_normalize(ide_home_dir());
	size_t length = strlen(home);
	if (length > 1 && !strncmp(normalized, home, length) && (normalized[length] == '/' || !normalized[length]))
		snprintf(out, size, "~%s", normalized + length);
	else
		snprintf(out, size, "%s", normalized);
	free(normalized);
	free(home);
}
