/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GAME_PID_H
#define _LINUX_GAME_PID_H

enum {
	SYMBOL_GAME_PID,
	NR_SYMBOLS,
};

struct symbol_entry {
	const char *name;
	unsigned long addr;
	bool found;
};

unsigned long lookup_symbol(int symbol_index);
bool check_game_pid(void);

#endif /* _LINUX_GAME_PID_H */
