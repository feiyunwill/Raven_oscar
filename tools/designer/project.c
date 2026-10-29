/**
 * Raven Designer - project.c
 * A project is a folder built with the tools of its language: CMake (C, C++),
 * Cargo (Rust), go (Go), dotnet (C#), zig build (Zig), or nothing to build
 * (Python, Lua). The content drawer (Ctrl+Space) shows its files; each opens where it belongs.
 * New project writes one that builds as it is, of one of two kinds: with a
 * Raven interface (its interface with its code in main.xmlc, main.xmlrs...,
 * what its tools need, and strings.csv), or native (what the tools of its
 * language need and a classic Hello world, nothing of Raven).
 * The settings of the IDE for a project are in its file .raven-ide.
 */

#include "ide.h"

#ifndef RAVEN_IDE_INCLUDE_DIR
#define RAVEN_IDE_INCLUDE_DIR ""
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* g_project_dir = NULL;
ide_settings_t g_settings;

/* ---------------------------------------------------------------------------
 *  Settings
 * ------------------------------------------------------------------------- */

/* The package Raven itself comes in, when the IDE runs from one (bin/raven_designer). */
static void guess_raven_prefix(char* out, size_t size) {
	char* prefix = ide_join(ide_exe_dir(), "..");
	char* config = ide_join(prefix, "lib/cmake/raven/ravenConfig.cmake");
	out[0] = 0;
	if (ide_file_exists(config)) snprintf(out, size, "%s", prefix);
	free(config);
	free(prefix);
}

static void load_settings(void) {
	char* path;
	char* text;
	size_t size = 0;
	{
		/* (the package already chosen stays, unless the project says another one) */
		char kept[sizeof(g_settings.raven_prefix)];
		snprintf(kept, sizeof(kept), "%s", g_settings.raven_prefix);
		memset(&g_settings, 0, sizeof(g_settings));
		snprintf(g_settings.raven_prefix, sizeof(g_settings.raven_prefix), "%s", kept);
	}
	snprintf(g_settings.build_type, sizeof(g_settings.build_type), "Debug");
	if (!g_settings.raven_prefix[0]) guess_raven_prefix(g_settings.raven_prefix, sizeof(g_settings.raven_prefix));
	/* the package chosen once (the start page, Build settings) serves every project */
	if (!g_settings.raven_prefix[0]) snprintf(g_settings.raven_prefix, sizeof(g_settings.raven_prefix), "%s", prefs_get("raven_prefix"));
	if (!g_project_dir) return;
	path = ide_join(g_project_dir, ".raven-ide");
	text = Rfile_read(path, &size);
	free(path);
	if (!text) return;
	{
		char* line = text;
		while (line && *line) {
			char* end = strchr(line, '\n');
			char* equal;
			if (end) *end = 0;
			if (end && end > line && end[-1] == '\r') end[-1] = 0;
			equal = strchr(line, '=');
			if (equal) {
				*equal = 0;
#define READ(field, key) if (!strcmp(line, key)) snprintf(g_settings.field, sizeof(g_settings.field), "%s", equal + 1)
				READ(build_type, "build_type");
				READ(raven_prefix, "raven_prefix");
				READ(generator, "generator");
				READ(run_arguments, "run_arguments");
#undef READ
			}
			line = end ? end + 1 : NULL;
		}
	}
	Rmemory_free(text);
}

void project_save_settings(void) {
	ide_buffer_t out = { 0 };
	char* path;
	if (!g_project_dir) return;
	ide_buffer_printf(&out, "build_type=%s\nraven_prefix=%s\ngenerator=%s\nrun_arguments=%s\n", g_settings.build_type,
		g_settings.raven_prefix, g_settings.generator, g_settings.run_arguments);
	path = ide_join(g_project_dir, ".raven-ide");
	Rfile_write(path, out.data, out.size);
	free(path);
	ide_buffer_free(&out);
}

/* ---------------------------------------------------------------------------
 *  The files of the project: the content drawer (drawer.c, Ctrl+Space)
 * ------------------------------------------------------------------------- */

void project_refresh_explorer(void) {
	/* (the drawer reads the folders each time it opens: an open one is read again) */
	if (drawer_is_open()) drawer_open();
}

/* ---------------------------------------------------------------------------
 *  Opening files
 * ------------------------------------------------------------------------- */

/* The visual editor holds changes: ask before it shows another interface. */
static int interface_can_change(void) {
	RMessageResult answer;
	if (!designer_is_modified()) return R_TRUE;
	answer = Rmessage_box(g_ide_window, "Unsaved changes", "The interface has changes that were not saved. Save them?", R_MESSAGE_YES_NO_CANCEL);
	if (answer == R_MESSAGE_RESULT_CANCEL) return R_FALSE;
	if (answer == R_MESSAGE_RESULT_YES) return designer_save();
	return R_TRUE;
}

void designer_open_file(const char* path);

/* The visual editor shows this file now. */
static int in_visual_editor(const char* path) {
	return designer_has_document() && designer_get_path() && ide_same_path(designer_get_path(), path);
}

static int is_interface_file(const char* path) {
	return ide_interface_language(path) != NULL || ide_iequal(ide_extension(path), "xml");
}

void ide_open_file(const char* path, int line) {
	const char* ext = ide_extension(path);
	if (!ide_file_exists(path)) {
		ide_status("%s does not exist.", path);
		return;
	}
	welcome_hide();
	if (is_interface_file(path) && in_visual_editor(path)) {
		/* it is in the preview: there it is edited (an .xmlc is whole in its XML panel, a line of the file is a line of the panel) */
		Rdock_area_open_panel(g_ide_dock, g_ide_center);
		if (line > 0) designer_go_to_line(line);
	} else if (is_interface_file(path)) {
		/* an interface opens as text, like any code: its preview is the island, or the visual editor when asked (ide_preview_file) */
		editors_open(path, line);
		island_show_preview();
	} else if (ide_iequal(ext, "csv") && line <= 0) {
		strings_open(path);
	} else {
		editors_open(path, line);
	}
}

void ide_preview_file(const char* path) {
	Rid editor;
	char* text = NULL;
	int modified = R_FALSE, ok;
	if (!is_interface_file(path)) {
		ide_open_file(path, 0);
		return;
	}
	if (!ide_file_exists(path)) {
		ide_status("%s does not exist.", path);
		return;
	}
	welcome_hide();
	if (!in_visual_editor(path)) {
		if (!interface_can_change()) return;
		/* the file leaves its code editor for the visual editor, its unsaved changes with it */
		editor = editors_find(path);
		if (editor != R_INVALID_ID) {
			text = ide_strdup(Rtext_editor_get_text(editor));
			modified = editors_is_modified(editor);
			editors_close_file(path);
		}
		if (ide_interface_language(path)) {
			ok = xmlc_open_text(path, text);
		} else {
			if (xmlc_is_open()) xmlc_close();
			if (text) designer_load_interface(text, path, R_FALSE);
			else designer_open_file(path);
			ok = designer_has_document();
		}
		free(text);
		if (!ok) return;
		if (modified) designer_set_modified(R_TRUE);
	}
	Rdock_area_open_panel(g_ide_dock, g_ide_center);
	ide_status("Preview of %s. Close the preview (View > Close the preview) to go back to its code.", ide_basename(path));
}

/* ---------------------------------------------------------------------------
 *  Templates of a new project, one per language
 *
 *  {{NAME}} is the name of the project, {{BINDINGS}} the folder of the
 *  bindings of Raven (share/raven/bindings in a package).
 * ------------------------------------------------------------------------- */

static const char TEMPLATE_STRINGS[] =
"key,en,fr\n"
"hello.title,Hello!,Bonjour !\n"
"hello.none,You have not clicked yet.,Vous n'avez pas encore cliqué.\n"
"hello.button,Click me,Cliquez-moi\n"
"clicks.one,{n} click,{n} clic\n"
"clicks.other,{n} clicks,{n} clics\n";

/* the interface, the same in every language: {{CALLBACK}} is the name of the function */
static const char TEMPLATE_WINDOW_XML[] =
"<Window Title=\"{{NAME}}\" Width=\"480\" Height=\"320\">\n"
"\t<VBox Spacing=\"12\" Slot.Padding=\"24\">\n"
"\t\t<Label Text=\"@hello.title\" TextSize=\"24\"/>\n"
"\t\t<Label Name=\"status\" Text=\"@hello.none\" Opacity=\"0.7\"/>\n"
"\t\t<Button Text=\"@hello.button\" Style=\"Primary\" OnClicked=\"{{{CALLBACK}}}\" Slot.HAlign=\"Left\"/>\n"
"\t</VBox>\n"
"</Window>\n";

static const char TEMPLATE_CMAKE[] =
"cmake_minimum_required(VERSION 3.16)\n"
"project({{NAME}} {{CMAKE_LANGUAGE}})\n"
"\n"
"# Raven: the package (bin/, include/, lib/). The IDE gives its folder in CMAKE_PREFIX_PATH.\n"
"find_package(raven REQUIRED)\n"
"\n"
"# {{MAIN}} holds the interface and its code: ravenc turns it into {{LANGUAGE}} (raven_target_xmlc)\n"
"add_executable({{NAME}} {{MAIN}} strings.csv)\n"
"target_link_libraries({{NAME}} PRIVATE raven::raven)\n"
"raven_target_xmlc({{NAME}})\n"
"if(MSVC)\n"
"\ttarget_compile_definitions({{NAME}} PRIVATE _CRT_SECURE_NO_WARNINGS)\n"
"endif()\n";

static const char SCRIPT_C[] =
"<!-- {{NAME}}: the interface and its C code, in one file. F5 builds and runs it. -->\n"
"<script>\n"
"#include <stdio.h>\n"
"\n"
"static int g_clicks = 0;\n"
"\n"
"/* OnClicked=\"{on_hello}\" below calls this function: the compiler checks it */\n"
"static void on_hello(Rid sender, const char* action, void* user_data) {\n"
"\t(void)sender;\n"
"\t(void)action;\n"
"\t(void)user_data;\n"
"\tg_clicks++;\n"
"\t/* ui.status: the widget with Name=\"status\" */\n"
"\tRlabel_set_text(ui.status, Rtr_plural(\"clicks\", g_clicks));\n"
"}\n"
"\n"
"int main(void) {\n"
"\tif (!Rinit(R_BACKEND_VULKAN)) return 1;\n"
"\tRlocale_load_csv(\"strings.csv\");\n"
"\tRlocale_set_language(Rlocale_get_system_language());\n"
"\tmain_create();\n"
"\twhile (Rdraw_frame()) {}\n"
"\tRshutdown();\n"
"\treturn 0;\n"
"}\n"
"</script>\n"
"\n";

static const char SCRIPT_CPP[] =
"<!-- {{NAME}}: the interface and its C++ code, in one file. F5 builds and runs it. -->\n"
"<script>\n"
"#include <string>\n"
"\n"
"static int g_clicks = 0;\n"
"\n"
"// OnClicked=\"{on_hello}\" below calls this function: the compiler checks it\n"
"static void on_hello(Rid sender, const char* action) {\n"
"\t(void)sender;\n"
"\t(void)action;\n"
"\tg_clicks++;\n"
"\t// ui.status: the widget with Name=\"status\"\n"
"\tRlabel_set_text(ui.status, Rtr_plural(\"clicks\", g_clicks));\n"
"}\n"
"\n"
"int main() {\n"
"\tif (!Rinit(R_BACKEND_VULKAN)) return 1;\n"
"\tRlocale_load_csv(\"strings.csv\");\n"
"\tRlocale_set_language(Rlocale_get_system_language());\n"
"\tmain_create();\n"
"\twhile (Rdraw_frame()) {}\n"
"\tRshutdown();\n"
"\treturn 0;\n"
"}\n"
"</script>\n"
"\n";

static const char CARGO_TOML[] =
"[package]\n"
"name = \"{{NAME}}\"\n"
"version = \"0.1.0\"\n"
"edition = \"2021\"\n"
"\n"
"[dependencies]\n"
"raven = { package = \"raven-gui\", path = \"{{BINDINGS}}/rust\" }\n"
"\n"
"[build-dependencies]\n"
"raven-build = { package = \"raven-gui-build\", path = \"{{BINDINGS}}/rust-build\" }\n";

static const char CARGO_BUILD_RS[] =
"// ravenc turns src/main.xmlrs into Rust ($OUT_DIR/main.xmlrs.rs)\n"
"fn main() {\n"
"    raven_build::compile(\"src/main.xmlrs\");\n"
"}\n";

static const char CARGO_MAIN_RS[] =
"// The program is src/main.xmlrs: the interface and its Rust code.\n"
"include!(concat!(env!(\"OUT_DIR\"), \"/main.xmlrs.rs\"));\n";

static const char SCRIPT_RUST[] =
"<!-- {{NAME}}: the interface and its Rust code, in one file. F5 builds and runs it. -->\n"
"<script>\n"
"use raven::*;\n"
"use std::sync::atomic::{AtomicI32, Ordering};\n"
"\n"
"static CLICKS: AtomicI32 = AtomicI32::new(0);\n"
"\n"
"/// OnClicked=\"{on_hello}\" below calls this function: the compiler checks it\n"
"fn on_hello(_sender: Rid, _action: &str) {\n"
"    let clicks = CLICKS.fetch_add(1, Ordering::SeqCst) + 1;\n"
"    // ui().status: the widget with Name=\"status\"\n"
"    unsafe { Rlabel_set_text(ui().status, Rtr_plural(cstr(\"clicks\").as_ptr(), clicks)) };\n"
"}\n"
"\n"
"fn main() {\n"
"    unsafe {\n"
"        if Rinit(R_BACKEND_VULKAN) == 0 {\n"
"            return;\n"
"        }\n"
"        Rlocale_load_csv(cstr(\"strings.csv\").as_ptr());\n"
"        Rlocale_set_language(Rlocale_get_system_language());\n"
"    }\n"
"    main_create();\n"
"    unsafe {\n"
"        while Rdraw_frame() != 0 {}\n"
"        Rshutdown();\n"
"    }\n"
"}\n"
"</script>\n"
"\n";

static const char SCRIPT_PYTHON[] =
"<!-- {{NAME}}: the interface and its Python code, in one file. F5 runs it. -->\n"
"<script>\n"
"import sys\n"
"import raven as r\n"
"\n"
"clicks = 0\n"
"\n"
"\n"
"# OnClicked=\"{on_hello}\" below calls this function\n"
"def on_hello(sender, action):\n"
"    global clicks\n"
"    clicks += 1\n"
"    # ui.status: the widget with Name=\"status\"\n"
"    r.Rlabel_set_text(ui.status, r.Rtr_plural(\"clicks\", clicks))\n"
"\n"
"\n"
"if __name__ == \"__main__\":\n"
"    if not r.Rinit(r.R_BACKEND_VULKAN):\n"
"        sys.exit(1)\n"
"    r.Rlocale_load_csv(\"strings.csv\")\n"
"    r.Rlocale_set_language(r.Rlocale_get_system_language())\n"
"    main_create()\n"
"    while r.Rdraw_frame():\n"
"        pass\n"
"    r.Rshutdown()\n"
"</script>\n"
"\n";

static const char GO_MOD[] =
"module {{NAME}}\n"
"\n"
"go 1.18\n"
"\n"
"require raven v0.0.0\n"
"\n"
"replace raven => {{BINDINGS}}/go\n";

static const char GO_GENERATE[] =
"package main\n"
"\n"
"// ravenc turns main.xmlgo into Go (main.xmlgo.go): go generate, then go build\n"
"//go:generate ravenc main.xmlgo -o main.xmlgo.go\n";

static const char SCRIPT_GO[] =
"<!-- {{NAME}}: the interface and its Go code, in one file. F5 builds and runs it. -->\n"
"<script>\n"
"package main\n"
"\n"
"import \"raven/raven\"\n"
"\n"
"var clicks = 0\n"
"\n"
"// OnClicked=\"{on_hello}\" below calls this function: the compiler checks it\n"
"func on_hello(sender raven.ID, action string) {\n"
"\tclicks++\n"
"\t// ui.status: the widget with Name=\"status\"\n"
"\traven.LabelSetText(ui.status, raven.TrPlural(\"clicks\", clicks))\n"
"}\n"
"\n"
"func main() {\n"
"\tif raven.Init(raven.R_BACKEND_VULKAN) == 0 {\n"
"\t\treturn\n"
"\t}\n"
"\traven.LocaleLoadCsv(\"strings.csv\")\n"
"\traven.LocaleSetLanguage(raven.LocaleGetSystemLanguage())\n"
"\tmain_create()\n"
"\tfor raven.DrawFrame() != 0 {\n"
"\t}\n"
"\traven.Shutdown()\n"
"}\n"
"</script>\n"
"\n";

static const char CSPROJ[] =
"<Project Sdk=\"Microsoft.NET.Sdk\">\n"
"  <PropertyGroup>\n"
"    <OutputType>Exe</OutputType>\n"
"    <TargetFramework>net8.0</TargetFramework>\n"
"    <Nullable>disable</Nullable>\n"
"    <ImplicitUsings>disable</ImplicitUsings>\n"
"  </PropertyGroup>\n"
"  <ItemGroup>\n"
"    <Compile Include=\"{{BINDINGS}}/csharp/Raven.cs\" Link=\"Raven.cs\" />\n"
"    <None Update=\"strings.csv\" CopyToOutputDirectory=\"PreserveNewest\" />\n"
"  </ItemGroup>\n"
"  <!-- ravenc turns every .xmlcs of the project into C# before the compiler runs -->\n"
"  <Import Project=\"{{BINDINGS}}/csharp/Raven.XmlCs.targets\" />\n"
"</Project>\n";

static const char SCRIPT_CSHARP[] =
"<!-- {{NAME}}: the interface and its C# code, in one file. F5 builds and runs it. -->\n"
"<script>\n"
"using Raven;\n"
"\n"
"public static partial class MainUi\n"
"{\n"
"    static int clicks;\n"
"\n"
"    // OnClicked=\"{OnHello}\" below calls this method: the compiler checks it\n"
"    static void OnHello(uint sender, string action)\n"
"    {\n"
"        clicks++;\n"
"        // status: the widget with Name=\"status\"\n"
"        R.Rlabel_set_text(status, R.Rtr_plural(\"clicks\", clicks));\n"
"    }\n"
"}\n"
"\n"
"public static class Program\n"
"{\n"
"    public static int Main()\n"
"    {\n"
"        if (R.Rinit(RBackend.R_BACKEND_VULKAN) == 0) return 1;\n"
"        R.Rlocale_load_csv(\"strings.csv\");\n"
"        R.Rlocale_set_language(R.Rlocale_get_system_language());\n"
"        MainUi.Create();\n"
"        while (R.Rdraw_frame() != 0) { }\n"
"        R.Rshutdown();\n"
"        return 0;\n"
"    }\n"
"}\n"
"</script>\n"
"\n";

static const char BUILD_ZIG[] =
"const std = @import(\"std\");\n"
"// (copies of bindings/zig of Raven: a build.zig only imports files of its project)\n"
"const xmlzig = @import(\"raven/xmlzig.zig\");\n"
"\n"
"pub fn build(b: *std.Build) void {\n"
"    const target = b.standardTargetOptions(.{});\n"
"    const optimize = b.standardOptimizeOption(.{});\n"
"    const raven = b.createModule(.{ .root_source_file = b.path(\"raven/raven.zig\"), .target = target, .optimize = optimize });\n"
"    // ravenc turns main.xmlzig into Zig: it is the program\n"
"    const exe = b.addExecutable(.{\n"
"        .name = \"{{NAME}}\",\n"
"        .root_module = b.createModule(.{ .root_source_file = xmlzig.compile(b, \"main.xmlzig\"), .target = target, .optimize = optimize }),\n"
"    });\n"
"    exe.root_module.addImport(\"raven\", raven);\n"
"    exe.root_module.linkSystemLibrary(\"raven\", .{});\n"
"    exe.root_module.link_libc = true;\n"
"    if (b.graph.env_map.get(\"RAVEN_LIB_DIR\")) |dir| exe.root_module.addLibraryPath(.{ .cwd_relative = dir });\n"
"    b.installArtifact(exe);\n"
"    const run = b.addRunArtifact(exe);\n"
"    run.setCwd(b.path(\".\"));\n"
"    b.step(\"run\", \"Run the program\").dependOn(&run.step);\n"
"}\n";

static const char SCRIPT_ZIG[] =
"<!-- {{NAME}}: the interface and its Zig code, in one file. F5 builds and runs it. -->\n"
"<script>\n"
"const r = @import(\"raven\");\n"
"\n"
"var clicks: c_int = 0;\n"
"\n"
"// OnClicked=\"{on_hello}\" below calls this function: the compiler checks it\n"
"fn on_hello(sender: r.Rid, action: ?[*:0]const u8, user_data: ?*anyopaque) callconv(.C) void {\n"
"    _ = sender;\n"
"    _ = action;\n"
"    _ = user_data;\n"
"    clicks += 1;\n"
"    // ui.status: the widget with Name=\"status\"\n"
"    r.Rlabel_set_text(ui.status, r.Rtr_plural(\"clicks\", clicks));\n"
"}\n"
"\n"
"pub fn main() void {\n"
"    if (r.Rinit(r.R_BACKEND_VULKAN) == 0) return;\n"
"    _ = r.Rlocale_load_csv(\"strings.csv\");\n"
"    _ = r.Rlocale_set_language(r.Rlocale_get_system_language());\n"
"    _ = main_create();\n"
"    while (r.Rdraw_frame() != 0) {}\n"
"    r.Rshutdown();\n"
"}\n"
"</script>\n"
"\n";

static const char SCRIPT_LUA[] =
"<!-- {{NAME}}: the interface and its Lua code (LuaJIT), in one file. F5 runs it. -->\n"
"<script>\n"
"local r = require(\"raven\")\n"
"\n"
"clicks = 0\n"
"\n"
"-- OnClicked=\"{on_hello}\" below calls this function (a global function)\n"
"function on_hello(sender, action)\n"
"\tclicks = clicks + 1\n"
"\t-- ui.status: the widget with Name=\"status\"\n"
"\tr.Rlabel_set_text(ui.status, r.Rtr_plural(\"clicks\", clicks))\n"
"end\n"
"\n"
"if r.Rinit(r.R_BACKEND_VULKAN) == 0 then os.exit(1) end\n"
"r.Rlocale_load_csv(\"strings.csv\")\n"
"r.Rlocale_set_language(r.Rlocale_get_system_language())\n"
"main_create()\n"
"while r.Rdraw_frame() ~= 0 do end\n"
"r.Rshutdown()\n"
"</script>\n"
"\n";

/* ---------------------------------------------------------------------------
 *  Native projects: the classic Hello world of each language, built with its
 *  own tools, and nothing of Raven (no interface file, no strings.csv).
 * ------------------------------------------------------------------------- */

static const char NATIVE_CMAKE[] =
"cmake_minimum_required(VERSION 3.16)\n"
"project({{NAME}} {{CMAKE_LANGUAGE}})\n"
"\n"
"add_executable({{NAME}} {{MAIN}})\n"
"if(MSVC)\n"
"\ttarget_compile_definitions({{NAME}} PRIVATE _CRT_SECURE_NO_WARNINGS)\n"
"endif()\n";

static const char NATIVE_C[] =
"#include <stdio.h>\n"
"\n"
"int main(void) {\n"
"\tprintf(\"Hello, world!\\n\");\n"
"\treturn 0;\n"
"}\n";

static const char NATIVE_CPP[] =
"#include <iostream>\n"
"\n"
"int main() {\n"
"\tstd::cout << \"Hello, world!\" << std::endl;\n"
"\treturn 0;\n"
"}\n";

static const char NATIVE_CARGO_TOML[] =
"[package]\n"
"name = \"{{NAME}}\"\n"
"version = \"0.1.0\"\n"
"edition = \"2021\"\n"
"\n"
"[dependencies]\n";

static const char NATIVE_RUST[] =
"fn main() {\n"
"    println!(\"Hello, world!\");\n"
"}\n";

static const char NATIVE_PYTHON[] =
"def main():\n"
"    print(\"Hello, world!\")\n"
"\n"
"\n"
"if __name__ == \"__main__\":\n"
"    main()\n";

static const char NATIVE_GO_MOD[] =
"module {{NAME}}\n"
"\n"
"go 1.18\n";

static const char NATIVE_GO[] =
"package main\n"
"\n"
"import \"fmt\"\n"
"\n"
"func main() {\n"
"\tfmt.Println(\"Hello, world!\")\n"
"}\n";

static const char NATIVE_CSPROJ[] =
"<Project Sdk=\"Microsoft.NET.Sdk\">\n"
"  <PropertyGroup>\n"
"    <OutputType>Exe</OutputType>\n"
"    <TargetFramework>net8.0</TargetFramework>\n"
"    <Nullable>enable</Nullable>\n"
"    <ImplicitUsings>enable</ImplicitUsings>\n"
"  </PropertyGroup>\n"
"</Project>\n";

static const char NATIVE_CSHARP[] =
"public static class Program\n"
"{\n"
"    public static void Main()\n"
"    {\n"
"        System.Console.WriteLine(\"Hello, world!\");\n"
"    }\n"
"}\n";

static const char NATIVE_BUILD_ZIG[] =
"const std = @import(\"std\");\n"
"\n"
"pub fn build(b: *std.Build) void {\n"
"    const target = b.standardTargetOptions(.{});\n"
"    const optimize = b.standardOptimizeOption(.{});\n"
"    const exe = b.addExecutable(.{\n"
"        .name = \"{{NAME}}\",\n"
"        .root_module = b.createModule(.{ .root_source_file = b.path(\"src/main.zig\"), .target = target, .optimize = optimize }),\n"
"    });\n"
"    b.installArtifact(exe);\n"
"    const run = b.addRunArtifact(exe);\n"
"    b.step(\"run\", \"Run the program\").dependOn(&run.step);\n"
"}\n";

static const char NATIVE_ZIG[] =
"const std = @import(\"std\");\n"
"\n"
"pub fn main() void {\n"
"    std.debug.print(\"Hello, world!\\n\", .{});\n"
"}\n";

static const char NATIVE_LUA[] =
"print(\"Hello, world!\")\n";

/* "{{NAME}}" and the others, replaced */
static char* expand(const char* text, const char* name, const char* bindings, const char* extra_key, const char* extra_value) {
	ide_buffer_t out = { 0 };
	const char* p = text;
	while (*p) {
		const char* open = strstr(p, "{{");
		const char* value = NULL;
		size_t skip = 0;
		if (!open) {
			ide_buffer_add(&out, p, strlen(p));
			break;
		}
		ide_buffer_add(&out, p, (size_t)(open - p));
		if (!strncmp(open, "{{NAME}}", 8)) { value = name; skip = 8; }
		else if (!strncmp(open, "{{BINDINGS}}", 12)) { value = bindings; skip = 12; }
		else if (extra_key && !strncmp(open, extra_key, strlen(extra_key))) { value = extra_value; skip = strlen(extra_key); }
		if (value) {
			ide_buffer_add(&out, value, strlen(value));
			p = open + skip;
		} else {
			ide_buffer_add(&out, "{", 1);
			p = open + 1;
		}
	}
	return out.data ? out.data : ide_strdup("");
}

/* Where the bindings of Raven are: the package, or the sources the IDE was built from. */
static char* bindings_dir(void) {
	char* candidates[3];
	char* found = NULL;
	int i;
	candidates[0] = g_settings.raven_prefix[0] ? ide_join(g_settings.raven_prefix, "share/raven/bindings") : NULL;
	candidates[1] = ide_join(ide_exe_dir(), "../share/raven/bindings");
	candidates[2] = RAVEN_IDE_INCLUDE_DIR[0] ? ide_join(RAVEN_IDE_INCLUDE_DIR, "../bindings") : NULL;
	for (i = 0; i < 3; i++) {
		if (!found && candidates[i] && ide_is_dir(candidates[i])) found = ide_normalize(candidates[i]);
		free(candidates[i]);
	}
	return found ? found : ide_strdup("RAVEN/share/raven/bindings");
}

static int copy_file(const char* from, const char* to) {
	size_t size = 0;
	char* data = Rfile_read(from, &size);
	int ok;
	if (!data) return R_FALSE;
	ok = Rfile_write(to, data, size);
	Rmemory_free(data);
	return ok;
}

/* ---------------------------------------------------------------------------
 *  Projects
 * ------------------------------------------------------------------------- */

int project_target_name(char* out, size_t size) {
	char* path;
	char* text;
	const char* p;
	size_t length = 0;
	out[0] = 0;
	if (!g_project_dir) return R_FALSE;
	path = ide_join(g_project_dir, "CMakeLists.txt");
	text = Rfile_read(path, &length);
	free(path);
	if (!text) return R_FALSE;
	for (p = text; (p = strstr(p, "add_executable")) != NULL; p++) {
		const char* q = p + 14;
		size_t n = 0;
		while (*q == ' ' || *q == '\t') q++;
		if (*q != '(') continue;
		q++;
		while (isspace((unsigned char)*q)) q++;
		while (q[n] && (isalnum((unsigned char)q[n]) || q[n] == '_' || q[n] == '-' || q[n] == '.' || q[n] == '$' || q[n] == '{' || q[n] == '}')) n++;
		if (n == 0) continue;
		/* add_executable(${PROJECT_NAME} ...): the name of the project */
		if (!strncmp(q, "${PROJECT_NAME}", 15)) {
			const char* pr = strstr(text, "project(");
			if (pr) {
				pr += 8;
				while (isspace((unsigned char)*pr)) pr++;
				n = 0;
				while (pr[n] && (isalnum((unsigned char)pr[n]) || pr[n] == '_' || pr[n] == '-')) n++;
				snprintf(out, size, "%.*s", (int)n, pr);
			}
		} else {
			snprintf(out, size, "%.*s", (int)n, q);
		}
		break;
	}
	Rmemory_free(text);
	return out[0] != 0;
}

/* What the IDE holds goes away: the interface, the code editors, the build. */
static void close_everything(void) {
	build_reset();
	editors_close_all();
	designer_close();
}

int project_close(void) {
	if (!ide_confirm_unsaved("closing the project")) return R_FALSE;
	close_everything();
	free(g_project_dir);
	g_project_dir = NULL;
	load_settings();
	project_refresh_explorer();
	strings_reload();
	welcome_show();
	designer_update_title();
	ide_status("Project closed.");
	return R_TRUE;
}

int project_open(const char* folder) {
	char* normalized;
	if (!ide_is_dir(folder)) {
		ide_status("%s is not a folder.", folder);
		Rmessage_box(g_ide_window, "Open a project", "This folder does not exist (any more).", R_MESSAGE_OK);
		return R_FALSE;
	}
	normalized = ide_normalize(folder);
	{
		size_t n = strlen(normalized);
		while (n > 1 && normalized[n - 1] == '/') normalized[--n] = 0;
	}
	/* another project: what the first one holds is saved (or not) and closed */
	if (!ide_confirm_unsaved("opening another project")) {
		free(normalized);
		return R_FALSE;
	}
	close_everything();
	free(g_project_dir);
	g_project_dir = normalized;
	load_settings();
	project_refresh_explorer();
	strings_reload();
	welcome_hide();
	recent_add(g_project_dir);
	/* the interface of the project, when there is one: main.xml?? at the top or in src/ */
	{
		static const char* const folders[] = { "", "src" };
		int f, i, opened = 0;
		for (f = 0; f < 2 && !opened; f++) {
			char* dir = f ? ide_join(g_project_dir, folders[f]) : ide_strdup(g_project_dir);
			for (i = 0; i < ide_language_count() && !opened; i++) {
				char name[64];
				char* path;
				snprintf(name, sizeof(name), "main.%s", ide_language_at(i)->extension);
				path = ide_join(dir, name);
				if (ide_file_exists(path)) {
					ide_open_file(path, 0);
					opened = 1;
				}
				free(path);
			}
			free(dir);
		}
		/* a native project: its program */
		{
			static const char* const natives[] = { "main.c", "main.cpp", "src/main.rs", "main.py", "main.go", "Program.cs", "src/main.zig", "main.lua" };
			for (i = 0; i < (int)(sizeof(natives) / sizeof(natives[0])) && !opened; i++) {
				char* path = ide_join(g_project_dir, natives[i]);
				if (ide_file_exists(path)) {
					ide_open_file(path, 0);
					opened = 1;
				}
				free(path);
			}
		}
	}
	designer_update_title();
	ide_status("Project %s opened. F5 builds and runs it.", ide_basename(g_project_dir));
	return R_TRUE;
}

static int valid_name(const char* name) {
	const char* p;
	if (!name || !name[0] || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return R_FALSE;
	for (p = name; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return R_FALSE;
	return R_TRUE;
}

static int write_new(const char* folder, const char* file, const char* text) {
	char* path = ide_join(folder, file);
	int ok = R_TRUE;
	if (!ide_file_exists(path)) ok = Rfile_write(path, text, strlen(text));
	free(path);
	return ok;
}

/* The interface, after the scripts, with the callback of the language. */
static char* main_file(const char* script, const char* name, const char* bindings, const char* callback) {
	char* head = expand(script, name, bindings, NULL, NULL);
	char* window = expand(TEMPLATE_WINDOW_XML, name, bindings, "{{CALLBACK}}", callback);
	char* both = (char*)malloc(strlen(head) + strlen(window) + 1);
	strcpy(both, head);
	strcat(both, window);
	free(head);
	free(window);
	return both;
}

static int write_expanded(const char* folder, const char* file, const char* text, const char* name, const char* bindings) {
	char* content = expand(text, name, bindings, NULL, NULL);
	int ok = write_new(folder, file, content);
	free(content);
	return ok;
}

/* A native project: the files its tools need and a Hello world, nothing of Raven. */
static int create_native(const char* folder, const char* name, const char* language) {
	int ok = R_TRUE;
	if (ide_iequal(language, "C") || ide_iequal(language, "C++")) {
		int cpp = ide_iequal(language, "C++");
		char* cmake = expand(NATIVE_CMAKE, name, "", "{{CMAKE_LANGUAGE}}", cpp ? "CXX" : "C");
		char* cmake2 = expand(cmake, name, "", "{{MAIN}}", cpp ? "main.cpp" : "main.c");
		ok = write_new(folder, "CMakeLists.txt", cmake2);
		free(cmake);
		free(cmake2);
		ok = ok && write_new(folder, cpp ? "main.cpp" : "main.c", cpp ? NATIVE_CPP : NATIVE_C);
		ok = ok && write_new(folder, ".gitignore", "build/\n.raven-ide\n");
	} else if (ide_iequal(language, "Rust")) {
		char* src = ide_join(folder, "src");
		ok = write_expanded(folder, "Cargo.toml", NATIVE_CARGO_TOML, name, "");
		ok = ok && ide_make_dirs(src);
		ok = ok && write_new(src, "main.rs", NATIVE_RUST);
		free(src);
		ok = ok && write_new(folder, ".gitignore", "target/\n.raven-ide\n");
	} else if (ide_iequal(language, "Python")) {
		ok = write_new(folder, "main.py", NATIVE_PYTHON);
		ok = ok && write_new(folder, ".gitignore", "__pycache__/\n.raven-ide\n");
	} else if (ide_iequal(language, "Go")) {
		ok = write_expanded(folder, "go.mod", NATIVE_GO_MOD, name, "");
		ok = ok && write_new(folder, "main.go", NATIVE_GO);
		ok = ok && write_new(folder, ".gitignore", ".raven-ide\n");
	} else if (ide_iequal(language, "C#")) {
		char csproj[300];
		snprintf(csproj, sizeof(csproj), "%s.csproj", name);
		ok = write_new(folder, csproj, NATIVE_CSPROJ);
		ok = ok && write_new(folder, "Program.cs", NATIVE_CSHARP);
		ok = ok && write_new(folder, ".gitignore", "bin/\nobj/\n.raven-ide\n");
	} else if (ide_iequal(language, "Zig")) {
		char* src = ide_join(folder, "src");
		ok = write_expanded(folder, "build.zig", NATIVE_BUILD_ZIG, name, "");
		ok = ok && ide_make_dirs(src);
		ok = ok && write_new(src, "main.zig", NATIVE_ZIG);
		free(src);
		ok = ok && write_new(folder, ".gitignore", "zig-out/\n.zig-cache/\n.raven-ide\n");
	} else if (ide_iequal(language, "Lua")) {
		ok = write_new(folder, "main.lua", NATIVE_LUA);
		ok = ok && write_new(folder, ".gitignore", ".raven-ide\n");
	} else {
		ide_status("Unknown language %s", language);
		ok = R_FALSE;
	}
	return ok;
}

int project_create(const char* parent_folder, const char* name, const char* language, int with_interface) {
	char* folder;
	char* bindings;
	char* text;
	const char* main_name = NULL;
	int ok = R_TRUE;
	if (!valid_name(name)) {
		ide_status("A project name is made of letters, digits, '_' and '-', and starts with a letter.");
		return R_FALSE;
	}
	folder = ide_join(parent_folder, name);
	if (!ide_make_dirs(folder)) {
		ide_status("Cannot create the folder %s", folder);
		free(folder);
		return R_FALSE;
	}
	if (!language || !language[0]) language = "C";

	if (!with_interface) {
		if (!create_native(folder, name, language)) {
			ide_status("Cannot write the files of the project in %s", folder);
			free(folder);
			return R_FALSE;
		}
		ok = project_open(folder);
		free(folder);
		return ok;
	}

	bindings = bindings_dir();
	if (ide_iequal(language, "C") || ide_iequal(language, "C++")) {
		int cpp = ide_iequal(language, "C++");
		char* cmake = expand(TEMPLATE_CMAKE, name, bindings, "{{CMAKE_LANGUAGE}}", cpp ? "CXX" : "C");
		char* cmake2 = expand(cmake, name, bindings, "{{MAIN}}", cpp ? "main.xmlc++" : "main.xmlc");
		char* cmake3 = expand(cmake2, name, bindings, "{{LANGUAGE}}", cpp ? "C++" : "C");
		ok = write_new(folder, "CMakeLists.txt", cmake3);
		free(cmake);
		free(cmake2);
		free(cmake3);
		main_name = cpp ? "main.xmlc++" : "main.xmlc";
		text = main_file(cpp ? SCRIPT_CPP : SCRIPT_C, name, bindings, "on_hello");
		ok = ok && write_new(folder, main_name, text);
		free(text);
		ok = ok && write_new(folder, ".gitignore", "build/\n.raven-ide\n");
	} else if (ide_iequal(language, "Rust")) {
		char* src = ide_join(folder, "src");
		ok = write_expanded(folder, "Cargo.toml", CARGO_TOML, name, bindings);
		ok = ok && write_new(folder, "build.rs", CARGO_BUILD_RS);
		ok = ok && ide_make_dirs(src);
		ok = ok && write_new(src, "main.rs", CARGO_MAIN_RS);
		text = main_file(SCRIPT_RUST, name, bindings, "on_hello");
		ok = ok && write_new(src, "main.xmlrs", text);
		free(text);
		free(src);
		ok = ok && write_new(folder, ".gitignore", "target/\n.raven-ide\n");
		main_name = "src/main.xmlrs";
	} else if (ide_iequal(language, "Python")) {
		main_name = "main.xmlpy";
		text = main_file(SCRIPT_PYTHON, name, bindings, "on_hello");
		ok = write_new(folder, main_name, text);
		free(text);
		ok = ok && write_new(folder, ".gitignore", "__pycache__/\n.raven-ide\n");
	} else if (ide_iequal(language, "Go")) {
		main_name = "main.xmlgo";
		ok = write_expanded(folder, "go.mod", GO_MOD, name, bindings);
		ok = ok && write_new(folder, "generate.go", GO_GENERATE);
		text = main_file(SCRIPT_GO, name, bindings, "on_hello");
		ok = ok && write_new(folder, main_name, text);
		free(text);
		ok = ok && write_new(folder, ".gitignore", "main.xmlgo.go\n.raven-ide\n");
	} else if (ide_iequal(language, "C#")) {
		char csproj[300];
		main_name = "main.xmlcs";
		snprintf(csproj, sizeof(csproj), "%s.csproj", name);
		ok = write_expanded(folder, csproj, CSPROJ, name, bindings);
		text = main_file(SCRIPT_CSHARP, name, bindings, "OnHello");
		ok = ok && write_new(folder, main_name, text);
		free(text);
		ok = ok && write_new(folder, ".gitignore", "bin/\nobj/\n.raven-ide\n");
	} else if (ide_iequal(language, "Zig")) {
		char* vendored = ide_join(folder, "raven");
		char* from;
		char* to;
		main_name = "main.xmlzig";
		ok = write_expanded(folder, "build.zig", BUILD_ZIG, name, bindings);
		ok = ok && ide_make_dirs(vendored);
		/* (a build.zig only imports the files of its project: the bindings are copied in it) */
		from = ide_join(bindings, "zig/raven.zig");
		to = ide_join(vendored, "raven.zig");
		ok = ok && copy_file(from, to);
		free(from);
		free(to);
		from = ide_join(bindings, "zig/xmlzig.zig");
		to = ide_join(vendored, "xmlzig.zig");
		ok = ok && copy_file(from, to);
		free(from);
		free(to);
		free(vendored);
		text = main_file(SCRIPT_ZIG, name, bindings, "on_hello");
		ok = ok && write_new(folder, main_name, text);
		free(text);
		ok = ok && write_new(folder, ".gitignore", "zig-out/\n.zig-cache/\n.raven-ide\n");
	} else if (ide_iequal(language, "Lua")) {
		main_name = "main.xmllua";
		text = main_file(SCRIPT_LUA, name, bindings, "on_hello");
		ok = write_new(folder, main_name, text);
		free(text);
		ok = ok && write_new(folder, ".gitignore", "*.xmllua.lua\n.raven-ide\n");
	} else {
		ide_status("Unknown language %s", language);
		ok = R_FALSE;
	}
	ok = ok && write_new(folder, "strings.csv", TEMPLATE_STRINGS);
	free(bindings);
	if (!ok) {
		ide_status("Cannot write the files of the project in %s (are the bindings of Raven there?)", folder);
		free(folder);
		return R_FALSE;
	}
	(void)main_name;
	ok = project_open(folder);
	free(folder);
	return ok;
}

/* ---------------------------------------------------------------------------
 *  Dialogs
 * ------------------------------------------------------------------------- */

/*
 * Rdialog_run deletes its content when it closes, and setting the text of an
 * input does not call its changed callback (Browse... does that): the dialogs
 * read their fields when a button is clicked, before closing.
 */

static const char NEW_PROJECT_XML[] =
"<Border Padding=\"24\" Width=\"600\">"
"  <VBox Spacing=\"14\">"
"    <Label Text=\"New project\" TextSize=\"22\"/>"
"    <Label Name=\"about\" Wrap=\"true\" Opacity=\"0.65\"/>"
"    <Grid Columns=\"0,1\" Spacing=\"10\">"
"      <Label Text=\"Name\" Slot.VAlign=\"Center\"/>"
"      <TextInput Name=\"name\" Slot.Column=\"1\" Placeholder=\"my_app\"/>"
"      <Label Text=\"Language\" Slot.Row=\"1\" Slot.VAlign=\"Center\"/>"
"      <ComboBox Name=\"language\" Slot.Row=\"1\" Slot.Column=\"1\" Items=\"C (CMake)|C++ (CMake)|Rust (Cargo)|Python|Go|C# (dotnet)|Zig (zig build)|Lua (LuaJIT)\" Selected=\"0\"/>"
"      <Label Text=\"Kind\" Slot.Row=\"2\" Slot.VAlign=\"Center\" Tooltip=\"With a Raven interface (.xmlc, .xmlrs, .xmlpy...), or the language alone\"/>"
"      <ComboBox Name=\"kind\" Slot.Row=\"2\" Slot.Column=\"1\" Items=\"Raven interface + code (main.xmlc, main.xmlrs, main.xmlpy...)|Native: the language alone, a classic Hello world\" Selected=\"0\"/>"
"      <Label Text=\"Location\" Slot.Row=\"3\" Slot.VAlign=\"Center\"/>"
"      <HBox Spacing=\"8\" Slot.Row=\"3\" Slot.Column=\"1\"><TextInput Name=\"folder\" Slot.Fill=\"1\"/><Button Name=\"browse\" Text=\"Browse...\"/></HBox>"
"      <Label Name=\"prefix.label\" Text=\"Raven package\" Slot.Row=\"4\" Slot.VAlign=\"Center\" Tooltip=\"The folder with bin/, include/ and lib/: what the project is built with\"/>"
"      <HBox Name=\"prefix.row\" Spacing=\"8\" Slot.Row=\"4\" Slot.Column=\"1\"><TextInput Name=\"prefix\" Slot.Fill=\"1\" Placeholder=\"the folder with bin/, include/ and lib/\"/><Button Name=\"browse.prefix\" Text=\"Browse...\"/></HBox>"
"    </Grid>"
"    <Label Name=\"where\" Opacity=\"0.6\" Wrap=\"true\"/>"
"    <Label Name=\"error\" Wrap=\"true\" Visibility=\"Collapsed\"/>"
"    <HBox Spacing=\"8\" Slot.HAlign=\"Right\"><Button Name=\"cancel\" Text=\"Cancel\"/><Button Name=\"create\" Text=\"Create\" Style=\"Primary\"/></HBox>"
"  </VBox>"
"</Border>";

static const char* const g_languages[] = { "C", "C++", "Rust", "Python", "Go", "C#", "Zig", "Lua" };

static struct {
	Rid content, name, language, kind, folder, prefix, where, error, about;
	/* what was read when Create was clicked */
	char name_text[256], folder_text[1024], prefix_text[1024];
	int language_index;
	int native;
} g_np;

/* The kind chosen: what the project will hold, said above the fields; the Raven package only matters with an interface. */
static void np_update_kind(void) {
	int native = Rcombo_box_get_selected(g_np.kind) == 1;
	Rlabel_set_text(g_np.about, native
		? "The language alone: what its tools need and a classic Hello world. No interface file and no translations: add them later if you want."
		: "An interface with its code in one file (main.xmlc, main.xmlrs...), what the tools of its language need, and its translations (strings.csv). It builds as it is.");
	Rwidget_set_visibility(Rwidget_find(g_np.content, "prefix.label"), native ? R_COLLAPSED : R_VISIBLE);
	Rwidget_set_visibility(Rwidget_find(g_np.content, "prefix.row"), native ? R_COLLAPSED : R_VISIBLE);
}

static void np_kind_changed(Rid combo, int index, void* user_data) {
	(void)combo;
	(void)index;
	(void)user_data;
	np_update_kind();
}

static void on_dialog_button(Rid button, void* user_data) {
	Rdialog_close(button, (int)(size_t)user_data);
}

static void browse_into(Rid input, const char* title, int folder) {
	RFileDialogDesc desc;
	const char* chosen;
	memset(&desc, 0, sizeof(desc));
	desc.parent = g_ide_window;
	desc.title = title;
	chosen = folder ? Rfile_dialog_select_folder(&desc) : Rfile_dialog_open(&desc);
	if (chosen) Rtext_input_set_text(input, chosen);
}

static void np_update_where(void);

static void on_browse_folder(Rid button, void* user_data) {
	(void)button;
	browse_into((Rid)(size_t)user_data, "Choose a folder", R_TRUE);
	/* (setting the text calls no callback: the line under the fields follows by hand) */
	if (g_ide_dialog != R_INVALID_ID && (Rid)(size_t)user_data == g_np.folder) np_update_where();
}

static void np_show_error(const char* text) {
	Rlabel_set_text(g_np.error, text ? text : "");
	if (text && text[0]) {
		float r, gr, b, a;
		Rtheme_get_color(R_COLOR_DANGER, &r, &gr, &b, &a);
		{
			char color[16];
			snprintf(color, sizeof(color), "#%02X%02X%02X", (int)(r * 255.0f), (int)(gr * 255.0f), (int)(b * 255.0f));
			Rwidget_set_attribute(g_np.error, "Color.Text", color);
		}
	}
	Rwidget_set_visibility(g_np.error, text && text[0] ? R_VISIBLE : R_COLLAPSED);
}

static int folder_is_empty(const char* path) {
	ide_entry_t* entries;
	int count = ide_list_dir(path, &entries);
	free(entries);
	return count <= 0;
}

/* Where the project goes, shown under the fields as they are typed. */
static void np_update_where(void) {
	char text[1400], pretty[1100];
	char* full;
	const char* name = Rtext_input_get_text(g_np.name);
	const char* folder = Rtext_input_get_text(g_np.folder);
	if (!folder[0] || !name[0]) {
		Rlabel_set_text(g_np.where, "");
		return;
	}
	full = ide_join(folder, name);
	ide_pretty_path(full, pretty, sizeof(pretty));
	snprintf(text, sizeof(text), "Creates the folder %s", pretty);
	Rlabel_set_text(g_np.where, text);
	free(full);
}

static void np_changed(Rid input, const char* text, void* user_data) {
	(void)input;
	(void)text;
	(void)user_data;
	np_update_where();
	np_show_error(NULL);
}

static int valid_name(const char* name);

/* Create: what is wrong is said in the dialog, which stays open. */
static void on_create(Rid button, void* user_data) {
	char* full;
	int language;
	(void)user_data;
	snprintf(g_np.name_text, sizeof(g_np.name_text), "%s", Rtext_input_get_text(g_np.name));
	snprintf(g_np.folder_text, sizeof(g_np.folder_text), "%s", Rtext_input_get_text(g_np.folder));
	snprintf(g_np.prefix_text, sizeof(g_np.prefix_text), "%s", Rtext_input_get_text(g_np.prefix));
	language = Rcombo_box_get_selected(g_np.language);
	g_np.language_index = language >= 0 && language < 8 ? language : 0;
	g_np.native = Rcombo_box_get_selected(g_np.kind) == 1;
	if (!valid_name(g_np.name_text)) {
		np_show_error("The name is made of letters, digits, '_' and '-', and starts with a letter.");
		return;
	}
	if (!g_np.folder_text[0]) {
		np_show_error("Choose where the project goes (Location).");
		return;
	}
	full = ide_join(g_np.folder_text, g_np.name_text);
	if (ide_file_exists(full) || (ide_is_dir(full) && !folder_is_empty(full))) {
		free(full);
		np_show_error("There is already something with that name there: choose another name or another location.");
		return;
	}
	free(full);
	/* the languages built by their own tools need the bindings of the package */
	if (!g_np.native && g_np.language_index >= 2 && g_np.language_index != 3 && g_np.language_index != 7 && g_np.prefix_text[0]) {
		char* bindings = ide_join(g_np.prefix_text, "share/raven/bindings");
		int found = ide_is_dir(bindings);
		free(bindings);
		if (!found) {
			np_show_error("This Raven package has no share/raven/bindings: the project could not find Raven. Choose the folder of a complete package.");
			return;
		}
	}
	Rdialog_close(button, 1);
}

void project_new(void);
void project_new(void) {
	Rid content = Rwidget_create_from_xml(NEW_PROJECT_XML, sizeof(NEW_PROJECT_XML) - 1);
	const char* location;
	if (content == R_INVALID_ID) return;
	memset(&g_np, 0, sizeof(g_np));
	g_np.content = content;
	g_np.name = Rwidget_find(content, "name");
	g_np.language = Rwidget_find(content, "language");
	g_np.kind = Rwidget_find(content, "kind");
	g_np.about = Rwidget_find(content, "about");
	g_np.folder = Rwidget_find(content, "folder");
	g_np.prefix = Rwidget_find(content, "prefix");
	g_np.where = Rwidget_find(content, "where");
	g_np.error = Rwidget_find(content, "error");
	/* a name not taken yet, in the folder of the last project created */
	location = prefs_get("projects_folder")[0] ? prefs_get("projects_folder") : ide_default_projects_dir();
	Rtext_input_set_text(g_np.folder, location);
	{
		char name[64];
		int i;
		snprintf(name, sizeof(name), "my_app");
		for (i = 2; i < 100; i++) {
			char* full = ide_join(location, name);
			int taken = ide_file_exists(full) || ide_is_dir(full);
			free(full);
			if (!taken) break;
			snprintf(name, sizeof(name), "my_app_%d", i);
		}
		Rtext_input_set_text(g_np.name, name);
	}
	Rtext_input_set_text(g_np.prefix, g_settings.raven_prefix);
	{
		int last = atoi(prefs_get("projects_language"));
		Rcombo_box_set_selected(g_np.language, last >= 0 && last < 8 ? last : 0);
	}
	/* (the kind of the last project created) */
	Rcombo_box_set_selected(g_np.kind, !strcmp(prefs_get("projects_native"), "1") ? 1 : 0);
	Rcombo_box_set_changed_callback(g_np.kind, np_kind_changed, NULL);
	np_update_kind();
	Rtext_input_set_changed_callback(g_np.name, np_changed, NULL);
	Rtext_input_set_changed_callback(g_np.folder, np_changed, NULL);
	np_update_where();
	Rbutton_set_clicked_callback(Rwidget_find(content, "browse"), on_browse_folder, (void*)(size_t)g_np.folder);
	Rbutton_set_clicked_callback(Rwidget_find(content, "browse.prefix"), on_browse_folder, (void*)(size_t)g_np.prefix);
	Rbutton_set_clicked_callback(Rwidget_find(content, "cancel"), on_dialog_button, (void*)0);
	Rbutton_set_clicked_callback(Rwidget_find(content, "create"), on_create, NULL);
	Rwidget_focus(g_np.name);
	g_ide_dialog = content;
	if (Rdialog_run(g_ide_window, content) != 1) {
		g_ide_dialog = R_INVALID_ID;
		return;
	}
	g_ide_dialog = R_INVALID_ID;
	/* (the fields are gone: what was read when Create was clicked) */
	prefs_set("projects_folder", g_np.folder_text);
	{
		char index[8];
		snprintf(index, sizeof(index), "%d", g_np.language_index);
		prefs_set("projects_language", index);
	}
	prefs_set("projects_native", g_np.native ? "1" : "0");
	if (!g_np.native && g_np.prefix_text[0]) {
		snprintf(g_settings.raven_prefix, sizeof(g_settings.raven_prefix), "%s", g_np.prefix_text);
		prefs_set("raven_prefix", g_np.prefix_text);
	}
	if (!project_create(g_np.folder_text, g_np.name_text, g_languages[g_np.language_index], !g_np.native)) {
		char text[1400];
		snprintf(text, sizeof(text), "The project %s could not be created in %s. The status bar says why.", g_np.name_text, g_np.folder_text);
		Rmessage_box(g_ide_window, "New project", text, R_MESSAGE_OK);
	}
}

static const char SETTINGS_XML[] =
"<Border Padding=\"24\" Width=\"640\">"
"  <VBox Spacing=\"14\">"
"    <Label Text=\"Build settings\" TextSize=\"22\"/>"
"    <Label Name=\"for\" Opacity=\"0.65\" Wrap=\"true\"/>"
"    <Grid Columns=\"0,1\" Spacing=\"10\">"
"      <Label Text=\"Build type\" Slot.VAlign=\"Center\"/>"
"      <ComboBox Name=\"type\" Slot.Column=\"1\" Items=\"Debug|Release|RelWithDebInfo|MinSizeRel\"/>"
"      <Label Text=\"Raven package\" Slot.Row=\"1\" Slot.VAlign=\"Center\" Tooltip=\"The folder of the Raven package (bin/, include/, lib/): find_package(raven) looks there\"/>"
"      <HBox Spacing=\"8\" Slot.Row=\"1\" Slot.Column=\"1\"><TextInput Name=\"prefix\" Slot.Fill=\"1\" Placeholder=\"the folder with bin/, include/ and lib/\"/><Button Name=\"browse.prefix\" Text=\"Browse...\"/></HBox>"
"      <Label Text=\"Generator\" Slot.Row=\"2\" Slot.VAlign=\"Center\"/>"
"      <TextInput Name=\"generator\" Slot.Row=\"2\" Slot.Column=\"1\" Placeholder=\"automatic: Ninja when it is installed (MinGW Makefiles, Unix Makefiles...)\"/>"
"      <Label Text=\"Run arguments\" Slot.Row=\"3\" Slot.VAlign=\"Center\"/>"
"      <TextInput Name=\"arguments\" Slot.Row=\"3\" Slot.Column=\"1\"/>"
"    </Grid>"
"    <HBox Spacing=\"8\" Slot.HAlign=\"Right\"><Button Name=\"cancel\" Text=\"Cancel\"/><Button Name=\"ok\" Text=\"Save\" Style=\"Primary\"/></HBox>"
"  </VBox>"
"</Border>";

static ide_settings_t g_edit;
static Rid g_settings_content;

static void on_settings_save(Rid button, void* user_data) {
	Rid content = g_settings_content, type = Rwidget_find(content, "type");
	int index = Rcombo_box_get_selected(type);
	(void)user_data;
	if (index >= 0) snprintf(g_edit.build_type, sizeof(g_edit.build_type), "%s", Rcombo_box_get_item_text(type, index));
#define READ_FIELD(name, field) snprintf(g_edit.field, sizeof(g_edit.field), "%s", Rtext_input_get_text(Rwidget_find(content, name)))
	READ_FIELD("prefix", raven_prefix);
	READ_FIELD("generator", generator);
	READ_FIELD("arguments", run_arguments);
#undef READ_FIELD
	Rdialog_close(button, 1);
}

void project_settings_dialog(void);
void project_settings_dialog(void) {
	Rid content = Rwidget_create_from_xml(SETTINGS_XML, sizeof(SETTINGS_XML) - 1);
	Rid type;
	int i;
	if (content == R_INVALID_ID) return;
	g_settings_content = content;
	g_edit = g_settings;
	Rlabel_set_text(Rwidget_find(content, "for"), g_project_dir ? "For this project (kept in its file .raven-ide). The Raven package is also the one new projects start with."
		: "No project is open: the Raven package is kept for the projects to come.");
	type = Rwidget_find(content, "type");
	for (i = 0; i < Rcombo_box_get_item_count(type); i++) if (ide_iequal(Rcombo_box_get_item_text(type, i), g_settings.build_type)) Rcombo_box_set_selected(type, i);
	Rtext_input_set_text(Rwidget_find(content, "prefix"), g_edit.raven_prefix);
	Rtext_input_set_text(Rwidget_find(content, "generator"), g_edit.generator);
	Rtext_input_set_text(Rwidget_find(content, "arguments"), g_edit.run_arguments);
	Rbutton_set_clicked_callback(Rwidget_find(content, "browse.prefix"), on_browse_folder, (void*)(size_t)Rwidget_find(content, "prefix"));
	Rbutton_set_clicked_callback(Rwidget_find(content, "cancel"), on_dialog_button, (void*)0);
	Rbutton_set_clicked_callback(Rwidget_find(content, "ok"), on_settings_save, NULL);
	g_ide_dialog = content;
	if (Rdialog_run(g_ide_window, content) != 1) {
		g_ide_dialog = R_INVALID_ID;
		return;
	}
	g_ide_dialog = R_INVALID_ID;
	g_settings = g_edit;
	if (g_settings.raven_prefix[0]) prefs_set("raven_prefix", g_settings.raven_prefix);
	project_save_settings();
	ide_status(g_project_dir ? "Settings saved. The next build configures the project again." : "Settings kept for the projects to come.");
}

static void action_project(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	if (!strcmp(action, "project.new")) {
		project_new();
	} else if (!strcmp(action, "project.open")) {
		RFileDialogDesc desc;
		const char* chosen;
		memset(&desc, 0, sizeof(desc));
		desc.parent = g_ide_window;
		desc.title = "Open a project folder";
		chosen = Rfile_dialog_select_folder(&desc);
		if (chosen) {
			char* copy = ide_strdup(chosen);
			project_open(copy);
			free(copy);
		}
	} else if (!strcmp(action, "project.refresh")) {
		project_refresh_explorer();
	} else if (!strcmp(action, "project.settings")) {
		project_settings_dialog();
	} else if (!strcmp(action, "project.close")) {
		if (g_project_dir) project_close();
		else welcome_show();
	}
}

void project_init(void) {
	Raction_bind("project.new", action_project, NULL);
	Raction_bind("project.open", action_project, NULL);
	Raction_bind("project.refresh", action_project, NULL);
	Raction_bind("project.settings", action_project, NULL);
	Raction_bind("project.close", action_project, NULL);
	load_settings();
	project_refresh_explorer();
}

void project_shutdown(void) {
	free(g_project_dir);
	g_project_dir = NULL;
}
