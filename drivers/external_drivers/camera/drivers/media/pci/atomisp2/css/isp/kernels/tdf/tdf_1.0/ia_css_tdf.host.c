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
#include "ia_css_types.h"
#include "sh_css_defs.h"
#include "ia_css_debug.h"
#include "ia_css_tdf.host.h"

#define TDF_BLOCK_SIZE_X (8)
#define TDF_BLOCK_SIZE_Y (8)

void
ia_css_tdf_vmem_encode(
	struct tdf_vmem_params *to,
	const struct ia_css_tdf_config *from,
	size_t size)
{
	unsigned block, c;
	unsigned rows = TDF_BLOCK_SIZE_Y;
	unsigned cols = TDF_BLOCK_SIZE_X;
	(void)size;

	/* encode thresholds for all four 8x8 blocks being processed.
	 * Blocks after Forward transform are transposed and interleaved
	 * while original threshold table is non-interleaved
	 * following code encodes threshold table in transposed + interleaved format
	 * in each inner loop it reads 2 rows and writes 2 collumns of output Threshold Blocks
	 */

	for (block = 0; block < TDF_PARAM_SIZE; block++) {
		for (c = 0; c < ISP_VEC_NELEMS/rows; c++)
		{
			int input_col = block*2 + c*rows;

			to->Threshold_Flat[block][c]          = from->thres_flat_table[input_col];
			to->Threshold_Flat[block][c + cols]   = from->thres_flat_table[input_col];
			to->Threshold_Flat[block][c + 2*cols] = from->thres_flat_table[input_col];
			to->Threshold_Flat[block][c + 3*cols] = from->thres_flat_table[input_col];

			to->Threshold_Detail[block][c]          = from->thres_detail_table[input_col];
			to->Threshold_Detail[block][c + cols]   = from->thres_detail_table[input_col];
			to->Threshold_Detail[block][c + 2*cols] = from->thres_detail_table[input_col];
			to->Threshold_Detail[block][c + 3*cols] = from->thres_detail_table[input_col];

			to->Threshold_Flat[block][c + 4*cols] = from->thres_flat_table[input_col + 1];
			to->Threshold_Flat[block][c + 5*cols] = from->thres_flat_table[input_col + 1];
			to->Threshold_Flat[block][c + 6*cols] = from->thres_flat_table[input_col + 1];
			to->Threshold_Flat[block][c + 7*cols] = from->thres_flat_table[input_col + 1];

			to->Threshold_Detail[block][c + 4*cols] = from->thres_detail_table[input_col + 1];
			to->Threshold_Detail[block][c + 5*cols] = from->thres_detail_table[input_col + 1];
			to->Threshold_Detail[block][c + 6*cols] = from->thres_detail_table[input_col + 1];
			to->Threshold_Detail[block][c + 7*cols] = from->thres_detail_table[input_col + 1];

		}
	}

}

#define TDF_WORD_MAX (8191)

void
ia_css_tdf_encode(
	struct tdf_dmem_params *to,
	const struct ia_css_tdf_config *from,
	size_t size)
{
	(void)size;
	to->Epsilon_0        = from->epsilon_0;
	to->Epsilon_1        = from->epsilon_1;
	to->EpsScaleText     = from->eps_scale_text;
	to->EpsScaleEdge     = from->eps_scale_edge;
	to->Sepa_Flat        = from->sepa_flat;
	to->Sepa_Edge        = from->sepa_edge;
	to->Blend_Flat	     = from->blend_flat;
	to->Blend_Text	     = from->blend_text;
	to->Blend_Edge	     = from->blend_edge;
	to->Shading_Gain     = from->shading_gain;
	to->Shading_BaseGain = from->shading_base_gain;
	to->LocalY_Gain      = from->local_y_gain;
	to->LocalY_BaseGain  = from->local_y_base_gain;
	to->RadX_Origin      = from->rad_x_origin;
	to->RadY_Origin      = from->rad_y_origin;
	to->Blend_Ratio      = from->blend_ratio;
	to->Min_Edge_Thres   = from->min_edge_thres;
	to->InvEpsScaleEdge  = (int32_t)((((float)TDF_WORD_MAX * 2)/from->eps_scale_edge) + 0.5f);
	to->InvEpsScaleText  = (int32_t)((((float)TDF_WORD_MAX * 2)/from->eps_scale_text) + 0.5f);
}

void
ia_css_tdf_debug_dtrace(
	const struct ia_css_tdf_config *config,
	unsigned level)
{
	(void)config;
	(void)level;
}
