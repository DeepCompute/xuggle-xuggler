/*
 * Sample rate convertion for both audio and video
 * Copyright (c) 2000 Gerard Lantau.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <math.h>
#include "avcodec.h"

#define NDEBUG
#include <assert.h>

#define FRAC_BITS 16
#define FRAC (1 << FRAC_BITS)

static void init_mono_resample(ReSampleChannelContext *s, float ratio)
{
    ratio = 1.0 / ratio;
    s->iratio = (int)floor(ratio);
    if (s->iratio == 0)
        s->iratio = 1;
    s->incr = (int)((ratio / s->iratio) * FRAC);
    s->frac = 0;
    s->last_sample = 0;
    s->icount = s->iratio;
    s->isum = 0;
    s->inv = (FRAC / s->iratio);
}

/* fractional audio resampling */
static int fractional_resample(ReSampleChannelContext *s, short *output, short *input, int nb_samples)
{
    unsigned int frac, incr;
    int l0, l1;
    short *q, *p, *pend;

    l0 = s->last_sample;
    incr = s->incr;
    frac = s->frac;

    p = input;
    pend = input + nb_samples;
    q = output;

    l1 = *p++;
    for(;;) {
        /* interpolate */
        *q++ = (l0 * (FRAC - frac) + l1 * frac) >> FRAC_BITS;
        frac = frac + s->incr;
        while (frac >= FRAC) {
            if (p >= pend)
                goto the_end;
            frac -= FRAC;
            l0 = l1;
            l1 = *p++;
        }
    }
 the_end:
    s->last_sample = l1;
    s->frac = frac;
    return q - output;
}

static int integer_downsample(ReSampleChannelContext *s, short *output, short *input, int nb_samples)
{
    short *q, *p, *pend;
    int c, sum;

    p = input;
    pend = input + nb_samples;
    q = output;

    c = s->icount;
    sum = s->isum;

    for(;;) {
        sum += *p++;
        if (--c == 0) {
            *q++ = (sum * s->inv) >> FRAC_BITS;
            c = s->iratio;
            sum = 0;
        }
        if (p >= pend)
            break;
    }
    s->isum = sum;
    s->icount = c;
    return q - output;
}

/* n1: number of samples */
static void stereo_to_mono(short *output, short *input, int n1)
{
    short *p, *q;
    int n = n1;

    p = input;
    q = output;
    while (n >= 4) {
        q[0] = (p[0] + p[1]) >> 1;
        q[1] = (p[2] + p[3]) >> 1;
        q[2] = (p[4] + p[5]) >> 1;
        q[3] = (p[6] + p[7]) >> 1;
        q += 4;
        p += 8;
        n -= 4;
    }
    while (n > 0) {
        q[0] = (p[0] + p[1]) >> 1;
        q++;
        p += 2;
        n--;
    }
}

/* XXX: should use more abstract 'N' channels system */
static void stereo_split(short *output1, short *output2, short *input, int n)
{
    int i;

    for(i=0;i<n;i++) {
        *output1++ = *input++;
        *output2++ = *input++;
    }
}

static void stereo_mux(short *output, short *input1, short *input2, int n)
{
    int i;

    for(i=0;i<n;i++) {
        *output++ = *input1++;
        *output++ = *input2++;
    }
}

static int mono_resample(ReSampleChannelContext *s, short *output, short *input, int nb_samples)
{
    short buf1[nb_samples];
    short *buftmp;

    /* first downsample by an integer factor with averaging filter */
    if (s->iratio > 1) {
        buftmp = buf1;
        nb_samples = integer_downsample(s, buftmp, input, nb_samples);
    } else {
        buftmp = input;
    }

    /* then do a fractional resampling with linear interpolation */
    if (s->incr != FRAC) {
        nb_samples = fractional_resample(s, output, buftmp, nb_samples);
    } else {
        memcpy(output, buftmp, nb_samples * sizeof(short));
    }
    return nb_samples;
}

/* ratio = output_rate / input_rate */
int audio_resample_init(ReSampleContext *s, 
                        int output_channels, int input_channels, 
                        int output_rate, int input_rate)
{
    int i;
    
    s->ratio = (float)output_rate / (float)input_rate;
    
    if (output_channels > 2 || input_channels > 2)
        return -1;
    s->input_channels = input_channels;
    s->output_channels = output_channels;

    for(i=0;i<output_channels;i++) {
        init_mono_resample(&s->channel_ctx[i], s->ratio);
    }
    return 0;
}

/* resample audio. 'nb_samples' is the number of input samples */
/* XXX: optimize it ! */
/* XXX: do it with polyphase filters, since the quality here is
   HORRIBLE. Return the number of samples available in output */
int audio_resample(ReSampleContext *s, short *output, short *input, int nb_samples)
{
    int i, nb_samples1;
    short buf[5][nb_samples];
    short *buftmp1, *buftmp2[2], *buftmp3[2];

    if (s->input_channels == s->output_channels && s->ratio == 1.0) {
        /* nothing to do */
        memcpy(output, input, nb_samples * s->input_channels * sizeof(short));
        return nb_samples;
    }

    if (s->input_channels == 2 &&
        s->output_channels == 1) {
        buftmp1 = buf[0];
        stereo_to_mono(buftmp1, input, nb_samples);
    } else if (s->input_channels == 1 &&
               s->output_channels == 2) {
        /* XXX: do it */
        abort();
    } else {
        buftmp1 = input;
    }

    if (s->output_channels == 2) {
        buftmp2[0] = buf[1];
        buftmp2[1] = buf[2];
        buftmp3[0] = buf[3];
        buftmp3[1] = buf[4];
        stereo_split(buftmp2[0], buftmp2[1], buftmp1, nb_samples);
    } else {
        buftmp2[0] = buftmp1;
        buftmp3[0] = output;
    }

    /* resample each channel */
    nb_samples1 = 0; /* avoid warning */
    for(i=0;i<s->output_channels;i++) {
        nb_samples1 = mono_resample(&s->channel_ctx[i], buftmp3[i], buftmp2[i], nb_samples);
    }

    if (s->output_channels == 2) {
        stereo_mux(output, buftmp3[0], buftmp3[1], nb_samples1);
    }

    return nb_samples1;
}
