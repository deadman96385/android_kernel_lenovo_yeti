/*
 * ISH platform-specific definitions
 *
 * Copyright (c) 2012-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#ifndef PLATFORM_CONFIG__H
#define PLATFORM_CONFIG__H

/* Build ID string */
#define BUILD_ID	"260-stdint"

#define	ISH_DBG_PRINT	no_printk
#define	ISH_INFO	1
#if ISH_INFO
#define	ISH_INFO_PRINT	printk
#else
#define	ISH_INFO_PRINT	no_printk
#endif

#define	REVISION_ID_CHT_A0	0x6
#define	REVISION_ID_CHT_Ax_SI	0x0
#define	REVISION_ID_CHT_Bx_SI	0x10
#define	REVISION_ID_CHT_Kx_SI	0x20
#define	REVISION_ID_CHT_Dx_SI	0x30
#define	REVISION_ID_CHT_B0	0xB0

#define	REVISION_ID_SI_MASK	0x70

/* Enable ISH register debugger */
#define	ISH_DEBUGGER	1
/* Enable ISH internal log */
#define ISH_LOG		0

#endif /* PLATFORM_CONFIG__H*/

