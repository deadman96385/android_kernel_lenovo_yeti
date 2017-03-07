/*! \file sx86xx.c
 * \brief	 Helper functions for Interrupt handling on SXxx products
 *
 * Copyright (c) 2011 Semtech Corp
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/input/smtc/sx86xx.h> /* main struct, interrupt,init,pointers */

#ifdef USE_THREADED_IRQ
static void sx86xx_process_interrupt(psx86xx_t this,u8 nirqlow)
{
	int status = 0;
	int counter = 0;
	if (!this) {
		pr_err("%s: NULL sx86xx_t\n",__func__);
		return;
	}
	/* since we are not in an interrupt don't need to disable irq. */
	status = this->refreshStatus(this);
	counter = -1;
	pr_debug("Worker - Refresh Status %d\n",status);
	while((++counter) < MAX_NUM_STATUS_BITS) { /* counter start from MSB */
		pr_debug("Looping Counter %d\n",counter);
		if (((status>>counter) & 0x01) && (this->statusFunc[counter])) {
			pr_debug("Function Pointer Found. Calling\n");
			this->statusFunc[counter](this);
		}
	}
	if (unlikely(this->useIrqTimer && nirqlow)) {
		/* In case we need to send a timer for example on a touchscreen
		 * checking penup, perform this here
		 */
		cancel_delayed_work(&this->dworker);
		schedule_delayed_work(&this->dworker,msecs_to_jiffies(this->irqTimeout));
		pr_info("Schedule Irq timer");
	}
}


static void sx86xx_worker_func(struct work_struct *work)
{
	psx86xx_t this = 0;
	if (work) {
		this = container_of(work,sx86xx_t,dworker.work);
		if (!this) {
			pr_err("%s: NULL sx86xx_t\n",__func__);
			return;
		}
		if ((!this->get_nirq_low) || (!this->get_nirq_low())) {
			/* only run if nirq is high */
			sx86xx_process_interrupt(this,0);
		}
	} else {
		pr_err("%s: NULL work_struct\n",__func__);
	}
}
static irqreturn_t sx86xx_interrupt_thread(int irq, void *data)
{
	psx86xx_t this = 0;
	this = data;
	mutex_lock(&this->mutex);
	pr_info("%s\n",__func__);
	if ((!this->get_nirq_low) || this->get_nirq_low()) {
		sx86xx_process_interrupt(this,1);
	}
	else
		pr_err("%s:nirq read high\n",__func__);
	mutex_unlock(&this->mutex);
	return IRQ_HANDLED;
}
#else
static void sx86xx_schedule_work(psx86xx_t this, unsigned long delay)
{
	if (this) {
		pr_info("%s\n",__func__);

		/* Stop any pending penup queues */
		cancel_delayed_work(&this->dworker);
		schedule_delayed_work(&this->dworker,delay);
	}
	else
		pr_err("%s: NULL psx86xx_t\n",__func__);
}

static irqreturn_t sx86xx_irq(int irq, void *pvoid)
{
	psx86xx_t this = 0;
	unsigned long flags;
	if (pvoid) {
		this = (psx86xx_t)pvoid;
		pr_info("%s\n",__func__);
		if ((!this->get_nirq_low) || this->get_nirq_low()) {
			pr_debug("sx86xx_irq - Schedule Work\n");
			spin_lock_irqsave(&this->lock,flags);
			sx86xx_schedule_work(this,0);
			spin_unlock_irqrestore(&this->lock,flags);
		}
		else
			pr_err("%s:nirq read high\n",__func__);
	}
	else
		pr_err("%s:NULL pvoid\n",__func__);
	return IRQ_HANDLED;
}

static void sx86xx_worker_func(struct work_struct *work)
{
	psx86xx_t this = 0;
	int status = 0;
	int counter = 0;
	u8 nirqLow = 0;
	if (work) {
		this = container_of(work,sx86xx_t,dworker.work);
		if (!this) {
			pr_err("%s: NULL sx86xx_t\n",__func__);
			return;
		}
		if (unlikely(this->useIrqTimer)) {
			if ((!this->get_nirq_low) || this->get_nirq_low()) {
				nirqLow = 1;
			}
		}
		/* since we are not in an interrupt don't need to disable irq. */
		status = this->refreshStatus(this);
		counter = -1;
		pr_debug("Worker - Refresh Status %d\n",status);
		while((++counter) < MAX_NUM_STATUS_BITS) { /* counter start from MSB */
			pr_debug("Looping Counter %d\n",counter);
			if (((status>>counter) & 0x01) && (this->statusFunc[counter])) {
				pr_debug("Function Pointer Found. Calling\n");
				this->statusFunc[counter](this);
			}
		}
		if (unlikely(this->useIrqTimer && nirqLow))
		{
			/* Early models and if RATE=0 for newer models require a penup timer */
			/* Queue up the function again for checking on penup */
			sx86xx_schedule_work(this,msecs_to_jiffies(this->irqTimeout));
		}
	} else {
		pr_err("%s: NULL work_struct\n",__func__);
	}
}
#endif

void sx86xx_suspend(psx86xx_t this)
{
	if(this){
		/* Stop any pending penup queues */
		cancel_delayed_work(&this->dworker);
		disable_irq(this->irq);
	}
}
void sx86xx_resume(psx86xx_t this)
{
	if (this) {
#ifdef USE_THREADED_IRQ
		mutex_lock(&this->mutex);
		/* Just in case need to reset any uncaught interrupts */
		sx86xx_process_interrupt(this,0);
		mutex_unlock(&this->mutex);
#else
		sx86xx_schedule_work(this, 0);
#endif
		enable_irq(this->irq);
	}
}

#ifdef CONFIG_HAS_WAKELOCK
#ifdef CONFIG_EARLYSUSPEND
/*TODO: Should actually call the device specific suspend/resume
 * As long as the kernel suspend/resume is setup, the device
 * specific ones will be called anyways
 */
extern suspend_state_t get_suspend_state(void);
void sx86xx_early_suspend(struct early_suspend *h)
{
	psx86xx_t this = 0;
	pr_debug("inside sx86xx_early_suspend()\n");
	this = container_of(h, sx86xx_t, early_suspend);
	sx86xx_suspend(this);
	pr_debug("exit sx86xx_early_suspend()\n");
}

void sx86xx_late_resume(struct early_suspend *h)
{
	psx86xx_t this = 0;
	pr_debug("inside sx86xx_late_resume()\n");
	this = container_of(h, sx86xx_t, early_suspend);
	sx86xx_resume(this);
	pr_debug("exit sx86xx_late_resume()\n");
}
#endif
#endif

int sx86xx_init(psx86xx_t this)
{
	int err = 0;
	if (this && this->pDevice)
	{
#ifdef USE_THREADED_IRQ
		/* initialize worker function */
		INIT_DELAYED_WORK(&this->dworker, sx86xx_worker_func);
		/* initialize mutex */
		mutex_init(&this->mutex);
		/* initailize interrupt reporting */
		this->irq_disabled = 0;
		err = request_threaded_irq(this->irq, NULL, sx86xx_interrupt_thread,
								IRQF_TRIGGER_FALLING, this->pdev->driver->name,
								this);
#else
		/* initialize spin lock */
		spin_lock_init(&this->lock);

		/* initialize worker function */
		INIT_DELAYED_WORK(&this->dworker, sx86xx_worker_func);

		/* initailize interrupt reporting */
		this->irq_disabled = 0;
		err = request_irq(this->irq, sx86xx_irq, IRQF_TRIGGER_FALLING,
									this->pdev->driver->name, this);
#endif
		if (err) {
			pr_err("irq %d busy?\n", this->irq);
			return err;
		}
#ifdef USE_THREADED_IRQ
		pr_info("%s:registered with threaded irq (%d)\n", __func__, this->irq);
#else
		pr_info("%s:registered with irq (%d)\n", __func__, this->irq);
#endif
#ifdef CONFIG_HAS_WAKELOCK
#ifdef CONFIG_EARLYSUSPEND
		this->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
		this->early_suspend.suspend = sx86xx_early_suspend;
		this->early_suspend.resume = sx86xx_late_resume;
		register_early_suspend(&this->early_suspend);
		if (has_wake_lock(WAKE_LOCK_SUSPEND) == 0 &&
			get_suspend_state() == PM_SUSPEND_ON)
			sx86xx_early_suspend(&this->early_suspend);
#endif
#endif //CONFIG_HAS_WAKELOCK
		/* call init function pointer (this should initialize all registers */
		if (this->init)
			return this->init(this);
		pr_err("%s:No init function!!!!\n",__func__);
	}
	return -ENOMEM;
}

int sx86xx_remove(psx86xx_t this)
{
	if (this) {
		cancel_delayed_work_sync(&this->dworker); /* Cancel the Worker Func */
		/*destroy_workqueue(this->workq); */
#ifdef CONFIG_HAS_WAKELOCK
#ifdef CONFIG_EARLYSUSPEND
		unregister_early_suspend(&this->early_suspend);
#endif
#endif
		free_irq(this->irq, this);
		kfree(this);
		return 0;
	}
	return -ENOMEM;
}
