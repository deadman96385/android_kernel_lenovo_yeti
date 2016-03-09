/**
Support for Intel Camera Imaging ISP subsystem.
Copyright (c) 2010 - 2015, Intel Corporation.

This program is free software; you can redistribute it and/or modify it
under the terms and conditions of the GNU General Public License,
version 2, as published by the Free Software Foundation.

This program is distributed in the hope it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
more details.
*/

#include "type_support.h"
#include "ia_css_bnlm.host.h"

#ifndef IA_CSS_NO_DEBUG
#include "ia_css_debug.h" /* ia_css_debug_dtrace() */
#endif
#include <assert_support.h>

#define BNLM_MAX_NUM_LUT_ENTRIES	(16)
#define BNLM_DIV_LUT_SIZE	(16)
static const int32_t div_lut_nearests[BNLM_DIV_LUT_SIZE] = {
	0, 454, 948, 1484, 2070, 2710, 3412, 4184, 5035, 5978, 7025, 8191, 8191, 8191, 8191, 8191
};

static const int32_t div_lut_slopes[BNLM_DIV_LUT_SIZE] = {
	-7760, -6960, -6216, -5536, -4912, -4344, -3832, -3360, -2936, -2552, -2208, -2208, -2208, -2208, -2208, -2208
};

static const int32_t div_lut_intercepts[BNLM_DIV_LUT_SIZE] = {
	8184, 7752, 7336, 6928, 6536, 6152, 5776, 5416, 5064, 4728, 4408, 4408, 4408, 4408, 4408, 4408
};

/* Encodes a look-up table from BNLM public parameters to vmem parameters.
 * Input:
 *	lut	:	bnlm_lut struct containing encoded vmem parameters look-up table
 *	lut_thr	:	array containing threshold values for lut
 *	lut_val	:	array containing output values related to lut_thr
 *	lut_size:	Size of lut_val array
 *	*/
static inline void
bnlm_lut_encode(struct bnlm_lut *lut, const int32_t *lut_thr, const int32_t *lut_val, const uint32_t lut_size)
{
	uint32_t blk, i;
	const uint32_t block_size = 16;
	const uint32_t total_blocks = (ISP_VEC_NELEMS / block_size);

	/* Create VMEM LUTs from the threshold and value arrays.
	 *
	 * Min size of the LUT is 2 entries.
	 *
	 * Max size of the LUT is 16 entries, so that the LUT can fit into a
	 * single group of 16 elements inside a vector.
	 * Then these elements are copied into other groups inside the same
	 * vector. If the LUT size is less than 16, then remaining elements are
	 * set to 0.
	 * */
	assert((lut_size >= 2) && (lut_size <= block_size));
	/* array lut_thr has (lut_size-1) entries */
	for (i = 0; i < lut_size-2; i++) {
		/* Check if the lut_thr is monotonically increasing */
		assert(lut_thr[i] <= lut_thr[i+1]);
	}

	/* Intialize */
	for (i = 0; i < total_blocks * block_size; i++) {
		lut->thr[0][i] = 0;
		lut->val[0][i] = 0;
	}

	/* Copy all data */
	for (i = 0; i < lut_size - 1; i++) {
		lut->thr[0][i] = lut_thr[i];
		lut->val[0][i] = lut_val[i];
	}
	lut->val[0][i] = lut_val[i]; /* val has one more element than thr */

	/* Copy data from first block to all blocks */
	for (blk = 1; blk < total_blocks; blk++) {
		uint32_t blk_offset = blk * block_size;
		for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++) {
			lut->thr[0][blk_offset + i] = lut->thr[0][i];
			lut->val[0][blk_offset + i] = lut->val[0][i];
		}
	}
}

/*
 * - Encodes BNLM public parameters into VMEM parameters
 * - Generates VMEM parameters which will needed internally ISP
 * */
void
ia_css_bnlm_vmem_encode(
			struct bnlm_vmem_params *to,
			const struct ia_css_bnlm_config *from,
			size_t size)
{
	int i;
	uint32_t blk;
	const uint32_t block_size = 16;
	const uint32_t total_blocks = (ISP_VEC_NELEMS / block_size);
	(void)size;

	/* Initialize LUTs in VMEM parameters */
	bnlm_lut_encode(&to->mu_root_lut, from->mu_root_lut_thr, from->mu_root_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->sad_norm_lut, from->sad_norm_lut_thr, from->sad_norm_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->sig_detail_lut, from->sig_detail_lut_thr, from->sig_detail_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->sig_rad_lut, from->sig_rad_lut_thr, from->sig_rad_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->rad_pow_lut, from->rad_pow_lut_thr, from->rad_pow_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->nl_0_lut, from->nl_0_lut_thr, from->nl_0_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->nl_1_lut, from->nl_1_lut_thr, from->nl_1_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->nl_2_lut, from->nl_2_lut_thr, from->nl_2_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);
	bnlm_lut_encode(&to->nl_3_lut, from->nl_3_lut_thr, from->nl_3_lut_val, BNLM_MAX_NUM_LUT_ENTRIES);

	/* Initialize arrays in VMEM parameters */
	memset(to->nl_th, 0, sizeof(to->nl_th));
	to->nl_th[0][0] = from->nl_th[0];
	to->nl_th[0][1] = from->nl_th[1];
	to->nl_th[0][2] = from->nl_th[2];

	memset(to->match_quality_max_idx, 0, sizeof(to->match_quality_max_idx));
	to->match_quality_max_idx[0][0] = from->match_quality_max_idx[0];
	to->match_quality_max_idx[0][1] = from->match_quality_max_idx[1];
	to->match_quality_max_idx[0][2] = from->match_quality_max_idx[2];
	to->match_quality_max_idx[0][3] = from->match_quality_max_idx[3];

	/* div lut - nearest, slopes and intercepts */
	memset(to->div_lut_intercepts, 0, sizeof(to->div_lut_intercepts));
	memset(to->div_lut.thr, 0, sizeof(to->div_lut.thr));
	memset(to->div_lut.val, 0, sizeof(to->div_lut.val));
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++) {
		to->div_lut_intercepts[0][i] = div_lut_intercepts[i];
		to->div_lut.thr[0][i] = div_lut_nearests[i];
		to->div_lut.val[0][i] = div_lut_slopes[i];
	}

	/* Power of 2 */
	memset(to->power_of_2, 0, sizeof(to->power_of_2));
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++) {
		if (i < ISP_VEC_ELEMBITS - 1) {
			to->power_of_2[0][i] = 1 << i;
		} else {
			/* pad remaining entries with last valid entry */
			to->power_of_2[0][i] = 1 << ((ISP_VEC_ELEMBITS - 1) - 1);
		}
	}

	/* Copy div luts (nearest, slope, intercept and power_of_2) from
	 * block[0] to blocks 1, 2 and 3
	 */
	for (blk = 1; blk < total_blocks; blk++) {
		uint32_t blk_offset = blk * block_size;
		for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++) {
			to->div_lut.thr[0][blk_offset + i] = to->div_lut.thr[0][i];
			to->div_lut.val[0][blk_offset + i] = to->div_lut.val[0][i];
			to->div_lut_intercepts[0][blk_offset + i] = to->div_lut_intercepts[0][i];
			to->power_of_2[0][blk_offset + i] = to->power_of_2[0][i];
			to->match_quality_max_idx[0][blk_offset + i] = to->match_quality_max_idx[0][i];
		}
	}
}

/* - Encodes BNLM public parameters into DMEM parameters */
void
ia_css_bnlm_encode(
	struct bnlm_dmem_params *to,
	const struct ia_css_bnlm_config *from,
	size_t size)
{
	(void)size;
	to->rad_enable = from->rad_enable;
	to->rad_x_origin = from->rad_x_origin;
	to->rad_y_origin = from->rad_y_origin;
	to->avg_min_th = from->avg_min_th;
	to->max_min_th = from->max_min_th;

	to->exp_coeff_a = from->exp_coeff_a;
	to->exp_coeff_b = from->exp_coeff_b;
	to->exp_coeff_c = from->exp_coeff_c;
	to->exp_exponent = from->exp_exponent;
}

/* Prints debug traces for BNLM public parameters */
void
ia_css_bnlm_debug_dtrace(
	const struct ia_css_bnlm_config *config,
	unsigned level)
{
#ifndef IA_CSS_NO_DEBUG
	int i;
#endif
	if (!config)
		return;

#ifndef IA_CSS_NO_DEBUG
	ia_css_debug_dtrace(level, "BNLM:\n");
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "rad_enable", config->rad_enable);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "rad_x_origin", config->rad_x_origin);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "rad_y_origin", config->rad_y_origin);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "avg_min_th", config->avg_min_th);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "max_min_th", config->max_min_th);

	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "exp_coeff_a", config->exp_coeff_a);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "exp_coeff_b", config->exp_coeff_b);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "exp_coeff_c", config->exp_coeff_c);
	ia_css_debug_dtrace(level, "\t%-32s = %d\n", "exp_exponent", config->exp_exponent);

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_th");
	for (i = 0; i < 3; i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_th[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "match_quality_max_idx");
	for (i = 0; i < 4; i++)
		ia_css_debug_dtrace(level, "%d, ", config->match_quality_max_idx[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "mu_root_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->mu_root_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "mu_root_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->mu_root_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "sad_norm_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->sad_norm_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "sad_norm_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->sad_norm_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "sig_detail_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->sig_detail_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "sig_detail_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->sig_detail_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "sig_rad_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->sig_rad_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "sig_rad_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->sig_rad_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "rad_pow_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->rad_pow_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "rad_pow_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->rad_pow_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_0_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_0_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_0_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_0_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_0_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_0_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_0_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_0_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_1_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_1_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_1_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_1_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_2_lut_thr");
	for (i = 0; i < (BNLM_MAX_NUM_LUT_ENTRIES - 1); i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_2_lut_thr[i]);
	ia_css_debug_dtrace(level, "}\n");

	ia_css_debug_dtrace(level, "\t%-32s = {", "nl_3_lut_val");
	for (i = 0; i < BNLM_MAX_NUM_LUT_ENTRIES; i++)
		ia_css_debug_dtrace(level, "%d, ", config->nl_3_lut_val[i]);
	ia_css_debug_dtrace(level, "}\n");

#endif /* IA_CSS_NO_DEBUG */

}
