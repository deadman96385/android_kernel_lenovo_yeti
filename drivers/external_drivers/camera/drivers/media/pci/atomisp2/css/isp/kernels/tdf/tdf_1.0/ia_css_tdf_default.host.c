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

#include "ia_css_tdf_types.h"

const struct ia_css_tdf_config default_tdf_config = {
	.thres_flat_table = {  0,   0,  92,  38,  77,  30,  62,  24,
	                       0,   0, 147,  92, 123,  77,  99,  62,
	                      92,  38,  92,  38,  77,  30,  62,  24,
	                     147,  92, 147,  92, 123,  77,  99,  62,
	                      77,  30,  77,  30,  77,  30,  62,  24,
	                     123,  77, 123,  77, 123,  77,  99,  62,
	                      62,  24,  62,  24,  62,  24,  30,  12,
	                      99,  62,  99,  62,  99,  62,  50,  30},
	.thres_detail_table = { 0, 0, 5, 2, 3, 2, 2, 1,
	                        0, 0, 6, 5, 4, 3, 2, 2,
	                        5, 2, 5, 2, 3, 2, 1, 0,
	                        6, 5, 6, 5, 4, 3, 1, 1,
	                        3, 2, 3, 2, 3, 2, 0, 0,
	                        4, 3, 4, 3, 4, 3, 1, 0,
	                        2, 1, 1, 0, 0, 0, 0, 0,
	                        2, 2, 1, 1, 1, 0, 0, 0},
	.epsilon_0 = 4095,
	.epsilon_1 = 5733,
	.eps_scale_text = 409,
	.eps_scale_edge = 3685,
	.sepa_flat = 81,
	.sepa_edge = 256,
	.blend_flat = 819,
	.blend_text = 819,
	.blend_edge = 8191,
	.shading_gain = 1024,
	.shading_base_gain = 512,
	.local_y_gain = 0,
	.local_y_base_gain = 512,
	.rad_x_origin = 0,
	.rad_y_origin = 0,
	.blend_ratio = 4096,
	.min_edge_thres = 0
};

