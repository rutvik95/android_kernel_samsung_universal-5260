/* /linux/drivers/misc/modem_if/modem_modemctl_device_xmm6262.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define DEBUG

#include <linux/init.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/cma.h>
#include <plat/devs.h>
#include <linux/platform_data/modem_v2.h>
#include "modem_prj.h"

static int m74xx_on(struct modem_ctl *mc)
{
	mif_info("\n");

	if (!mc->gpio_cp_on) {
		mif_err("no gpio data\n");
		return -ENXIO;
	}

	gpio_set_value(mc->gpio_cp_on, 1);
	mdelay(100);

	mc->phone_state = STATE_BOOTING;

	return 0;
}

static int m74xx_off(struct modem_ctl *mc)
{
	mif_info("\n");

	if (!mc->gpio_cp_on) {
		mif_err("no gpio data\n");
		return -ENXIO;
	}

	gpio_set_value(mc->gpio_cp_on, 0);

	mc->phone_state = STATE_OFFLINE;

	return 0;
}

static int m74xx_force_crash_exit(struct modem_ctl *mc)
{
	mif_info("\n");

	return mc->cp_force_crash_exit();
}

static irqreturn_t phone_active_irq_handler(int irq, void *_mc)
{
#if 0
	int phone_reset = 0;
#endif
	int phone_active_value = 0;
	int cp_dump_value = 0;
	int phone_state = 0;
	struct modem_ctl *mc = (struct modem_ctl *)_mc;

	disable_irq_nosync(mc->irq_phone_active);
#if 0
	if (!mc->gpio_cp_reset || !mc->gpio_phone_active ||
			!mc->gpio_cp_dump_int) {
		mif_err("no gpio data\n");
		return IRQ_HANDLED;
	}

	phone_reset = gpio_get_value(mc->gpio_cp_reset);
#endif
	usleep_range(5000, 10000);
	phone_active_value = gpio_get_value(mc->gpio_phone_active);
	cp_dump_value = gpio_get_value(mc->gpio_cp_dump_int);

	mif_info("PA EVENT : pa=%d, cp_dump=%d\n",
		phone_active_value, cp_dump_value);

	if (phone_active_value) {
		phone_state = mc->phone_state;
	} else if (!phone_active_value) {
		if (cp_dump_value)
			phone_state = STATE_CRASH_EXIT;
		else
			phone_state = STATE_CRASH_RESET;
	} else if (mc->phone_state == STATE_CRASH_EXIT
					|| mc->phone_state == STATE_CRASH_RESET)
		phone_state = mc->phone_state;

	if (!phone_active_value) {
		if (mc->iod && mc->iod->modem_state_changed)
			mc->iod->modem_state_changed(mc->iod, phone_state);

		if (mc->bootd && mc->bootd->modem_state_changed)
			mc->bootd->modem_state_changed(mc->bootd, phone_state);
	}

	if (phone_active_value)
		irq_set_irq_type(mc->irq_phone_active, IRQ_TYPE_EDGE_FALLING);
	else
		irq_set_irq_type(mc->irq_phone_active, IRQ_TYPE_EDGE_RISING);

	enable_irq(mc->irq_phone_active);

	return IRQ_HANDLED;
}

static irqreturn_t sim_detect_irq_handler(int irq, void *_mc)
{
	struct modem_ctl *mc = (struct modem_ctl *)_mc;

	if (mc->iod && mc->iod->sim_state_changed)
		mc->iod->sim_state_changed(mc->iod,
			gpio_get_value(mc->gpio_sim_detect) == mc->sim_polarity
			);

	return IRQ_HANDLED;
}

static void m74xx_get_ops(struct modem_ctl *mc)
{
	mc->ops.modem_on = m74xx_on;
	mc->ops.modem_off = m74xx_off;
	mc->ops.modem_force_crash_exit = m74xx_force_crash_exit;
}

int m74xx_init_modemctl_device(struct modem_ctl *mc,
			struct modem_data *pdata)
{
	int ret = 0;
	struct platform_device *pdev;

	mc->gpio_cp_on = pdata->gpio_cp_on;
	mc->gpio_cp_reset = pdata->gpio_cp_reset;
	mc->gpio_pda_active = pdata->gpio_pda_active;
	mc->gpio_phone_active = pdata->gpio_phone_active;
	mc->gpio_cp_dump_int = pdata->gpio_cp_dump_int;
	mc->gpio_ap_dump_int = pdata->gpio_ap_dump_int;
	mc->gpio_flm_uart_sel = pdata->gpio_flm_uart_sel;
	mc->gpio_cp_warm_reset = pdata->gpio_cp_warm_reset;
	mc->gpio_sim_detect = pdata->gpio_sim_detect;
	mc->sim_polarity = pdata->sim_polarity;
	mc->cp_force_crash_exit = pdata->cp_force_crash;

	pdev = to_platform_device(mc->dev);
	mc->irq_phone_active = gpio_to_irq(mc->gpio_phone_active);

	if (mc->gpio_sim_detect)
		mc->irq_sim_detect = gpio_to_irq(mc->gpio_sim_detect);

	m74xx_get_ops(mc);

	ret = request_threaded_irq(mc->irq_phone_active, NULL,
			phone_active_irq_handler,
			IRQF_NO_SUSPEND | IRQF_TRIGGER_RISING,
			"phone_active", mc);
	if (ret) {
		mif_err("failed to request_irq:%d\n", ret);
		goto err_phone_active_request_irq;
	}

	ret = enable_irq_wake(mc->irq_phone_active);
	if (ret) {
		mif_err("failed to enable_irq_wake:%d\n", ret);
		goto err_phone_active_set_wake_irq;
	}

	/* initialize sim_state if gpio_sim_detect exists */
	mc->sim_state.online = false;
	mc->sim_state.changed = false;
	if (mc->gpio_sim_detect) {
		ret = request_irq(mc->irq_sim_detect, sim_detect_irq_handler,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				"sim_detect", mc);
		if (ret) {
			mif_err("failed to request_irq: %d\n", ret);
			goto err_sim_detect_request_irq;
		}

		ret = enable_irq_wake(mc->irq_sim_detect);
		if (ret) {
			mif_err("failed to enable_irq_wake: %d\n", ret);
			goto err_sim_detect_set_wake_irq;
		}

		/* initialize sim_state => insert: gpio=0, remove: gpio=1 */
		mc->sim_state.online =
			gpio_get_value(mc->gpio_sim_detect) == mc->sim_polarity;
	}

	return ret;

err_sim_detect_set_wake_irq:
	free_irq(mc->irq_sim_detect, mc);
err_sim_detect_request_irq:
	mc->sim_state.online = false;
	mc->sim_state.changed = false;
err_phone_active_set_wake_irq:
	free_irq(mc->irq_phone_active, mc);
err_phone_active_request_irq:
	return ret;
}
