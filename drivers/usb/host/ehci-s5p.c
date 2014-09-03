/*
 * SAMSUNG S5P USB HOST EHCI Controller
 *
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 * Author: Jingoo Han <jg1.han@samsung.com>
 * Author: Joonyoung Shim <jy0922.shim@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <linux/pm_qos.h>

#include <plat/cpu.h>
#include <plat/ehci.h>
#include <plat/usb-phy.h>

#include <mach/regs-pmu.h>
#include <mach/regs-usb-host.h>

#if defined(CONFIG_MDM_HSIC_PM)
//#include <mach/subsystem_restart.h>
#include <linux/mdm_hsic_pm.h>
#endif

#if defined(CONFIG_EHCI_IRQ_DISTRIBUTION)
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/pm_qos.h>

/* 4: First BIG core */
#define EHCI_IRQ_CPU	4

int irq_select_affinity_usr(unsigned int irq, struct cpumask *mask);

static struct notifier_block rndis_notifier;
static struct notifier_block ehci_cpu_notifier;
static atomic_t use_rndis;
static int g_ehci_irq;
#endif

static struct pm_qos_request s5p_ehci_mif_qos;

struct s5p_ehci_hcd {
	struct device *dev;
	struct usb_hcd *hcd;
	struct clk *clk;
	int power_on;
};

#ifdef CONFIG_USB_EXYNOS_SWITCH
int s5p_ehci_port_power_off(struct platform_device *pdev)
{
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	(void) ehci_hub_control(hcd,
			ClearPortFeature,
			USB_PORT_FEAT_POWER,
			1, NULL, 0);
	/* Flush those writes */
	ehci_readl(ehci, &ehci->regs->command);
	return 0;
}
EXPORT_SYMBOL_GPL(s5p_ehci_port_power_off);

int s5p_ehci_port_power_on(struct platform_device *pdev)
{
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	(void) ehci_hub_control(hcd,
			SetPortFeature,
			USB_PORT_FEAT_POWER,
			1, NULL, 0);
	/* Flush those writes */
	ehci_readl(ehci, &ehci->regs->command);
	return 0;
}
EXPORT_SYMBOL_GPL(s5p_ehci_port_power_on);
#endif

static int s5p_ehci_configurate(struct usb_hcd *hcd)
{
	int delay_count = 0;

	/* This is for waiting phy before ehci configuration */
	do {
		if (readl(hcd->regs))
			break;
		udelay(1);
		++delay_count;
	} while (delay_count < 200);
	if (delay_count)
		dev_info(hcd->self.controller, "phy delay count = %d\n",
			delay_count);

	/* DMA burst Enable, set utmi suspend_on_n */
	writel(readl(INSNREG00(hcd->regs)) | ENA_DMA_INCR | OHCI_SUSP_LGCY,
		INSNREG00(hcd->regs));
	return 0;
}

#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_LINK_DEVICE_USB) ||\
	defined(CONFIG_CDMA_MODEM_MDM6600) || defined(CONFIG_MDM_HSIC_PM)
#ifdef CONFIG_MACH_P8LTE
#define CP_PORT		 1  /* HSIC0 in S5PC210 */
#else
#define CP_PORT      2  /* HSIC0 in S5PC210 */
#endif
#define RETRY_CNT_LIMIT 30  /* Max 300ms wait for cp resume*/

int s5p_ehci_port_control(struct platform_device *pdev, int port, int enable)
{
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	(void) ehci_hub_control(hcd,
			enable ? SetPortFeature : ClearPortFeature,
			USB_PORT_FEAT_POWER,
			port, NULL, 0);
	/* Flush those writes */
	ehci_readl(ehci, &ehci->regs->command);
	return 0;
}
#endif

static void s5p_ehci_phy_init(struct platform_device *pdev)
{
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;

	if (pdata && pdata->phy_init) {
		pdata->phy_init(pdev, S5P_USB_PHY_HOST);

		s5p_ehci_configurate(hcd);
	}
}

#ifdef CONFIG_PM
static int s5p_ehci_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	unsigned long flags;
	int rc = 0;

#if defined(CONFIG_MDM_HSIC_PM)
	/*
	 * check suspend returns 1 if it is possible to suspend
	 * otherwise, it returns 0 impossible or returns some error
	 */
	rc = check_udev_suspend_allowed();
	if (rc > 0) {
		int ret = usb2phy_notifier(STATE_HSIC_SUSPEND, NULL);
		if ((ret != NOTIFY_DONE) && (ret != NOTIFY_OK)) {
			pr_info("%s: \n", __func__);
			return -EBUSY;
		}
	} else if (rc == -ENODEV) {
		/* no hsic pm driver loaded, proceed suspend */
		pr_debug("%s: suspend without hsic pm\n", __func__);
	} else {
		pm_runtime_resume(&pdev->dev);
		return -EBUSY;
	}
	rc = 0;
#endif
#if defined(CONFIG_LINK_DEVICE_HSIC)
	if (is_cp_wait_for_resume()) {
		pr_info("%s: suspend fail by host wakeup irq\n", __func__);
		pm_runtime_resume(&pdev->dev);
		return -EBUSY;
	}
#endif

	if (time_before(jiffies, ehci->next_statechange))
		msleep(10);

	/* Root hub was already suspended. Disable irq emission and
	 * mark HW unaccessible, bail out if RH has been resumed. Use
	 * the spinlock to properly synchronize with possible pending
	 * RH suspend or resume activity.
	 *
	 * This is still racy as hcd->state is manipulated outside of
	 * any locks =P But that will be a different fix.
	 */

	spin_lock_irqsave(&ehci->lock, flags);
	if (ehci->rh_state != EHCI_RH_SUSPENDED && ehci->rh_state != EHCI_RH_HALTED) {
		spin_unlock_irqrestore(&ehci->lock, flags);
		return -EINVAL;
	}
	ehci_writel(ehci, 0, &ehci->regs->intr_enable);
	(void)ehci_readl(ehci, &ehci->regs->intr_enable);

	clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	spin_unlock_irqrestore(&ehci->lock, flags);

	if (pdata && pdata->phy_exit)
		pdata->phy_exit(pdev, S5P_USB_PHY_HOST);

	clk_disable(s5p_ehci->clk);

	if (pm_qos_request_active(&s5p_ehci_mif_qos))
		pm_qos_update_request(&s5p_ehci_mif_qos, 0);
	else
		pm_qos_add_request(&s5p_ehci_mif_qos, PM_QOS_BUS_THROUGHPUT, 0);

	return rc;
}

static int s5p_ehci_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	if (pm_qos_request_active(&s5p_ehci_mif_qos))
		pm_qos_update_request(&s5p_ehci_mif_qos, 266000);
	else
		pm_qos_add_request(&s5p_ehci_mif_qos, PM_QOS_BUS_THROUGHPUT, 266000);

	clk_enable(s5p_ehci->clk);

	s5p_ehci_phy_init(pdev);

	if (time_before(jiffies, ehci->next_statechange))
		msleep(10);

	/* Mark hardware accessible again as we are out of D3 state by now */
	set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	if (ehci_readl(ehci, &ehci->regs->configured_flag) == FLAG_CF) {
		int	mask = INTR_MASK;

		if (!hcd->self.root_hub->do_remote_wakeup)
			mask &= ~STS_PCD;
		ehci_writel(ehci, mask, &ehci->regs->intr_enable);
		ehci_readl(ehci, &ehci->regs->intr_enable);
		return 0;
	}

	ehci_dbg(ehci, "lost power, restarting\n");
	usb_root_hub_lost_power(hcd->self.root_hub);

	(void) ehci_halt(ehci);
	(void) ehci_reset(ehci);

	/* emptying the schedule aborts any urbs */
	spin_lock_irq(&ehci->lock);
	if (ehci->reclaim)
		end_unlink_async(ehci);
	ehci_work(ehci);
	spin_unlock_irq(&ehci->lock);

	ehci_writel(ehci, ehci->command, &ehci->regs->command);
	ehci_writel(ehci, FLAG_CF, &ehci->regs->configured_flag);
	ehci_readl(ehci, &ehci->regs->command);	/* unblock posted writes */

	/* here we "know" root ports should always stay powered */
	ehci_port_power(ehci, 1);

	ehci->rh_state = EHCI_RH_SUSPENDED;

	/* Update runtime PM status and clear runtime_error */
	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* Prevent device from runtime suspend during resume time */
	pm_runtime_get_sync(dev);

#if defined(CONFIG_MDM_HSIC_PM)
	usb2phy_notifier(STATE_HSIC_RESUME, NULL);
#endif
#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_MDM_HSIC_PM)
	pm_runtime_mark_last_busy(&hcd->self.root_hub->dev);
#endif
	return 0;
}

int s5p_ehci_bus_suspend(struct usb_hcd *hcd)
{
	int ret;
	ret = ehci_bus_suspend(hcd);

#ifdef CONFIG_USB_SUSPEND
	/* Decrease pm_count that was increased at s5p_ehci_resume func. */
	if (hcd->self.controller->power.runtime_auto)
		pm_runtime_put_noidle(hcd->self.controller);
#endif

	return ret;
}

int s5p_ehci_bus_resume(struct usb_hcd *hcd)
{
	/* When suspend is failed, re-enable clocks & PHY */
	pm_runtime_resume(hcd->self.controller);

	return ehci_bus_resume(hcd);
}
#else
#define s5p_ehci_suspend	NULL
#define s5p_ehci_resume		NULL
#define s5p_ehci_bus_resume	NULL
#define s5p_ehci_bus_suspend	NULL
#endif

#ifdef CONFIG_USB_SUSPEND
static int s5p_ehci_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;

	if (pdata && pdata->phy_suspend)
		pdata->phy_suspend(pdev, S5P_USB_PHY_HOST);

#if defined(CONFIG_MDM_HSIC_PM)
	request_active_lock_release();
#endif
#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_MDM_HSIC_PM)
	pr_info("%s: usage=%d, child=%d\n", __func__,
					atomic_read(&dev->power.usage_count),
					atomic_read(&dev->power.child_count));
#endif

	if (pm_qos_request_active(&s5p_ehci_mif_qos))
		pm_qos_update_request(&s5p_ehci_mif_qos, 0);
	else
		pm_qos_add_request(&s5p_ehci_mif_qos, PM_QOS_BUS_THROUGHPUT, 0);

	return 0;
}

static int s5p_ehci_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	int rc = 0;

	if (dev->power.is_suspended)
		return 0;

#if defined(CONFIG_MDM_HSIC_PM)
	request_active_lock_set();
#endif
	if (pm_qos_request_active(&s5p_ehci_mif_qos))
		pm_qos_update_request(&s5p_ehci_mif_qos, 266000);
	else
		pm_qos_add_request(&s5p_ehci_mif_qos, PM_QOS_BUS_THROUGHPUT, 266000);

	/* platform device isn't suspended */
	if (pdata && pdata->phy_resume)
		rc = pdata->phy_resume(pdev, S5P_USB_PHY_HOST);

	if (rc) {
		s5p_ehci_configurate(hcd);

		/* emptying the schedule aborts any urbs */
		spin_lock_irq(&ehci->lock);
		if (ehci->reclaim)
			end_unlink_async(ehci);
		ehci_work(ehci);
		spin_unlock_irq(&ehci->lock);

		usb_root_hub_lost_power(hcd->self.root_hub);

		ehci_writel(ehci, FLAG_CF, &ehci->regs->configured_flag);
		ehci_writel(ehci, INTR_MASK, &ehci->regs->intr_enable);
		(void)ehci_readl(ehci, &ehci->regs->intr_enable);

		/* here we "know" root ports should always stay powered */
		ehci_port_power(ehci, 1);

		ehci->rh_state = EHCI_RH_SUSPENDED;
#if defined(CONFIG_MDM_HSIC_PM)
		pr_info("%s: %d\n", __func__, __LINE__);
		usb2phy_notifier(STATE_HSIC_RESUME, NULL);
#endif
	}
#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_MDM_HSIC_PM)
	pr_info("%s: usage=%d, child=%d\n", __func__,
					atomic_read(&dev->power.usage_count),
					atomic_read(&dev->power.child_count));
#endif
	return 0;
}
#else
#define s5p_ehci_runtime_suspend	NULL
#define s5p_ehci_runtime_resume		NULL
#endif

static const struct hc_driver s5p_ehci_hc_driver = {
	.description		= hcd_name,
	.product_desc		= "S5P EHCI Host Controller",
	.hcd_priv_size		= sizeof(struct ehci_hcd),

	.irq			= ehci_irq,
	.flags			= HCD_MEMORY | HCD_USB2,

	.reset			= ehci_init,
	.start			= ehci_run,
	.stop			= ehci_stop,
	.shutdown		= ehci_shutdown,

	.get_frame_number	= ehci_get_frame,

	.urb_enqueue		= ehci_urb_enqueue,
	.urb_dequeue		= ehci_urb_dequeue,
	.endpoint_disable	= ehci_endpoint_disable,
	.endpoint_reset		= ehci_endpoint_reset,

	.hub_status_data	= ehci_hub_status_data,
	.hub_control		= ehci_hub_control,
	.bus_suspend		= s5p_ehci_bus_suspend,
	.bus_resume		= s5p_ehci_bus_resume,

	.relinquish_port	= ehci_relinquish_port,
	.port_handed_over	= ehci_port_handed_over,

	.clear_tt_buffer_complete	= ehci_clear_tt_buffer_complete,
};

static ssize_t show_ehci_power(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);

	return snprintf(buf, PAGE_SIZE, "EHCI Power %s\n",
			(s5p_ehci->power_on) ? "on" : "off");
}

static ssize_t store_ehci_power(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;
	int power_on;
	int irq;
	int retval;

	if (sscanf(buf, "%d", &power_on) != 1)
		return -EINVAL;

	device_lock(dev);

	if (!power_on && s5p_ehci->power_on) {
		printk(KERN_DEBUG "%s: EHCI turns off\n", __func__);
#if defined(CONFIG_LINK_DEVICE_HSIC)
		if (hcd->self.root_hub)
			pm_runtime_forbid(&hcd->self.root_hub->dev);
#endif
		pm_runtime_forbid(dev);
		s5p_ehci->power_on = 0;
		usb_remove_hcd(hcd);

		if (pdata && pdata->phy_exit)
			pdata->phy_exit(pdev, S5P_USB_PHY_HOST);

		if (pm_qos_request_active(&s5p_ehci_mif_qos))
			pm_qos_remove_request(&s5p_ehci_mif_qos);
	} else if (power_on) {
		printk(KERN_DEBUG "%s: EHCI turns on\n", __func__);
		if (s5p_ehci->power_on) {
			pm_runtime_forbid(dev);
			usb_remove_hcd(hcd);
		} else
			s5p_ehci_phy_init(pdev);

		irq = platform_get_irq(pdev, 0);
		retval = usb_add_hcd(hcd, irq, IRQF_SHARED);
		if (retval < 0) {
			dev_err(dev, "Power On Fail\n");
			goto exit;
		}

		/*
		 * EHCI root hubs are expected to handle remote wakeup.
		 * So, wakeup flag init defaults for root hubs.
		 */
		device_wakeup_enable(&hcd->self.root_hub->dev);

		s5p_ehci->power_on = 1;
#if defined(CONFIG_LINK_DEVICE_HSIC)
		/* Sometimes XMM6262 send remote wakeup when hub enter suspend
		 * So, set the hub waiting 500ms autosuspend delay*/
		if (hcd->self.root_hub)
			pm_runtime_set_autosuspend_delay(
				&hcd->self.root_hub->dev, 500);
		/* mif allow the ehci runtime after enumeration */
		pm_runtime_forbid(dev);
#else
		pm_runtime_allow(dev);
#endif
	}
exit:
	device_unlock(dev);
	return count;
}
static DEVICE_ATTR(ehci_power, S_IWUSR | S_IWGRP | S_IRUSR | S_IRGRP,
	show_ehci_power, store_ehci_power);

#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_LINK_DEVICE_USB)
static ssize_t store_port_power(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
#if 0
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;
#endif

	int power_on, port;
	int err;

	err = sscanf(buf, "%d %d", &power_on, &port);
	if (err < 2 || port < 0 || port > 3 || power_on < 0 || power_on > 1) {
		pr_err("port power fail: port_power 1 2(port 2 enable 1)\n");
		return count;
	}

	pr_debug("%s: Port:%d power: %d\n", __func__, port, power_on);
	device_lock(dev);
	s5p_ehci_port_control(pdev, port, power_on);

#if 0
	/*HSIC IPC control the ACTIVE_STATE*/
	if (pdata && pdata->noti_host_states && port == CP_PORT)
		pdata->noti_host_states(pdev, power_on ? S5P_HOST_ON :
			S5P_HOST_OFF);
#endif
	device_unlock(dev);
	return count;
}
static DEVICE_ATTR(port_power, 0664, NULL, store_port_power);
#endif

static inline int create_ehci_sys_file(struct ehci_hcd *ehci)
{
#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_LINK_DEVICE_USB)
	BUG_ON(device_create_file(ehci_to_hcd(ehci)->self.controller,
			&dev_attr_port_power));
#endif
	return device_create_file(ehci_to_hcd(ehci)->self.controller,
			&dev_attr_ehci_power);
}

static inline void remove_ehci_sys_file(struct ehci_hcd *ehci)
{
	device_remove_file(ehci_to_hcd(ehci)->self.controller,
			&dev_attr_ehci_power);
#if defined(CONFIG_LINK_DEVICE_HSIC) || defined(CONFIG_LINK_DEVICE_USB)
	device_remove_file(ehci_to_hcd(ehci)->self.controller,
			&dev_attr_port_power);
#endif
}

#if defined(CONFIG_EHCI_IRQ_DISTRIBUTION)
static int set_cpu_core_from_usb_irq(int enable)
{
	int err = 0;
	unsigned int irq = g_ehci_irq;
	cpumask_var_t new_value;

	if (!irq_can_set_affinity(irq))
		return -EIO;

	if (enable) {
		err = irq_set_affinity(irq, cpumask_of(4));
	} else {

		if (!alloc_cpumask_var(&new_value, GFP_KERNEL))
			return -ENOMEM;

		cpumask_setall(new_value);

		if (!cpumask_intersects(new_value, cpu_online_mask))
			err = irq_select_affinity_usr(irq, new_value);
		else
			err = irq_set_affinity(irq, new_value);

		free_cpumask_var(new_value);
	}

	return err;
}

static int __cpuinit s5p_ehci_cpu_notify(struct notifier_block *self,
				unsigned long action, void *hcpu)
{
	int cpu = (unsigned long)hcpu;

	if (!g_ehci_irq || cpu != EHCI_IRQ_CPU || !atomic_read(&use_rndis))
		goto exit;

	switch (action) {
	case CPU_ONLINE:
	case CPU_DOWN_FAILED:
	case CPU_ONLINE_FROZEN:
		set_cpu_core_from_usb_irq(true);
		pr_info("%s: set ehci irq to cpu%d\n", __func__, cpu);
		break;
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		set_cpu_core_from_usb_irq(false);
		pr_info("%s: set ehci irq to cpu%d\n", __func__, 0);
		break;
	default:
		break;
	}
exit:
	return NOTIFY_OK;
}

static int rndis_notify_callback(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	struct net_device *dev = ptr;

	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;

	if (!strncmp(dev->name, "rndis", 5)) {
		switch (event) {
		case NETDEV_UP:
			atomic_inc(&use_rndis);
			set_cpu_core_from_usb_irq(true);
			break;
		case NETDEV_DOWN:
			set_cpu_core_from_usb_irq(false);
			atomic_dec(&use_rndis);
			break;
		}
	}
	return NOTIFY_DONE;
}
#endif

static int __devinit s5p_ehci_probe(struct platform_device *pdev)
{
	struct s5p_ehci_platdata *pdata;
	struct s5p_ehci_hcd *s5p_ehci;
	struct usb_hcd *hcd;
	struct ehci_hcd *ehci;
	struct resource *res;
	int irq;
	int err;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data defined\n");
		return -EINVAL;
	}

	s5p_ehci = kzalloc(sizeof(struct s5p_ehci_hcd), GFP_KERNEL);
	if (!s5p_ehci)
		return -ENOMEM;

	s5p_ehci->dev = &pdev->dev;

	hcd = usb_create_hcd(&s5p_ehci_hc_driver, &pdev->dev,
					dev_name(&pdev->dev));
	if (!hcd) {
		dev_err(&pdev->dev, "Unable to create HCD\n");
		err = -ENOMEM;
		goto fail_hcd;
	}

	s5p_ehci->hcd = hcd;
	s5p_ehci->clk = clk_get(&pdev->dev, "usbhost");

	if (IS_ERR(s5p_ehci->clk)) {
		dev_err(&pdev->dev, "Failed to get usbhost clock\n");
		err = PTR_ERR(s5p_ehci->clk);
		goto fail_clk;
	}

	err = clk_enable(s5p_ehci->clk);
	if (err)
		goto fail_clken;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get I/O memory\n");
		err = -ENXIO;
		goto fail_io;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);
	hcd->regs = ioremap(res->start, resource_size(res));
	if (!hcd->regs) {
		dev_err(&pdev->dev, "Failed to remap I/O memory\n");
		err = -ENOMEM;
		goto fail_io;
	}

	irq = platform_get_irq(pdev, 0);
	if (!irq) {
		dev_err(&pdev->dev, "Failed to get IRQ\n");
		err = -ENODEV;
		goto fail;
	}

	platform_set_drvdata(pdev, s5p_ehci);

#if defined(CONFIG_EHCI_IRQ_DISTRIBUTION)
	if (num_possible_cpus() > 1) {
		atomic_set(&use_rndis, 0);
		g_ehci_irq = irq;
		rndis_notifier.notifier_call = rndis_notify_callback;
		register_netdevice_notifier(&rndis_notifier);
		ehci_cpu_notifier.notifier_call = s5p_ehci_cpu_notify;
		register_cpu_notifier(&ehci_cpu_notifier);
	}
#endif
	s5p_ehci_phy_init(pdev);

	ehci = hcd_to_ehci(hcd);
	ehci->caps = hcd->regs;
	ehci->regs = hcd->regs +
		HC_LENGTH(ehci, readl(&ehci->caps->hc_capbase));

	dbg_hcs_params(ehci, "reset");
	dbg_hcc_params(ehci, "reset");

	/* cache this readonly data; minimize chip reads */
	ehci->hcs_params = readl(&ehci->caps->hcs_params);

	err = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (err) {
		dev_err(&pdev->dev, "Failed to add USB HCD\n");
		goto fail;
	}

	/*
	 * EHCI root hubs are expected to handle remote wakeup.
	 * So, wakeup flag init defaults for root hubs.
	 */
	device_wakeup_enable(&hcd->self.root_hub->dev);

	create_ehci_sys_file(ehci);
	s5p_ehci->power_on = 1;

#ifdef CONFIG_USB_SUSPEND
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get(hcd->self.controller);
#endif
#if defined(CONFIG_MDM_HSIC_PM)
	if (pdev->dev.power.disable_depth > 0)
		pm_runtime_enable(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&hcd->self.root_hub->dev, 10);
	pm_runtime_forbid(&pdev->dev);
#endif
#if defined(CONFIG_SEC_MODEM_ERICSSON)
    /* forbid runtime pm until modem enumeration */
    pm_runtime_forbid(&pdev->dev);
#endif
	return 0;

fail:
	iounmap(hcd->regs);
fail_io:
	clk_disable(s5p_ehci->clk);
fail_clken:
	clk_put(s5p_ehci->clk);
fail_clk:
	usb_put_hcd(hcd);
fail_hcd:
	kfree(s5p_ehci);
	return err;
}

static int __devexit s5p_ehci_remove(struct platform_device *pdev)
{
	struct s5p_ehci_platdata *pdata = pdev->dev.platform_data;
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;

#ifdef CONFIG_USB_SUSPEND
	pm_runtime_put(hcd->self.controller);
#if !defined(CONFIG_MDM_HSIC_PM)
	pm_runtime_disable(&pdev->dev);
#endif
#endif
	s5p_ehci->power_on = 0;
	remove_ehci_sys_file(hcd_to_ehci(hcd));
	usb_remove_hcd(hcd);

#if defined(CONFIG_EHCI_IRQ_DISTRIBUTION)
	if (num_possible_cpus() > 1) {
		atomic_set(&use_rndis, 0);
		g_ehci_irq = 0;
		unregister_netdevice_notifier(&rndis_notifier);
		unregister_cpu_notifier(&ehci_cpu_notifier);
	}
#endif

	if (pdata && pdata->phy_exit)
		pdata->phy_exit(pdev, S5P_USB_PHY_HOST);

	iounmap(hcd->regs);

	clk_disable(s5p_ehci->clk);
	clk_put(s5p_ehci->clk);

	usb_put_hcd(hcd);
	kfree(s5p_ehci);

	return 0;
}

static void s5p_ehci_shutdown(struct platform_device *pdev)
{
	struct s5p_ehci_hcd *s5p_ehci = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = s5p_ehci->hcd;

	if (!hcd->rh_registered)
		return;

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);
}

static const struct dev_pm_ops s5p_ehci_pm_ops = {
	.suspend		= s5p_ehci_suspend,
	.resume			= s5p_ehci_resume,
	.runtime_suspend	= s5p_ehci_runtime_suspend,
	.runtime_resume		= s5p_ehci_runtime_resume,
};

static struct platform_driver s5p_ehci_driver = {
	.probe		= s5p_ehci_probe,
	.remove		= __devexit_p(s5p_ehci_remove),
	.shutdown	= s5p_ehci_shutdown,
	.driver = {
		.name	= "s5p-ehci",
		.owner	= THIS_MODULE,
		.pm = &s5p_ehci_pm_ops,
	}
};

MODULE_ALIAS("platform:s5p-ehci");