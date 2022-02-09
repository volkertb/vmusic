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

#define LOG_GROUP LOG_GROUP_DEV_SB16

#include <VBox/err.h>
#include <VBox/log.h>
#include <alsa/asoundlib.h>
#include "pcmalsa.h"

PCMOutAlsa::PCMOutAlsa() : _pcm(NULL)
{

}

PCMOutAlsa::~PCMOutAlsa()
{
}

int PCMOutAlsa::open(const char *dev, unsigned int sampleRate, unsigned int channels)
{
    int err;

    err = snd_pcm_open(&_pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        LogWarn(("ALSA playback open error: %s\n", snd_strerror(err)));
        return VERR_AUDIO_STREAM_COULD_NOT_CREATE;
    }

    // TODO: Right now, setting a too large period size means we will not let the main
    // thread run long enough to actually change notes/voices. Need some actual synchronization.

    int periodSize = 10  /*msec*/;
    int bufferSize = 100 /*msec*/;

    err = setParams(sampleRate, channels, bufferSize * 1000 /*usec*/, periodSize * 1000);
    if (err < 0) {
        snd_pcm_close(_pcm);
        _pcm = NULL;
        return VERR_AUDIO_STREAM_COULD_NOT_CREATE;
    }

    return VINF_SUCCESS;
}

int PCMOutAlsa::close()
{
    if (_pcm) {
        snd_pcm_drain(_pcm);
        snd_pcm_close(_pcm);
    }
    return VINF_SUCCESS;
}

ssize_t PCMOutAlsa::avail()
{
    snd_pcm_sframes_t frames = snd_pcm_avail(_pcm);
    if (frames < 0) {
        LogWarn(("ALSA trying to recover from avail error: %s\n", snd_strerror(frames)));
        frames = snd_pcm_recover(_pcm, frames, 0);
        if (frames == 0) {
            frames = snd_pcm_avail(_pcm);
        }
    }
    if (frames < 0) {
        LogWarn(("ALSA avail error: %s\n", snd_strerror(frames)));
        return VERR_AUDIO_STREAM_NOT_READY;
    }

    return frames;
}

int PCMOutAlsa::wait()
{
    int err = snd_pcm_wait(_pcm, -1);
    if (err < 0) {
        LogWarn(("ALSA trying to recover from wait error: %s\n", snd_strerror(err)));
        err = snd_pcm_recover(_pcm, err, 0);
    }
    if (err < 0) {
        LogWarn(("ALSA wait error: %s\n", snd_strerror(err)));
        return VERR_AUDIO_STREAM_NOT_READY;
    }
    return VINF_SUCCESS;
}


ssize_t PCMOutAlsa::write(int16_t *buf, size_t n)
{
    snd_pcm_sframes_t frames = snd_pcm_writei(_pcm, buf, n);
    if (frames < 0) {
        LogFlow(("ALSA trying to recover from error: %s\n", snd_strerror(frames)));
        frames = snd_pcm_recover(_pcm, frames, 0);
    }
    if (frames < 0) {
        LogWarn(("ALSA write error: %s\n", snd_strerror(frames)));
        return VERR_AUDIO_STREAM_NOT_READY;
    }
    return frames;
}

int PCMOutAlsa::setParams(unsigned int sampleRate, unsigned int channels, unsigned int bufferTime, unsigned int periodTime)
{
    snd_pcm_hw_params_t *hwparams;
    snd_pcm_sw_params_t *swparams;
    int err;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);

    /* choose all parameters */
    err = snd_pcm_hw_params_any(_pcm, hwparams);
    if (err < 0) {
        LogWarnFunc(("Broken PCM configuration: no configurations available: %s\n", snd_strerror(err)));
        return err;
    }
    /* set software resampling */
    err = snd_pcm_hw_params_set_rate_resample(_pcm, hwparams, 1);
    if (err < 0) {
        LogWarnFunc(("Resampling setup failed: %s\n", snd_strerror(err)));
        return err;
    }
    /* set the selected read/write format */
    err = snd_pcm_hw_params_set_access(_pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LogWarnFunc(("Access type not available: %s\n", snd_strerror(err)));
        return err;
    }
    /* set the sample format */
    err = snd_pcm_hw_params_set_format(_pcm, hwparams, SND_PCM_FORMAT_S16);
    if (err < 0) {
        LogWarnFunc(("Sample format not available: %s\n", snd_strerror(err)));
        return err;
    }
    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(_pcm, hwparams, channels);
    if (err < 0) {
        LogWarnFunc(("Channels count (%i) not available: %s\n", channels, snd_strerror(err)));
        return err;
    }
    /* set the stream rate */
    unsigned int rrate = sampleRate;
    err = snd_pcm_hw_params_set_rate_near(_pcm, hwparams, &rrate, 0);
    if (err < 0) {
        LogWarnFunc(("Rate %iHz not available for playback: %s\n", sampleRate, snd_strerror(err)));
        return err;
    }
    if (rrate != sampleRate) {
        LogWarnFunc(("Rate doesn't match (requested %iHz, get %iHz)\n", sampleRate, rrate));
        return -EINVAL;
    }

    /* set the buffer time */
    err = snd_pcm_hw_params_set_buffer_time_near(_pcm, hwparams, &bufferTime, NULL);
    if (err < 0) {
        LogWarnFunc(("Unable to set buffer time %u for playback: %s\n", bufferTime, snd_strerror(err)));
        return err;
    }
    err = snd_pcm_hw_params_get_buffer_size(hwparams, &_bufferSize);
    if (err < 0) {
        LogWarnFunc(("Unable to get buffer size for playback: %s\n", snd_strerror(err)));
        return err;
    }

    /* set the period time */
    err = snd_pcm_hw_params_set_period_time_near(_pcm, hwparams, &periodTime, NULL);
    if (err < 0) {
        LogWarnFunc(("Unable to set period time %u for playback: %s\n", periodTime, snd_strerror(err)));
        return err;
    }
    err = snd_pcm_hw_params_get_period_size(hwparams, &_periodSize, NULL);
    if (err < 0) {
        LogWarnFunc(("Unable to get period size for playback: %s\n", snd_strerror(err)));
        return err;
    }

    /* write the parameters to device */
    err = snd_pcm_hw_params(_pcm, hwparams);
    if (err < 0) {
        LogWarnFunc(("Unable to set hw params: %s\n", snd_strerror(err)));
        return err;
    }

    Log2Func(("using bufferSize=%lu periodSize=%lu\n", _bufferSize, _periodSize));

    /* get the current swparams */
    err = snd_pcm_sw_params_current(_pcm, swparams);
    if (err < 0) {
        LogWarnFunc(("Unable to determine current swparams: %s\n", snd_strerror(err)));
        return err;
    }
    /* start the transfer when the buffer is almost full: */
    /* (buffer_size / avail_min) * avail_min */
    err = snd_pcm_sw_params_set_start_threshold(_pcm, swparams, (_bufferSize / _periodSize) * _periodSize);
    if (err < 0) {
        LogWarnFunc(("Unable to set start threshold mode: %s\n", snd_strerror(err)));
        return err;
    }
    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(_pcm, swparams, _periodSize);
    if (err < 0) {
        LogWarnFunc(("Unable to set avail min: %s\n", snd_strerror(err)));
        return err;
    }
    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(_pcm, swparams);
    if (err < 0) {
        LogWarnFunc(("Unable to set sw params: %s\n", snd_strerror(err)));
        return err;
    }

    return 0;
}
