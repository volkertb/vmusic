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

#include <iprt/types.h>

typedef struct _snd_rawmidi snd_rawmidi_t;

class MIDIAlsa
{
public:
    MIDIAlsa();
    ~MIDIAlsa();

    /** dev has no effect right now. */
    int open(const char *dev);
	int close();

    /** reset device, dropping all buffers, but keep open */
    int reset();

    int poll(uint32_t events, uint32_t *revents, RTMSINTERVAL millies);
    int pollInterrupt();

    ssize_t write(uint8_t *data, size_t len);

    ssize_t read(uint8_t *buf, size_t len);

private:
    snd_rawmidi_t *_in;
    snd_rawmidi_t *_out;
    int _eventfd;
};

#endif
