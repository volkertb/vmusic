/*
 * PCem - IBM PC emulator
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 * Portions:
 * VMusic - a VirtualBox extension pack with various music devices
 * Copyright (C) 2022 Javier S. Pedro
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef EMU8K_H
#define EMU8K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct emu8k_t emu8k_t;

emu8k_t* emu8k_alloc(void *rom, size_t onboard_ram);
void emu8k_free(emu8k_t *emu8k);

void emu8k_reset(emu8k_t *emu8k);

uint16_t emu8k_inw(emu8k_t *emu8k, uint16_t addr);
void emu8k_outw(emu8k_t *emu8k, uint16_t addr, uint16_t val);

uint8_t emu8k_inb(emu8k_t *emu8k, uint16_t addr);
void emu8k_outb(emu8k_t *emu8k, uint16_t addr, uint8_t val);

void emu8k_render(emu8k_t *emu8k, int16_t *buf, size_t frames);

/** Between calls to emu8k_render, the virtual sample count is used to keep the "sample count" register ticking at a reasonable pace. */
void emu8k_update_virtual_sample_count(emu8k_t *emu8k, uint8_t sample_count);
/*  Many programs seem to rely in this counter incrementing frequently, and may hang/error out if it doesn't.
 *  It is reset to 0 whenever we render and therefore increment the real sample count.
 *  This means that effectively the sample count register may readjust itself (go back or jump ahead) on _render :(. */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // EMU8K_H
