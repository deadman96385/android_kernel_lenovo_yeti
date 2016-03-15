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

#ifndef __IA_CSS_TDF_PARAM_H
#define __IA_CSS_TDF_PARAM_H

#include "type_support.h"
#include "vmem.h" /* needed for VMEM_ARRAY */
#include "ia_css_tdf_types.h"

struct tdf_vmem_params {
	VMEM_ARRAY(Threshold_Flat, TDF_PARAM_SIZE*ISP_VEC_NELEMS);
	VMEM_ARRAY(Threshold_Detail, TDF_PARAM_SIZE*ISP_VEC_NELEMS);
};

struct tdf_dmem_params {
	int32_t Epsilon_0;
	int32_t Epsilon_1;
	int32_t EpsScaleText;
	int32_t EpsScaleEdge;
	int32_t Sepa_Flat;
	int32_t Sepa_Edge;
	int32_t Blend_Flat;
	int32_t Blend_Text;
	int32_t Blend_Edge;
	int32_t Shading_Gain;
	int32_t Shading_BaseGain;
	int32_t LocalY_Gain;
	int32_t LocalY_BaseGain;
	int32_t RadX_Origin;
	int32_t RadY_Origin;
	int32_t Blend_Ratio;
	int32_t Min_Edge_Thres;
	int32_t InvEpsScaleEdge;
	int32_t InvEpsScaleText;
};

#endif /* __IA_CSS_TDF_PARAM_H */
