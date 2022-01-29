/*
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

#ifndef VMUSIC_MIDIALSA_H
#define VMUSIC_MIDIALSA_H

#include <stddef.h>
#include <stdint.h>

typedef struct _snd_rawmidi snd_rawmidi_t;

class MIDIOutAlsa
{
public:
    MIDIOutAlsa();
    ~MIDIOutAlsa();

    int open(const char *dev);
	int close();

    ssize_t write(uint8_t *data, size_t len);

private:
    snd_rawmidi_t *_out;
};

#endif
