/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Danny Lin <danny@kdrag0n.dev>
 */

#ifndef _LINUX_TOUCHPAINT_H
#define _LINUX_TOUCHPAINT_H

void touchpaint_finger_down(int slot);
void touchpaint_finger_up(int slot);
void touchpaint_finger_point(int slot, int x, int y);

#endif
