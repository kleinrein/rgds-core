// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip sleep mode configuration for the "mem" suspend state (RG DS OS).
 *
 * On RK356x the deep sleep is done by the closed BL31 (rkbin): it powers the CPUs and the logic down, stops the
 * PLLs and the 24 MHz oscillator and asserts the PMIC's SLEEP pin, but only the parts of that its "suspend mode"
 * word allows. The vendor kernel's rockchip_pm_config driver passes that word and the wake sources to BL31 over
 * the Rockchip SiP call from a rockchip-suspend node; mainline has no equivalent, so BL31 runs with its defaults.
 * This is the small part of that driver we need: the two words and the debug switch, sent at probe and again
 * before every suspend (as the vendor driver does).
 *
 *   rockchip_suspend: rockchip-suspend {
 *       compatible = "rockchip,pm-rk3568";
 *       rockchip,sleep-mode-config = <(RKPM_SLP_ARMOFF_LOGOFF | ...)>;   dt-bindings/suspend/rockchip-rk3568.h
 *       rockchip,wakeup-config = <(RKPM_GPIO_WKUP_EN)>;
 *       rockchip,sleep-debug-en = <0>;
 *   };
 */
#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

#define ROCKCHIP_SIP_SUSPEND_MODE	0x82000003
#define SUSPEND_MODE_CONFIG		0x01
#define WKUP_SOURCE_CONFIG		0x02
#define SUSPEND_DEBUG_ENABLE		0x05

struct rockchip_pm_config {
	struct device *dev;
	u32 mode;
	u32 wakeup;
	u32 debug;
	bool have_mode, have_wakeup, have_debug;
};

static int rockchip_pm_sip(struct rockchip_pm_config *pm, u32 ctrl, u32 value, const char *what)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ROCKCHIP_SIP_SUSPEND_MODE, ctrl, value, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		dev_warn(pm->dev, "%s 0x%x refused by BL31 (%ld)\n", what, value, (long)res.a0);
	return (int)res.a0;
}

static void rockchip_pm_apply(struct rockchip_pm_config *pm)
{
	if (pm->have_mode)
		rockchip_pm_sip(pm, SUSPEND_MODE_CONFIG, pm->mode, "sleep-mode-config");
	if (pm->have_wakeup)
		rockchip_pm_sip(pm, WKUP_SOURCE_CONFIG, pm->wakeup, "wakeup-config");
}

static int rockchip_pm_config_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct rockchip_pm_config *pm;

	pm = devm_kzalloc(&pdev->dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;
	pm->dev = &pdev->dev;
	pm->have_mode = !of_property_read_u32(np, "rockchip,sleep-mode-config", &pm->mode);
	pm->have_wakeup = !of_property_read_u32(np, "rockchip,wakeup-config", &pm->wakeup);
	pm->have_debug = !of_property_read_u32(np, "rockchip,sleep-debug-en", &pm->debug);
	platform_set_drvdata(pdev, pm);

	if (pm->have_debug)
		rockchip_pm_sip(pm, SUSPEND_DEBUG_ENABLE, pm->debug, "sleep-debug-en");
	rockchip_pm_apply(pm);
	dev_info(&pdev->dev, "sleep mode 0x%x, wake sources 0x%x\n", pm->have_mode ? pm->mode : 0,
		 pm->have_wakeup ? pm->wakeup : 0);
	return 0;
}

static int rockchip_pm_config_prepare(struct device *dev)
{
	rockchip_pm_apply(dev_get_drvdata(dev));
	return 0;
}

static const struct dev_pm_ops rockchip_pm_config_pm_ops = {
	.prepare = rockchip_pm_config_prepare,
};

static const struct of_device_id rockchip_pm_config_of_match[] = {
	{ .compatible = "rockchip,pm-config" },
	{ .compatible = "rockchip,pm-rk3568" },
	{ }
};
MODULE_DEVICE_TABLE(of, rockchip_pm_config_of_match);

static struct platform_driver rockchip_pm_config_driver = {
	.probe = rockchip_pm_config_probe,
	.driver = {
		.name = "rockchip-pm-config",
		.of_match_table = rockchip_pm_config_of_match,
		.pm = &rockchip_pm_config_pm_ops,
	},
};
builtin_platform_driver(rockchip_pm_config_driver);
