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

#define LOG_ENABLED 1
#define LOG_ENABLE_FLOW 1
#define LOG_GROUP LOG_GROUP_DEV_SB16

#include <VBox/err.h>
#include <VBox/log.h>
#include <alsa/asoundlib.h>
#include "midialsa.h"

MIDIOutAlsa::MIDIOutAlsa() : _out(NULL)
{

}

MIDIOutAlsa::~MIDIOutAlsa()
{
}

int MIDIOutAlsa::open(const char *dev)
{
    int err;

    if ((err = snd_rawmidi_open(NULL, &_out, "virtual", SND_RAWMIDI_NONBLOCK))) {
        LogWarn(("ALSA rawmidi open error: %s\n", snd_strerror(err)));
        return VERR_AUDIO_STREAM_COULD_NOT_CREATE;
    }

    // TODO: Connect somewhere
    NOREF(dev);

    return VINF_SUCCESS;
}

int MIDIOutAlsa::close()
{
    if (_out) {
        snd_rawmidi_close(_out);
        _out = NULL;
    }
    return VINF_SUCCESS;
}

ssize_t MIDIOutAlsa::write(uint8_t *data, size_t len)
{
    return snd_rawmidi_write(_out, data, len);
}
