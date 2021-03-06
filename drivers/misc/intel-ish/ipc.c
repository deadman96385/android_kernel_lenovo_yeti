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

#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include "client.h"
#include "hw-ish.h"
#include "utils.h"
#include "hbm.h"

/* global vars for reset flow */
struct work_struct	fw_reset_work;
struct ishtp_device	*ishtp_dev;

/* ish_reg_read - reads 32bit register */
static inline uint32_t ish_reg_read(const struct ishtp_device *dev,
	unsigned long offset)
{
	struct ish_hw *hw = to_ish_hw(dev);
	return readl(hw->mem_addr + offset);
}

/* ish_reg_write - writes 32bit register */
static inline void ish_reg_write(struct ishtp_device *dev, unsigned long offset,
	uint32_t value)
{
	struct ish_hw *hw = to_ish_hw(dev);
	writel(value, hw->mem_addr + offset);
}

static inline uint32_t _ish_read_fw_sts_reg(struct ishtp_device *dev)
{
	return ish_reg_read(dev, IPC_REG_ISH_HOST_FWSTS);
}

bool check_generated_interrupt(struct ishtp_device *dev)
{
	bool interrupt_generated = true;
	uint32_t pisr_val = 0;

	pisr_val = ish_reg_read(dev, IPC_REG_PISR);
	interrupt_generated = IPC_INT_FROM_ISH_TO_HOST(pisr_val);

	return interrupt_generated;
}

/* ish_is_input_ready - check if ISH FW is ready for receiving data */
static bool ish_is_input_ready(struct ishtp_device *dev)
{
	uint32_t doorbell_val;

	doorbell_val = ish_reg_read(dev, IPC_REG_HOST2ISH_DRBL);
	return !IPC_IS_BUSY(doorbell_val);
}

/* ish_intr_enable - enables ishtp device interrupts */
void ish_intr_enable(struct ishtp_device *dev)
{
	if (ishtp_pci_device->revision == REVISION_ID_CHT_A0 ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Ax_SI)
		ish_reg_write(dev, IPC_REG_HOST_COMM, 0x81);
	else if (ishtp_pci_device->revision == REVISION_ID_CHT_B0 ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Bx_SI ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Kx_SI ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Dx_SI) {
		uint32_t host_comm_val;
		host_comm_val = ish_reg_read(dev, IPC_REG_HOST_COMM);
		host_comm_val |= IPC_HOSTCOMM_INT_EN_BIT | 0x81;
		ish_reg_write(dev, IPC_REG_HOST_COMM, host_comm_val);
	}
}

/* ish_intr_disable - disables ishtp device interrupts */
void ish_intr_disable(struct ishtp_device *dev)
{
	if (ishtp_pci_device->revision == REVISION_ID_CHT_A0 ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Ax_SI)
		/* Do nothing */;
	else if (ishtp_pci_device->revision == REVISION_ID_CHT_B0 ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Bx_SI ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Kx_SI ||
			(ishtp_pci_device->revision & REVISION_ID_SI_MASK) ==
			REVISION_ID_CHT_Dx_SI) {
		uint32_t host_comm_val;
		host_comm_val = ish_reg_read(dev, IPC_REG_HOST_COMM);
		host_comm_val &= ~IPC_HOSTCOMM_INT_EN_BIT;
		host_comm_val |= 0xC1;
		ish_reg_write(dev, IPC_REG_HOST_COMM, host_comm_val);
	}
}

/* ishtp_fw_is_ready - check if the hw is ready */
static bool ishtp_fw_is_ready(struct ishtp_device *dev)
{
	uint32_t ish_status = _ish_read_fw_sts_reg(dev);
	return IPC_IS_ISH_ILUP(ish_status) &&
		IPC_IS_ISH_ISHTP_READY(ish_status);
}

void ish_set_host_rdy(struct ishtp_device *dev)
{
	uint32_t host_status = ish_reg_read(dev, IPC_REG_HOST_COMM);
	IPC_SET_HOST_READY(host_status);
	ish_reg_write(dev, IPC_REG_HOST_COMM, host_status);
}

void ish_clr_host_rdy(struct ishtp_device *dev)
{
	uint32_t host_status = ish_reg_read(dev, IPC_REG_HOST_COMM);
	IPC_CLEAR_HOST_READY(host_status);
	ish_reg_write(dev, IPC_REG_HOST_COMM, host_status);
}

/* _ish_read_hdr - reads hdr of 32 bit length. */
static uint32_t _ishtp_read_hdr(const struct ishtp_device *dev)
{
	return ish_reg_read(dev, IPC_REG_ISH2HOST_MSG);
}

/* ish_read - reads a message from ishtp device. */
static int _ishtp_read(struct ishtp_device *dev, unsigned char *buffer,
	unsigned long buffer_length)
{
	uint32_t	i;
	uint32_t	*r_buf = (uint32_t *)buffer;
	uint32_t	msg_offs;

	msg_offs = IPC_REG_ISH2HOST_MSG + sizeof(struct ishtp_msg_hdr);
	for (i = 0; i < buffer_length; i += sizeof(uint32_t))
		*r_buf++ = ish_reg_read(dev, msg_offs + i);

	return 0;
}

/*
 * write_ipc_from_queue - try to write ipc msg from Tx queue to device
 *
 * check if DRBL is cleared. if it is - write the first IPC msg,
 * then call the callback function (unless it's NULL)
 */
int write_ipc_from_queue(struct ishtp_device *dev)
{
	struct wr_msg_ctl_info	*ipc_link;
	unsigned long	length;
	unsigned long	rem;
	unsigned long	flags;
	uint32_t	doorbell_val;
	uint32_t	*r_buf;
	uint32_t	reg_addr;
	int	i;
	void	(*ipc_send_compl)(void *);
	void	*ipc_send_compl_prm;
	static int	out_ipc_locked;
	unsigned long	out_ipc_flags;

	if (dev->dev_state == ISHTP_DEV_DISABLED)
		return	-EINVAL;

	spin_lock_irqsave(&dev->out_ipc_spinlock, out_ipc_flags);
	if (out_ipc_locked) {
		spin_unlock_irqrestore(&dev->out_ipc_spinlock, out_ipc_flags);
		return -EBUSY;
	}
	out_ipc_locked = 1;
	if (!ish_is_input_ready(dev)) {
		out_ipc_locked = 0;
		spin_unlock_irqrestore(&dev->out_ipc_spinlock, out_ipc_flags);
		return -EBUSY;
	}
	spin_unlock_irqrestore(&dev->out_ipc_spinlock, out_ipc_flags);

	spin_lock_irqsave(&dev->wr_processing_spinlock, flags);
	/*
	 * if tx send list is empty - return 0;
	 * may happen, as RX_COMPLETE handler doesn't check list emptiness.
	 */
	if (list_empty(&dev->wr_processing_list_head.link)) {
		spin_unlock_irqrestore(&dev->wr_processing_spinlock, flags);
		out_ipc_locked = 0;
		return	0;
	}

	ipc_link = list_entry(dev->wr_processing_list_head.link.next,
		struct wr_msg_ctl_info, link);
	/* first 4 bytes of the data is the doorbell value (IPC header) */
	length = ipc_link->length - sizeof(uint32_t);
	doorbell_val = *(uint32_t *)ipc_link->inline_data;
	r_buf = (uint32_t *)(ipc_link->inline_data + sizeof(uint32_t));

	/* If sending MNG_SYNC_FW_CLOCK, update clock again */
	if (IPC_HEADER_GET_PROTOCOL(doorbell_val) == IPC_PROTOCOL_MNG &&
		IPC_HEADER_GET_MNG_CMD(doorbell_val) == MNG_SYNC_FW_CLOCK) {

		struct timespec	ts;
		uint64_t	usec;

		get_monotonic_boottime(&ts);
		usec = (uint64_t)ts.tv_sec * 1000000 +
			(uint32_t)ts.tv_nsec / 1000;
		r_buf[0] = (uint32_t)(usec & 0xFFFFFFFF);
		r_buf[1] = (uint32_t)(usec >> 32);
	}

	for (i = 0, reg_addr = IPC_REG_HOST2ISH_MSG; i < length >> 2; i++,
			reg_addr += 4)
		ish_reg_write(dev, reg_addr, r_buf[i]);

	rem = length & 0x3;
	if (rem > 0) {
		uint32_t reg = 0;
		memcpy(&reg, &r_buf[length >> 2], rem);
		ish_reg_write(dev, reg_addr, reg);
	}

	/* Update IPC counters */
	++dev->ipc_tx_cnt;
	dev->ipc_tx_bytes_cnt += IPC_HEADER_GET_LENGTH(doorbell_val);

	ish_reg_write(dev, IPC_REG_HOST2ISH_DRBL, doorbell_val);
	out_ipc_locked = 0;

	ipc_send_compl = ipc_link->ipc_send_compl;
	ipc_send_compl_prm = ipc_link->ipc_send_compl_prm;
	list_del_init(&ipc_link->link);
	list_add_tail(&ipc_link->link, &dev->wr_free_list_head.link);
	spin_unlock_irqrestore(&dev->wr_processing_spinlock, flags);

	/*
	 * callback will be called out of spinlock,
	 * after ipc_link returned to free list
	 */
	if (ipc_send_compl)
		ipc_send_compl(ipc_send_compl_prm);
	return 0;
}

/*
 * write_ipc_to_queue - write ipc msg to Tx queue
 *
 * Got msg with IPC (and upper protocol) header
 * and add it to the device Tx-to-write list
 * then try to send the first IPC waiting msg (if DRBL is cleared)
 * RETURN VALUE:	negative -	fail (means free links list is empty,
 *					or msg too long)
 *			0 -		succeed
 */
static int write_ipc_to_queue(struct ishtp_device *dev,
	void (*ipc_send_compl)(void *), void *ipc_send_compl_prm,
	unsigned char *msg, int length)
{
	struct wr_msg_ctl_info *ipc_link;
	unsigned long	flags;

	if (length > IPC_FULL_MSG_SIZE)
		return -EMSGSIZE;

	spin_lock_irqsave(&dev->wr_processing_spinlock, flags);
	if (list_empty(&dev->wr_free_list_head.link)) {
		spin_unlock_irqrestore(&dev->wr_processing_spinlock, flags);
		return -ENOMEM;
	}
	ipc_link = list_entry(dev->wr_free_list_head.link.next,
		struct wr_msg_ctl_info, link);
	list_del_init(&ipc_link->link);

	ipc_link->ipc_send_compl = ipc_send_compl;
	ipc_link->ipc_send_compl_prm = ipc_send_compl_prm;
	ipc_link->length = length;
	memcpy(ipc_link->inline_data, msg, length);

	list_add_tail(&ipc_link->link, &dev->wr_processing_list_head.link);
	spin_unlock_irqrestore(&dev->wr_processing_spinlock, flags);

	write_ipc_from_queue(dev);
	return 0;
}

static int	ipc_send_mng_msg(struct ishtp_device *dev, uint32_t msg_code,
	void *msg, size_t size)
{
	unsigned char	ipc_msg[IPC_FULL_MSG_SIZE];
	uint32_t	drbl_val = IPC_BUILD_MNG_MSG(msg_code, size);

	memcpy(ipc_msg, &drbl_val, sizeof(uint32_t));
	memcpy(ipc_msg + sizeof(uint32_t), msg, size);
	return	write_ipc_to_queue(dev, NULL, NULL, ipc_msg,
		sizeof(uint32_t) + size);
}

static int	ish_fw_reset_handler(struct ishtp_device *dev)
{
	uint32_t	reset_id;
	unsigned long	flags;
	struct wr_msg_ctl_info *processing, *next;

	/* Read reset ID */
	reset_id = ish_reg_read(dev, IPC_REG_ISH2HOST_MSG) & 0xFFFF;

	/* Clear IPC output queue */
	spin_lock_irqsave(&dev->wr_processing_spinlock, flags);
	list_for_each_entry_safe(processing, next,
			&dev->wr_processing_list_head.link, link) {
		list_del(&processing->link);
		list_add_tail(&processing->link, &dev->wr_free_list_head.link);
	}
	spin_unlock_irqrestore(&dev->wr_processing_spinlock, flags);

	/* ISHTP notification in IPC_RESET */
	ishtp_reset_handler(dev);

	if (!ish_is_input_ready(dev))
		timed_wait_for_timeout(WAIT_FOR_SEND_SLICE,
			ish_is_input_ready(dev), (2 * HZ));

	/* ISH FW is dead */
	if (!ish_is_input_ready(dev)) {
		return	-EPIPE;
	} else {
		/*
		 * Set HOST2ISH.ILUP. Apparently we need this BEFORE sending
		 * RESET_NOTIFY_ACK - FW will be checking for it
		 */
		ish_set_host_rdy(dev);
		/* Send RESET_NOTIFY_ACK (with reset_id) */
		ipc_send_mng_msg(dev, MNG_RESET_NOTIFY_ACK, &reset_id,
			sizeof(uint32_t));
	}

	/* Wait for ISH FW'es ILUP and ISHTP_READY */
	timed_wait_for_timeout(WAIT_FOR_SEND_SLICE, ishtp_fw_is_ready(dev),
		(2 * HZ));
	if (!ishtp_fw_is_ready(dev)) {
		/* ISH FW is dead */
		uint32_t	ish_status;
		ish_status = _ish_read_fw_sts_reg(dev);
		dev_err(dev->devc,
			"[ishtp-ish]: completed reset, ISH is dead "
			"(FWSTS = %08X)\n",
			ish_status);
		return -ENODEV;
	}
	return	0;
}

static void	fw_reset_work_fn(struct work_struct *unused)
{
	int	rv;
	static int reset_cnt;

	reset_cnt++;

	rv = ish_fw_reset_handler(ishtp_dev);
	if (!rv) {
		/* ISH is ILUP & ISHTP-ready. Restart ISHTP */
		/*
		 * when reset flow occurs, sometimes the sysfs entries which
		 * were removed in ish_fw_reset_handler are still up,
		 * but the driver tries to create the same entries and fails.
		 * so wait some time here and then the sysfs entries removal
		 * will	be done.
		 */
		if (reset_cnt != 0) /* not the boot flow */
			schedule_timeout(HZ / 3);
		ishtp_dev->recvd_hw_ready = 1;
		if (waitqueue_active(&ishtp_dev->wait_hw_ready))
			wake_up(&ishtp_dev->wait_hw_ready);

		/* ISHTP notification in IPC_RESET sequence completion */
		ishtp_reset_compl_handler(ishtp_dev);
	} else
		dev_err(ishtp_dev->devc, "[ishtp-ish]: FW reset failed (%d)\n",
			rv);
}

static void	_ish_sync_fw_clock(struct ishtp_device *dev)
{
	static unsigned long	prev_sync;
	struct timespec	ts;
	uint64_t	usec;

	if (prev_sync && jiffies - prev_sync < 20 * HZ)
		return;

	prev_sync = jiffies;
	get_monotonic_boottime(&ts);
	usec = (uint64_t)ts.tv_sec * 1000000 + ((uint32_t)ts.tv_nsec / 1000);
	ipc_send_mng_msg(dev, MNG_SYNC_FW_CLOCK, &usec, sizeof(uint64_t));
}

/*
 * recv_ipc - Receive and process IPC management messages
 *
 * (!) ISR context
 *
 * NOTE: Any other mng command than reset_notify and reset_notify_ack
 * won't wake BH handler
 */
static void	recv_ipc(struct ishtp_device *dev, uint32_t doorbell_val)
{
	uint32_t	mng_cmd;

	mng_cmd = IPC_HEADER_GET_MNG_CMD(doorbell_val);

	switch (mng_cmd) {
	default:
		break;

	case MNG_RX_CMPL_INDICATION:
		if (suspend_flag) {
			suspend_flag = 0;
			if (waitqueue_active(&suspend_wait))
				wake_up(&suspend_wait);
		}
		write_ipc_from_queue(dev);
		break;

	case MNG_RESET_NOTIFY:
		if (!ishtp_dev) {
			ishtp_dev = dev;
			INIT_WORK(&fw_reset_work, fw_reset_work_fn);
		}
		schedule_work(&fw_reset_work);
		break;

	case MNG_RESET_NOTIFY_ACK:
		dev->recvd_hw_ready = 1;
		if (waitqueue_active(&dev->wait_hw_ready))
			wake_up(&dev->wait_hw_ready);
		break;
	}
}

/* ish_irq_handler - ISR of the ISHTP device */
irqreturn_t ish_irq_handler(int irq, void *dev_id)
{
	struct ishtp_device	*dev = dev_id;
	uint32_t	doorbell_val;
	bool	interrupt_generated;

	/* Check that it's interrupt from ISH (may be shared) */
	interrupt_generated = check_generated_interrupt(dev);

	if (!interrupt_generated)
		return IRQ_NONE;

	doorbell_val = ish_reg_read(dev, IPC_REG_ISH2HOST_DRBL);
	if (!IPC_IS_BUSY(doorbell_val))
		return IRQ_HANDLED;

	if (dev->dev_state == ISHTP_DEV_DISABLED)
		return	IRQ_HANDLED;

	ish_intr_disable(dev);

	/* Sanity check: IPC dgram length in header */
	if (IPC_HEADER_GET_LENGTH(doorbell_val) > IPC_PAYLOAD_SIZE) {
		dev_err(dev->devc,
			"IPC hdr - bad length: %u; dropped\n",
			(unsigned)IPC_HEADER_GET_LENGTH(doorbell_val));
		goto	eoi;
	}

	switch (IPC_HEADER_GET_PROTOCOL(doorbell_val)) {
	default:
		break;
	case IPC_PROTOCOL_MNG:
		recv_ipc(dev, doorbell_val);
		break;
	case IPC_PROTOCOL_ISHTP:
		recv_ishtp(dev);
		break;
	}

eoi:
	/* Update IPC counters */
	++dev->ipc_rx_cnt;
	dev->ipc_rx_bytes_cnt += IPC_HEADER_GET_LENGTH(doorbell_val);

	ish_reg_write(dev, IPC_REG_ISH2HOST_DRBL, 0);
	ish_intr_enable(dev);
	return	IRQ_HANDLED;
}

static int _ish_hw_reset(struct ishtp_device *dev)
{
	struct pci_dev *pdev = ishtp_pci_device;
	int	rv;
	unsigned	dma_delay;
	uint16_t csr;

	if (!pdev)
		return	-ENODEV;

	rv = pci_reset_function(pdev);
	if (!rv)
		dev->dev_state = ISHTP_DEV_RESETTING;

	if (!pdev->pm_cap) {
		dev_err(&pdev->dev, "Can't reset - no PM caps\n");
		return	-EINVAL;
	}

	/* Now trigger reset to FW */
	ish_reg_write(dev, IPC_REG_ISH_RMP2, 0);

	for (dma_delay = 0; dma_delay < MAX_DMA_DELAY &&
		_ish_read_fw_sts_reg(dev) & (IPC_ISH_IN_DMA);
		dma_delay += 5)
			mdelay(5);

	if (dma_delay >= MAX_DMA_DELAY) {
		dev_err(&pdev->dev,
			"Can't reset - stuck with DMA in-progress\n");
		return	-EBUSY;
	}

	pci_read_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, &csr);

	csr &= ~PCI_PM_CTRL_STATE_MASK;
	csr |= PCI_D3hot;
	pci_write_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, csr);

	mdelay(pdev->d3_delay);

	csr &= ~PCI_PM_CTRL_STATE_MASK;
	csr |= PCI_D0;
	pci_write_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, csr);

	ish_reg_write(dev, IPC_REG_ISH_RMP2, IPC_RMP2_DMA_ENABLED);

	/* Send 0 IPC message so that ISH FW wakes up if it was already
	 asleep */
	ish_reg_write(dev, IPC_REG_HOST2ISH_DRBL, IPC_DRBL_BUSY_BIT);

	return	0;
}

/* _ish_ipc_reset - resets host and fw IPC and upper layers. */
static int _ish_ipc_reset(struct ishtp_device *dev)
{
	struct ipc_rst_payload_type ipc_mng_msg;
	int	rv = 0;

	ipc_mng_msg.reset_id = 1;
	ipc_mng_msg.reserved = 0;

	ish_intr_enable(dev);

	/* Clear the incoming doorbell */
	ish_reg_write(dev, IPC_REG_ISH2HOST_DRBL, 0);

	dev->recvd_hw_ready = 0;

	/* send message */
	rv = ipc_send_mng_msg(dev, MNG_RESET_NOTIFY, &ipc_mng_msg,
		sizeof(struct ipc_rst_payload_type));
	if (rv) {
		dev_err(dev->devc, "Failed to send IPC MNG_RESET_NOTIFY\n");
		return	rv;
	}

	wait_event_timeout(dev->wait_hw_ready, dev->recvd_hw_ready, 2*HZ);
	if (!dev->recvd_hw_ready) {
		dev_err(dev->devc, "Timed out waiting for HW ready\n");
		rv = -ENODEV;
	}

	return rv;
}

int ish_hw_start(struct ishtp_device *dev)
{
	ish_set_host_rdy(dev);
	/* After that we can enable ISH DMA operation */
	ish_reg_write(dev, IPC_REG_ISH_RMP2, IPC_RMP2_DMA_ENABLED);

	/* Send 0 IPC message so that ISH FW wakes up if it was already
	 asleep */
	ish_reg_write(dev, IPC_REG_HOST2ISH_DRBL, IPC_DRBL_BUSY_BIT);
	ish_intr_enable(dev);

	/* wait for FW-initiated reset flow */
	if (!dev->recvd_hw_ready)
		wait_event_timeout(dev->wait_hw_ready, dev->recvd_hw_ready,
			10 * HZ);

	if (!dev->recvd_hw_ready) {
		dev_err(dev->devc,
			"[ishtp-ish]: Timed out waiting for "
			"FW-initiated reset\n");
		return	-ENODEV;
	}

	return 0;
}

static const struct ishtp_hw_ops ish_hw_ops = {
	.hw_reset = _ish_hw_reset,
	.ipc_reset = _ish_ipc_reset,
	.ishtp_read = _ishtp_read,
	.write = write_ipc_to_queue,
	.get_fw_status = _ish_read_fw_sts_reg,
	.sync_fw_clock = _ish_sync_fw_clock,
	.ishtp_read_hdr = _ishtp_read_hdr
};

struct ishtp_device *ish_dev_init(struct pci_dev *pdev)
{
	struct ishtp_device *dev;
	int	i;

	dev = kzalloc(sizeof(struct ishtp_device) + sizeof(struct ish_hw),
		GFP_KERNEL);
	if (!dev)
		return NULL;

	ishtp_device_init(dev);

	init_waitqueue_head(&dev->wait_hw_ready);

	spin_lock_init(&dev->wr_processing_spinlock);
	spin_lock_init(&dev->out_ipc_spinlock);

	/* Init IPC processing and free lists */
	INIT_LIST_HEAD(&dev->wr_processing_list_head.link);
	INIT_LIST_HEAD(&dev->wr_free_list_head.link);
	for (i = 0; i < IPC_TX_FIFO_SIZE; ++i) {
		struct wr_msg_ctl_info	*tx_buf;

		tx_buf = kmalloc(sizeof(struct wr_msg_ctl_info), GFP_KERNEL);
		if (!tx_buf) {
			/*
			 * IPC buffers may be limited or not available
			 * at all - although this shouldn't happen
			 */
			dev_err(dev->devc,
				"[ishtp-ish]: failure in Tx FIFO "
				"allocations (%d)\n", i);
			break;
		}
		memset(tx_buf, 0, sizeof(struct wr_msg_ctl_info));
		list_add_tail(&tx_buf->link, &dev->wr_free_list_head.link);
	}

	dev->ops = &ish_hw_ops;
	dev->devc = &pdev->dev;
	dev->mtu = IPC_PAYLOAD_SIZE - sizeof(struct ishtp_msg_hdr);
	return dev;
}

void	ishtp_device_disable(struct ishtp_device *dev)
{
	dev->dev_state = ISHTP_DEV_DISABLED;
	ish_clr_host_rdy(dev);
	ish_intr_disable(dev);
	kfree(dev->fw_clients);
}

