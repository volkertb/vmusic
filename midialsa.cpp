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

#define MAX_POLL_FDS 4

static ssize_t rawmidi_avail(snd_rawmidi_t *rmidi)
{
    struct pollfd pfds[MAX_POLL_FDS];

    int nfds = snd_rawmidi_poll_descriptors(rmidi, pfds, MAX_POLL_FDS);
    if (nfds <= 0) {
        LogWarn(("ALSA rawmidi avail: no descriptors to poll!\n"));
        return VERR_AUDIO_ENUMERATION_FAILED;
    }

    int ready = poll(pfds, nfds, 0);
    if (ready < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            LogWarnFunc(("Cannot poll, errno=%d\n", errno));
            return VERR_AUDIO_STREAM_NOT_READY;
        }
        return 0;
    } else if (ready == 0) {
        return 0;
    } else /* ready > 0 */ {
        unsigned short revents;
        int err = snd_rawmidi_poll_descriptors_revents(rmidi, pfds, nfds, &revents);
        if (err != 0) {
            LogWarnFunc(("Cannot call revents, err=%d\n", err));
            return VERR_AUDIO_STREAM_NOT_READY;
        }

        if (revents & POLLNVAL) {
            LogWarnFunc(("POLLNVAL\n"));
        }
        if (revents & POLLERR) {
            LogWarnFunc(("POLLERR\n"));
        }

        return revents & (POLLIN | POLLOUT);
    }
}

MIDIAlsa::MIDIAlsa() : _out(NULL)
{

}

MIDIAlsa::~MIDIAlsa()
{
}

int MIDIAlsa::open(const char *dev)
{
    int err;

    if ((err = snd_rawmidi_open(&_in, &_out, "virtual", SND_RAWMIDI_NONBLOCK))) {
        LogWarn(("ALSA rawmidi open error: %s\n", snd_strerror(err)));
        return VERR_AUDIO_STREAM_COULD_NOT_CREATE;
    }

    // TODO: Connect somewhere
    NOREF(dev);

    return VINF_SUCCESS;
}

int MIDIAlsa::close()
{
    if (_in) {
        snd_rawmidi_close(_in);
        _in = NULL;
    }
    if (_out) {
        snd_rawmidi_close(_out);
        _out = NULL;
    }
    return VINF_SUCCESS;
}

ssize_t MIDIAlsa::writeAvail()
{
    return _out ? rawmidi_avail(_out) : 0;
}


ssize_t MIDIAlsa::write(uint8_t *data, size_t len)
{
    ssize_t result = snd_rawmidi_write(_out, data, len);
    if (result < 0) {
        LogWarn(("ALSA midi write error: %s\n", snd_strerror(result)));
        return VERR_AUDIO_STREAM_NOT_READY;
    }
    return result;
}

ssize_t MIDIAlsa::readAvail()
{
    return _in ? rawmidi_avail(_in) : 0;
}

ssize_t MIDIAlsa::read(uint8_t *buf, size_t len)
{
    ssize_t result = snd_rawmidi_read(_in, buf, len);
    if (result < 0) {
        LogWarn(("ALSA midi read error: %s\n", snd_strerror(result)));
        return VERR_AUDIO_STREAM_NOT_READY;
    }
    return result;
}

int MIDIAlsa::reset()
{
    if (_in) {
        snd_rawmidi_drop(_in);
    }
    if (_out) {
        snd_rawmidi_drop(_out);
    }
    return VINF_SUCCESS;
}
