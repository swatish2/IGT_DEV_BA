/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "intel_io.h"
#include "drmtest.h"

#define LC_FREQ 2700
#define LC_FREQ_2K (LC_FREQ * 2000)

#define P_MIN 2
#define P_MAX 64
#define P_INC 2

/* Constraints for PLL good behavior */
#define REF_MIN 48
#define REF_MAX 400
#define VCO_MIN 2400
#define VCO_MAX 4800

#define ABS_DIFF(a, b) ((a > b) ? (a - b) : (b - a))

struct wrpll_rnp {
	unsigned p, n2, r2;
};

static unsigned wrpll_get_budget_for_freq(int clock)
{
	unsigned budget;

	switch (clock) {
	case 25175000:
	case 25200000:
	case 27000000:
	case 27027000:
	case 37762500:
	case 37800000:
	case 40500000:
	case 40541000:
	case 54000000:
	case 54054000:
	case 59341000:
	case 59400000:
	case 72000000:
	case 74176000:
	case 74250000:
	case 81000000:
	case 81081000:
	case 89012000:
	case 89100000:
	case 108000000:
	case 108108000:
	case 111264000:
	case 111375000:
	case 148352000:
	case 148500000:
	case 162000000:
	case 162162000:
	case 222525000:
	case 222750000:
	case 296703000:
	case 297000000:
		budget = 0;
		break;
	case 233500000:
	case 245250000:
	case 247750000:
	case 253250000:
	case 298000000:
		budget = 1500;
		break;
	case 169128000:
	case 169500000:
	case 179500000:
	case 202000000:
		budget = 2000;
		break;
	case 256250000:
	case 262500000:
	case 270000000:
	case 272500000:
	case 273750000:
	case 280750000:
	case 281250000:
	case 286000000:
	case 291750000:
		budget = 4000;
		break;
	case 267250000:
	case 268500000:
		budget = 5000;
		break;
	default:
		budget = 1000;
		break;
	}

	return budget;
}

static void wrpll_update_rnp(uint64_t freq2k, unsigned budget,
			     unsigned r2, unsigned n2, unsigned p,
			     struct wrpll_rnp *best)
{
	uint64_t a, b, c, d, diff, diff_best;

	/* No best (r,n,p) yet */
	if (best->p == 0) {
		best->p = p;
		best->n2 = n2;
		best->r2 = r2;
		return;
	}

	/*
	 * Output clock is (LC_FREQ_2K / 2000) * N / (P * R), which compares to
	 * freq2k.
	 *
	 * delta = 1e6 *
	 *	   abs(freq2k - (LC_FREQ_2K * n2/(p * r2))) /
	 *	   freq2k;
	 *
	 * and we would like delta <= budget.
	 *
	 * If the discrepancy is above the PPM-based budget, always prefer to
	 * improve upon the previous solution.  However, if you're within the
	 * budget, try to maximize Ref * VCO, that is N / (P * R^2).
	 */
	a = freq2k * budget * p * r2;
	b = freq2k * budget * best->p * best->r2;
	diff = ABS_DIFF((freq2k * p * r2), (LC_FREQ_2K * n2));
	diff_best = ABS_DIFF((freq2k * best->p * best->r2),
			     (LC_FREQ_2K * best->n2));
	c = 1000000 * diff;
	d = 1000000 * diff_best;

	if (a < c && b < d) {
		/* If both are above the budget, pick the closer */
		if (best->p * best->r2 * diff < p * r2 * diff_best) {
			best->p = p;
			best->n2 = n2;
			best->r2 = r2;
		}
	} else if (a >= c && b < d) {
		/* If A is below the threshold but B is above it?  Update. */
		best->p = p;
		best->n2 = n2;
		best->r2 = r2;
	} else if (a >= c && b >= d) {
		/* Both are below the limit, so pick the higher n2/(r2*r2) */
		if (n2 * best->r2 * best->r2 > best->n2 * r2 * r2) {
			best->p = p;
			best->n2 = n2;
			best->r2 = r2;
		}
	}
	/* Otherwise a < c && b >= d, do nothing */
}

static void
wrpll_compute_rnp(int clock /* in Hz */,
		  unsigned *r2_out, unsigned *n2_out, unsigned *p_out)
{
	uint64_t freq2k;
	unsigned p, n2, r2;
	struct wrpll_rnp best = { 0, 0, 0 };
	unsigned budget;

	freq2k = clock / 100;

	budget = wrpll_get_budget_for_freq(clock);

	/* Special case handling for 540 pixel clock: bypass WR PLL entirely
	 * and directly pass the LC PLL to it. */
	if (freq2k == 5400000) {
		*n2_out = 2;
		*p_out = 1;
		*r2_out = 2;
		return;
	}

	/*
	 * Ref = LC_FREQ / R, where Ref is the actual reference input seen by
	 * the WR PLL.
	 *
	 * We want R so that REF_MIN <= Ref <= REF_MAX.
	 * Injecting R2 = 2 * R gives:
	 *   REF_MAX * r2 > LC_FREQ * 2 and
	 *   REF_MIN * r2 < LC_FREQ * 2
	 *
	 * Which means the desired boundaries for r2 are:
	 *  LC_FREQ * 2 / REF_MAX < r2 < LC_FREQ * 2 / REF_MIN
	 *
	 */
        for (r2 = LC_FREQ * 2 / REF_MAX + 1;
	     r2 <= LC_FREQ * 2 / REF_MIN;
	     r2++) {

		/*
		 * VCO = N * Ref, that is: VCO = N * LC_FREQ / R
		 *
		 * Once again we want VCO_MIN <= VCO <= VCO_MAX.
		 * Injecting R2 = 2 * R and N2 = 2 * N, we get:
		 *   VCO_MAX * r2 > n2 * LC_FREQ and
		 *   VCO_MIN * r2 < n2 * LC_FREQ)
		 *
		 * Which means the desired boundaries for n2 are:
		 * VCO_MIN * r2 / LC_FREQ < n2 < VCO_MAX * r2 / LC_FREQ
		 */
		for (n2 = VCO_MIN * r2 / LC_FREQ + 1;
		     n2 <= VCO_MAX * r2 / LC_FREQ;
		     n2++) {

			for (p = P_MIN; p <= P_MAX; p += P_INC)
				wrpll_update_rnp(freq2k, budget,
						 r2, n2, p, &best);
		}
	}

	*n2_out = best.n2;
	*p_out = best.p;
	*r2_out = best.r2;
}

/* WRPLL clock dividers */
struct wrpll_tmds_clock {
	uint32_t clock;
	uint16_t p;		/* Post divider */
	uint16_t n2;		/* Feedback divider */
	uint16_t r2;		/* Reference divider */
};

/* Table of matching values for WRPLL clocks programming for each frequency.
 * The code assumes this table is sorted. */
static const struct wrpll_tmds_clock wrpll_tmds_clock_table[] = {
	{19750000,	38,	25,	18},
	{20000000,	48,	32,	18},
	{21000000,	36,	21,	15},
	{21912000,	42,	29,	17},
	{22000000,	36,	22,	15},
	{23000000,	36,	23,	15},
	{23500000,	40,	40,	23},
	{23750000,	26,	16,	14},
	{24000000,	36,	24,	15},
	{25000000,	36,	25,	15},
	{25175000,	26,	40,	33},
	{25200000,	30,	21,	15},
	{26000000,	36,	26,	15},
	{27000000,	30,	21,	14},
	{27027000,	18,	100,	111},
	{27500000,	30,	29,	19},
	{28000000,	34,	30,	17},
	{28320000,	26,	30,	22},
	{28322000,	32,	42,	25},
	{28750000,	24,	23,	18},
	{29000000,	30,	29,	18},
	{29750000,	32,	30,	17},
	{30000000,	30,	25,	15},
	{30750000,	30,	41,	24},
	{31000000,	30,	31,	18},
	{31500000,	30,	28,	16},
	{32000000,	30,	32,	18},
	{32500000,	28,	32,	19},
	{33000000,	24,	22,	15},
	{34000000,	28,	30,	17},
	{35000000,	26,	32,	19},
	{35500000,	24,	30,	19},
	{36000000,	26,	26,	15},
	{36750000,	26,	46,	26},
	{37000000,	24,	23,	14},
	{37762500,	22,	40,	26},
	{37800000,	20,	21,	15},
	{38000000,	24,	27,	16},
	{38250000,	24,	34,	20},
	{39000000,	24,	26,	15},
	{40000000,	24,	32,	18},
	{40500000,	20,	21,	14},
	{40541000,	22,	147,	89},
	{40750000,	18,	19,	14},
	{41000000,	16,	17,	14},
	{41500000,	22,	44,	26},
	{41540000,	22,	44,	26},
	{42000000,	18,	21,	15},
	{42500000,	22,	45,	26},
	{43000000,	20,	43,	27},
	{43163000,	20,	24,	15},
	{44000000,	18,	22,	15},
	{44900000,	20,	108,	65},
	{45000000,	20,	25,	15},
	{45250000,	20,	52,	31},
	{46000000,	18,	23,	15},
	{46750000,	20,	45,	26},
	{47000000,	20,	40,	23},
	{48000000,	18,	24,	15},
	{49000000,	18,	49,	30},
	{49500000,	16,	22,	15},
	{50000000,	18,	25,	15},
	{50500000,	18,	32,	19},
	{51000000,	18,	34,	20},
	{52000000,	18,	26,	15},
	{52406000,	14,	34,	25},
	{53000000,	16,	22,	14},
	{54000000,	16,	24,	15},
	{54054000,	16,	173,	108},
	{54500000,	14,	24,	17},
	{55000000,	12,	22,	18},
	{56000000,	14,	45,	31},
	{56250000,	16,	25,	15},
	{56750000,	14,	25,	17},
	{57000000,	16,	27,	16},
	{58000000,	16,	43,	25},
	{58250000,	16,	38,	22},
	{58750000,	16,	40,	23},
	{59000000,	14,	26,	17},
	{59341000,	14,	40,	26},
	{59400000,	16,	44,	25},
	{60000000,	16,	32,	18},
	{60500000,	12,	39,	29},
	{61000000,	14,	49,	31},
	{62000000,	14,	37,	23},
	{62250000,	14,	42,	26},
	{63000000,	12,	21,	15},
	{63500000,	14,	28,	17},
	{64000000,	12,	27,	19},
	{65000000,	14,	32,	19},
	{65250000,	12,	29,	20},
	{65500000,	12,	32,	22},
	{66000000,	12,	22,	15},
	{66667000,	14,	38,	22},
	{66750000,	10,	21,	17},
	{67000000,	14,	33,	19},
	{67750000,	14,	58,	33},
	{68000000,	14,	30,	17},
	{68179000,	14,	46,	26},
	{68250000,	14,	46,	26},
	{69000000,	12,	23,	15},
	{70000000,	12,	28,	18},
	{71000000,	12,	30,	19},
	{72000000,	12,	24,	15},
	{73000000,	10,	23,	17},
	{74000000,	12,	23,	14},
	{74176000,	8,	100,	91},
	{74250000,	10,	22,	16},
	{74481000,	12,	43,	26},
	{74500000,	10,	29,	21},
	{75000000,	12,	25,	15},
	{75250000,	10,	39,	28},
	{76000000,	12,	27,	16},
	{77000000,	12,	53,	31},
	{78000000,	12,	26,	15},
	{78750000,	12,	28,	16},
	{79000000,	10,	38,	26},
	{79500000,	10,	28,	19},
	{80000000,	12,	32,	18},
	{81000000,	10,	21,	14},
	{81081000,	6,	100,	111},
	{81624000,	8,	29,	24},
	{82000000,	8,	17,	14},
	{83000000,	10,	40,	26},
	{83950000,	10,	28,	18},
	{84000000,	10,	28,	18},
	{84750000,	6,	16,	17},
	{85000000,	6,	17,	18},
	{85250000,	10,	30,	19},
	{85750000,	10,	27,	17},
	{86000000,	10,	43,	27},
	{87000000,	10,	29,	18},
	{88000000,	10,	44,	27},
	{88500000,	10,	41,	25},
	{89000000,	10,	28,	17},
	{89012000,	6,	90,	91},
	{89100000,	10,	33,	20},
	{90000000,	10,	25,	15},
	{91000000,	10,	32,	19},
	{92000000,	10,	46,	27},
	{93000000,	10,	31,	18},
	{94000000,	10,	40,	23},
	{94500000,	10,	28,	16},
	{95000000,	10,	44,	25},
	{95654000,	10,	39,	22},
	{95750000,	10,	39,	22},
	{96000000,	10,	32,	18},
	{97000000,	8,	23,	16},
	{97750000,	8,	42,	29},
	{98000000,	8,	45,	31},
	{99000000,	8,	22,	15},
	{99750000,	8,	34,	23},
	{100000000,	6,	20,	18},
	{100500000,	6,	19,	17},
	{101000000,	6,	37,	33},
	{101250000,	8,	21,	14},
	{102000000,	6,	17,	15},
	{102250000,	6,	25,	22},
	{103000000,	8,	29,	19},
	{104000000,	8,	37,	24},
	{105000000,	8,	28,	18},
	{106000000,	8,	22,	14},
	{107000000,	8,	46,	29},
	{107214000,	8,	27,	17},
	{108000000,	8,	24,	15},
	{108108000,	8,	173,	108},
	{109000000,	6,	23,	19},
	{110000000,	6,	22,	18},
	{110013000,	6,	22,	18},
	{110250000,	8,	49,	30},
	{110500000,	8,	36,	22},
	{111000000,	8,	23,	14},
	{111264000,	8,	150,	91},
	{111375000,	8,	33,	20},
	{112000000,	8,	63,	38},
	{112500000,	8,	25,	15},
	{113100000,	8,	57,	34},
	{113309000,	8,	42,	25},
	{114000000,	8,	27,	16},
	{115000000,	6,	23,	18},
	{116000000,	8,	43,	25},
	{117000000,	8,	26,	15},
	{117500000,	8,	40,	23},
	{118000000,	6,	38,	29},
	{119000000,	8,	30,	17},
	{119500000,	8,	46,	26},
	{119651000,	8,	39,	22},
	{120000000,	8,	32,	18},
	{121000000,	6,	39,	29},
	{121250000,	6,	31,	23},
	{121750000,	6,	23,	17},
	{122000000,	6,	42,	31},
	{122614000,	6,	30,	22},
	{123000000,	6,	41,	30},
	{123379000,	6,	37,	27},
	{124000000,	6,	51,	37},
	{125000000,	6,	25,	18},
	{125250000,	4,	13,	14},
	{125750000,	4,	27,	29},
	{126000000,	6,	21,	15},
	{127000000,	6,	24,	17},
	{127250000,	6,	41,	29},
	{128000000,	6,	27,	19},
	{129000000,	6,	43,	30},
	{129859000,	4,	25,	26},
	{130000000,	6,	26,	18},
	{130250000,	6,	42,	29},
	{131000000,	6,	32,	22},
	{131500000,	6,	38,	26},
	{131850000,	6,	41,	28},
	{132000000,	6,	22,	15},
	{132750000,	6,	28,	19},
	{133000000,	6,	34,	23},
	{133330000,	6,	37,	25},
	{134000000,	6,	61,	41},
	{135000000,	6,	21,	14},
	{135250000,	6,	167,	111},
	{136000000,	6,	62,	41},
	{137000000,	6,	35,	23},
	{138000000,	6,	23,	15},
	{138500000,	6,	40,	26},
	{138750000,	6,	37,	24},
	{139000000,	6,	34,	22},
	{139050000,	6,	34,	22},
	{139054000,	6,	34,	22},
	{140000000,	6,	28,	18},
	{141000000,	6,	36,	23},
	{141500000,	6,	22,	14},
	{142000000,	6,	30,	19},
	{143000000,	6,	27,	17},
	{143472000,	4,	17,	16},
	{144000000,	6,	24,	15},
	{145000000,	6,	29,	18},
	{146000000,	6,	47,	29},
	{146250000,	6,	26,	16},
	{147000000,	6,	49,	30},
	{147891000,	6,	23,	14},
	{148000000,	6,	23,	14},
	{148250000,	6,	28,	17},
	{148352000,	4,	100,	91},
	{148500000,	6,	33,	20},
	{149000000,	6,	48,	29},
	{150000000,	6,	25,	15},
	{151000000,	4,	19,	17},
	{152000000,	6,	27,	16},
	{152280000,	6,	44,	26},
	{153000000,	6,	34,	20},
	{154000000,	6,	53,	31},
	{155000000,	6,	31,	18},
	{155250000,	6,	50,	29},
	{155750000,	6,	45,	26},
	{156000000,	6,	26,	15},
	{157000000,	6,	61,	35},
	{157500000,	6,	28,	16},
	{158000000,	6,	65,	37},
	{158250000,	6,	44,	25},
	{159000000,	6,	53,	30},
	{159500000,	6,	39,	22},
	{160000000,	6,	32,	18},
	{161000000,	4,	31,	26},
	{162000000,	4,	18,	15},
	{162162000,	4,	131,	109},
	{162500000,	4,	53,	44},
	{163000000,	4,	29,	24},
	{164000000,	4,	17,	14},
	{165000000,	4,	22,	18},
	{166000000,	4,	32,	26},
	{167000000,	4,	26,	21},
	{168000000,	4,	46,	37},
	{169000000,	4,	104,	83},
	{169128000,	4,	64,	51},
	{169500000,	4,	39,	31},
	{170000000,	4,	34,	27},
	{171000000,	4,	19,	15},
	{172000000,	4,	51,	40},
	{172750000,	4,	32,	25},
	{172800000,	4,	32,	25},
	{173000000,	4,	41,	32},
	{174000000,	4,	49,	38},
	{174787000,	4,	22,	17},
	{175000000,	4,	35,	27},
	{176000000,	4,	30,	23},
	{177000000,	4,	38,	29},
	{178000000,	4,	29,	22},
	{178500000,	4,	37,	28},
	{179000000,	4,	53,	40},
	{179500000,	4,	73,	55},
	{180000000,	4,	20,	15},
	{181000000,	4,	55,	41},
	{182000000,	4,	31,	23},
	{183000000,	4,	42,	31},
	{184000000,	4,	30,	22},
	{184750000,	4,	26,	19},
	{185000000,	4,	37,	27},
	{186000000,	4,	51,	37},
	{187000000,	4,	36,	26},
	{188000000,	4,	32,	23},
	{189000000,	4,	21,	15},
	{190000000,	4,	38,	27},
	{190960000,	4,	41,	29},
	{191000000,	4,	41,	29},
	{192000000,	4,	27,	19},
	{192250000,	4,	37,	26},
	{193000000,	4,	20,	14},
	{193250000,	4,	53,	37},
	{194000000,	4,	23,	16},
	{194208000,	4,	23,	16},
	{195000000,	4,	26,	18},
	{196000000,	4,	45,	31},
	{197000000,	4,	35,	24},
	{197750000,	4,	41,	28},
	{198000000,	4,	22,	15},
	{198500000,	4,	25,	17},
	{199000000,	4,	28,	19},
	{200000000,	4,	37,	25},
	{201000000,	4,	61,	41},
	{202000000,	4,	112,	75},
	{202500000,	4,	21,	14},
	{203000000,	4,	146,	97},
	{204000000,	4,	62,	41},
	{204750000,	4,	44,	29},
	{205000000,	4,	38,	25},
	{206000000,	4,	29,	19},
	{207000000,	4,	23,	15},
	{207500000,	4,	40,	26},
	{208000000,	4,	37,	24},
	{208900000,	4,	48,	31},
	{209000000,	4,	48,	31},
	{209250000,	4,	31,	20},
	{210000000,	4,	28,	18},
	{211000000,	4,	25,	16},
	{212000000,	4,	22,	14},
	{213000000,	4,	30,	19},
	{213750000,	4,	38,	24},
	{214000000,	4,	46,	29},
	{214750000,	4,	35,	22},
	{215000000,	4,	43,	27},
	{216000000,	4,	24,	15},
	{217000000,	4,	37,	23},
	{218000000,	4,	42,	26},
	{218250000,	4,	42,	26},
	{218750000,	4,	34,	21},
	{219000000,	4,	47,	29},
	{220000000,	4,	44,	27},
	{220640000,	4,	49,	30},
	{220750000,	4,	36,	22},
	{221000000,	4,	36,	22},
	{222000000,	4,	23,	14},
	{222525000,	4,	150,	91},
	{222750000,	4,	33,	20},
	{227000000,	4,	37,	22},
	{230250000,	4,	29,	17},
	{233500000,	4,	38,	22},
	{235000000,	4,	40,	23},
	{238000000,	4,	30,	17},
	{241500000,	2,	17,	19},
	{245250000,	2,	20,	22},
	{247750000,	2,	22,	24},
	{253250000,	2,	15,	16},
	{256250000,	2,	18,	19},
	{262500000,	2,	31,	32},
	{267250000,	2,	66,	67},
	{268500000,	2,	94,	95},
	{270000000,	2,	14,	14},
	{272500000,	2,	77,	76},
	{273750000,	2,	57,	56},
	{280750000,	2,	24,	23},
	{281250000,	2,	23,	22},
	{286000000,	2,	17,	16},
	{291750000,	2,	26,	24},
	{296703000,	2,	100,	91},
	{297000000,	2,	22,	20},
	{298000000,	2,	21,	19},
};

int main(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(wrpll_tmds_clock_table); i++) {
		const struct wrpll_tmds_clock *ref = &wrpll_tmds_clock_table[i];
		unsigned r2, n2, p;

		wrpll_compute_rnp(ref->clock, &r2, &n2, &p);
		igt_fail_on_f(ref->r2 != r2 || ref->n2 != n2 || ref->p != p,
			      "Computed value differs for %"PRId64" Hz:\n""  Reference: (%u,%u,%u)\n""  Computed:  (%u,%u,%u)\n", (int64_t)ref->clock * 1000, ref->r2, ref->n2, ref->p, r2, n2, p);
	}

	return 0;
}
