/*
 * H/W layer of ISHTP provider device (ISH)
 *
 * Copyright (c) 2014-2016, Intel Corporation.
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

#ifndef _ISHTP_HW_ISH_H_
#define _ISHTP_HW_ISH_H_

#include <linux/pci.h>
#include <linux/interrupt.h>
#include "hw-ish-regs.h"
#include "ishtp_dev.h"

extern struct pci_dev *ishtp_pci_device;
extern int	suspend_flag;
extern wait_queue_head_t	suspend_wait;

struct ipc_rst_payload_type {
	uint16_t	reset_id;
	uint16_t	reserved;
};

struct ish_hw {
	void __iomem *mem_addr;
};

#define to_ish_hw(dev) (struct ish_hw *)((dev)->hw)

irqreturn_t ish_irq_handler(int irq, void *dev_id);
struct ishtp_device *ish_dev_init(struct pci_dev *pdev);
int ish_hw_start(struct ishtp_device *dev);

void g_ish_print_log(char *format, ...);

#endif /* _ISHTP_HW_ISH_H_ */

