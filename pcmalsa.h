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

#ifndef VMUSIC_PCMALSA_H
#define VMUSIC_PCMALSA_H

#include <stddef.h>
#include <stdint.h>

typedef struct _snd_pcm snd_pcm_t;

class PCMOutAlsa
{
public:
    PCMOutAlsa();
    ~PCMOutAlsa();

    int open(const char *dev, unsigned int sampleRate, unsigned int channels);
    int close();

    ssize_t avail();
    int wait();

    ssize_t write(int16_t *buf, size_t n);

private:
    int setParams(unsigned int sampleRate, unsigned int channels, unsigned int bufferTime, unsigned int periodTime);

private:
    snd_pcm_t * _pcm;
    size_t _bufferSize;
    size_t _periodSize;
};

#endif
