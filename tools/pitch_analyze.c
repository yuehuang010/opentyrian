/* -----------------------------------------------------------------------
 * pitch_analyze.c - hot-path f0 estimator for tools/validate_pitch.py TEST B.
 *
 * This is a line-for-line C99 port of the pure-python YIN estimator in
 * validate_pitch.py (fft/ifft/estimate_f0). validate_pitch.py remains the
 * driver: it renders the solo WAVs, selects analysis windows, and does the
 * RMS-relative gating / offset math. This binary only replaces the hot inner
 * loop (FFT-based autocorrelation + YIN search), which is what made a full
 * validator run take tens of minutes in pure python.
 *
 * Usage:
 *   pitch_analyze <wav_file>
 *
 * Reads window specs on stdin, one per line:
 *   start_sample num_samples expected_hz
 * (start_sample/num_samples index into the MONO-downmixed sample stream;
 * expected_hz is the frequency estimate_f0's tau search is centered on -
 * see validate_pitch.py's build_specs()/analyze_voice_c()).
 *
 * Writes one line per input window, in order, to stdout:
 *   f0_hz rms
 * f0_hz is 0.0 when no confident pitch estimate was found (matching the
 * python estimate_f0's (0.0, rms) / (0.0, 0.0) sentinel - the RMS-relative
 * gate that turns "low rms" into a skip is applied by the python driver,
 * not here, so it can gate across the whole batch consistently).
 *
 * WAV support: PCM 16-bit mono or stereo (or more channels; downmixed by
 * averaging), any sample rate - this is what fluidsynth's `-F file.wav`
 * output looks like. No external libraries; plain C99 + libm.
 * ----------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <complex.h>

/* ---------------------------------------------------------------------
 * Minimal WAV reader (RIFF/WAVE, PCM 16-bit).
 * ------------------------------------------------------------------- */

static uint32_t rd_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Reads the whole file, downmixes to mono (float, same scale as the int16
 * samples - matches validate_pitch.py's read_wav_mono, which just averages
 * the raw shorts without normalizing to [-1,1]). */
static double *load_wav_mono(const char *path, uint32_t *out_n, uint32_t *out_sr)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "pitch_analyze: cannot open %s\n", path);
		return NULL;
	}

	unsigned char hdr[12];
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
	{
		fprintf(stderr, "pitch_analyze: %s is not a RIFF/WAVE file\n", path);
		fclose(f);
		return NULL;
	}

	uint16_t num_channels = 0, bits_per_sample = 0, audio_format = 0;
	uint32_t sample_rate = 0;
	long data_offset = -1;
	uint32_t data_size = 0;

	for (;;)
	{
		unsigned char chdr[8];
		if (fread(chdr, 1, 8, f) != 8)
			break;
		uint32_t csize = rd_le32(chdr + 4);
		if (memcmp(chdr, "fmt ", 4) == 0)
		{
			unsigned char fmt[16];
			if (csize < 16 || fread(fmt, 1, 16, f) != 16)
			{
				fprintf(stderr, "pitch_analyze: bad fmt chunk in %s\n", path);
				fclose(f);
				return NULL;
			}
			audio_format = rd_le16(fmt + 0);
			num_channels = rd_le16(fmt + 2);
			sample_rate = rd_le32(fmt + 4);
			bits_per_sample = rd_le16(fmt + 14);
			long remaining = (long)csize - 16;
			if (remaining > 0)
				fseek(f, remaining, SEEK_CUR);
		}
		else if (memcmp(chdr, "data", 4) == 0)
		{
			data_offset = ftell(f);
			data_size = csize;
			fseek(f, (long)csize + (long)(csize & 1), SEEK_CUR);
		}
		else
		{
			fseek(f, (long)csize + (long)(csize & 1), SEEK_CUR);
		}
	}

	if (audio_format != 1 || bits_per_sample != 16 || num_channels == 0 || data_offset < 0)
	{
		fprintf(stderr, "pitch_analyze: %s: expected 16-bit PCM WAV (fmt=%u bits=%u ch=%u data=%ld)\n",
			path, audio_format, bits_per_sample, num_channels, data_offset);
		fclose(f);
		return NULL;
	}

	uint32_t total_shorts = data_size / 2;
	uint32_t n_frames = total_shorts / num_channels;

	int16_t *raw = (int16_t *)malloc((size_t)total_shorts * sizeof(int16_t));
	if (!raw)
	{
		fprintf(stderr, "pitch_analyze: out of memory reading %s\n", path);
		fclose(f);
		return NULL;
	}
	fseek(f, data_offset, SEEK_SET);
	size_t got = fread(raw, sizeof(int16_t), total_shorts, f);
	fclose(f);
	if (got != total_shorts)
		n_frames = (uint32_t)(got / num_channels);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	for (uint32_t i = 0; i < n_frames * num_channels; i++)
	{
		uint16_t v = (uint16_t)raw[i];
		v = (uint16_t)((v << 8) | (v >> 8));
		raw[i] = (int16_t)v;
	}
#endif

	double *mono = (double *)malloc((size_t)n_frames * sizeof(double));
	if (!mono)
	{
		fprintf(stderr, "pitch_analyze: out of memory downmixing %s\n", path);
		free(raw);
		return NULL;
	}
	for (uint32_t i = 0; i < n_frames; i++)
	{
		double s = 0.0;
		for (uint16_t c = 0; c < num_channels; c++)
			s += (double)raw[(size_t)i * num_channels + c];
		mono[i] = s / (double)num_channels;
	}
	free(raw);

	*out_n = n_frames;
	*out_sr = sample_rate;
	return mono;
}

/* ---------------------------------------------------------------------
 * FFT (iterative radix-2 Cooley-Tukey), mirrors validate_pitch.py's fft().
 * ------------------------------------------------------------------- */

static uint32_t next_pow2(uint32_t n)
{
	uint32_t p = 1;
	while (p < n)
		p <<= 1;
	return p;
}

/* In-place FFT (sign = -1 forward, +1 inverse-without-scaling). n must be a
 * power of two. */
static void fft(double complex *a, uint32_t n, int sign)
{
	if (n <= 1)
		return;

	uint32_t j = 0;
	for (uint32_t i = 1; i < n; i++)
	{
		uint32_t bit = n >> 1;
		while (j & bit)
		{
			j ^= bit;
			bit >>= 1;
		}
		j ^= bit;
		if (i < j)
		{
			double complex tmp = a[i];
			a[i] = a[j];
			a[j] = tmp;
		}
	}

	for (uint32_t length = 2; length <= n; length <<= 1)
	{
		double ang = sign * -2.0 * M_PI / (double)length;
		double complex wlen = cos(ang) + I * sin(ang);
		uint32_t half = length >> 1;
		for (uint32_t i = 0; i < n; i += length)
		{
			double complex w = 1.0;
			for (uint32_t k = 0; k < half; k++)
			{
				double complex u = a[i + k];
				double complex v = a[i + k + half] * w;
				a[i + k] = u + v;
				a[i + k + half] = u - v;
				w *= wlen;
			}
		}
	}
}

/* ---------------------------------------------------------------------
 * YIN f0 estimator - direct port of validate_pitch.py's estimate_f0().
 * ------------------------------------------------------------------- */

static void estimate_f0(const double *samples, uint32_t n0, uint32_t sr, double f_expected,
                         double *out_f0, double *out_rms)
{
	*out_f0 = 0.0;
	*out_rms = 0.0;
	if (n0 < 128)
		return;

	double sumsq = 0.0;
	for (uint32_t i = 0; i < n0; i++)
		sumsq += samples[i] * samples[i];
	double rms = sqrt(sumsq / (double)n0);
	if (rms <= 0.0)
		return;
	*out_rms = rms;

	double mean = 0.0;
	for (uint32_t i = 0; i < n0; i++)
		mean += samples[i];
	mean /= (double)n0;

	double *x = (double *)malloc((size_t)n0 * sizeof(double));
	if (!x)
		return;
	for (uint32_t i = 0; i < n0; i++)
		x[i] = samples[i] - mean;

	long tau_min = (long)(sr / (f_expected * 4.5));
	if (tau_min < 2)
		tau_min = 2;
	long tau_max = (long)(sr / (f_expected / 4.5));
	if (tau_max > (long)n0 - tau_min - 4)
		tau_max = (long)n0 - tau_min - 4;
	if (tau_max <= tau_min + 2)
	{
		free(x);
		return;
	}

	uint32_t nfft = next_pow2(2 * n0);
	double complex *buf = (double complex *)calloc(nfft, sizeof(double complex));
	if (!buf)
	{
		free(x);
		return;
	}
	for (uint32_t i = 0; i < n0; i++)
		buf[i] = x[i];

	fft(buf, nfft, -1);
	for (uint32_t i = 0; i < nfft; i++)
		buf[i] = buf[i] * conj(buf[i]);
	/* inverse FFT via conjugation trick, matching python's ifft() */
	for (uint32_t i = 0; i < nfft; i++)
		buf[i] = conj(buf[i]);
	fft(buf, nfft, -1);
	for (uint32_t i = 0; i < nfft; i++)
		buf[i] = conj(buf[i]) / (double)nfft;

	long P_len = tau_max + 1;
	double *P = (double *)malloc((size_t)P_len * sizeof(double));
	for (long t = 0; t < P_len; t++)
		P[t] = creal(buf[t]);
	free(buf);

	double *cs = (double *)malloc((size_t)(n0 + 1) * sizeof(double));
	cs[0] = 0.0;
	for (uint32_t i = 0; i < n0; i++)
		cs[i + 1] = cs[i] + x[i] * x[i];
	free(x);

	double *d = (double *)malloc((size_t)P_len * sizeof(double));
	for (long tau = 0; tau <= tau_max; tau++)
	{
		double A = cs[n0 - tau]; /* i = 0 .. n0-1-tau */
		double B = cs[n0] - cs[tau]; /* i = tau .. n0-1 */
		double dv = A + B - 2.0 * P[tau];
		if (dv < 0.0)
			dv = 0.0;
		d[tau] = dv;
	}
	free(cs);
	free(P);

	double *dp = (double *)malloc((size_t)P_len * sizeof(double));
	for (long i = 0; i < P_len; i++)
		dp[i] = 1.0;
	{
		double running = 0.0;
		for (long tau = 1; tau <= tau_max; tau++)
		{
			running += d[tau];
			dp[tau] = (running > 0.0) ? (d[tau] * (double)tau / running) : 1.0;
		}
	}

	const double threshold = 0.15;
	long best_tau = -1;
	{
		long tau = tau_min;
		while (tau < tau_max)
		{
			if (dp[tau] < threshold)
			{
				while (tau + 1 < tau_max && dp[tau + 1] < dp[tau])
					tau++;
				best_tau = tau;
				break;
			}
			tau++;
		}
	}
	if (best_tau < 0)
	{
		best_tau = tau_min;
		double best_val = dp[tau_min];
		for (long t = tau_min + 1; t < tau_max; t++)
		{
			if (dp[t] < best_val)
			{
				best_val = dp[t];
				best_tau = t;
			}
		}
	}

	double shift = 0.0;
	long t = best_tau;
	if (t >= 1 && t < tau_max)
	{
		double a = d[t - 1], b = d[t], c = d[t + 1];
		double denom = a - 2.0 * b + c;
		shift = (denom != 0.0) ? (0.5 * (a - c) / denom) : 0.0;
	}
	free(d);
	free(dp);

	double period = (double)t + shift;
	if (period <= 0.0)
		return;

	*out_f0 = (double)sr / period;
}

/* ---------------------------------------------------------------------
 * Driver: read window specs from stdin, run estimate_f0 on each, print
 * "f0_hz rms" per line.
 * ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <wav_file>\n", argv[0]);
		return 2;
	}

	uint32_t n_frames = 0, sr = 0;
	double *mono = load_wav_mono(argv[1], &n_frames, &sr);
	if (!mono)
		return 1;

	char line[256];
	while (fgets(line, sizeof(line), stdin))
	{
		long start, len;
		double f_expected;
		if (sscanf(line, "%ld %ld %lf", &start, &len, &f_expected) != 3)
			continue;
		if (start < 0)
			start = 0;
		if (len < 0)
			len = 0;
		if ((uint64_t)start > n_frames)
			start = (long)n_frames;
		if ((uint64_t)(start + len) > n_frames)
			len = (long)n_frames - start;

		double f0 = 0.0, rms = 0.0;
		if (len > 0 && f_expected > 0.0)
			estimate_f0(mono + start, (uint32_t)len, sr, f_expected, &f0, &rms);
		printf("%.6f %.6f\n", f0, rms);
	}

	free(mono);
	return 0;
}
