/*
 * hdkernels.c -- C ports of the three hot per-pixel kernels used by the HD
 * asset pipeline: the separable Lanczos-3 upscaler, the xBRZ 4x pixel-art
 * upscaler, and the QOI encoder/decoder.
 *
 * Built by CMakeLists.txt into a shared library (hdkernels.dll / libhdkernels.so
 * / libhdkernels.dylib) and loaded from Python via ctypes (tools/hdkernels.py).
 * Purely OPTIONAL: every tool keeps its pure-Python implementation and falls
 * back to it when this library is not present, so the pipeline still runs on a
 * machine with no compiler and no third-party Python packages.
 *
 * ---------------------------------------------------------------------------
 * HARD REQUIREMENT: byte-identical output to the Python kernels.
 *
 * The distributable paks (tyrian.base / tyrian.hd) are built from every
 * generated asset and verified by sha256, so a single off-by-one pixel is a
 * build failure. The Python originals do all floating-point work in IEEE
 * double, so this file must use double throughout, perform the operations in
 * the same order, and round the same way. Two consequences that are easy to
 * get wrong:
 *
 *   - Python's round() is round-half-to-EVEN, not C's round() (half away from
 *     zero). See py_round() below; every Lanczos output byte goes through it.
 *   - Float contraction (a*b + c -> fma) would change the low bit of the xBRZ
 *     colour-distance metric and the Lanczos accumulator. CMakeLists.txt
 *     compiles this file with /fp:precise (MSVC) and -ffp-contract=off
 *     (GCC/Clang) to forbid it. Do not "optimize" that away.
 *
 * ---------------------------------------------------------------------------
 * Sources ported from (keep in sync if either side changes):
 *   Lanczos  -- tools/hd_extract_anim.py: sinc/lanczos/build_taps/clamp_byte/
 *               resample_axis_horizontal/resample_axis_vertical
 *               (tools/hd_extract.py has a byte-identical copy).
 *   xBRZ     -- tools/hd_extract.py: the "STEP 4b" block (xbrz_scale_4x and
 *               friends), itself a port of Zenju's xbrz.cpp.
 *   QOI      -- tools/mkbundle.py: qoi_encode / qoi_decode.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#  define HDK_API __declspec(dllexport)
#else
#  define HDK_API __attribute__((visibility("default")))
#endif

/* The double nearest to pi -- the same value Python's math.pi holds. */
#define HDK_PI 3.141592653589793

/* ===========================================================================
 * Python-compatible rounding
 * ======================================================================== */

/*
 * CPython's float.__round__() with ndigits=None, transcribed: C round(), then
 * a half-to-even correction for exact .5 cases. Python's round(2.5) is 2, C's
 * round(2.5) is 3 -- and a Lanczos accumulator lands exactly on .5 often
 * enough that using the wrong one shifts real pixels.
 */
static double py_round(double x)
{
	double r = round(x);
	if (fabs(x - r) == 0.5)
		r = 2.0 * round(x / 2.0);
	return r;
}

/* Mirrors clamp_byte() in the Python tools: int(round(v)), clamped to 0..255. */
static uint8_t hdk_clamp_byte(double v)
{
	double r = py_round(v);
	if (r < 0.0)
		return 0;
	if (r > 255.0)
		return 255;
	return (uint8_t)(int32_t)r;
}

/* ===========================================================================
 * Separable Lanczos-3 upscale
 * ======================================================================== */

#define HDK_LANCZOS_A 3
/* A destination sample draws from src_i in [floor(c)-a+1, floor(c)+a], i.e. 2a taps. */
#define HDK_TAPS_MAX (2 * HDK_LANCZOS_A)

static double hdk_sinc(double x)
{
	double px;

	if (x == 0.0)
		return 1.0;
	px = HDK_PI * x;
	return sin(px) / px;
}

static double hdk_lanczos(double x)
{
	if (x <= -(double)HDK_LANCZOS_A || x >= (double)HDK_LANCZOS_A)
		return 0.0;
	return hdk_sinc(x) * hdk_sinc(x / (double)HDK_LANCZOS_A);
}

typedef struct
{
	int32_t *idx;    /* [dst_n * HDK_TAPS_MAX] clamped source indices */
	double *weight;  /* [dst_n * HDK_TAPS_MAX] normalized weights */
	int32_t *count;  /* [dst_n] taps actually used per destination sample */
} hdk_taps;

static void hdk_taps_free(hdk_taps *t)
{
	free(t->idx);
	free(t->weight);
	free(t->count);
	t->idx = NULL;
	t->weight = NULL;
	t->count = NULL;
}

/* Mirrors build_taps(): "align sampling grid centers" mapping, zero-weight taps
 * dropped, weights normalized to sum to 1, source indices edge-clamped. */
static int hdk_build_taps(hdk_taps *t, int32_t src_n, int32_t dst_n)
{
	double scale;
	int32_t dst_i;

	t->idx = (int32_t *)malloc((size_t)dst_n * HDK_TAPS_MAX * sizeof(int32_t));
	t->weight = (double *)malloc((size_t)dst_n * HDK_TAPS_MAX * sizeof(double));
	t->count = (int32_t *)malloc((size_t)dst_n * sizeof(int32_t));
	if (t->idx == NULL || t->weight == NULL || t->count == NULL)
	{
		hdk_taps_free(t);
		return -1;
	}

	scale = (double)src_n / (double)dst_n;

	for (dst_i = 0; dst_i < dst_n; ++dst_i)
	{
		double src_center = ((double)dst_i + 0.5) * scale - 0.5;
		int64_t floor_center = (int64_t)floor(src_center);
		int64_t lo = floor_center - HDK_LANCZOS_A + 1;
		int64_t hi = floor_center + HDK_LANCZOS_A;
		int32_t base = dst_i * HDK_TAPS_MAX;
		int32_t cnt = 0;
		double total = 0.0;
		int64_t src_i;
		int32_t k;

		for (src_i = lo; src_i <= hi; ++src_i)
		{
			double w = hdk_lanczos(src_center - (double)src_i);
			int64_t clamped;

			if (w == 0.0)
				continue;

			clamped = src_i;
			if (clamped < 0)
				clamped = 0;
			if (clamped > src_n - 1)
				clamped = src_n - 1;

			t->idx[base + cnt] = (int32_t)clamped;
			t->weight[base + cnt] = w;
			++cnt;
			total += w;
		}

		if (total == 0.0)
		{
			/* Degenerate fallback (unreachable for a >= 1): nearest sample. */
			int64_t nearest = (int64_t)py_round(src_center);

			if (nearest < 0)
				nearest = 0;
			if (nearest > src_n - 1)
				nearest = src_n - 1;

			t->idx[base] = (int32_t)nearest;
			t->weight[base] = 1.0;
			cnt = 1;
			total = 1.0;
		}

		for (k = 0; k < cnt; ++k)
			t->weight[base + k] = t->weight[base + k] / total;

		t->count[dst_i] = cnt;
	}

	return 0;
}

static void hdk_resample_horizontal(const uint8_t *src, int32_t src_w, int32_t src_h,
                                    int32_t dst_w, const hdk_taps *taps, int32_t channels,
                                    uint8_t *dst)
{
	int32_t y;

	for (y = 0; y < src_h; ++y)
	{
		int64_t row_off = (int64_t)y * src_w * channels;
		int64_t out_off = (int64_t)y * dst_w * channels;
		int32_t x;

		for (x = 0; x < dst_w; ++x)
		{
			double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
			int32_t base = x * HDK_TAPS_MAX;
			int32_t cnt = taps->count[x];
			int64_t out_pixel = out_off + (int64_t)x * channels;
			int32_t k, c;

			for (k = 0; k < cnt; ++k)
			{
				int64_t o = row_off + (int64_t)taps->idx[base + k] * channels;
				double w = taps->weight[base + k];

				for (c = 0; c < channels; ++c)
					acc[c] += (double)src[o + c] * w;
			}

			for (c = 0; c < channels; ++c)
				dst[out_pixel + c] = hdk_clamp_byte(acc[c]);
		}
	}
}

static void hdk_resample_vertical(const uint8_t *src, int32_t src_w, int32_t src_h,
                                  int32_t dst_h, const hdk_taps *taps, int32_t channels,
                                  uint8_t *dst)
{
	int32_t y;

	(void)src_h;  /* the source height is implied by the taps table */

	for (y = 0; y < dst_h; ++y)
	{
		int64_t out_row_off = (int64_t)y * src_w * channels;
		int32_t base = y * HDK_TAPS_MAX;
		int32_t cnt = taps->count[y];
		int32_t x;

		for (x = 0; x < src_w; ++x)
		{
			double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
			int64_t xo = (int64_t)x * channels;
			int32_t k, c;

			for (k = 0; k < cnt; ++k)
			{
				int64_t o = (int64_t)taps->idx[base + k] * src_w * channels + xo;
				double w = taps->weight[base + k];

				for (c = 0; c < channels; ++c)
					acc[c] += (double)src[o + c] * w;
			}

			for (c = 0; c < channels; ++c)
				dst[out_row_off + xo + c] = hdk_clamp_byte(acc[c]);
		}
	}
}

/*
 * Two-pass separable Lanczos-3 resample of an interleaved `channels`-per-pixel
 * buffer. `dst` must hold dst_w * dst_h * channels bytes. Returns 0 on success,
 * -1 on bad arguments or allocation failure.
 */
HDK_API int32_t hdk_lanczos_upscale(const uint8_t *src, int32_t src_w, int32_t src_h,
                                    int32_t dst_w, int32_t dst_h, int32_t channels,
                                    uint8_t *dst)
{
	hdk_taps h_taps = { NULL, NULL, NULL };
	hdk_taps v_taps = { NULL, NULL, NULL };
	uint8_t *stage1 = NULL;
	int32_t rc = -1;

	if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || channels < 1 || channels > 4)
		return -1;

	stage1 = (uint8_t *)malloc((size_t)dst_w * (size_t)src_h * (size_t)channels);
	if (stage1 == NULL)
		goto done;
	if (hdk_build_taps(&h_taps, src_w, dst_w) != 0)
		goto done;
	if (hdk_build_taps(&v_taps, src_h, dst_h) != 0)
		goto done;

	hdk_resample_horizontal(src, src_w, src_h, dst_w, &h_taps, channels, stage1);
	hdk_resample_vertical(stage1, dst_w, src_h, dst_h, &v_taps, channels, dst);
	rc = 0;

done:
	free(stage1);
	hdk_taps_free(&h_taps);
	hdk_taps_free(&v_taps);
	return rc;
}

/* ===========================================================================
 * xBRZ 4x (ARGB), ported from the Python port of Zenju's xbrz.cpp
 * ======================================================================== */

/* ScalerCfg defaults (xbrz config.h). */
#define XBRZ_EQUAL_COLOR_TOLERANCE 30.0
#define XBRZ_DOMINANT_DIRECTION_THRESHOLD 3.6
#define XBRZ_STEEP_DIRECTION_THRESHOLD 2.2

/* ITU-R BT.2020 YCbCr weights. Written as the same expressions the Python side
 * evaluates, so the constants are bit-for-bit the same doubles. */
#define XBRZ_K_B 0.0593
#define XBRZ_K_R 0.2627
#define XBRZ_K_G (1.0 - XBRZ_K_B - XBRZ_K_R)
#define XBRZ_SCALE_B (0.5 / (1.0 - XBRZ_K_B))
#define XBRZ_SCALE_R (0.5 / (1.0 - XBRZ_K_R))

#define XBRZ_BLEND_NONE 0
#define XBRZ_BLEND_NORMAL 1
#define XBRZ_BLEND_DOMINANT 2

/* ColorDistanceARGB::dist(): alpha-aware YCbCr distance, lumaWeight fixed at 1. */
static double xbrz_dist(uint32_t p1, uint32_t p2)
{
	int32_t a1 = (int32_t)((p1 >> 24) & 0xFF);
	int32_t a2 = (int32_t)((p2 >> 24) & 0xFF);
	int32_t r_diff = (int32_t)((p1 >> 16) & 0xFF) - (int32_t)((p2 >> 16) & 0xFF);
	int32_t g_diff = (int32_t)((p1 >> 8) & 0xFF) - (int32_t)((p2 >> 8) & 0xFF);
	int32_t b_diff = (int32_t)(p1 & 0xFF) - (int32_t)(p2 & 0xFF);

	double y = XBRZ_K_R * r_diff + XBRZ_K_G * g_diff + XBRZ_K_B * b_diff;
	double c_b = XBRZ_SCALE_B * (b_diff - y);
	double c_r = XBRZ_SCALE_R * (r_diff - y);
	double d = sqrt(y * y + c_b * c_b + c_r * c_r);

	double fa1 = a1 / 255.0;
	double fa2 = a2 / 255.0;

	if (fa1 < fa2)
		return fa1 * d + 255.0 * (fa2 - fa1);
	return fa2 * d + 255.0 * (fa1 - fa2);
}

/* gradientARGB<M,N>(): alpha-weighted blend, integer floor division throughout
 * (all operands are non-negative, so C's / matches Python's //). */
static uint32_t xbrz_gradient(uint32_t pix_front, uint32_t pix_back, int32_t m, int32_t n)
{
	int32_t weight_front = (int32_t)((pix_front >> 24) & 0xFF) * m;
	int32_t weight_back = (int32_t)((pix_back >> 24) & 0xFF) * (n - m);
	int32_t weight_sum = weight_front + weight_back;
	int32_t r, g, b, a;

	if (weight_sum == 0)
		return 0;

	r = ((int32_t)((pix_front >> 16) & 0xFF) * weight_front
	     + (int32_t)((pix_back >> 16) & 0xFF) * weight_back) / weight_sum;
	g = ((int32_t)((pix_front >> 8) & 0xFF) * weight_front
	     + (int32_t)((pix_back >> 8) & 0xFF) * weight_back) / weight_sum;
	b = ((int32_t)(pix_front & 0xFF) * weight_front
	     + (int32_t)(pix_back & 0xFF) * weight_back) / weight_sum;
	a = weight_sum / n;

	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* preProcessCorners(): classify the shared corner of the f/g/j/k 2x2 block. */
static void xbrz_preprocess_corners(const uint32_t *p, uint8_t *blend_f, uint8_t *blend_g,
                                    uint8_t *blend_j, uint8_t *blend_k)
{
	/* p is the 4x4 neighborhood a..p, row-major. */
	uint32_t b = p[1], c = p[2];
	uint32_t e = p[4], f = p[5], g = p[6], h = p[7];
	uint32_t i = p[8], j = p[9], k = p[10], l = p[11];
	uint32_t n = p[13], o = p[14];
	double jg, fk;
	uint8_t bt;

	*blend_f = XBRZ_BLEND_NONE;
	*blend_g = XBRZ_BLEND_NONE;
	*blend_j = XBRZ_BLEND_NONE;
	*blend_k = XBRZ_BLEND_NONE;

	if ((f == g && j == k) || (f == j && g == k))
		return;

	jg = xbrz_dist(i, f) + xbrz_dist(f, c) + xbrz_dist(n, k) + xbrz_dist(k, h)
	     + 4 * xbrz_dist(j, g);
	fk = xbrz_dist(e, j) + xbrz_dist(j, o) + xbrz_dist(b, g) + xbrz_dist(g, l)
	     + 4 * xbrz_dist(f, k);

	if (jg < fk)
	{
		bt = (XBRZ_DOMINANT_DIRECTION_THRESHOLD * jg < fk) ? XBRZ_BLEND_DOMINANT
		                                                   : XBRZ_BLEND_NORMAL;
		if (f != g && f != j)
			*blend_f = bt;
		if (k != j && k != g)
			*blend_k = bt;
	}
	else if (fk < jg)
	{
		bt = (XBRZ_DOMINANT_DIRECTION_THRESHOLD * fk < jg) ? XBRZ_BLEND_DOMINANT
		                                                   : XBRZ_BLEND_NORMAL;
		if (j != f && j != k)
			*blend_j = bt;
		if (g != f && g != k)
			*blend_g = bt;
	}
}

static uint8_t xbrz_rotate_blend_info(uint8_t b, int32_t rot)
{
	switch (rot)
	{
	case 0:
		return b;
	case 1:
		return (uint8_t)(((b << 2) | (b >> 6)) & 0xFF);
	case 2:
		return (uint8_t)(((b << 4) | (b >> 4)) & 0xFF);
	default:
		return (uint8_t)(((b << 6) | (b >> 2)) & 0xFF);
	}
}

/* The get_a<rotDeg>..get_i<rotDeg> DEF_GETTER permutations of a flat Kernel_3x3. */
static const int32_t xbrz_rot_idx[4][9] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8 },
	{ 6, 3, 0, 7, 4, 1, 8, 5, 2 },
	{ 8, 7, 6, 5, 4, 3, 2, 1, 0 },
	{ 2, 5, 8, 1, 4, 7, 0, 3, 6 },
};

/* OutputMatrix<4, rotDeg>: rotation-relative (i, j) -> flat index in the 4x4 block. */
static int32_t xbrz_out_index(int32_t bi, int32_t bj, int32_t rot)
{
	int32_t io, jo;

	switch (rot)
	{
	case 0:
		io = bi;
		jo = bj;
		break;
	case 1:
		io = 3 - bj;
		jo = bi;
		break;
	case 2:
		io = 3 - bi;
		jo = 3 - bj;
		break;
	default:
		io = bj;
		jo = 3 - bi;
		break;
	}
	return io * 4 + jo;
}

static void xbrz_alpha_grad(uint32_t *block, int32_t bi, int32_t bj, int32_t rot,
                            uint32_t col, int32_t m, int32_t n)
{
	int32_t idx = xbrz_out_index(bi, bj, rot);

	block[idx] = xbrz_gradient(col, block[idx], m, n);
}

/* Scaler4x<ColorGradientARGB>::blendLine* / blendCorner, scale 4 baked in. */

static void xbrz_blend_line_shallow(uint32_t *block, uint32_t col, int32_t rot)
{
	xbrz_alpha_grad(block, 3, 0, rot, col, 1, 4);
	xbrz_alpha_grad(block, 2, 2, rot, col, 1, 4);
	xbrz_alpha_grad(block, 3, 1, rot, col, 3, 4);
	xbrz_alpha_grad(block, 2, 3, rot, col, 3, 4);
	block[xbrz_out_index(3, 2, rot)] = col;
	block[xbrz_out_index(3, 3, rot)] = col;
}

static void xbrz_blend_line_steep(uint32_t *block, uint32_t col, int32_t rot)
{
	xbrz_alpha_grad(block, 0, 3, rot, col, 1, 4);
	xbrz_alpha_grad(block, 2, 2, rot, col, 1, 4);
	xbrz_alpha_grad(block, 1, 3, rot, col, 3, 4);
	xbrz_alpha_grad(block, 3, 2, rot, col, 3, 4);
	block[xbrz_out_index(2, 3, rot)] = col;
	block[xbrz_out_index(3, 3, rot)] = col;
}

static void xbrz_blend_line_steep_and_shallow(uint32_t *block, uint32_t col, int32_t rot)
{
	xbrz_alpha_grad(block, 3, 1, rot, col, 3, 4);
	xbrz_alpha_grad(block, 1, 3, rot, col, 3, 4);
	xbrz_alpha_grad(block, 3, 0, rot, col, 1, 4);
	xbrz_alpha_grad(block, 0, 3, rot, col, 1, 4);
	xbrz_alpha_grad(block, 2, 2, rot, col, 1, 3);
	block[xbrz_out_index(3, 3, rot)] = col;
	block[xbrz_out_index(3, 2, rot)] = col;
	block[xbrz_out_index(2, 3, rot)] = col;
}

static void xbrz_blend_line_diagonal(uint32_t *block, uint32_t col, int32_t rot)
{
	xbrz_alpha_grad(block, 3, 2, rot, col, 1, 2);
	xbrz_alpha_grad(block, 2, 3, rot, col, 1, 2);
	block[xbrz_out_index(3, 3, rot)] = col;
}

static void xbrz_blend_corner(uint32_t *block, uint32_t col, int32_t rot)
{
	xbrz_alpha_grad(block, 3, 3, rot, col, 68, 100);
	xbrz_alpha_grad(block, 3, 2, rot, col, 9, 100);
	xbrz_alpha_grad(block, 2, 3, rot, col, 9, 100);
}

/* blendPixel<Scaler4x, ColorDistanceARGB, rotDeg>() for one rotation. */
static void xbrz_blend_pixel_rot(uint32_t *block, const uint32_t *k3, uint8_t blend_byte,
                                 int32_t rot)
{
	uint8_t br = xbrz_rotate_blend_info(blend_byte, rot);
	uint8_t bottom_r = (uint8_t)((br >> 4) & 0x3);
	uint8_t top_r, bottom_l;
	const int32_t *ri = xbrz_rot_idx[rot];
	uint32_t rb, rc, rd, re, rf, rg, rh, rii;
	uint32_t px;
	int32_t do_line_blend;

	if (bottom_r == XBRZ_BLEND_NONE)
		return;

	rb = k3[ri[1]];
	rc = k3[ri[2]];
	rd = k3[ri[3]];
	re = k3[ri[4]];
	rf = k3[ri[5]];
	rg = k3[ri[6]];
	rh = k3[ri[7]];
	rii = k3[ri[8]];

	top_r = (uint8_t)((br >> 2) & 0x3);
	bottom_l = (uint8_t)((br >> 6) & 0x3);

	do_line_blend = 1;
	if (bottom_r >= XBRZ_BLEND_DOMINANT)
		do_line_blend = 1;
	else if (top_r != XBRZ_BLEND_NONE && xbrz_dist(re, rg) >= XBRZ_EQUAL_COLOR_TOLERANCE)
		do_line_blend = 0;
	else if (bottom_l != XBRZ_BLEND_NONE && xbrz_dist(re, rc) >= XBRZ_EQUAL_COLOR_TOLERANCE)
		do_line_blend = 0;
	else if (xbrz_dist(re, rii) >= XBRZ_EQUAL_COLOR_TOLERANCE
	         && xbrz_dist(rg, rh) < XBRZ_EQUAL_COLOR_TOLERANCE
	         && xbrz_dist(rh, rii) < XBRZ_EQUAL_COLOR_TOLERANCE
	         && xbrz_dist(rii, rf) < XBRZ_EQUAL_COLOR_TOLERANCE
	         && xbrz_dist(rf, rc) < XBRZ_EQUAL_COLOR_TOLERANCE)
		do_line_blend = 0;

	px = (xbrz_dist(re, rf) <= xbrz_dist(re, rh)) ? rf : rh;

	if (do_line_blend)
	{
		double fg = xbrz_dist(rf, rg);
		double hc = xbrz_dist(rh, rc);
		int32_t have_shallow = (XBRZ_STEEP_DIRECTION_THRESHOLD * fg <= hc)
		                       && re != rg && rd != rg;
		int32_t have_steep = (XBRZ_STEEP_DIRECTION_THRESHOLD * hc <= fg)
		                     && re != rc && rb != rc;

		if (have_shallow)
		{
			if (have_steep)
				xbrz_blend_line_steep_and_shallow(block, px, rot);
			else
				xbrz_blend_line_shallow(block, px, rot);
		}
		else
		{
			if (have_steep)
				xbrz_blend_line_steep(block, px, rot);
			else
				xbrz_blend_line_diagonal(block, px, rot);
		}
	}
	else
	{
		xbrz_blend_corner(block, px, rot);
	}
}

/*
 * xBRZ-scale an RGBA buffer by 4x. `dst` must hold (src_w*4) * (src_h*4) * 4
 * bytes. Returns 0 on success, -1 on bad arguments or allocation failure.
 */
HDK_API int32_t hdk_xbrz_scale_4x(const uint8_t *rgba, int32_t src_w, int32_t src_h,
                                  uint8_t *dst)
{
	int32_t w = src_w;
	int32_t h = src_h;
	int64_t total;
	uint32_t *src = NULL;
	uint8_t *blend = NULL;
	int32_t dst_w, y, x;
	int64_t idx;

	if (w <= 0 || h <= 0)
		return -1;

	total = (int64_t)w * h;
	src = (uint32_t *)malloc((size_t)total * sizeof(uint32_t));
	blend = (uint8_t *)calloc((size_t)total, 1);
	if (src == NULL || blend == NULL)
	{
		free(src);
		free(blend);
		return -1;
	}

	/* Pack to 0xAARRGGBB. Fully-transparent source pixels get RGB forced to 0
	 * so stray colour baked into an invisible texel can never bleed into a blend. */
	for (idx = 0; idx < total; ++idx)
	{
		int64_t o = idx * 4;
		uint32_t r = rgba[o];
		uint32_t g = rgba[o + 1];
		uint32_t b = rgba[o + 2];
		uint32_t a = rgba[o + 3];

		if (a == 0)
			r = g = b = 0;
		src[idx] = (a << 24) | (r << 16) | (g << 8) | b;
	}

	/* Pass 1: classify every interior corner. */
	for (y = 0; y < h; ++y)
	{
		int32_t ym1 = (y > 0) ? y - 1 : 0;
		int32_t yp1 = (y + 1 < h) ? y + 1 : h - 1;
		int32_t yp2 = (y + 2 < h) ? y + 2 : h - 1;
		int64_t rows[4];

		rows[0] = (int64_t)ym1 * w;
		rows[1] = (int64_t)y * w;
		rows[2] = (int64_t)yp1 * w;
		rows[3] = (int64_t)yp2 * w;

		for (x = 0; x < w; ++x)
		{
			int32_t xm1 = (x > 0) ? x - 1 : 0;
			int32_t xp1 = (x + 1 < w) ? x + 1 : w - 1;
			int32_t xp2 = (x + 2 < w) ? x + 2 : w - 1;
			int32_t cols[4];
			uint32_t nb[16];
			uint8_t bf, bg, bj, bk;
			int32_t ri, ci;

			cols[0] = xm1;
			cols[1] = x;
			cols[2] = xp1;
			cols[3] = xp2;

			for (ri = 0; ri < 4; ++ri)
				for (ci = 0; ci < 4; ++ci)
					nb[ri * 4 + ci] = src[rows[ri] + cols[ci]];

			xbrz_preprocess_corners(nb, &bf, &bg, &bj, &bk);

			if (bf)
				blend[rows[1] + x] |= (uint8_t)(bf << 4);
			if (bg && x + 1 < w)
				blend[rows[1] + x + 1] |= (uint8_t)(bg << 6);
			if (bj && y + 1 < h)
				blend[(int64_t)(y + 1) * w + x] |= (uint8_t)(bj << 2);
			if (bk && y + 1 < h && x + 1 < w)
				blend[(int64_t)(y + 1) * w + x + 1] |= bk;
		}
	}

	/* Pass 2: expand each source pixel into its 4x4 output block. */
	dst_w = w * 4;
	for (y = 0; y < h; ++y)
	{
		int32_t ym1 = (y > 0) ? y - 1 : 0;
		int32_t yp1 = (y + 1 < h) ? y + 1 : h - 1;
		int64_t row_m1 = (int64_t)ym1 * w;
		int64_t row_0 = (int64_t)y * w;
		int64_t row_p1 = (int64_t)yp1 * w;
		int32_t dst_row_base = y * 4;

		for (x = 0; x < w; ++x)
		{
			uint32_t center = src[row_0 + x];
			uint8_t bl = blend[row_0 + x];
			uint32_t block[16];
			int32_t li, lj;

			if (bl)
			{
				int32_t xm1 = (x > 0) ? x - 1 : 0;
				int32_t xp1 = (x + 1 < w) ? x + 1 : w - 1;
				uint32_t k3[9];

				k3[0] = src[row_m1 + xm1];
				k3[1] = src[row_m1 + x];
				k3[2] = src[row_m1 + xp1];
				k3[3] = src[row_0 + xm1];
				k3[4] = center;
				k3[5] = src[row_0 + xp1];
				k3[6] = src[row_p1 + xm1];
				k3[7] = src[row_p1 + x];
				k3[8] = src[row_p1 + xp1];

				for (li = 0; li < 16; ++li)
					block[li] = center;

				xbrz_blend_pixel_rot(block, k3, bl, 0);
				xbrz_blend_pixel_rot(block, k3, bl, 1);
				xbrz_blend_pixel_rot(block, k3, bl, 2);
				xbrz_blend_pixel_rot(block, k3, bl, 3);
			}
			else
			{
				for (li = 0; li < 16; ++li)
					block[li] = center;
			}

			for (li = 0; li < 4; ++li)
			{
				int64_t row_off = ((int64_t)(dst_row_base + li) * dst_w + (int64_t)x * 4) * 4;

				for (lj = 0; lj < 4; ++lj)
				{
					uint32_t px = block[li * 4 + lj];
					int64_t o = row_off + lj * 4;

					dst[o] = (uint8_t)((px >> 16) & 0xFF);
					dst[o + 1] = (uint8_t)((px >> 8) & 0xFF);
					dst[o + 2] = (uint8_t)(px & 0xFF);
					dst[o + 3] = (uint8_t)((px >> 24) & 0xFF);
				}
			}
		}
	}

	free(src);
	free(blend);
	return 0;
}

/* ===========================================================================
 * QOI encode / decode (https://qoiformat.org), matching tools/mkbundle.py
 * ======================================================================== */

#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF 0x40
#define QOI_OP_LUMA 0x80
#define QOI_OP_RUN 0xc0
#define QOI_OP_RGB 0xfe
#define QOI_OP_RGBA 0xff

/*
 * Encode raw RGBA8888 into a bare QOI chunk stream (no container header -- the
 * HDPX header already carries width/height). Returns the number of bytes
 * written, or -1 on bad arguments / insufficient output capacity.
 *
 * Worst case is 5 bytes per pixel (OP_RGBA), so out_cap >= px_count * 5 always
 * suffices.
 */
HDK_API int64_t hdk_qoi_encode(const uint8_t *rgba, int64_t px_count, uint8_t *out,
                               int64_t out_cap)
{
	uint32_t index[64];
	/* Canonical QOI seed pixel r=0,g=0,b=0,a=255, packed r|g<<8|b<<16|a<<24 --
	 * must stay in lockstep with mkbundle.py's qoi_encode() and with the
	 * decoders (hdk_qoi_decode below, mkbundle.py's qoi_decode, src/qoi.h). */
	uint32_t prev = 0xff000000u;
	int64_t out_len = 0;
	int64_t run = 0;
	int64_t i;

	if (px_count < 0 || out == NULL)
		return -1;

	for (i = 0; i < 64; ++i)
		index[i] = 0;

	for (i = 0; i < px_count; ++i)
	{
		int64_t so = i * 4;
		uint32_t px = (uint32_t)rgba[so] | ((uint32_t)rgba[so + 1] << 8)
		              | ((uint32_t)rgba[so + 2] << 16) | ((uint32_t)rgba[so + 3] << 24);
		int32_t r, g, b, a, hash_idx;

		if (px == prev)
		{
			++run;
			if (run == 62 || i == px_count - 1)
			{
				if (out_len + 1 > out_cap)
					return -1;
				out[out_len++] = (uint8_t)(QOI_OP_RUN | (run - 1));
				run = 0;
			}
			continue;
		}

		if (run > 0)
		{
			if (out_len + 1 > out_cap)
				return -1;
			out[out_len++] = (uint8_t)(QOI_OP_RUN | (run - 1));
			run = 0;
		}

		r = (int32_t)(px & 0xff);
		g = (int32_t)((px >> 8) & 0xff);
		b = (int32_t)((px >> 16) & 0xff);
		a = (int32_t)((px >> 24) & 0xff);

		hash_idx = (r * 3 + g * 5 + b * 7 + a * 11) % 64;

		if (out_len + 5 > out_cap)
			return -1;

		if (index[hash_idx] == px)
		{
			out[out_len++] = (uint8_t)(QOI_OP_INDEX | hash_idx);
		}
		else
		{
			int32_t pa = (int32_t)((prev >> 24) & 0xff);

			index[hash_idx] = px;

			if (a == pa)
			{
				int32_t pr = (int32_t)(prev & 0xff);
				int32_t pg = (int32_t)((prev >> 8) & 0xff);
				int32_t pb = (int32_t)((prev >> 16) & 0xff);
				/* Python: (x + 128) % 256 - 128, i.e. wrap into [-128, 127]. The
				 * operands are already in 0..255, so masking is equivalent. */
				int32_t dr = ((r - pr + 128) & 0xff) - 128;
				int32_t dg = ((g - pg + 128) & 0xff) - 128;
				int32_t db = ((b - pb + 128) & 0xff) - 128;

				if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1)
				{
					out[out_len++] = (uint8_t)(QOI_OP_DIFF | ((dr + 2) << 4)
					                           | ((dg + 2) << 2) | (db + 2));
				}
				else
				{
					int32_t dg_r = dr - dg;
					int32_t dg_b = db - dg;

					if (dg >= -32 && dg <= 31 && dg_r >= -8 && dg_r <= 7
					    && dg_b >= -8 && dg_b <= 7)
					{
						out[out_len++] = (uint8_t)(QOI_OP_LUMA | (dg + 32));
						out[out_len++] = (uint8_t)(((dg_r + 8) << 4) | (dg_b + 8));
					}
					else
					{
						out[out_len++] = QOI_OP_RGB;
						out[out_len++] = (uint8_t)r;
						out[out_len++] = (uint8_t)g;
						out[out_len++] = (uint8_t)b;
					}
				}
			}
			else
			{
				out[out_len++] = QOI_OP_RGBA;
				out[out_len++] = (uint8_t)r;
				out[out_len++] = (uint8_t)g;
				out[out_len++] = (uint8_t)b;
				out[out_len++] = (uint8_t)a;
			}
		}

		prev = px;
	}

	return out_len;
}

/*
 * Decode a bare QOI chunk stream back to RGBA8888. `out` must hold px_count * 4
 * bytes. Returns 0 on success, -1 on bad arguments. Mirrors mkbundle.py's
 * qoi_decode(), including its behavior when the stream runs out early (the last
 * decoded pixel is simply repeated).
 */
HDK_API int32_t hdk_qoi_decode(const uint8_t *stream, int64_t stream_len, int64_t px_count,
                               uint8_t *out)
{
	uint32_t index[64];
	uint8_t px[4] = { 0, 0, 0, 255 };
	int64_t pos = 0;
	int64_t run = 0;
	int64_t i;

	if (px_count < 0 || out == NULL)
		return -1;

	for (i = 0; i < 64; ++i)
		index[i] = 0;

	for (i = 0; i < px_count; ++i)
	{
		int64_t o = i * 4;

		if (run > 0)
		{
			--run;
		}
		else if (pos < stream_len)
		{
			uint8_t b1 = stream[pos++];

			if (b1 == QOI_OP_RGB)
			{
				px[0] = stream[pos];
				px[1] = stream[pos + 1];
				px[2] = stream[pos + 2];
				pos += 3;
			}
			else if (b1 == QOI_OP_RGBA)
			{
				px[0] = stream[pos];
				px[1] = stream[pos + 1];
				px[2] = stream[pos + 2];
				px[3] = stream[pos + 3];
				pos += 4;
			}
			else if ((b1 & 0xc0) == QOI_OP_INDEX)
			{
				uint32_t v = index[b1 & 0x3f];

				px[0] = (uint8_t)(v & 0xff);
				px[1] = (uint8_t)((v >> 8) & 0xff);
				px[2] = (uint8_t)((v >> 16) & 0xff);
				px[3] = (uint8_t)((v >> 24) & 0xff);
			}
			else if ((b1 & 0xc0) == QOI_OP_DIFF)
			{
				px[0] = (uint8_t)(px[0] + ((b1 >> 4) & 0x03) - 2);
				px[1] = (uint8_t)(px[1] + ((b1 >> 2) & 0x03) - 2);
				px[2] = (uint8_t)(px[2] + (b1 & 0x03) - 2);
			}
			else if ((b1 & 0xc0) == QOI_OP_LUMA)
			{
				uint8_t b2 = stream[pos++];
				int32_t vg = (b1 & 0x3f) - 32;

				px[0] = (uint8_t)(px[0] + vg - 8 + ((b2 >> 4) & 0x0f));
				px[1] = (uint8_t)(px[1] + vg);
				px[2] = (uint8_t)(px[2] + vg - 8 + (b2 & 0x0f));
			}
			else if ((b1 & 0xc0) == QOI_OP_RUN)
			{
				run = b1 & 0x3f;
			}

			index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] =
				(uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16)
				| ((uint32_t)px[3] << 24);
		}

		out[o] = px[0];
		out[o + 1] = px[1];
		out[o + 2] = px[2];
		out[o + 3] = px[3];
	}

	return 0;
}
