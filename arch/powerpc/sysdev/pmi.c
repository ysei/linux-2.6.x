/*
 * pmi driver
 *
 * (C) Copyright IBM Deutschland Entwicklung GmbH 2005
 *
 * PMI (Platform Management Interrupt) is a way to communicate
 * with the BMC (Baseboard Management Controller) via interrupts.
 * Unlike IPMI it is bidirectional and has a low latency.
 *
 * Author: Christian Krafft <krafft@de.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <asm/of_device.h>
#include <asm/of_platform.h>
#include <asm/io.h>
#include <asm/pmi.h>


struct pmi_data {
	struct list_head	handler;
	spinlock_t		handler_spinlock;
	spinlock_t		pmi_spinlock;
	struct mutex		msg_mutex;
	pmi_message_t		msg;
	struct completion	*completion;
	struct of_device	*dev;
	int			irq;
	u8 __iomem		*pmi_reg;
	struct work_struct	work;
};



static void __iomem *of_iomap(struct device_node *np)
{
	struct resource res;

	if (of_address_to_resource(np, 0, &res))
		return NULL;

	pr_debug("Resource start: 0x%lx\n", res.start);
	pr_debug("Resource end: 0x%lx\n", res.end);

	return ioremap(res.start, 1 + res.end - res.start);
}


static int pmi_irq_handler(int irq, void *dev_id)
{
	struct pmi_data *data;
	u8 type;
	int rc;

	data = dev_id;

	spin_lock(&data->pmi_spinlock);

	type = ioread8(data->pmi_reg + PMI_READ_TYPE);
	pr_debug("pmi: got message of type %d\n", type);

	if (type & PMI_ACK && !data->completion) {
		printk(KERN_WARNING "pmi: got unexpected ACK message.\n");
		rc = -EIO;
		goto unlock;
	}

	if (data->completion && !(type & PMI_ACK)) {
		printk(KERN_WARNING "pmi: expected ACK, but got %d\n", type);
		rc = -EIO;
		goto unlock;
	}

	data->msg.type = type;
	data->msg.data0 = ioread8(data->pmi_reg + PMI_READ_DATA0);
	data->msg.data1 = ioread8(data->pmi_reg + PMI_READ_DATA1);
	data->msg.data2 = ioread8(data->pmi_reg + PMI_READ_DATA2);
	rc = 0;
unlock:
	spin_unlock(&data->pmi_spinlock);

	if (rc == -EIO) {
		rc = IRQ_HANDLED;
		goto out;
	}

	if (data->msg.type & PMI_ACK) {
		complete(data->completion);
		rc = IRQ_HANDLED;
		goto out;
	}

	schedule_work(&data->work);

	rc = IRQ_HANDLED;
out:
	return rc;
}


static struct of_device_id pmi_match[] = {
	{ .type = "ibm,pmi", .name = "ibm,pmi" },
	{},
};

MODULE_DEVICE_TABLE(of, pmi_match);

static void pmi_notify_handlers(struct work_struct *work)
{
	struct pmi_data *data;
	struct pmi_handler *handler;

	data = container_of(work, struct pmi_data, work);

	spin_lock(&data->handler_spinlock);
	list_for_each_entry(handler, &data->handler, node) {
		pr_debug(KERN_INFO "pmi: notifying handler %p\n", handler);
		if (handler->type == data->msg.type)
			handler->handle_pmi_message(data->dev, data->msg);
	}
	spin_unlock(&data->handler_spinlock);
}

static int pmi_of_probe(struct of_device *dev,
			const struct of_device_id *match)
{
	struct device_node *np = dev->node;
	struct pmi_data *data;
	int rc;

	data = kzalloc(sizeof(struct pmi_data), GFP_KERNEL);
	if (!data) {
		printk(KERN_ERR "pmi: could not allocate memory.\n");
		rc = -ENOMEM;
		goto out;
	}

	data->pmi_reg = of_iomap(np);
	if (!data->pmi_reg) {
		printk(KERN_ERR "pmi: invalid register address.\n");
		rc = -EFAULT;
		goto error_cleanup_data;
	}

	INIT_LIST_HEAD(&data->handler);

	mutex_init(&data->msg_mutex);
	spin_lock_init(&data->pmi_spinlock);
	spin_lock_init(&data->handler_spinlock);

	INIT_WORK(&data->work, pmi_notify_handlers);

	dev->dev.driver_data = data;
	data->dev = dev;

	data->irq = irq_of_parse_and_map(np, 0);
	if (data->irq == NO_IRQ) {
		printk(KERN_ERR "pmi: invalid interrupt.\n");
		rc = -EFAULT;
		goto error_cleanup_iomap;
	}

	rc = request_irq(data->irq, pmi_irq_handler, 0, "pmi", data);
	if (rc) {
		printk(KERN_ERR "pmi: can't request IRQ %d: returned %d\n",
				data->irq, rc);
		goto error_cleanup_iomap;
	}

	printk(KERN_INFO "pmi: found pmi device at addr %p.\n", data->pmi_reg);

	goto out;

error_cleanup_iomap:
	iounmap(data->pmi_reg);

error_cleanup_data:
	kfree(data);

out:
	return rc;
}

static int pmi_of_remove(struct of_device *dev)
{
	struct pmi_data *data;
	struct pmi_handler *handler, *tmp;

	data = dev->dev.driver_data;

	free_irq(data->irq, data);
	iounmap(data->pmi_reg);

	spin_lock(&data->handler_spinlock);

	list_for_each_entry_safe(handler, tmp, &data->handler, node)
		list_del(&handler->node);

	spin_unlock(&data->handler_spinlock);

	kfree(dev->dev.driver_data);

	return 0;
}

static struct of_platform_driver pmi_of_platform_driver = {
	.name		= "pmi",
	.match_table	= pmi_match,
	.probe		= pmi_of_probe,
	.remove		= pmi_of_remove
};

static int __init pmi_module_init(void)
{
	return of_register_platform_driver(&pmi_of_platform_driver);
}
module_init(pmi_module_init);

static void __exit pmi_module_exit(void)
{
	of_unregister_platform_driver(&pmi_of_platform_driver);
}
module_exit(pmi_module_exit);

void pmi_send_message(struct of_device *device, pmi_message_t msg)
{
	struct pmi_data *data;
	unsigned long flags;
	DECLARE_COMPLETION_ONSTACK(completion);

	data = device->dev.driver_data;

	mutex_lock(&data->msg_mutex);

	data->msg = msg;
	pr_debug("pmi_send_message: msg is %08x\n", *(u32*)&msg);

	data->completion = &completion;

	spin_lock_irqsave(&data->pmi_spinlock, flags);
	iowrite8(msg.data0, data->pmi_reg + PMI_WRITE_DATA0);
	iowrite8(msg.data1, data->pmi_reg + PMI_WRITE_DATA1);
	iowrite8(msg.data2, data->pmi_reg + PMI_WRITE_DATA2);
	iowrite8(msg.type, data->pmi_reg + PMI_WRITE_TYPE);
	spin_unlock_irqrestore(&data->pmi_spinlock, flags);

	pr_debug("pmi_send_message: wait for completion\n");

	wait_for_completion_interruptible_timeout(data->completion,
						  PMI_TIMEOUT);

	data->completion = NULL;

	mutex_unlock(&data->msg_mutex);
}
EXPORT_SYMBOL_GPL(pmi_send_message);

void pmi_register_handler(struct of_device *device,
			  struct pmi_handler *handler)
{
	struct pmi_data *data;
	data = device->dev.driver_data;

	spin_lock(&data->handler_spinlock);
	list_add_tail(&handler->node, &data->handler);
	spin_unlock(&data->handler_spinlock);
}
EXPORT_SYMBOL_GPL(pmi_register_handler);

void pmi_unregister_handler(struct of_device *device,
			    struct pmi_handler *handler)
{
	struct pmi_data *data;

	pr_debug("pmi: unregistering handler %p\n", handler);

	data = device->dev.driver_data;

	spin_lock(&data->handler_spinlock);
	list_del(&handler->node);
	spin_unlock(&data->handler_spinlock);
}
EXPORT_SYMBOL_GPL(pmi_unregister_handler);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Christian Krafft <krafft@de.ibm.com>");
MODULE_DESCRIPTION("IBM Platform Management Interrupt driver");
