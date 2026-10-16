// SPDX-License-Identifier: GPL-2.0-only
/*
 * CXL Type-2 Device Driver
 *
 * Minimal PCI driver for the QEMU emulated CXL type-2 device
 * (cxl-accel).
 *
 * Copyright (C) 2026 Red Hat, Inc.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/completion.h>

#include <linux/idr.h>

#include <cxl/cxl.h>
#include <cxl/pci.h>

#include "cxl_type2.h"

struct cxl_memdev *devm_cxl_add_classdev(struct cxl_dev_state *cxlds);

static DEFINE_IDA(cxl_accel_ida);

#define PCI_VENDOR_ID_REDHAT		0x1b36
#define CXL_TYPE2_DEVICE_ID		0x0123

/* Command BAR capability registers */
#define CXL_ACCEL_CAP_MAGIC		0x00
#define CXL_ACCEL_CAP_VERSION		0x04
#define CXL_ACCEL_CAP_CAPS		0x08
#define CXL_ACCEL_CAP_MAX_XFER		0x0C

#define CXL_ACCEL_MAGIC			0x32584C43  /* "CLX2" */

/* Command register offsets (relative to CXL_ACCEL_CMD_BASE) */
#define CXL_ACCEL_CMD_BASE		0x10
#define CXL_ACCEL_CMD_OPCODE		0x00
#define CXL_ACCEL_CMD_SRC_OFFSET	0x08
#define CXL_ACCEL_CMD_SRC_LENGTH	0x10
#define CXL_ACCEL_CMD_DST_OFFSET	0x18
#define CXL_ACCEL_CMD_DST_LENGTH	0x20
#define CXL_ACCEL_CMD_DOORBELL		0x28
#define CXL_ACCEL_CMD_STATUS		0x30
#define CXL_ACCEL_CMD_RESULT_LEN	0x38

#define CXL_ACCEL_STATUS_IDLE		0x00
#define CXL_ACCEL_STATUS_DONE		0x02
#define CXL_ACCEL_STATUS_ERROR		0x03

#define CXL_ACCEL_CMD_BAR		5
#define CXL_ACCEL_MSIX_CMD		2

struct cxl_type2 {
	struct cxl_dev_state cxlds;	/* must be at offset 0 */
	struct cxl_memdev *cxlmd;
	void __iomem *bar_regs;
	void __iomem *cmd_regs;
	u32 caps;
	u32 max_xfer;
	u64 capacity;
	int accel_id;
	struct miscdevice miscdev;
	struct completion cmd_done;
	struct mutex cmd_lock;
};

static irqreturn_t cxl_type2_cmd_irq(int irq, void *data)
{
	struct cxl_type2 *ct2d = data;

	complete(&ct2d->cmd_done);
	return IRQ_HANDLED;
}

static long cxl_type2_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct cxl_type2 *ct2d = container_of(filp->private_data,
					       struct cxl_type2, miscdev);
	struct cxl_accel_cmd ucmd;
	u32 status;
	u64 end;

	/* TODO: add CXL_ACCEL_IOCTL_GET_INFO for caps, max_xfer, capacity */
	if (cmd != CXL_ACCEL_IOCTL_SUBMIT)
		return -ENOTTY;

	if (copy_from_user(&ucmd, (void __user *)arg, sizeof(ucmd)))
		return -EFAULT;

	if (!ucmd.opcode || ucmd.opcode >= 32 || !(ct2d->caps & BIT(ucmd.opcode)))
		return -EINVAL;

	if (!ucmd.src_length || !ucmd.dst_length)
		return -EINVAL;

	if (ucmd.src_length > ct2d->max_xfer || ucmd.dst_length > ct2d->max_xfer)
		return -EINVAL;

	if (check_add_overflow(ucmd.src_offset, ucmd.src_length, &end) ||
	    end > ct2d->capacity)
		return -EINVAL;

	if (check_add_overflow(ucmd.dst_offset, ucmd.dst_length, &end) ||
	    end > ct2d->capacity)
		return -EINVAL;

	if (mutex_lock_interruptible(&ct2d->cmd_lock))
		return -ERESTARTSYS;

	reinit_completion(&ct2d->cmd_done);

	writel(ucmd.opcode, ct2d->cmd_regs + CXL_ACCEL_CMD_OPCODE);
	writeq(ucmd.src_offset, ct2d->cmd_regs + CXL_ACCEL_CMD_SRC_OFFSET);
	writeq(ucmd.src_length, ct2d->cmd_regs + CXL_ACCEL_CMD_SRC_LENGTH);
	writeq(ucmd.dst_offset, ct2d->cmd_regs + CXL_ACCEL_CMD_DST_OFFSET);
	writeq(ucmd.dst_length, ct2d->cmd_regs + CXL_ACCEL_CMD_DST_LENGTH);

	/* Ring the doorbell */
	writel(1, ct2d->cmd_regs + CXL_ACCEL_CMD_DOORBELL);

	if (!wait_for_completion_timeout(&ct2d->cmd_done,
					 msecs_to_jiffies(5000))) {
		mutex_unlock(&ct2d->cmd_lock);
		return -ETIMEDOUT;
	}

	status = readl(ct2d->cmd_regs + CXL_ACCEL_CMD_STATUS);
	if (status != CXL_ACCEL_STATUS_DONE) {
		mutex_unlock(&ct2d->cmd_lock);
		return -EIO;
	}

	ucmd.result_len = readq(ct2d->cmd_regs + CXL_ACCEL_CMD_RESULT_LEN);

	mutex_unlock(&ct2d->cmd_lock);

	if (copy_to_user((void __user *)arg, &ucmd, sizeof(ucmd)))
		return -EFAULT;

	return 0;
}

static const struct file_operations cxl_type2_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= cxl_type2_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static u64 cxl_type2_get_capacity(struct pci_dev *pdev, u16 dvsec)
{
	u32 hi, lo;

	if (pci_read_config_dword(pdev, dvsec + PCI_DVSEC_CXL_RANGE_SIZE_HIGH(0), &hi) ||
	    pci_read_config_dword(pdev, dvsec + PCI_DVSEC_CXL_RANGE_SIZE_LOW(0), &lo))
		return 0;

	if (!(lo & PCI_DVSEC_CXL_MEM_INFO_VALID))
		return 0;

	return ((u64)hi << 32) | (lo & PCI_DVSEC_CXL_MEM_SIZE_LOW);
}

static int cxl_type2_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct cxl_type2 *ct2d;
	struct cxl_dev_state *cxlds;
	u64 capacity;
	u16 dvsec;
	int rc;

	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	pci_set_master(pdev);

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_DEVICE);
	if (!dvsec) {
		pci_err(pdev, "CXL Device DVSEC not found\n");
		return -ENODEV;
	}

	ct2d = devm_cxl_dev_state_create(&pdev->dev, CXL_DEVTYPE_DEVMEM,
					 pci_get_dsn(pdev), dvsec,
					 struct cxl_type2, cxlds, false);
	if (!ct2d)
		return -ENOMEM;

	cxlds = &ct2d->cxlds;
	pci_set_drvdata(pdev, ct2d);

	rc = cxl_pci_setup_regs(pdev, CXL_REGLOC_RBI_COMPONENT,
				&cxlds->reg_map);
	if (rc) {
		pci_err(pdev, "Component register setup failed: %d\n", rc);
		return rc;
	}

	if (!cxlds->reg_map.component_map.hdm_decoder.valid) {
		pci_err(pdev, "HDM decoder registers not found\n");
		return -ENODEV;
	}

	if (!cxlds->reg_map.component_map.ras.valid) {
		pci_err(pdev, "RAS registers not found\n");
		return -ENODEV;
	}

	/* Mailbox not used yet; assert media ready explicitly */
	cxlds->media_ready = true;

	capacity = cxl_type2_get_capacity(pdev, dvsec);
	if (!capacity) {
		pci_err(pdev, "No CXL memory capacity reported\n");
		return -ENODEV;
	}

	ct2d->capacity = capacity;

	rc = cxl_set_capacity(cxlds, capacity);
	if (rc) {
		pci_err(pdev, "DPA capacity setup failed: %d\n", rc);
		return rc;
	}

	/*
	 * Register memdev without an attach callback so the CXL region
	 * probe creates a DAX region instead of handing memory directly
	 * to this driver. This lets the memory go through the standard
	 * DAX -> system-RAM path.
	 */
	ct2d->cxlmd = devm_cxl_add_classdev(cxlds);
	if (IS_ERR(ct2d->cxlmd)) {
		rc = PTR_ERR(ct2d->cxlmd);
		if (rc == -ENXIO)
			rc = -EPROBE_DEFER;
		pci_err(pdev, "CXL add_classdev failed: %d\n", rc);
		return rc;
	}

	/* Map command BAR and discover capabilities */
	ct2d->bar_regs = pcim_iomap(pdev, CXL_ACCEL_CMD_BAR, 0);
	if (!ct2d->bar_regs) {
		pci_err(pdev, "Failed to map command BAR\n");
		return -ENOMEM;
	}

	if (readl(ct2d->bar_regs + CXL_ACCEL_CAP_MAGIC) != CXL_ACCEL_MAGIC) {
		pci_err(pdev, "Bad command BAR magic\n");
		return -ENODEV;
	}

	if (readl(ct2d->bar_regs + CXL_ACCEL_CAP_VERSION) != 1) {
		pci_err(pdev, "Unsupported command BAR version\n");
		return -ENODEV;
	}

	ct2d->caps = readl(ct2d->bar_regs + CXL_ACCEL_CAP_CAPS);
	ct2d->max_xfer = readl(ct2d->bar_regs + CXL_ACCEL_CAP_MAX_XFER);
	ct2d->cmd_regs = ct2d->bar_regs + CXL_ACCEL_CMD_BASE;

	/* Vector 2 is command completion (0=mbox-bg, 1=mbox-urgent) */
	rc = pci_alloc_irq_vectors(pdev, CXL_ACCEL_MSIX_CMD + 1,
				   CXL_ACCEL_MSIX_CMD + 1, PCI_IRQ_MSIX);
	if (rc < 0) {
		pci_err(pdev, "Failed to allocate MSI-X vectors: %d\n", rc);
		return rc;
	}

	init_completion(&ct2d->cmd_done);
	mutex_init(&ct2d->cmd_lock);

	rc = devm_request_irq(&pdev->dev,
			      pci_irq_vector(pdev, CXL_ACCEL_MSIX_CMD),
			      cxl_type2_cmd_irq, 0, "cxl-type2-cmd", ct2d);
	if (rc) {
		pci_err(pdev, "Failed to request command IRQ: %d\n", rc);
		return rc;
	}

	/* Register char device */
	ct2d->accel_id = ida_alloc(&cxl_accel_ida, GFP_KERNEL);
	if (ct2d->accel_id < 0)
		return ct2d->accel_id;

	ct2d->miscdev.minor = MISC_DYNAMIC_MINOR;
	ct2d->miscdev.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					     "cxl-accel%d", ct2d->accel_id);
	if (!ct2d->miscdev.name) {
		ida_free(&cxl_accel_ida, ct2d->accel_id);
		return -ENOMEM;
	}
	ct2d->miscdev.fops = &cxl_type2_fops;
	ct2d->miscdev.parent = &pdev->dev;

	rc = misc_register(&ct2d->miscdev);
	if (rc) {
		pci_err(pdev, "Failed to register misc device: %d\n", rc);
		ida_free(&cxl_accel_ida, ct2d->accel_id);
		return rc;
	}

	pci_info(pdev, "CXL type-2: %llu MiB DPA, caps 0x%x, max_xfer %u MiB, accel %s\n",
		 capacity >> 20, ct2d->caps, ct2d->max_xfer >> 20,
		 ct2d->miscdev.name);

	return 0;
}

static void cxl_type2_remove(struct pci_dev *pdev)
{
	struct cxl_type2 *ct2d = pci_get_drvdata(pdev);

	misc_deregister(&ct2d->miscdev);
	ida_free(&cxl_accel_ida, ct2d->accel_id);
}

static const struct pci_device_id cxl_type2_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REDHAT, CXL_TYPE2_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, cxl_type2_ids);

static struct pci_driver cxl_type2_driver = {
	.name		= "cxl-type2",
	.id_table	= cxl_type2_ids,
	.probe		= cxl_type2_probe,
	.remove		= cxl_type2_remove,
};
module_pci_driver(cxl_type2_driver);

MODULE_IMPORT_NS("CXL");
MODULE_AUTHOR("Cedric Le Goater <clg@redhat.com>");
MODULE_DESCRIPTION("CXL Type-2 Device Driver");
MODULE_LICENSE("GPL");
