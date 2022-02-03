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

//#define LOG_ENABLED 1
//#define LOG_ENABLE_FLOW 1
#define LOG_GROUP LOG_GROUP_DEV_SB16

#include <iprt/assert.h>
#include <iprt/poll.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <alsa/asoundlib.h>
#include <sys/eventfd.h>
#include "midialsa.h"

#define MAX_POLL_FDS 4



MIDIAlsa::MIDIAlsa() : _out(NULL), _eventfd(-1)
{

}

MIDIAlsa::~MIDIAlsa()
{
}

int MIDIAlsa::open(const char *dev)
{
    int err;

    _eventfd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_eventfd == -1) {
        LogWarn(("eventfd error: %s\n", strerror(errno)));
        return RTErrConvertFromErrno(errno);
    }

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
    if (_eventfd) {
        ::close(_eventfd);
    }
    return VINF_SUCCESS;
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

int MIDIAlsa::poll(uint32_t events, uint32_t *revents, RTMSINTERVAL millies)
{
    struct pollfd pfds[MAX_POLL_FDS];
    int i_pipe = -1, i_in = -1, i_out = -1;
    int n_in = 0, n_out = 0;
    int nfds = 0;

    LogFlowFuncEnter();

    *revents = 0;

    if (millies > 0 && _eventfd != -1) {
        LogFlowFunc(("including eventfd\n"));
        i_pipe = nfds;
        pfds[nfds].fd = _eventfd;
        pfds[nfds].events = POLLIN;
        nfds++;
    }

    if (_in && events & RTPOLL_EVT_READ) {
        LogFlowFunc(("including in\n"));
        i_in = nfds;
        n_in = snd_rawmidi_poll_descriptors(_in, &pfds[i_in], MAX_POLL_FDS - nfds);
        AssertLogRelReturn(n_in > 0, VERR_IO_NOT_READY);
        nfds+=n_in;
    }
    if (_out && events & RTPOLL_EVT_WRITE) {
        LogFlowFunc(("including out\n"));
        i_out = nfds;
        n_out = snd_rawmidi_poll_descriptors(_out, &pfds[i_out], MAX_POLL_FDS - nfds);
        AssertLogRelReturn(n_out > 0, VERR_IO_NOT_READY);
        nfds+=n_out;
    }

    AssertReturn(millies != RT_INDEFINITE_WAIT || nfds > 0, VERR_DEADLOCK);

    int rc;
    int ready = ::poll(pfds, nfds, millies = RT_INDEFINITE_WAIT ? -1 : millies);
    LogFlowFunc(("poll ready=%d\n", ready));
    if (ready < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            LogWarnFunc(("Cannot poll, errno=%d\n", errno));
            rc = VERR_IO_NOT_READY;
        } else {
            rc = VINF_TRY_AGAIN;
        }
    } else if (ready == 0) {
        // Timeout
        rc = VINF_TIMEOUT;
    } else /* ready > 0 */ {
        rc = VINF_SUCCESS;
        if (i_pipe != -1) {
            if (pfds[i_pipe].revents) {
                uint64_t val;
                ssize_t r = ::read(_eventfd, &val, sizeof(val));
                Assert(r == sizeof(val));
                NOREF(r);
                rc = VINF_INTERRUPTED;
            }
        }
        if (i_in != -1) {
            Assert(n_in > 0);
            unsigned short rev;

            int err = snd_rawmidi_poll_descriptors_revents(_in, &pfds[i_in], n_in, &rev);
            AssertLogRelReturn(err >= 0, VERR_IO_GEN_FAILURE);

            if (rev & POLLIN)             *revents |= RTPOLL_EVT_READ;
            if (rev & (POLLNVAL|POLLERR)) *revents |= RTPOLL_EVT_ERROR;
        }
        if (i_out != -1) {
            Assert(n_out > 0);
            unsigned short rev;
            int err = snd_rawmidi_poll_descriptors_revents(_out, &pfds[i_out], n_out, &rev);
            AssertLogRelReturn(err >= 0, VERR_IO_GEN_FAILURE);

            if (rev & POLLOUT)            *revents |= RTPOLL_EVT_WRITE;
            if (rev & (POLLNVAL|POLLERR)) *revents |= RTPOLL_EVT_ERROR;
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int MIDIAlsa::pollInterrupt()
{
    if (_eventfd) {
        uint64_t val = 1;
        ssize_t r = ::write(_eventfd, &val, sizeof(val));
        Assert(r == sizeof(uint64_t));
        NOREF(r);
        return VINF_SUCCESS;
    } else {
        return VERR_INVALID_STATE;
    }
}

ssize_t MIDIAlsa::write(uint8_t *data, size_t len)
{
    ssize_t result = snd_rawmidi_write(_out, data, len);
    if (result < 0) {
        LogWarn(("ALSA midi write error: %s\n", snd_strerror(result)));
        return VERR_BROKEN_PIPE;
    }
    return result;
}

ssize_t MIDIAlsa::read(uint8_t *buf, size_t len)
{
    ssize_t result = snd_rawmidi_read(_in, buf, len);
    if (result < 0) {
        LogWarn(("ALSA midi read error: %s\n", snd_strerror(result)));
        return VERR_BROKEN_PIPE;
    }
    return result;
}
