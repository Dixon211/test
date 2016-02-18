/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "menu.h"

#include <curses.h>

#include <assert.h> /* assert() */
#include <stddef.h> /* NULL wchar_t */
#include <stdio.h> /* pclose() popen() */
#include <stdlib.h> /* free() */
#include <string.h>

#include "../cfg/config.h"
#include "../compat/reallocarray.h"
#include "../engine/cmds.h"
#include "../engine/keys.h"
#include "../engine/mode.h"
#include "../menus/menus.h"
#include "../modes/dialogs/msg_dialog.h"
#include "../ui/fileview.h"
#include "../ui/statusbar.h"
#include "../ui/ui.h"
#include "../utils/macros.h"
#include "../utils/path.h"
#include "../utils/str.h"
#include "../utils/utils.h"
#include "../cmd_core.h"
#include "../filelist.h"
#include "../status.h"
#include "cmdline.h"
#include "modes.h"

static const int SCROLL_GAP = 2;

static int complete_args(int id, const cmd_info_t *cmd_info, int arg_pos,
		void *extra_arg);
static int swap_range(void);
static int resolve_mark(char mark);
static char * menu_expand_macros(const char str[], int for_shell, int *usr1,
		int *usr2);
static char * menu_expand_envvars(const char *str);
static void post(int id);
static void menu_select_range(int id, const cmd_info_t *cmd_info);
static int skip_at_beginning(int id, const char *args);

static int key_handler(wchar_t key);
static void cmd_ctrl_b(key_info_t key_info, keys_info_t *keys_info);
static int can_scroll_menu_up(const menu_info *menu);
static void cmd_ctrl_c(key_info_t key_info, keys_info_t *keys_info);
static void cmd_ctrl_d(key_info_t key_info, keys_info_t *keys_info);
static void cmd_ctrl_e(key_info_t key_info, keys_info_t *keys_info);
static void cmd_ctrl_f(key_info_t key_info, keys_info_t *keys_info);
static int can_scroll_menu_down(const menu_info *menu);
static void change_menu_top(menu_info *const menu, int delta);
static void cmd_ctrl_l(key_info_t key_info, keys_info_t *keys_info);
static void cmd_ctrl_m(key_info_t key_info, keys_info_t *keys_info);
static void update_ui_on_leaving(void);
static void cmd_ctrl_u(key_info_t key_info, keys_info_t *keys_info);
static int get_effective_menu_scroll_offset(const menu_info *menu);
static void cmd_ctrl_y(key_info_t key_info, keys_info_t *keys_info);
static void cmd_slash(key_info_t key_info, keys_info_t *keys_info);
static void cmd_colon(key_info_t key_info, keys_info_t *keys_info);
static void cmd_question(key_info_t key_info, keys_info_t *keys_info);
static void cmd_B(key_info_t key_info, keys_info_t *keys_info);
static void cmd_G(key_info_t key_info, keys_info_t *keys_info);
static void cmd_H(key_info_t key_info, keys_info_t *keys_info);
static void cmd_L(key_info_t key_info, keys_info_t *keys_info);
static void cmd_M(key_info_t key_info, keys_info_t *keys_info);
static void cmd_N(key_info_t key_info, keys_info_t *keys_info);
static void cmd_b(key_info_t key_info, keys_info_t *keys_info);
static void dump_into_custom_view(int very);
static void cmd_dd(key_info_t key_info, keys_info_t *keys_info);
static void cmd_gf(key_info_t key_info, keys_info_t *keys_info);
static int pass_combination_to_khandler(const wchar_t keys[]);
static void cmd_gg(key_info_t key_info, keys_info_t *keys_info);
static void cmd_j(key_info_t key_info, keys_info_t *keys_info);
static void cmd_k(key_info_t key_info, keys_info_t *keys_info);
static void cmd_n(key_info_t key_info, keys_info_t *keys_info);
static void cmd_v(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zb(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zH(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zL(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zh(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zl(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zt(key_info_t key_info, keys_info_t *keys_info);
static void cmd_zz(key_info_t key_info, keys_info_t *keys_info);
static int all_lines_visible(const menu_info *const menu);
static int goto_cmd(const cmd_info_t *cmd_info);
static int quit_cmd(const cmd_info_t *cmd_info);
static void leave_menu_mode(int reset_selection);

static FileView *view;
static menu_info *menu;
static int last_search_backward;
static int was_redraw;
static int saved_top, saved_pos;

static keys_add_info_t builtin_cmds[] = {
	{L"\x02", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_b}}},
	{L"\x03", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_c}}},
	{L"\x04", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_d}}},
	{L"\x05", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_e}}},
	{L"\x06", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_f}}},
	{L"\x0c", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_l}}},
	/* return */
	{L"\x0d", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_m}}},
	{L"\x0e", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_j}}},
	{L"\x10", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_k}}},
	{L"\x15", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_u}}},
	{L"\x19", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_y}}},
	/* escape */
	{L"\x1b", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_c}}},
	{L"/", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_slash}}},
	{L":", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_colon}}},
	{L"?", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_question}}},
	{L"B", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_B}}},
	{L"G", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_G}}},
	{L"H", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_H}}},
	{L"L", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_L}}},
	{L"M", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_M}}},
	{L"N", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_N}}},
	{L"ZZ", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_c}}},
	{L"ZQ", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_c}}},
	{L"b", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_b}}},
	{L"dd", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_dd}}},
	{L"gf", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_gf}}},
	{L"gg", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_gg}}},
	{L"j", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_j}}},
	{L"k", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_k}}},
	{L"l", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_m}}},
	{L"n", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_n}}},
	{L"q", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_c}}},
	{L"v", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_v}}},
	{L"zb", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zb}}},
	{L"zH", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zH}}},
	{L"zL", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zL}}},
	{L"zh", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zh}}},
	{L"zl", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zl}}},
	{L"zt", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zt}}},
	{L"zz", {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zz}}},
#ifdef ENABLE_EXTENDED_KEYS
	{{KEY_PPAGE}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_b}}},
	{{KEY_NPAGE}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_f}}},
	{{KEY_UP}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_k}}},
	{{KEY_DOWN}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_j}}},
	{{KEY_RIGHT}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_ctrl_m}}},
	{{KEY_HOME}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_gg}}},
	{{KEY_END}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_G}}},
	{{L'z', KEY_LEFT}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zh}}},
	{{L'z', KEY_RIGHT}, {BUILTIN_KEYS, FOLLOWED_BY_NONE, {.handler = cmd_zl}}},
#endif /* ENABLE_EXTENDED_KEYS */
};

static const cmd_add_t commands[] = {
	{ .name = "",                 .abbr = NULL,    .emark = 0,  .id = -1,              .range = 1,    .bg = 0, .quote = 0, .regexp = 0,
		.handler = goto_cmd,        .qmark = 0,      .expand = 0, .cust_sep = 0,         .min_args = 0, .max_args = 0,       .select = 0, },
	{ .name = "exit",             .abbr = "exi",   .emark = 1,  .id = -1,              .range = 0,    .bg = 0, .quote = 0, .regexp = 0,
		.handler = quit_cmd,        .qmark = 0,      .expand = 0, .cust_sep = 0,         .min_args = 0, .max_args = 0,       .select = 0, },
	{ .name = "quit",             .abbr = "q",     .emark = 1,  .id = -1,              .range = 0,    .bg = 0, .quote = 0, .regexp = 0,
		.handler = quit_cmd,        .qmark = 0,      .expand = 0, .cust_sep = 0,         .min_args = 0, .max_args = 0,       .select = 0, },
	{ .name = "xit",              .abbr = "x",     .emark = 1,  .id = -1,              .range = 0,    .bg = 0, .quote = 0, .regexp = 0,
		.handler = quit_cmd,        .qmark = 0,      .expand = 0, .cust_sep = 0,         .min_args = 0, .max_args = 0,       .select = 0, },
};

/* Settings for the cmds unit. */
static cmds_conf_t cmds_conf = {
	.complete_args = &complete_args,
	.swap_range = &swap_range,
	.resolve_mark = &resolve_mark,
	.expand_macros = &menu_expand_macros,
	.expand_envvars = &menu_expand_envvars,
	.post = &post,
	.select_range = &menu_select_range,
	.skip_at_beginning = &skip_at_beginning,
};

static int
complete_args(int id, const cmd_info_t *cmd_info, int arg_pos, void *extra_arg)
{
	return 0;
}

static int
swap_range(void)
{
	return 0;
}

static int
resolve_mark(char mark)
{
	return -1;
}

/* Implementation of macros expansion callback for cmds unit.  Returns newly
 * allocated memory. */
static char *
menu_expand_macros(const char str[], int for_shell, int *usr1, int *usr2)
{
	return strdup(str);
}

static char *
menu_expand_envvars(const char *str)
{
	return strdup(str);
}

static void
post(int id)
{
}

static void
menu_select_range(int id, const cmd_info_t *cmd_info)
{
}

static int
skip_at_beginning(int id, const char *args)
{
	return -1;
}

void
init_menu_mode(void)
{
	int ret_code;

	ret_code = add_cmds(builtin_cmds, ARRAY_LEN(builtin_cmds), MENU_MODE);
	assert(ret_code == 0);

	(void)ret_code;

	set_def_handler(MENU_MODE, key_handler);

	init_cmds(0, &cmds_conf);
	add_builtin_commands((const cmd_add_t *)&commands, ARRAY_LEN(commands));
}

static int
key_handler(wchar_t key)
{
	const wchar_t shortcut[] = {key, L'\0'};

	if(pass_combination_to_khandler(shortcut) && menu->len == 0)
	{
		show_error_msg("No more items in the menu", "Menu will be closed");
		leave_menu_mode(1);
	}

	return 0;
}

void
enter_menu_mode(menu_info *m, FileView *active_view)
{
	if(curr_stats.load_stage < 2)
		return;

	assert(m->len > 0 && "Menu cannot be empty.");

	werase(status_bar);

	view = active_view;
	menu = m;
	vle_mode_set(MENU_MODE, VMT_PRIMARY);
	curr_stats.need_update = UT_FULL;
	was_redraw = 0;

	init_cmds(0, &cmds_conf);
}

void
menu_pre(void)
{
	touchwin(ruler_win);
	wrefresh(ruler_win);
}

void
menu_post(void)
{
	if(curr_stats.need_update != UT_NONE)
	{
		menu_redraw();
		curr_stats.need_update = UT_NONE;
	}
	status_bar_message(curr_stats.save_msg ? NULL : "");
}

void
menu_redraw(void)
{
	was_redraw = 1;
	redraw_menu(menu);
}

static void
cmd_ctrl_b(key_info_t key_info, keys_info_t *keys_info)
{
	if(can_scroll_menu_up(menu))
	{
		const int s = get_effective_menu_scroll_offset(menu);
		const int off = (menu->win_rows - 2) - SCROLL_GAP;
		menu->pos = get_last_visible_line(menu) - off;
		change_menu_top(menu, -off);
		if(cfg.scroll_off > 0 && menu->top + (menu->win_rows - 3) - menu->pos < s)
			menu->pos -= s - (menu->top + (menu->win_rows - 3) - menu->pos);

		update_menu();
	}
}

/* Returns non-zero if menu can be scrolled up. */
static int
can_scroll_menu_up(const menu_info *menu)
{
	return menu->top > 0;
}

static void
cmd_ctrl_c(key_info_t key_info, keys_info_t *keys_info)
{
	leave_menu_mode(1);
}

static void
cmd_ctrl_d(key_info_t key_info, keys_info_t *keys_info)
{
	const int s = get_effective_menu_scroll_offset(menu);
	clean_menu_position(menu);
	menu->top += DIV_ROUND_UP(menu->win_rows - 3, 2);
	menu->pos += DIV_ROUND_UP(menu->win_rows - 3, 2);
	if(cfg.scroll_off > 0 && menu->pos - menu->top < s)
		menu->pos += s - (menu->pos - menu->top);

	update_menu();
}

static void
cmd_ctrl_e(key_info_t key_info, keys_info_t *keys_info)
{
	if(can_scroll_menu_down(menu))
	{
		int off = MAX(cfg.scroll_off, 0);
		if(menu->pos <= menu->top + off)
			menu->pos = menu->top + 1 + off;

		menu->top++;
		update_menu();
	}
}

static void
cmd_ctrl_f(key_info_t key_info, keys_info_t *keys_info)
{
	if(can_scroll_menu_down(menu))
	{
		const int s = get_effective_menu_scroll_offset(menu);
		const int off = (menu->win_rows - 2) - SCROLL_GAP;
		menu->pos = menu->top + off;
		change_menu_top(menu, off);
		if(cfg.scroll_off > 0 && menu->pos - menu->top < s)
			menu->pos += s - (menu->pos - menu->top);

		update_menu();
	}
}

/* Returns non-zero if menu can be scrolled down. */
static int
can_scroll_menu_down(const menu_info *menu)
{
	return get_last_visible_line(menu) < menu->len - 1;
}

/* Moves top line of the menu ensuring that its value is correct. */
static void
change_menu_top(menu_info *const menu, int delta)
{
	menu->top = MAX(MIN(menu->top + delta, menu->len - (menu->win_rows - 2)), 0);
}

int
get_last_visible_line(const menu_info *menu)
{
	return menu->top + (menu->win_rows - 2) - 1;
}

/* Redraw TUI. */
static void
cmd_ctrl_l(key_info_t key_info, keys_info_t *keys_info)
{
	menu_redraw();
}

static void
cmd_ctrl_m(key_info_t key_info, keys_info_t *keys_info)
{
	static menu_info *saved_menu;

	vle_mode_set(NORMAL_MODE, VMT_PRIMARY);
	saved_menu = menu;
	if(menu->execute_handler != NULL && menu->execute_handler(curr_view, menu))
	{
		vle_mode_set(MENU_MODE, VMT_PRIMARY);
		menu_redraw();
		return;
	}

	if(!vle_mode_is(MENU_MODE))
	{
		reset_popup_menu(saved_menu);
	}
	else if(menu != saved_menu)
	{
		reset_popup_menu(saved_menu);
		update_menu();
	}

	update_ui_on_leaving();
}

/* Updates UI on leaving the mode trying to minimize efforts to do this. */
static void
update_ui_on_leaving(void)
{
	if(was_redraw)
	{
		update_screen(UT_FULL);
	}
	else
	{
		update_all_windows();
	}
}

static void
cmd_ctrl_u(key_info_t key_info, keys_info_t *keys_info)
{
	const int s = get_effective_menu_scroll_offset(menu);
	clean_menu_position(menu);

	if(cfg.scroll_off > 0 && menu->top + menu->win_rows - menu->pos < s)
		menu->pos -= s - (menu->top + (menu->win_rows - 3) - menu->pos);

	menu->top -= DIV_ROUND_UP(menu->win_rows - 3, 2);
	if(menu->top < 0)
		menu->top = 0;
	menu->pos -= DIV_ROUND_UP(menu->win_rows - 3, 2);

	update_menu();
}

/* Returns scroll offset value for the menu taking menu height into account. */
static int
get_effective_menu_scroll_offset(const menu_info *menu)
{
	return MIN(DIV_ROUND_UP(menu->win_rows - 3, 2) - 1, cfg.scroll_off);
}

static void
cmd_ctrl_y(key_info_t key_info, keys_info_t *keys_info)
{
	if(can_scroll_menu_up(menu))
	{
		int off = MAX(cfg.scroll_off, 0);
		if(menu->pos >= menu->top + menu->win_rows - 3 - off)
			menu->pos = menu->top - 1 + menu->win_rows - 3 - off;

		menu->top--;
		update_menu();
	}
}

static void
cmd_slash(key_info_t key_info, keys_info_t *keys_info)
{
	menu->search_repeat = def_count(key_info.count);
	last_search_backward = 0;
	menu->backward_search = 0;
	free(menu->regexp);
	menu->regexp = NULL;
	enter_cmdline_mode(CLS_MENU_FSEARCH, "", menu);
}

static void
cmd_colon(key_info_t key_info, keys_info_t *keys_info)
{
	cmds_conf.begin = 1;
	cmds_conf.current = menu->pos;
	cmds_conf.end = menu->len;
	enter_cmdline_mode(CLS_MENU_COMMAND, "", menu);
}

static void
cmd_question(key_info_t key_info, keys_info_t *keys_info)
{
	menu->search_repeat = def_count(key_info.count);
	last_search_backward = 1;
	menu->backward_search = 0;
	free(menu->regexp);
	enter_cmdline_mode(CLS_MENU_BSEARCH, "", menu);
}

/* Populates very custom (unsorted) view with list of files. */
static void
cmd_B(key_info_t key_info, keys_info_t *keys_info)
{
	dump_into_custom_view(1);
}

static void
cmd_G(key_info_t key_info, keys_info_t *keys_info)
{
	if(key_info.count == NO_COUNT_GIVEN)
		key_info.count = menu->len;

	clean_menu_position(menu);
	move_to_menu_pos(key_info.count - 1, menu);
	wrefresh(menu_win);
}

static void
cmd_H(key_info_t key_info, keys_info_t *keys_info)
{
	int top;
	int off = MAX(cfg.scroll_off, 0);
	if(off > menu->win_rows/2)
		return;

	if(menu->top == 0)
		top = 0;
	else
		top = menu->top + off;

	clean_menu_position(menu);
	move_to_menu_pos(top, menu);
	wrefresh(menu_win);
}

static void
cmd_L(key_info_t key_info, keys_info_t *keys_info)
{
	int top;
	int off;
	if(menu->key_handler != NULL)
	{
		if(pass_combination_to_khandler(L"L"))
		{
			return;
		}
	}

	off = MAX(cfg.scroll_off, 0);
	if(off > menu->win_rows/2)
		return;

	if(menu->top + menu->win_rows < menu->len - 1)
		top = menu->top + menu->win_rows - off;
	else
		top = menu->top + menu->win_rows;

	clean_menu_position(menu);
	move_to_menu_pos(top - 3, menu);
	wrefresh(menu_win);
}

/* Moves cursor to the middle of the window. */
static void
cmd_M(key_info_t key_info, keys_info_t *keys_info)
{
	int new_pos;
	if(menu->len < menu->win_rows)
		new_pos = DIV_ROUND_UP(menu->len, 2);
	else
		new_pos = menu->top + DIV_ROUND_UP(menu->win_rows - 3, 2);

	clean_menu_position(menu);
	move_to_menu_pos(MAX(0, new_pos - 1), menu);
	wrefresh(menu_win);
}

static void
cmd_N(key_info_t key_info, keys_info_t *keys_info)
{
	key_info.count = def_count(key_info.count);
	while(key_info.count-- > 0)
	{
		menus_search(menu, !last_search_backward);
	}
}

/* Populates custom view with list of files. */
static void
cmd_b(key_info_t key_info, keys_info_t *keys_info)
{
	dump_into_custom_view(0);
}

/* Makees custom view of specified type out of menu items. */
static void
dump_into_custom_view(int very)
{
	if(menu_to_custom_view(menu, view, very) != 0)
	{
		show_error_msg("Menu transformation",
				"No valid paths discovered in menu content");
		return;
	}

	leave_menu_mode(1);
}

static void
cmd_dd(key_info_t key_info, keys_info_t *keys_info)
{
	if(pass_combination_to_khandler(L"dd") && menu->len == 0)
	{
		show_error_msg("Menu is closing", "No more items in the menu");
		leave_menu_mode(1);
	}
}

/* Passes "gf" shortcut to menu as otherwise the shortcut is not available. */
static void
cmd_gf(key_info_t key_info, keys_info_t *keys_info)
{
	(void)pass_combination_to_khandler(L"gf");
}

/* Gives menu-specific keyboard routine to process the shortcut.  Returns zero
 * if the shortcut wasn't processed, otherwise non-zero is returned. */
static int
pass_combination_to_khandler(const wchar_t keys[])
{
	KHandlerResponse handler_response;

	if(menu->key_handler == NULL)
	{
		return 0;
	}

	handler_response = menu->key_handler(menu, keys);

	switch(handler_response)
	{
		case KHR_REFRESH_WINDOW:
			wrefresh(menu_win);
			return 1;
		case KHR_CLOSE_MENU:
			leave_menu_mode(1);
			return 1;
		case KHR_MORPHED_MENU:
			assert(!vle_mode_is(MENU_MODE) && "Wrong use of KHR_MORPHED_MENU.");
			return 1;
		case KHR_UNHANDLED:
			return 0;

		default:
			assert(0 && "Unknown menu-specific keyboard handler response.");
			return 0;
	}
}

static void
cmd_gg(key_info_t key_info, keys_info_t *keys_info)
{
	if(key_info.count == NO_COUNT_GIVEN)
		key_info.count = 1;

	clean_menu_position(menu);
	move_to_menu_pos(key_info.count - 1, menu);
	wrefresh(menu_win);
}

static void
cmd_j(key_info_t key_info, keys_info_t *keys_info)
{
	if(menu->pos == menu->len - 1)
		return;
	if(key_info.count == NO_COUNT_GIVEN)
		key_info.count = 1;

	clean_menu_position(menu);
	menu->pos += key_info.count;
	move_to_menu_pos(menu->pos, menu);
	wrefresh(menu_win);
}

static void
cmd_k(key_info_t key_info, keys_info_t *keys_info)
{
	if(menu->pos == 0)
		return;
	if(key_info.count == NO_COUNT_GIVEN)
		key_info.count = 1;

	clean_menu_position(menu);
	menu->pos -= key_info.count;
	move_to_menu_pos(menu->pos, menu);
	wrefresh(menu_win);
}

static void
cmd_n(key_info_t key_info, keys_info_t *keys_info)
{
	key_info.count = def_count(key_info.count);
	while(key_info.count-- > 0)
	{
		menus_search(menu, last_search_backward);
	}
}

/* Handles current content of the menu to Vim as quickfix list. */
static void
cmd_v(key_info_t key_info, keys_info_t *keys_info)
{
	int bg;
	const char *vi_cmd;
	FILE *vim_stdin;
	char *cmd;
	int i;
	int qf = 1;

	/* If both first and last lines do not contain colons, treat lines as list of
	 * file names. */
	if(strchr(menu->items[0], ':') == NULL &&
			strchr(menu->items[menu->len - 1], ':') == NULL)
	{
		qf = 0;
	}

	endwin();
	curr_stats.need_update = UT_FULL;

	vi_cmd = cfg_get_vicmd(&bg);
	if(!qf)
	{
		char *const arg = shell_like_escape("+exe 'bd!|args' "
				"join(map(getline('1','$'),'fnameescape(v:val)'))", 0);
		cmd = format_str("%s %s +argument%d -", vi_cmd, arg, menu->pos + 1);
		free(arg);
	}
	else if(menu->pos == 0)
	{
		/* For some reason +cc1 causes noisy messages on status line, so handle this
		 * case separately. */
		cmd = format_str("%s +cgetbuffer +bd! +cfirst -", vi_cmd);
	}
	else
	{
		cmd = format_str("%s +cgetbuffer +bd! +cfirst +cc%d -", vi_cmd,
				menu->pos + 1);
	}

	vim_stdin = popen(cmd, "w");
	free(cmd);

	if(vim_stdin == NULL)
	{
		recover_after_shellout();
		show_error_msg("Vim QuickFix", "Failed to send list of files to editor.");
		return;
	}

	for(i = 0; i < menu->len; ++i)
	{
		fprintf(vim_stdin, "%s\n", menu->items[i]);
	}

	pclose(vim_stdin);
	recover_after_shellout();
}

static void
cmd_zb(key_info_t key_info, keys_info_t *keys_info)
{
	if(can_scroll_menu_up(menu))
	{
		if(menu->pos < menu->win_rows)
			menu->top = 0;
		else
			menu->top = menu->pos - (menu->win_rows - 3);
		update_menu();
	}
}

static void
cmd_zH(key_info_t key_info, keys_info_t *keys_info)
{
	if(menu->hor_pos == 0)
		return;
	menu->hor_pos = MAX(0, menu->hor_pos - (getmaxx(menu_win) - 4));
	update_menu();
}

static void
cmd_zL(key_info_t key_info, keys_info_t *keys_info)
{
	menu->hor_pos += getmaxx(menu_win) - 4;
	update_menu();
}

static void
cmd_zh(key_info_t key_info, keys_info_t *keys_info)
{
	if(menu->hor_pos == 0)
		return;
	if(key_info.count == NO_COUNT_GIVEN)
		key_info.count = 1;
	menu->hor_pos = MAX(0, menu->hor_pos - key_info.count);
	update_menu();
}

static void
cmd_zl(key_info_t key_info, keys_info_t *keys_info)
{
	if(key_info.count == NO_COUNT_GIVEN)
		key_info.count = 1;
	menu->hor_pos += key_info.count;
	update_menu();
}

static void
cmd_zt(key_info_t key_info, keys_info_t *keys_info)
{
	if(can_scroll_menu_down(menu))
	{
		if(menu->len - menu->pos >= menu->win_rows - 3 + 1)
			menu->top = menu->pos;
		else
			menu->top = menu->len - (menu->win_rows - 3 + 1);
		update_menu();
	}
}

static void
cmd_zz(key_info_t key_info, keys_info_t *keys_info)
{
	if(!all_lines_visible(menu))
	{
		if(menu->pos <= (menu->win_rows - 3)/2)
			menu->top = 0;
		else if(menu->pos > menu->len - DIV_ROUND_UP(menu->win_rows - 3, 2))
			menu->top = menu->len - (menu->win_rows - 3 + 1);
		else
			menu->top = menu->pos - DIV_ROUND_UP(menu->win_rows - 3, 2);

		update_menu();
	}
}

/* Returns non-zero if all menu lines are visible, so no scrolling is needed. */
static int
all_lines_visible(const menu_info *const menu)
{
	return menu->len <= menu->win_rows - 2;
}

void
update_menu(void)
{
	draw_menu(menu);
	move_to_menu_pos(menu->pos, menu);
	wrefresh(menu_win);
}

static int
goto_cmd(const cmd_info_t *cmd_info)
{
	if(cmd_info->end == NOT_DEF)
		return 0;
	clean_menu_position(menu);
	move_to_menu_pos(cmd_info->end, menu);
	wrefresh(menu_win);
	return 0;
}

static int
quit_cmd(const cmd_info_t *cmd_info)
{
	leave_menu_mode(1);
	return 0;
}

void
save_menu_pos(void)
{
	saved_top = menu->top;
	saved_pos = menu->pos;
}

void
load_menu_pos(void)
{
	menu->top = saved_top;
	menu->pos = saved_pos;
}

void
menu_morph_into_cmdline(CmdLineSubmode submode, const char input[],
		int external)
{
	/* input might point to part of menu data. */
	char *input_copy;

	if(input[0] == '\0')
	{
		show_error_msg("Command insertion", "Ignoring empty command");
		return;
	}

	input_copy = external ? format_str("!%s", input) : strdup(input);
	if(input_copy == NULL)
	{
		show_error_msg("Error", "Not enough memory");
		return;
	}

	leave_menu_mode(0);
	enter_cmdline_mode(submode, input_copy, NULL);

	free(input_copy);
}

/* Leaves menu mode, possibly resetting selection.  Does nothing if current mode
 * isn't menu mode. */
static void
leave_menu_mode(int reset_selection)
{
	/* Some menu implementation could have switched mode from one of handlers. */
	if(!vle_mode_is(MENU_MODE))
	{
		return;
	}

	reset_popup_menu(menu);

	if(reset_selection)
	{
		clean_selected_files(view);
		redraw_view(view);
	}

	vle_mode_set(NORMAL_MODE, VMT_PRIMARY);

	update_ui_on_leaving();
}

void
execute_cmdline_command(const char cmd[])
{
	if(exec_command(cmd, curr_view, CIT_COMMAND) < 0)
	{
		status_bar_error("An error occurred while trying to execute command");
	}
	init_cmds(0, &cmds_conf);
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
