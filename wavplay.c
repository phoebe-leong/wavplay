/*-
 * wavplay - a C library to play WAV sound via OSS/ALSA
 *
 * Copyright (c) 2011 Zhihao Yuan.
 * All rights reserved.
 *
 * This file is distributed under the 2-clause BSD License.
 */

#include "wavplay.h"
#include <string.h>
#define BUF_SIZE	4096
#define PERIODS	4
#define eputs(s) (fprintf(stderr, "%s: " s "\n", __func__))

#ifndef USE_ALSA

#define WAV_FMT_8	AFMT_U8
#define WAV_FMT_16	AFMT_S16_LE
#ifdef	AFMT_S24_PACKED
#define WAV_FMT_24	AFMT_S24_PACKED
#elif	__FreeBSD__
#define WAV_FMT_24	AFMT_S24_LE
#endif
#ifdef	AFMT_S32_LE
#define WAV_FMT_32	AFMT_S32_LE
#endif
#define WAV_FMT_A_LAW	AFMT_A_LAW
#define WAV_FMT_MU_LAW	AFMT_MU_LAW
#define WAV_FMT_IMA_ADPCM	AFMT_IMA_ADPCM

static int devfd = -1;

int snd_init(void) {
	if (devfd > -1) snd_end();
	devfd = open(DEV_NAME, O_WRONLY);
	return devfd;
}

int snd_set(int format, int nchannels, int framerate) {
	int st = 0;
	st |= ioctl(devfd, SNDCTL_DSP_RESET, NULL);
	st |= ioctl(devfd, SNDCTL_DSP_SETFMT, &format);
	st |= ioctl(devfd, SNDCTL_DSP_CHANNELS, &nchannels);
	st |= ioctl(devfd, SNDCTL_DSP_SPEED, &framerate);
	return st;
}

int snd_end(void) {
	int st = close(devfd);
	devfd = -1;
	return st;
}

int snd_send(FILE *fp, size_t n) {
	unsigned char buf[BUF_SIZE];
	size_t l;
	while (n > sizeof(buf)) {
		if ((l = fread(buf, 1, sizeof(buf), fp)))
			write(devfd, buf, l);
		else goto EOS;
		n -= l;
	}
	if ((l = fread(buf, 1, n, fp)))
		write(devfd, buf, l);
	if (l < n)
		EOS: eputs("Unexpected end of stream");
	return ioctl(devfd, SNDCTL_DSP_SYNC, NULL);
}

#else

#define WAV_FMT_8	SND_PCM_FORMAT_U8
#define WAV_FMT_16	SND_PCM_FORMAT_S16_LE
#define WAV_FMT_24	SND_PCM_FORMAT_S24_3LE
#define WAV_FMT_32	SND_PCM_FORMAT_S32_LE
#define WAV_FMT_A_LAW	SND_PCM_FORMAT_A_LAW
#define WAV_FMT_MU_LAW	SND_PCM_FORMAT_MU_LAW
#define WAV_FMT_IMA_ADPCM	SND_PCM_FORMAT_IMA_ADPCM

static snd_pcm_t *pcm = NULL;

int snd_init(void) {
	if (pcm) snd_end();
	return snd_pcm_open(&pcm, DEV_NAME, SND_PCM_STREAM_PLAYBACK, 0);
}

int snd_set(int format, int nchannels, int framerate) {
	int st = 0;
	unsigned int val = framerate;
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_any(pcm, params);
	st |= snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
	st |= snd_pcm_hw_params_set_format(pcm, params, format);
	st |= snd_pcm_hw_params_set_channels(pcm, params, nchannels);
	st |= snd_pcm_hw_params_set_rate_near(pcm, params, &val, 0);
	st |= snd_pcm_hw_params_set_periods(pcm, params, PERIODS, 0);
	st |= snd_pcm_hw_params(pcm, params);
	return st;
}

int snd_end(void) {
	int st = snd_pcm_close(pcm);
	pcm = NULL;
	return st;
}

int snd_send(FILE *fp, size_t n) {
	snd_pcm_format_t format;
	unsigned int nchannels;
	snd_pcm_uframes_t period;
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	snd_pcm_hw_params_current(pcm, params);
	snd_pcm_hw_params_get_format(params, &format);
	snd_pcm_hw_params_get_channels(params, &nchannels);
	snd_pcm_hw_params_get_period_size(params, &period, 0);
	int framesize = snd_pcm_format_width(format) / 8 * nchannels;
	unsigned char buf[period * framesize];
	size_t l;
	while (n > sizeof(buf)) {
		if ((l = fread(buf, 1, sizeof(buf), fp)))
			if (snd_pcm_writei(pcm, buf, l / framesize) == -EPIPE)
#ifndef NDEBUG
				snd_pcm_recover(pcm, -EPIPE, 0);
#else
				snd_pcm_prepare(pcm);
#endif
			else;
		else goto EOS;
		n -= l;
	}
	if ((l = fread(buf, 1, n, fp)))
		snd_pcm_writei(pcm, buf, l / framesize);
	if (l < n)
		EOS: eputs("Unexpected end of stream");
	return snd_pcm_drain(pcm);
}

#endif

int wav_getformat(const wavheader_t *wav) {
	switch (wav->comptype) {
	case 1:
	case -2:
		switch ((wav->bitdepth + 7) / 8) {
		case 1: return WAV_FMT_8;
		case 2: return WAV_FMT_16;
#ifdef	WAV_FMT_24
		case 3: return WAV_FMT_24;
#endif
#ifdef	WAV_FMT_32
		case 4: return WAV_FMT_32;
#endif
		default: return -1;
		}
	case 258:
	case 6: return WAV_FMT_A_LAW;
	case 257:
	case 7: return WAV_FMT_MU_LAW;
	case 17: return wav->bitdepth == 4 ? WAV_FMT_IMA_ADPCM : -1;
	default: return -1;
	}
}

#define skip(n) do { \
	char c[n]; \
	fread(c, sizeof(c), 1, fp); \
} while (0)
#define read2(t) (fread(&t, sizeof(t), 1, fp))
#define chkid(s) (!strncmp(ck.id, s, 4))

size_t wav_read(wavheader_t *wav, FILE *fp) {
	riffchunk_t ck;
	if (!read2(ck) || !chkid("RIFF"))
		eputs("Not an RIFF file");
	else if (!read2(ck.id) || !chkid("WAVE"))
		eputs("Not a WAVE file");
	else {
		while (read2(ck))
			if (ck.size < 0) {
				eputs("RIFF chunk size > 2GB");
				return 0;
			}
			else if (chkid("fmt ")) {
				fread(wav, sizeof(wavheader_t), 1, fp);
				if (ck.size < sizeof(wavheader_t))
					eputs("Bad format chunk");
				else
					skip(ck.size - sizeof(wavheader_t));
			}
			else if (chkid("data"))
				return ck.size;
			else skip(ck.size);
		eputs("Malformed RIFF file");
	}
	return 0;
}

#undef skip
#undef read2
#undef chkid

int wav_setdev(const wavheader_t *wav) {
	int format = wav_getformat(wav);
	if (format < 0)
		eputs("Unsupported PCM format");
	else if (snd_set(format, wav->nchannels, wav->framerate))
		eputs("Failed to setup the sound device");
	else return 0;
	return -1;
}

int wav_send(FILE *fp) {
	wavheader_t wav[1];
	size_t size = wav_read(wav, fp);
	if (size && !wav_setdev(wav))
		return snd_send(fp, size);
	return -1;
}

int wav_play(const char *fn) {
	int st = -1;
	FILE *fp = fopen(fn, "rb");
	if (fp) {
		st = wav_send(fp);
		fclose(fp);
	} else perror(__func__);
	return st;
}

