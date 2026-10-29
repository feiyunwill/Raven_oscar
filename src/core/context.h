/**
 * Raven - core/context.h
 * Global state of the library. Like HRL, Raven has one implicit context.
 */

#ifndef RVN_CONTEXT_H
#define RVN_CONTEXT_H

#include "base.h"
#include "thread.h"
#include "handle.h"
#include "jobs.h"
#include "../platform/platform.h"

struct rvn_window_t;
struct rvn_widget_t;

/** Values that replace the ones of the theme: for one widget, or for the widgets a style is made for. */
typedef struct rvn_style_t {
	uint64_t color_mask;
	uint32_t metric_mask;
	rvn_color_t colors[R_COLOR_COUNT];
	float metrics[R_METRIC_COUNT];
} rvn_style_t;

/** A style of a theme: values for the widgets of a type and / or of a class. */
typedef struct rvn_style_rule_t {
	/** RWidgetType, or 0 for every type. */
	int type;
	/** One class, or NULL for the widgets of the type whatever their classes. */
	char* cls;
	rvn_style_t style;
	/** 0 = not given. */
	RMaterial material;
} rvn_style_rule_t;

/** What the styles of a theme give, all together, to the widgets of one type with one set of classes. */
typedef struct rvn_style_match_t {
	int type;
	char* classes;
	rvn_style_t style;
	RMaterial material;
} rvn_style_match_t;

/** Number of kinds of token of the text editor (rvn_token_t in widgets/common.h). */
#define RVN_THEME_TOKENS 16

typedef struct rvn_theme_t {
	/** Name of a named theme; NULL for the theme in use (its name is g_rvn.theme_name). */
	char* name;
	/** File a named theme was read from, or NULL. */
	char* path;
	rvn_color_t colors[R_COLOR_COUNT];
	float metrics[R_METRIC_COUNT];
	Rid font;
	/** Material of the widgets that have none of their own. */
	RMaterial material;
	/** R_TRUE when that material is a glass. */
	int glass;
	/** Dark theme: glass is tinted accordingly. */
	int dark;
	/** Background drawn behind the content of windows, or R_INVALID_ID. */
	Rid wallpaper;
	/** Colors of the code, for the kinds of token of `token_mask`; the others follow `dark`. */
	rvn_color_t tokens[RVN_THEME_TOKENS];
	uint32_t token_mask;
	rvn_style_rule_t* rules;
	uint32_t rule_count;
	uint32_t rule_cap;
	/** Cache: what the rules give to each (type, classes) met so far. Emptied when a style changes. */
	rvn_style_match_t** matches;
	uint32_t match_count;
	uint32_t match_cap;
} rvn_theme_t;

typedef struct rvn_task_t {
	RTaskCallback function;
	void* user_data;
} rvn_task_t;

typedef struct rvn_timer_t {
	Rid id;
	double interval;
	double deadline;
	int repeat;
	RTimerCallback callback;
	void* user_data;
} rvn_timer_t;

typedef struct rvn_action_t {
	char* name;
	RActionCallback callback;
	void* user_data;
} rvn_action_t;

typedef struct rvn_context_t {
	int initialized;
	uint64_t ui_thread;
	double start_time;
	float ui_scale;
	int vsync;
	RBackend backend;
	const rvn_platform_t* platform;

	rvn_pool_t windows;
	rvn_pool_t widgets;
	rvn_pool_t fonts;
	rvn_pool_t textures;
	rvn_pool_t shaders;
	rvn_pool_t timers;
	rvn_pool_t tree_items;
	rvn_pool_t xml_documents;
	rvn_pool_t xml_nodes;

	/* main loop */
	rvn_atomic_t quit_requested;
	rvn_atomic_t exit_code;
	/** Set by Rwindow_request_redraw when called from another thread. */
	rvn_atomic_t redraw_all;
	int had_window;
	int in_update;
	/** While positive, the windows ignore the mouse and the keyboard (a dialog of the OS is open). */
	int input_blocked;
	double last_update_time;
	/** Seconds between the last two frames. */
	double delta_time;
	RFrameCallback frame_callback;
	void* frame_user;

	/* loss of the device (render/device.c) */
	RDeviceResetCallback device_reset_callback;
	void* device_reset_user;
	/** What Rinit gave the backend: it is started again with the same. */
	char* app_name;
	int enable_validation;
	/** When to try again after a recovery that failed (rvn_clock_seconds). */
	double device_retry_time;
	int device_retry_reported;

	/* tasks posted from other threads */
	rvn_mutex_t task_mutex;
	rvn_task_t* tasks;
	uint32_t task_count;
	uint32_t task_cap;

	/* actions */
	rvn_action_t* actions;
	uint32_t action_count;
	uint32_t action_cap;
	RActionCallback default_action;
	void* default_action_user;

	/*
	 * Deferred destruction. While events are being dispatched, deleted
	 * widgets and windows lose their handle at once but their memory is
	 * released only when the dispatch is over, so the code that called the
	 * application back can finish safely.
	 */
	int dispatch_depth;
	struct rvn_widget_t** dead_widgets;
	uint32_t dead_widget_count;
	uint32_t dead_widget_cap;
	struct rvn_window_t** dead_windows;
	uint32_t dead_window_count;
	uint32_t dead_window_cap;

	/* animations (widget/anim.c) */
	int animations_disabled;
	/** Multiplies the time of the animations; 0 = 1. */
	float animation_speed;
	/** When positive, every frame advances the animations by this many seconds, whatever the clock says. */
	double animation_time_step;

	/** The theme in use. Widgets given a theme of their own point to one of `themes` instead. */
	rvn_theme_t theme;
	char* theme_name;
	/** Named themes (widget/theme.c). Their addresses never change. */
	rvn_theme_t** themes;
	uint32_t theme_count;
	uint32_t theme_cap;
	/**
	 * Changes each time a theme, a style or the classes of a widget may have
	 * changed what a widget reads: each widget then looks again for the
	 * styles that apply to it (rvn_widget_restyle).
	 */
	uint32_t style_generation;
	rvn_mutex_t style_mutex;
	/** The built-in wallpapers, created on first use: dark, light. */
	Rid wallpapers[2];
	Rid default_font;
	Rid mono_font;
	char* clipboard;
	char* xml_base_path;
} rvn_context_t;

extern rvn_context_t g_rvn;

#define RVN_THEME_COLOR(role) (g_rvn.theme.colors[(int)(role) - (int)R_COLOR_WINDOW])
#define RVN_THEME_METRIC(metric) (g_rvn.theme.metrics[(int)(metric) - (int)R_METRIC_FONT_SIZE])

/**
 * Checks that the library is initialized and that the caller is the UI
 * thread; reports an error naming `function` otherwise.
 */
int rvn_api_check(const char* function);

#define RVN_API_GUARD(result) do { if (!rvn_api_check(__func__)) return result; } while (0)
#define RVN_API_GUARD_VOID() do { if (!rvn_api_check(__func__)) return; } while (0)

/** Calls the function bound to an action name (or the default handler). */
void rvn_fire_action(Rid sender, const char* action);

/** Seconds until the next timer fires, or a negative value when there is none. */
double rvn_timers_next_delay(void);

/** Brackets code that calls application callbacks (see dispatch_depth). */
void rvn_dispatch_begin(void);
void rvn_dispatch_end(void);

/** Tasks, timers, frame callback, then layout and paint of the windows that changed (one frame). */
void rvn_update(void);

/** Wakes the event loop so that a frame is produced soon. Thread-safe. */
void rvn_wake(void);

/**
 * Marks every widget for a new layout and every window for a new paint
 * (theme, font or texture size change). Implemented in widget/window.c.
 */
void rvn_invalidate_all_windows(void);
/** After the loss of the device: makes the surface of every window again (widget/window.c). */
void rvn_windows_device_reset(void);
/** The theme changed from dark to light or back: the windows that are seen through tell the window system. */
void rvn_windows_theme_changed(void);
/** Marks every window for a new paint, keeping the layout. Implemented in widget/window.c. */
void rvn_repaint_all_windows(void);

#endif /* RVN_CONTEXT_H */
